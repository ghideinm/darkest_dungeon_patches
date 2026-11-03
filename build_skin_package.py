"""Stage a two-rig test package in /tmp; never writes the game or formal release."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile

from paths import DEFAULT_GAME_ROOT, DEFAULT_PROJECT_ROOT, hero_root


def build(output, project_root=DEFAULT_PROJECT_ROOT, game_root=DEFAULT_GAME_ROOT):
    hero = hero_root(project_root)
    game = game_root.expanduser().resolve(strict=True)
    output.mkdir(parents=True, exist_ok=True)
    package = output / 'DeepSeek强盗'
    if package.exists():
        raise ValueError('Use an empty staging directory')
    shutil.copytree(hero / '发布/DeepSeek强盗', package)
    source = game / 'heroes/highwayman'
    target = package / 'heroes/highwayman/anim'
    records = []
    for skeleton in sorted((source / 'anim').glob('*.skel')):
        name = skeleton.name.replace('highwayman.sprite.', 'highwayman.sprite.rig_vanilla.', 1)
        shutil.copy2(skeleton, target / name)
        atlas = skeleton.with_suffix('.atlas')
        lines = atlas.read_text().splitlines()
        pages = [i for i, line in enumerate(lines) if line.strip().lower().endswith(('.png', '.tga')) and not line.startswith(' ')]
        for index in pages:
            image_name = lines[index].strip()
            original = source / 'highwayman_A/anim' / image_name
            if not original.is_file():
                raise ValueError(f'Vanilla palette image missing: {original}')
            destination = target / 'rig_vanilla' / image_name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, destination)
            # Native texture override extracts basename; A-D still use their
            # original PNG names while the default texture has its own directory.
            lines[index] = 'rig_vanilla/' + image_name
        (target / Path(name).with_suffix('.atlas')).write_text('\n'.join(lines) + '\n')
        records.append({'skeleton': name, 'source': str(skeleton), 'pages': len(pages)})
    if len(records) != 12:
        raise ValueError('Unexpected vanilla resource set')
    mapping = output / 'skin_rig_map.txt'
    shutil.copy2(Path(__file__).with_name('skin_rig_map.txt'), mapping)
    files = sorted(p for p in package.rglob('*') if p.is_file() and p.name != 'modfiles.txt')
    (package / 'modfiles.txt').write_text('\n'.join(f'{p.relative_to(package).as_posix()} {p.stat().st_size}' for p in files))
    report = {'status': 'STAGED_NOT_GAME_VALIDATED', 'source_release_unchanged': True,
              'package': str(package), 'rig_map': str(mapping), 'vanilla_resources': records,
              'scope': 'Linux28212 highwayman A-D vanilla, E DeepSeek; raid body and character preview prototype'}
    (output / 'skin_package_build.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'output': str(output), 'package': str(package), 'rig_map': str(mapping)}, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--project-root', type=Path, default=DEFAULT_PROJECT_ROOT)
    parser.add_argument('--game-root', type=Path, default=DEFAULT_GAME_ROOT)
    args = parser.parse_args()
    build(args.output or Path(tempfile.mkdtemp(prefix='dd-rig-probe-')),
          args.project_root, args.game_root)
