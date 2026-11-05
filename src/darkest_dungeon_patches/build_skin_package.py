"""Build a configured multi-hero skin package outside the game installation."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tempfile
from pathlib import Path

from .paths import DEFAULT_GAME_ROOT, DEFAULT_PROJECT_ROOT
from .skin_config import (
    DEFAULT_CONFIG,
    MANIFEST,
    config_digest,
    group_for_name,
    load_config,
    relative_path,
    route_rows,
    token,
    validate_routes,
    write_rig_map,
)


def skeleton_prefix(directory):
    prefixes = {p.stem.rsplit(".", 1)[0] + "." for p in directory.glob("*.skel")}
    if len(prefixes) != 1:
        raise ValueError(
            f"Cannot infer one skeleton prefix in {directory}; configure heroes explicitly"
        )
    return token(prefixes.pop(), 159)


def discover_config(config, release, game):
    """Resolve skin-local rigs first, then the same Mod's hero rig, then vanilla."""
    if "heroes" in config:
        return config
    heroes = []
    for root in sorted((release / "heroes").iterdir()):
        if not root.is_dir():
            continue
        hero = token(root.name, 255)
        native = game / "heroes" / hero
        skins = {p.name: p for p in root.glob(hero + "_*") if p.is_dir()}
        if not skins:
            continue
        vanilla = {p.name for p in native.glob(hero + "_*") if p.is_dir()}
        groups, shared = [], []
        for skin, directory in sorted(skins.items()):
            local = directory / "anim"
            if any(local.glob("*.skel")):
                groups.append(
                    {
                        "id": f"skin_{skin}",
                        "prefix": f"{hero}.sprite.rig_{skin}.",
                        "skins": [skin],
                        "assets": {
                            "root": "source",
                            "anim": local.relative_to(release).as_posix(),
                            "textures": local.relative_to(release).as_posix(),
                            "prefix": skeleton_prefix(local),
                        },
                    }
                )
                vanilla.discard(skin)
            else:
                shared.append(skin)
        if shared and any((root / "anim").glob("*.skel")):
            groups.append({"id": "mod", "prefix": skeleton_prefix(root / "anim"), "skins": shared})
            vanilla.difference_update(shared)
        else:
            vanilla.update(shared)
        if vanilla:
            if not any((native / "anim").glob("*.skel")):
                raise ValueError(f"No native skeleton for texture-only skins: {hero}")
            palette = next(
                (p for p in sorted(native.glob(hero + "_*")) if (p / "anim").is_dir()), None
            )
            if palette is None:
                raise ValueError(f"No native texture palette for {hero}")
            groups.append(
                {
                    "id": "vanilla",
                    "prefix": f"{hero}.sprite.rig_vanilla.",
                    "skins": sorted(vanilla),
                    "assets": {
                        "root": "game",
                        "anim": f"heroes/{hero}/anim",
                        "textures": (palette / "anim").relative_to(game).as_posix(),
                        "prefix": skeleton_prefix(native / "anim"),
                    },
                }
            )
        if groups:
            heroes.append({"id": hero, "groups": groups})
    resolved = dict(config, heroes=heroes)
    validate_routes(route_rows(resolved))
    return resolved


def atlas_pages(text):
    lines = text.splitlines()
    pages = [
        (i, line.strip())
        for i, line in enumerate(lines)
        if line
        and not line[0].isspace()
        and line.lower().endswith((".png", ".tga"))
        and i + 1 < len(lines)
        and lines[i + 1].startswith(("size:", "format:", "filter:"))
    ]
    if not pages:
        raise ValueError("Atlas has no PNG/TGA texture pages")
    for _, name in pages:
        relative_path(name)
    return lines, pages


def copy_asset(source, destination):
    if not source.is_file():
        raise ValueError(f"Missing asset: {source}")
    if destination.exists():
        if source.read_bytes() != destination.read_bytes():
            raise ValueError(f"Conflicting asset: {destination}")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def validate_assets(config, package, game=None):
    for hero in config["heroes"]:
        root = package / "heroes" / hero["id"]
        for group in hero["groups"]:
            for skin in group["skins"]:
                if (
                    game is not None
                    and not (root / skin).is_dir()
                    and not (game / "heroes" / hero["id"] / skin).is_dir()
                ):
                    raise ValueError(f"Missing configured skin directory: {root / skin}")
            skeletons = [
                p for p in (root / "anim").glob("*.skel") if group_for_name(hero, p.name) is group
            ]
            if not skeletons:
                raise ValueError(f"No skeletons for {hero['id']}/{group['id']}")
            for skeleton in skeletons:
                atlas = skeleton.with_suffix(".atlas")
                _, pages = atlas_pages(atlas.read_text())
                for _, name in pages:
                    if not (atlas.parent / name).is_file():
                        raise ValueError(f"Missing default atlas texture: {atlas.parent / name}")


def validate_installed(config, package, game=None):
    manifest = json.loads((package / MANIFEST).read_text())
    if manifest.get("config_sha256") != config_digest(config):
        raise ValueError("Installed skin configuration differs; run manage.py install")
    files = {p.relative_to(package).as_posix() for p in package.rglob("*") if p.is_file()}
    if files != set(manifest["files"]) | {MANIFEST, "modfiles.txt"}:
        raise ValueError("Installed file list differs; reinstall the skin package")
    if any(p.is_symlink() for p in package.rglob("*")):
        raise ValueError("Installed packages must not contain symbolic links")
    for name, digest in manifest["files"].items():
        path = package / relative_path(name)
        if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Installed asset differs: {path}; reinstall the skin package")
    resolved = dict(config, heroes=manifest["heroes"])
    validate_routes(route_rows(resolved))
    validate_assets(resolved, package, game)
    return resolved


def original_release(config, project_root, source=None):
    project = Path(project_root).expanduser().resolve()
    release = Path(source or config["source"]).expanduser()
    release = (project / release).resolve(strict=True)
    if not (release / "project.xml").is_file() or (release / MANIFEST).exists():
        raise ValueError(
            "Source must be an original Mod release with project.xml, not a routed package"
        )
    if any(p.is_symlink() for p in release.rglob("*")):
        raise ValueError("Mod releases must not contain symbolic links")
    return release


def build(
    output,
    project_root=DEFAULT_PROJECT_ROOT,
    game_root=DEFAULT_GAME_ROOT,
    config_path=DEFAULT_CONFIG,
    source=None,
):
    config_path = Path(config_path).expanduser().resolve(strict=True)
    config = load_config(config_path)
    project = Path(project_root).expanduser().resolve()
    game = Path(game_root).expanduser().resolve(strict=True)
    release = original_release(config, project, source)
    resolved = discover_config(config, release, game)
    output = Path(output).resolve()
    if output == release or release in output.parents or output == game or game in output.parents:
        raise ValueError("Build outside the source release and game installation")
    output.mkdir(parents=True, exist_ok=True)
    package = output / "skin_package"
    if package.exists():
        raise ValueError("Use an empty staging directory")
    shutil.copytree(release, package)
    roots = {"game": game, "source": release, "project": project, "config": config_path.parent}
    for hero in resolved["heroes"]:
        target = package / "heroes" / hero["id"] / "anim"
        for group in hero["groups"]:
            if "assets" not in group:
                continue
            assets = group["assets"]
            origin = roots[assets["root"]]
            skeletons = sorted((origin / assets["anim"]).glob(assets["prefix"] + "*.skel"))
            if not skeletons:
                raise ValueError(f"No source skeletons for {hero['id']}/{group['id']}")
            for skeleton in skeletons:
                name = group["prefix"] + skeleton.name[len(assets["prefix"]) :]
                if group_for_name(hero, name) is not group:
                    raise ValueError(f"A renamed resource matches another group: {name}")
                copy_asset(skeleton, target / name)
                atlas = skeleton.with_suffix(".atlas")
                lines, pages = atlas_pages(atlas.read_text())
                for index, image in pages:
                    renamed = f"rig_{group['id']}/{image}"
                    copy_asset(origin / assets["textures"] / image, target / renamed)
                    lines[index] = renamed
                rewritten = "\n".join(lines) + "\n"
                destination = target / Path(name).with_suffix(".atlas")
                if destination.exists() and destination.read_text() != rewritten:
                    raise ValueError(f"Conflicting atlas: {destination}")
                destination.write_text(rewritten)
    validate_assets(resolved, package, game)
    files = {
        p.relative_to(package).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(package.rglob("*"))
        if p.is_file() and p.name != "modfiles.txt"
    }
    (package / MANIFEST).write_text(
        json.dumps(
            {"config_sha256": config_digest(config), "heroes": resolved["heroes"], "files": files},
            ensure_ascii=False,
            indent=2,
        )
        + "\n"
    )
    listed = sorted(p for p in package.rglob("*") if p.is_file() and p.name != "modfiles.txt")
    (package / "modfiles.txt").write_text(
        "\n".join(f"{p.relative_to(package).as_posix()} {p.stat().st_size}" for p in listed) + "\n"
    )
    write_rig_map(resolved, output / "skin_rig_map.txt")
    return package


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--project-root", type=Path, default=DEFAULT_PROJECT_ROOT)
    parser.add_argument("--game-root", type=Path, default=DEFAULT_GAME_ROOT)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--source", type=Path, help="Override the original Mod release directory")
    args = parser.parse_args()
    output = args.output or Path(tempfile.mkdtemp(prefix="dd-rig-build-"))
    package = build(output, args.project_root, args.game_root, args.config, args.source)
    print(
        json.dumps(
            {
                "package": str(package),
                "rig_map": str(output / "skin_rig_map.txt"),
                "status": "STAGED_NOT_GAME_VALIDATED",
            },
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
