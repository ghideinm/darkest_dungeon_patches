"""Linux 补丁统一入口，以及已核对 Windows x64 的离线检查／交叉编译。"""
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
    parser.add_argument('action', choices=['check', 'install', 'launch', 'restore',
                                           'steam_install', 'steam_status', 'steam_uninstall',
                                           'windows_check', 'windows_build'])
    parser.add_argument('--config', type=Path, default=DEFAULT_CONFIG)
    parser.add_argument('--source', type=Path, help='原始 Mod 发布包；覆盖配置中的 source')
    parser.add_argument('--project-root', type=Path, default=DEFAULT_PROJECT_ROOT)
    parser.add_argument('--game-root', type=Path, default=DEFAULT_GAME_ROOT)
    parser.add_argument('--bridge-root', type=Path)
    parser.add_argument('--destination', type=Path)
    parser.add_argument('--with-mcp', action='store_true', help='启动时附带观察器和调试入口')
    parser.add_argument('--packaged-rig', action='store_true', help='使用旧的隔离打包与路由索引；默认按运行时来源解析')
    parser.add_argument('--steam-root', type=Path, default=Path.home() / '.local/share/Steam')
    parser.add_argument('--steam-user', help='Steam userdata 数字账号 ID；默认选择最近登录账号')
    parser.add_argument('--install-root', type=Path, help='Steam 原生启动器与预编译补丁的固定安装目录')
    parser.add_argument('--windows-cc', help='Windows x64 LLVM-MinGW Clang 编译器路径')
    args = parser.parse_args()
    if args.action.startswith('windows_'):
        import windows_port

        game = args.game_root.expanduser().resolve() / '_windows/win64/Darkest.exe'
        result = (windows_port.build(game, args.windows_cc)
                  if args.action == 'windows_build' else windows_port.validate(game))
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    if args.action.startswith('steam_'):
        from steam_integration import default_install_root, integrate

        if args.action != 'steam_status':
            ensure_game_stopped()
        result = integrate(args.action,
                           args.game_root.expanduser().resolve() / '_linux/darkest.bin.x86_64',
                           args.steam_root, args.install_root or default_install_root(), args.steam_user)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    game = args.game_root.expanduser().resolve(strict=True)
    executable = game / '_linux/darkest.bin.x86_64'
    if args.action == 'check':
        validate(executable, native_render=True, rig_routing=True, runtime_rig=not args.packaged_rig)
        libraries = [build_library(), build_library('skin_rig_patch.c', 'libdd_skin_rig_patch.so')]
        print(json.dumps({'game_sha256': EXPECTED_SHA256, 'check_only': True,
                          'disk_binary_modified': False, 'libraries': list(map(str, libraries)),
                          'routing_mode': 'package_map' if args.packaged_rig else 'runtime_source',
                          'package_checked': False}, ensure_ascii=False, indent=2))
        return
    if args.action in {'install', 'restore'} or args.packaged_rig:
        config = load_config(args.config)
        destination = (args.destination or game / config['destination']).expanduser()
        if destination.is_symlink() or destination.parent.resolve() != (game / 'mods').resolve():
            raise ValueError('Destination must be one direct Mod directory under <game-root>/mods')
    if args.action in {'install', 'restore'}:
        ensure_game_stopped()
        validate(executable, native_render=True, rig_routing=True)
        if args.action == 'restore' or not args.packaged_rig:
            release = original_release(config, args.project_root, args.source)
            result = install_package(release, destination)
        else:
            with tempfile.TemporaryDirectory(prefix='dd-rig-install-') as directory:
                package = build(Path(directory), args.project_root, game, args.config, args.source)
                result = install_package(package, destination)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    if args.action == 'launch' and args.packaged_rig:
        try:
            config = validate_installed(config, destination, game)
        except (OSError, ValueError, KeyError) as error:
            raise RuntimeError('隔离 Mod 未就绪或与配置不符；退出游戏后执行 manage.py install --packaged-rig') from error
    command = [sys.executable, str(HERE / 'launch.py'), '--game', str(executable),
               '--project-root', str(args.project_root), '--native-render']
    if args.bridge_root:
        command.extend(['--bridge-root', str(args.bridge_root)])
    if args.with_mcp:
        command.append('--with-mcp')
    if not args.packaged_rig:
        subprocess.run(command + ['--runtime-rig'], check=True)
        return
    # Explicit compatibility mode for previously generated isolated packages.
    with tempfile.TemporaryDirectory(prefix='dd-rig-launch-') as directory:
        mapping = Path(directory) / 'skin_rig_map.txt'
        write_rig_map(config, mapping)
        subprocess.run(command + ['--rig-map', str(mapping)], check=True)


if __name__ == '__main__':
    main()
