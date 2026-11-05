"""Linux 补丁统一入口，以及已核对 Windows x64 的离线检查／交叉编译。"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

from .build_skin_package import build, original_release
from .linux_port import build_library, validate
from .paths import DEFAULT_GAME_ROOT, DEFAULT_PROJECT_ROOT, TEST_ROOT
from .skin_config import DEFAULT_CONFIG, load_config


def ensure_game_stopped():
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            if (process / "exe").readlink().name in {"darkest.bin.x86_64", "darkest.bin.x86"}:
                raise RuntimeError("Game is running; exit normally before installing or restoring")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass


def mod_title(package):
    text = (package / "project.xml").read_text()
    try:
        title = json.loads(text)["Title"]
    except json.JSONDecodeError:
        title = ET.fromstring(text).findtext(".//Title")
    if not isinstance(title, str) or not title.strip():
        raise ValueError(f"Mod title missing: {package}")
    return title


def install_package(package, destination):
    """Replace one identified Mod directory, retaining the old copy on failure."""
    package, destination = Path(package), Path(destination)
    title = mod_title(package)
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise ValueError("Mod destination must be a directory, not a symlink or file")
    if destination.exists() and mod_title(destination) != title:
        raise ValueError("Destination belongs to another Mod; choose a different destination")
    if any(p.is_symlink() for p in package.rglob("*")):
        raise ValueError("Mod packages must not contain symbolic links")
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Stage beside the destination so directory switches and rollback stay atomic.
    work = Path(tempfile.mkdtemp(prefix=".dd-skin-install-", dir=destination.parent))
    staged, previous = work / "package", work / "previous"
    try:
        shutil.copytree(package, staged)
        ensure_game_stopped()
        if destination.exists():
            destination.rename(previous)
        try:
            staged.rename(destination)
        except BaseException:
            if previous.exists():
                previous.rename(destination)
            raise
    except BaseException:
        if not previous.exists():
            shutil.rmtree(work)
        raise
    result = {"destination": str(destination), "title": title}
    if previous.exists():
        gio = shutil.which("gio")
        if gio and subprocess.run([gio, "trash", "--", str(previous)], check=False).returncode == 0:
            work.rmdir()
        else:
            result["previous_package"] = str(previous)
    else:
        work.rmdir()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "action",
        choices=[
            "check",
            "install",
            "restore",
            "windows_check",
            "windows_build",
            "bundle",
            "test",
        ],
    )
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--source", type=Path, help="原始 Mod 发布包；覆盖配置中的 source")
    parser.add_argument("--project-root", type=Path, default=DEFAULT_PROJECT_ROOT)
    parser.add_argument("--game-root", type=Path, default=DEFAULT_GAME_ROOT)
    parser.add_argument("--destination", type=Path)
    parser.add_argument(
        "--packaged-rig",
        action="store_true",
        help="使用旧的隔离打包与路由索引；默认按运行时来源解析",
    )
    parser.add_argument("--windows-cc", help="Windows x64 LLVM-MinGW Clang 编译器路径")
    parser.add_argument("--output", type=Path, help="发布包空目录；默认使用系统临时目录")
    args = parser.parse_args()
    if args.action == "test":
        import unittest

        suite = unittest.defaultTestLoader.discover(str(TEST_ROOT))
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if args.action == "bundle":
        from .release_bundle import build as build_bundle

        print(json.dumps(build_bundle(args.output, args.windows_cc), ensure_ascii=False, indent=2))
        return
    if args.action.startswith("windows_"):
        from . import windows_port

        game = args.game_root.expanduser().resolve() / "_windows/win64/Darkest.exe"
        result = (
            windows_port.build(game, args.windows_cc)
            if args.action == "windows_build"
            else windows_port.validate(game)
        )
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    game = args.game_root.expanduser().resolve(strict=True)
    executable = game / "_linux/darkest.bin.x86_64"
    if args.action == "check":
        compatibility = validate(
            executable, native_render=True, rig_routing=True, runtime_rig=not args.packaged_rig
        )
        libraries = [build_library(), build_library("skin_rig_patch.c", "libdd_skin_rig_patch.so")]
        print(
            json.dumps(
                {
                    **compatibility,
                    "check_only": True,
                    "disk_binary_modified": False,
                    "libraries": list(map(str, libraries)),
                    "routing_mode": "package_map" if args.packaged_rig else "runtime_source",
                    "package_checked": False,
                },
                ensure_ascii=False,
                indent=2,
            )
        )
        return
    if args.action in {"install", "restore"} or args.packaged_rig:
        config = load_config(args.config)
        destination = (args.destination or game / config["destination"]).expanduser()
        if destination.is_symlink() or destination.parent.resolve() != (game / "mods").resolve():
            raise ValueError("Destination must be one direct Mod directory under <game-root>/mods")
    if args.action in {"install", "restore"}:
        ensure_game_stopped()
        validate(executable, native_render=True, rig_routing=True)
        if args.action == "restore" or not args.packaged_rig:
            release = original_release(config, args.project_root, args.source)
            result = install_package(release, destination)
        else:
            with tempfile.TemporaryDirectory(prefix="dd-rig-install-") as directory:
                package = build(Path(directory), args.project_root, game, args.config, args.source)
                result = install_package(package, destination)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return


if __name__ == "__main__":
    main()
