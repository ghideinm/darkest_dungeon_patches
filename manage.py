"""三项补丁的统一安装／检查／启动入口；只支持已核对的Linux28212。"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from build_skin_package import build
from launch import validate
from paths import DEFAULT_GAME_ROOT, DEFAULT_PROJECT_ROOT, hero_root

HERE = Path(__file__).resolve().parent
RIG_MAP = HERE / 'skin_rig_map.txt'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check', 'install', 'launch', 'restore'])
    parser.add_argument('--project-root', type=Path, default=DEFAULT_PROJECT_ROOT)
    parser.add_argument('--game-root', type=Path, default=DEFAULT_GAME_ROOT)
    parser.add_argument('--bridge-root', type=Path)
    parser.add_argument('--destination', type=Path)
    parser.add_argument('--with-mcp', action='store_true', help='启动时附带观察器和调试入口')
    args = parser.parse_args()
    game = args.game_root.expanduser().resolve(strict=True)
    hero = hero_root(args.project_root)
    destination = args.destination or game / 'mods/deepseek_skin_review.json'
    executable = game / '_linux/darkest.bin.x86_64'
    launcher = HERE / 'launch.py'
    command = [sys.executable, str(launcher), '--game', str(executable),
               '--project-root', str(args.project_root),
               '--native-render', '--rig-map', str(RIG_MAP)]
    if args.bridge_root:
        command.extend(['--bridge-root', str(args.bridge_root)])
    if args.action == 'check':
        subprocess.run(command + ['--check'], check=True)
        return
    if args.action in {'install', 'restore'}:
        sys.path.insert(0, str(hero / '构建'))
        import 安装Linux测试包 as installer

        installer.ensure_game_stopped()
        if args.action == 'restore':
            result = installer.install(installer.plan_install(hero / '发布/DeepSeek强盗', destination))
        else:
            validate(executable, native_render=True, rig_routing=True)
            with tempfile.TemporaryDirectory(prefix='dd-rig-install-') as directory:
                stage = Path(directory)
                build(stage, args.project_root, game)
                result = installer.install(installer.plan_install(stage / 'DeepSeek强盗', destination))
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    # 两部分必须配套：补丁负责选组，隔离Mod负责提供可独立寻址的原版资源。
    native_names = [p.name.replace('highwayman.sprite.', 'highwayman.sprite.rig_vanilla.', 1)
                    for p in (game / 'heroes/highwayman/anim').glob('*.skel')]
    installed = destination / 'heroes/highwayman/anim'
    missing = [name for name in native_names if not (installed / name).is_file()
               or not (installed / Path(name).with_suffix('.atlas')).is_file()]
    if len(native_names) != 12 or missing:
        raise RuntimeError('隔离Mod尚未准备好；请先正常退出游戏，再执行本入口的install。')
    if args.with_mcp:
        command.append('--with-mcp')
    os.execv(sys.executable, command)


if __name__ == '__main__':
    main()
