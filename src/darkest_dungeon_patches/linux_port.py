"""Build Linux libraries and inspect instruction compatibility without a version gate."""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from .address_scan import read_image, resolve, signatures
from .paths import COMMON_NATIVE, LINUX_NATIVE


def validate(game, native_render=False, rig_routing=False, runtime_rig=False):
    groups = 1 | (2 if native_render else 0) | (4 if rig_routing or runtime_rig else 0)
    resolved, issues = resolve(read_image(game, "linux"), signatures("linux"), groups)
    return {
        "compatible": not issues,
        "compatibility_issues": issues,
        "resolved_symbols": resolved,
        "address_resolution": "unique_signature",
    }


def build_library(
    source_name="resource_loading_patch.c", library_name="libdd_resource_loading_patch.so"
):
    sources = {
        "resource_loading_patch.c": LINUX_NATIVE / "resource_loading_patch.c",
        "skin_rig_patch.c": COMMON_NATIVE / "skin_rig_patch.c",
    }
    if source_name not in sources:
        raise ValueError("Unknown native patch source")
    source = sources[source_name]
    digest = hashlib.sha256(source.read_bytes())
    for header in sorted(COMMON_NATIVE.glob("*.h")) + sorted(LINUX_NATIVE.glob("*.h")):
        digest.update(header.name.encode())
        digest.update(header.read_bytes())
    key = digest.hexdigest()[:16]
    directory = Path(tempfile.gettempdir()) / f"dd-loadbuffer-{os.getuid()}-{key}"
    directory.mkdir(mode=0o700, exist_ok=True)
    if (
        directory.is_symlink()
        or directory.stat().st_uid != os.getuid()
        or directory.stat().st_mode & 0o077
    ):
        raise ValueError("Patch build directory must be private and owned by this user")
    library = directory / library_name
    if library.is_symlink():
        raise ValueError("Patch library must not be a symbolic link")
    if library.exists():
        return library
    compiler = shutil.which("clang")
    if not compiler:
        raise RuntimeError("Clang is required to build the native patches")
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fPIC",
            "-shared",
            str(source),
            "-ldl",
            "-lm",
            "-o",
            str(library),
        ],
        check=True,
    )
    return library
