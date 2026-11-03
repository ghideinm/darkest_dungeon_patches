"""Opt-in, process-local 32 MiB loader patch for the verified Linux 28212 game."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from paths import DEFAULT_GAME_ROOT, DEFAULT_PROJECT_ROOT, bridge_root as project_bridge_root
EXPECTED_SHA256 = 'fe1a629f7afa74e716cb440b14db6470d572ed244e0a2420521e521bc1102156'
SITES = [0xc3c288, 0xc3c343, 0xc3c3fa, 0xc3c4b1, 0xc3c568, 0xc3c61f, 0xc3f77f]
RENDER_SITES = [(0xca13ca, bytes.fromhex('bd 80 07 00 00')),
                (0xca13d3, bytes.fromhex('41 be 38 04 00 00')),
                (0x10af426, bytes.fromhex('0f 47 e8')),
                (0xca1485, bytes.fromhex('bf 80 07 00 00')),
                (0xca148a, bytes.fromhex('be 38 04 00 00')),
                (0xca14a6, bytes.fromhex('bf 80 07 00 00')),
                (0xca14ab, bytes.fromhex('be 38 04 00 00'))]
RIG_SITES = [(0xefc214, bytes.fromhex('e8 f7 2e b7 ff')),
             (0xd66e80, bytes.fromhex('41 57 41 56 53')),
             (0x11a8400, bytes.fromhex('41 56 53 48 81 ec 08 02 00 00'))]


def validate(game, native_render=False, rig_routing=False):
    import struct

    data = game.read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED_SHA256:
        raise ValueError('Unsupported game SHA256; no launch or patch attempted')
    offset = struct.unpack_from('<Q', data, 32)[0]
    entry_size, count = struct.unpack_from('<HH', data, 54)
    segments = []
    for i in range(count):
        kind, flags, pos, address, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', data, offset + entry_size * i)
        if kind == 1 and flags & 1:
            segments.append((address, address + size, pos))
    for i, address in enumerate(SITES):
        start, _, pos = next(s for s in segments if s[0] <= address and address + 5 <= s[1])
        actual = data[pos + address - start:pos + address - start + 5]
        if actual != bytes([0xb9 if i == 6 else 0xbf, 0, 0, 0x60, 0]):
            raise ValueError(f'Unexpected patch site {address:x}')
    extra_sites = (RENDER_SITES if native_render else []) + (RIG_SITES if rig_routing else [])
    if extra_sites:
        for address, expected in extra_sites:
            start, _, pos = next(s for s in segments if s[0] <= address and address + len(expected) <= s[1])
            if data[pos + address - start:pos + address - start + len(expected)] != expected:
                raise ValueError(f'Unexpected render patch site {address:x}')


def build_library(source_name='resource_loading_patch.c', library_name='libdd_resource_loading_patch.so'):
    source = Path(__file__).with_name(source_name)
    key = hashlib.sha256(source.read_bytes()).hexdigest()[:16]
    directory = Path(tempfile.gettempdir()) / f'dd-loadbuffer-{os.getuid()}-{key}'
    directory.mkdir(mode=0o700, exist_ok=True)
    if directory.is_symlink() or directory.stat().st_uid != os.getuid() or directory.stat().st_mode & 0o077:
        raise ValueError('Patch build directory must be private and owned by this user')
    library = directory / library_name
    if library.is_symlink():
        raise ValueError('Patch library must not be a symbolic link')
    if library.exists():
        return library
    compiler = shutil.which('clang')
    if not compiler:
        raise RuntimeError('Clang is required to build the native patches')
    subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                    str(source), '-ldl', '-lm', '-o', str(library)], check=True)
    return library


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=DEFAULT_GAME_ROOT / '_linux/darkest.bin.x86_64')
    parser.add_argument('--project-root', type=Path, default=DEFAULT_PROJECT_ROOT,
                        help='Optional art project root, used to locate its MCP bridge')
    parser.add_argument('--bridge-root', type=Path,
                        help='Override the MCP bridge repository; only needed with --with-mcp')
    parser.add_argument('--check', action='store_true', help='Verify binary and compile patch; do not start the game')
    parser.add_argument('--with-mcp', action='store_true', help='Also load the existing observer with debugging and normal gameplay control disabled')
    parser.add_argument('--native-render', action='store_true', help='Make main, dungeon and GUI targets follow the physical game canvas without the 1920 cap')
    parser.add_argument('--rig-map', type=Path, help='Experimental per-skin rig routing map; requires the matching staged Mod package')
    parser.add_argument('--launch-report', type=Path, default=Path(tempfile.gettempdir()) / 'dd-loadbuffer-launch.json')
    args = parser.parse_args()
    bridge_root = (args.bridge_root or project_bridge_root(args.project_root)).expanduser().resolve()
    bridge_python = bridge_root / '.venv/bin/python'
    if args.with_mcp and Path(sys.prefix).resolve() != bridge_python.parent.parent.resolve():
        if not bridge_python.is_file():
            parser.error('MCP Python environment not found; set --bridge-root or --project-root')
        os.execv(bridge_python, [str(bridge_python), str(Path(__file__).resolve()), *sys.argv[1:]])
    game = args.game.resolve(strict=True)
    validate(game, args.native_render, bool(args.rig_map))
    library = build_library()
    report = {'game_sha256': EXPECTED_SHA256, 'cap_bytes': 0x2000000,
              'buffers': 6, 'patch_sites': len(SITES), 'library': str(library),
              'disk_binary_modified': False, 'check_only': args.check,
              'native_render_enabled': args.native_render,
              'render_patch_sites': len(RENDER_SITES) if args.native_render else 0,
              'render_cap_pixels': None if args.native_render else [1920, 1080],
              'render_size_policy': 'physical_game_canvas' if args.native_render else 'original_caps'}
    rig_library = None
    if args.rig_map:
        args.rig_map = args.rig_map.resolve(strict=True)
        rig_library = build_library('skin_rig_patch.c', 'libdd_skin_rig_patch.so')
        report.update(rig_routing_experimental=True, rig_map=str(args.rig_map), rig_library=str(rig_library))
    if args.check:
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return
    steam_present = False
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if (process / 'exe').readlink().name in {'darkest.bin.x86_64', 'darkest.bin.x86'}:
                raise RuntimeError('Game is already running; exit normally before launching')
            if process.stat().st_uid == os.getuid() and (process / 'comm').read_text().strip() == 'steam':
                steam_present = True
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
    if not steam_present:
        raise RuntimeError('Steam is not running')
    os.environ['DD_LOADBUFFER_PATCH'] = '1'
    if args.native_render:
        os.environ['DD_NATIVE_RENDER'] = '1'
    else:
        os.environ.pop('DD_NATIVE_RENDER', None)
    if args.rig_map:
        os.environ['DD_RIG_MAP'] = str(args.rig_map)
    else:
        os.environ.pop('DD_RIG_MAP', None)
    preload = str(library) + (':' + str(rig_library) if rig_library else '')
    os.environ['LD_PRELOAD'] = preload + (':' + os.environ['LD_PRELOAD'] if os.environ.get('LD_PRELOAD') else '')
    report['game_pid'] = os.getpid()
    if args.with_mcp:
        sys.path.insert(0, str(bridge_root / 'src'))
        from dd_bridge.launch import default_library, new_runtime, exec_game

        runtime = new_runtime()
        if args.native_render:
            os.environ['DD_RENDER_CAPTURE_DIR'] = str(runtime)
        report.update(snapshot_path=str(runtime / 'snapshot.json'), game_log=str(runtime / 'game.log'),
                      control_enabled=False, debug_enabled=True)
        args.launch_report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps(report, ensure_ascii=False), flush=True)
        log = os.open(runtime / 'game.log', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.dup2(log, 1)
        os.dup2(log, 2)
        os.close(log)
        exec_game(game, default_library(), runtime, control=False, debug=True, open_saves=True)
    else:
        args.launch_report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        os.environ['LD_LIBRARY_PATH'] = str(game.parent / 'lib64') + (':' + os.environ['LD_LIBRARY_PATH'] if os.environ.get('LD_LIBRARY_PATH') else '')
        os.chdir(game.parent.parent)
        print(json.dumps(report, ensure_ascii=False), flush=True)
        os.execv(game, [str(game)])


if __name__ == '__main__':
    main()
