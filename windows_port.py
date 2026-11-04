"""Build and inspect the identity-bound Windows x64 port; never starts a game."""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def profile():
    source = (HERE / 'windows_profile.h').read_text().replace('\\\n', '')
    identity = re.search(r'#define DD_WINDOWS_SHA256\s+"([0-9a-f]{64})"', source)
    image_size = re.search(r'#define DD_WINDOWS_IMAGE_SIZE (0x[0-9a-f]+)U', source)
    if not identity or not image_size:
        raise ValueError('Windows identity profile is incomplete')
    tables = {}
    for name in ('resource', 'hook', 'api', 'render'):
        match = re.search(r'windows_' + name + r'_sites\[\] = \{(.*?)\n\};',
                          source, re.S)
        if not match:
            raise ValueError('Windows instruction table is incomplete')
        entries = []
        for rva, length, operand, raw in re.findall(
                r'\{\s*(0x[0-9a-f]+),\s*(\d+),\s*(\d+),\s*\{([^}]+)\}\}',
                match[1], re.S):
            expected = bytes(int(value.strip(), 16) for value in raw.split(','))
            if len(expected) != int(length):
                raise ValueError('Windows instruction length is inconsistent')
            entries.append((int(rva, 16), expected, int(operand)))
        if not entries:
            raise ValueError('Windows instruction table is empty')
        tables[name] = entries
    return identity[1], int(image_size[1], 16), tables


def validate(game: Path):
    data = Path(game).read_bytes()
    identity, image_size, tables = profile()
    if hashlib.sha256(data).hexdigest() != identity:
        raise ValueError('Unsupported Windows executable SHA256; no launch or patch attempted')
    if data[:2] != b'MZ' or len(data) < 64:
        raise ValueError('Invalid Windows DOS header')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if pe < 64 or pe + 264 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid Windows PE header')
    machine, count = struct.unpack_from('<HH', data, pe + 4)
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    if (machine != 0x8664 or not 0 < count <= 32 or optional_size != 240 or
            struct.unpack_from('<H', data, pe + 24)[0] != 0x20b or
            struct.unpack_from('<I', data, pe + 80)[0] != image_size):
        raise ValueError('Unsupported Windows PE architecture or layout')
    sections = []
    for i in range(count):
        position = pe + 24 + optional_size + i * 40
        if position + 40 > len(data):
            raise ValueError('Truncated Windows section table')
        _, _, rva, size, offset, _, _, _, _, flags = struct.unpack_from(
            '<8sIIIIIIHHI', data, position)
        if offset + size > len(data):
            raise ValueError('Truncated Windows section')
        if flags & 0x20000000:
            sections.append((rva, rva + size, offset))
    for entries in tables.values():
        for rva, expected, _ in entries:
            section = next((s for s in sections if s[0] <= rva and
                            rva + len(expected) <= s[1]), None)
            if section is None:
                raise ValueError(f'Windows code section missing at {rva:x}')
            offset = section[2] + rva - section[0]
            if data[offset:offset + len(expected)] != expected:
                raise ValueError(f'Unexpected Windows instruction at {rva:x}')
    return {'game_sha256': identity, 'platform': 'windows_x64',
            'resource_buffers': 12, 'old_cap_bytes': 0x1000000,
            'cap_bytes': 0x2000000, 'resource_patch_sites': len(tables['resource']),
            'rig_hook_sites': len(tables['hook']),
            'rig_api_sites': len(tables['api']), 'routing_mode': 'runtime_source',
            'render_patch_sites': len(tables['render']),
            'native_render_opt_in': True, 'disk_binary_modified': False,
            'runtime_verified': False}


def build(game: Path, compiler: str | None = None):
    result = validate(game)
    compiler = compiler or shutil.which('x86_64-w64-mingw32-clang')
    if not compiler:
        raise RuntimeError('Set --windows-cc to an LLVM-MinGW x64 Clang compiler')
    output = Path(tempfile.mkdtemp(prefix='dd-windows-build-'))
    common = [str(compiler), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
              '-Wl,--no-insert-timestamp']
    launcher = output / 'dd_windows_launcher.exe'
    library = output / 'dd_skin_rig_patch.dll'
    subprocess.run([*common, '-municode', str(HERE / 'windows_launcher.c'),
                    '-lbcrypt', '-o', str(launcher)], check=True)
    subprocess.run([*common, '-shared', str(HERE / 'skin_rig_patch.c'),
                    '-lm', '-o', str(library)], check=True)
    for name in ('windows_deploy.ps1', 'windows_start.cmd',
                 'windows_diagnose.cmd', 'windows_resources_only.cmd'):
        source = (HERE / name).read_text()
        (output / name).write_bytes(source.replace('\n', '\r\n').encode('ascii'))
    result.update(launcher=str(launcher), library=str(library),
                  start_script=str(output / 'windows_start.cmd'), check_only=True)
    return result
