"""Build a flat Linux x64 / Windows x64 release without any game resources."""

from __future__ import annotations

import hashlib
import platform
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path

from .paths import COMMON_NATIVE, LINUX_NATIVE, PATCH_ROOT
from .windows_port import compile_binaries

HERE = PATCH_ROOT


def build(
    output: Path | None = None, windows_cc: str | None = None, windows_proxy: Path | None = None
):
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise RuntimeError("Combined release build requires a Linux x86-64 host")
    root = Path(output) if output else Path(tempfile.mkdtemp(prefix="dd-release-"))
    if root.exists() and any(root.iterdir()):
        raise ValueError("Release output must be an empty directory")
    archive = root.parent / (root.name + ".zip")
    if archive.exists():
        raise ValueError("Release archive already exists; choose a new output directory")
    root.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("clang")
    if not compiler:
        raise RuntimeError("Release build requires Linux x64 Clang")
    common = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
    for source, name in (
        (LINUX_NATIVE / "resource_loading_patch.c", "libdd_resource_loading_patch.so"),
        (COMMON_NATIVE / "skin_rig_patch.c", "libdd_skin_rig_patch.so"),
    ):
        subprocess.run(
            [*common, "-fPIC", "-shared", str(source), "-ldl", "-lm", "-o", str(root / name)],
            check=True,
        )
    windows = compile_binaries(root, windows_cc, windows_proxy)
    shutil.copyfile(HERE / "README.md", root / "README.md")
    shutil.copyfile(
        HERE / "licenses/ultimate_asi_loader.txt", root / "ultimate_asi_loader_license.txt"
    )
    # DLL Mod Loader recursively scans Mod directories. Keep the optional WinMM
    # proxy inside an archive so it cannot be mistaken for another Mod DLL.
    with zipfile.ZipFile(root / "manual_winmm.zip", "w", zipfile.ZIP_DEFLATED) as manual:
        for name in ("winmm.dll", "winmm.ini"):
            manual.write(root / name, name)
    for name in ("winmm.dll", "winmm.ini"):
        (root / name).unlink()
    files = sorted(path for path in root.iterdir() if path.is_file())
    (root / "SHA256SUMS").write_text(
        "".join(
            hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.name + "\n"
            for path in files
        )
    )
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(root.iterdir()):
            bundle.write(path, root.name + "/" + path.name)
    return {
        "directory": str(root),
        "archive": str(archive),
        "files": [path.name for path in sorted(root.iterdir())],
        "windows_library": windows["library"],
        "game_files_required": False,
        "runtime_verified": False,
    }
