"""Build a flat Linux x64 / Windows x64 release without any game resources."""
from __future__ import annotations

import hashlib
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import zipfile

from steam_integration import native_config
from windows_port import compile_binaries

HERE = Path(__file__).resolve().parent


def build(output: Path | None = None, windows_cc: str | None = None):
    if platform.system() != 'Linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        raise RuntimeError('Combined release build requires a Linux x86-64 host')
    root = Path(output) if output else Path(tempfile.mkdtemp(prefix='dd-release-'))
    if root.exists() and any(root.iterdir()):
        raise ValueError('Release output must be an empty directory')
    archive = root.parent / (root.name + '.zip')
    if archive.exists():
        raise ValueError('Release archive already exists; choose a new output directory')
    root.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which('clang')
    if not compiler:
        raise RuntimeError('Release build requires Linux x64 Clang')
    common = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
    for source, name in (('resource_loading_patch.c', 'libdd_resource_loading_patch.so'),
                         ('skin_rig_patch.c', 'libdd_skin_rig_patch.so')):
        subprocess.run([*common, '-fPIC', '-shared', str(HERE / source),
                        '-ldl', '-lm', '-o', str(root / name)], check=True)
    subprocess.run([*common, str(HERE / 'steam_launcher.c'),
                    '-o', str(root / 'steam_launcher')], check=True)
    (root / 'launcher.conf').write_text(native_config(
        Path('/absolute/path/to/DarkestDungeon/_linux/darkest.bin.x86_64'),
        Path('/absolute/path/to/darkest_dungeon_patches/steam_launch.log')))
    windows = compile_binaries(root, windows_cc)
    shutil.copyfile(HERE / 'README.md', root / 'README.md')
    files = sorted(path for path in root.iterdir() if path.is_file())
    (root / 'SHA256SUMS').write_text(''.join(
        hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.name + '\n'
        for path in files))
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(root.iterdir()):
            bundle.write(path, root.name + '/' + path.name)
    return {'directory': str(root), 'archive': str(archive),
            'files': [path.name for path in sorted(root.iterdir())],
            'windows_setup': windows['setup'], 'game_files_required': False,
            'runtime_verified': False}
