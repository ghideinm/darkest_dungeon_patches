"""三项补丁的统一安装／检查／启动入口；只支持已核对的 Linux 28212。"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from build_skin_package import build, original_release, validate_installed
from launch import EXPECTED_SHA256, build_library, validate
from paths import DEFAULT_GAME_ROOT, DEFAULT_PROJECT_ROOT
from skin_config import DEFAULT_CONFIG, load_config, write_rig_map

HERE = Path(__file__).resolve().parent


def ensure_game_stopped():
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if (process / 'exe').readlink().name in {'darkest.bin.x86_64', 'darkest.bin.x86'}:
                raise RuntimeError('Game is running; exit normally before installing or restoring')
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass


def mod_title(package):
    text = (package / 'project.xml').read_text()
    try:
        title = json.loads(text)['Title']
    except json.JSONDecodeError:
        title = ET.fromstring(text).findtext('.//Title')
    if not isinstance(title, str) or not title.strip():
        raise ValueError(f'Mod title missing: {package}')
    return title


def install_package(package, destination):
    """Replace one identified Mod directory, retaining the old copy on failure."""
    package, destination = Path(package), Path(destination)
    title = mod_title(package)
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise ValueError('Mod destination must be a directory, not a symlink or file')
    if destination.exists() and mod_title(destination) != title:
        raise ValueError('Destination belongs to another Mod; choose a different destination')
    if any(p.is_symlink() for p in package.rglob('*')):
        raise ValueError('Mod packages must not contain symbolic links')
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Stage beside the destination so directory switches and rollback stay atomic.
    work = Path(tempfile.mkdtemp(prefix='.dd-skin-install-', dir=destination.parent))
    staged, previous = work / 'package', work / 'previous'
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
    result = {'destination': str(destination), 'title': title}
    if previous.exists():
        gio = shutil.which('gio')
        if gio and subprocess.run([gio, 'trash', '--', str(previous)], check=False).returncode == 0:
            work.rmdir()
        else:
            result['previous_package'] = str(previous)
    else:
        work.rmdir()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check', 'install', 'launch', 'restore'])
    parser.add_argument('--config', type=Path, default=DEFAULT_CONFIG)
    parser.add_argument('--source', type=Path, help='原始 Mod 发布包；覆盖配置中的 source')
    parser.add_argument('--project-root', type=Path, default=DEFAULT_PROJECT_ROOT)
    parser.add_argument('--game-root', type=Path, default=DEFAULT_GAME_ROOT)
    parser.add_argument('--bridge-root', type=Path)
    parser.add_argument('--destination', type=Path)
    parser.add_argument('--with-mcp', action='store_true', help='启动时附带观察器和调试入口')
    args = parser.parse_args()
    config = load_config(args.config)
    game = args.game_root.expanduser().resolve(strict=True)
    destination = (args.destination or game / config['destination']).expanduser()
    if destination.is_symlink() or destination.parent.resolve() != (game / 'mods').resolve():
        raise ValueError('Destination must be one direct Mod directory under <game-root>/mods')
    executable = game / '_linux/darkest.bin.x86_64'
    if args.action == 'check':
        validate(executable, native_render=True, rig_routing=True)
        libraries = [build_library(), build_library('skin_rig_patch.c', 'libdd_skin_rig_patch.so')]
        print(json.dumps({'game_sha256': EXPECTED_SHA256, 'check_only': True,
                          'disk_binary_modified': False, 'libraries': list(map(str, libraries)),
                          'routing_mode': 'configured' if 'heroes' in config else 'automatic_discovery',
                          'package_checked': False}, ensure_ascii=False, indent=2))
        return
    if args.action in {'install', 'restore'}:
        ensure_game_stopped()
        validate(executable, native_render=True, rig_routing=True)
        if args.action == 'restore':
            release = original_release(config, args.project_root, args.source)
            result = install_package(release, destination)
        else:
            with tempfile.TemporaryDirectory(prefix='dd-rig-install-') as directory:
                package = build(Path(directory), args.project_root, game, args.config, args.source)
                result = install_package(package, destination)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    if args.action == 'launch':
        try:
            config = validate_installed(config, destination, game)
        except (OSError, ValueError, KeyError) as error:
            raise RuntimeError('隔离 Mod 未就绪或与配置不符；退出游戏后执行 manage.py install') from error
    # The same configuration builds the package and generates its runtime routes.
    with tempfile.TemporaryDirectory(prefix='dd-rig-launch-') as directory:
        mapping = Path(directory) / 'skin_rig_map.txt'
        write_rig_map(config, mapping)
        command = [sys.executable, str(HERE / 'launch.py'), '--game', str(executable),
                   '--project-root', str(args.project_root),
                   '--native-render', '--rig-map', str(mapping)]
        if args.bridge_root:
            command.extend(['--bridge-root', str(args.bridge_root)])
        if args.with_mcp:
            command.append('--with-mcp')
        subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
