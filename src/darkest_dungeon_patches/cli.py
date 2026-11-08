"""原生引擎补丁的只读检查、编译、测试和发布包构建入口。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from .linux_port import build_library, validate
from .paths import DEFAULT_GAME_ROOT, TEST_ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "action", choices=["check", "windows_check", "windows_build", "bundle", "test"]
    )
    parser.add_argument("--game-root", type=Path, default=DEFAULT_GAME_ROOT)
    parser.add_argument("--windows-cc", help="Windows x64 LLVM-MinGW Clang 编译器路径")
    parser.add_argument(
        "--windows-proxy", type=Path, help="手动安装用的固定版本 x64 ASI Loader；省略则下载"
    )
    parser.add_argument("--output", type=Path, help="发布包空目录；默认使用系统临时目录")
    args = parser.parse_args()
    if args.action == "test":
        import unittest

        suite = unittest.defaultTestLoader.discover(str(TEST_ROOT))
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if args.action == "bundle":
        from .release_bundle import build

        print(
            json.dumps(
                build(args.output, args.windows_cc, args.windows_proxy),
                ensure_ascii=False,
                indent=2,
            )
        )
        return
    if args.action.startswith("windows_"):
        from . import windows_port

        game = args.game_root.expanduser().resolve() / "_windows/win64/Darkest.exe"
        result = (
            windows_port.build(game, args.windows_cc, args.windows_proxy)
            if args.action == "windows_build"
            else windows_port.validate(game)
        )
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    executable = args.game_root.expanduser().resolve(strict=True) / "_linux/darkest.bin.x86_64"
    compatibility = validate(executable, native_render=True, runtime_rig=True)
    libraries = [build_library(), build_library("skin_rig_patch.c", "libdd_skin_rig_patch.so")]
    print(
        json.dumps(
            {
                **compatibility,
                "check_only": True,
                "disk_binary_modified": False,
                "libraries": list(map(str, libraries)),
                "routing_mode": "runtime_source",
            },
            ensure_ascii=False,
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
