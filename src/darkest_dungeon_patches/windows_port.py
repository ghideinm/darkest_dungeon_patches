"""Build the Windows x64 port and report instruction compatibility; never starts a game."""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from .address_scan import check_instructions, read_image, resolve, signatures
from .paths import COMMON_NATIVE, CONFIG_SCRIPTS, PATCH_ROOT, WINDOWS_NATIVE
from .windows_proxy import acquire

HERE = WINDOWS_NATIVE


def profile():
    source = (HERE / "windows_profile.h").read_text().replace("\\\n", "")
    tables = {}
    for name in ("resource", "hook", "api", "render"):
        match = re.search(r"windows_" + name + r"_sites\[\] = \{(.*?)\n\};", source, re.S)
        if not match:
            raise ValueError("Windows instruction table is incomplete")
        entries = []
        for rva, length, operand, raw in re.findall(
            r"\{\s*DD_(\w+),\s*(\d+),\s*(\d+),\s*\{([^}]+)\}\}", match[1], re.S
        ):
            expected = bytes(int(value.strip(), 16) for value in raw.split(","))
            if len(expected) != int(length):
                raise ValueError("Windows instruction length is inconsistent")
            entries.append((rva, expected, int(operand)))
        if not entries:
            raise ValueError("Windows instruction table is empty")
        tables[name] = entries
    return tables


def validate(game: Path):
    tables = profile()
    ranges = read_image(Path(game), "windows")
    resolved, issues = resolve(ranges, signatures("windows"))
    issues += check_instructions(
        ranges, resolved, [entry for rows in tables.values() for entry in rows]
    )
    return {
        "compatible": not issues,
        "address_resolution": "unique_signature",
        "resolved_symbols": resolved,
        "compatibility_issues": issues,
        "platform": "windows_x64",
        "resource_buffers": 12,
        "old_cap_bytes": 0x1000000,
        "cap_bytes": 0x2000000,
        "resource_patch_sites": len(tables["resource"]),
        "rig_hook_sites": len(tables["hook"]),
        "rig_api_sites": len(tables["api"]),
        "routing_mode": "runtime_source",
        "render_patch_sites": len(tables["render"]),
        "native_render_enabled_by_default": True,
        "disk_binary_modified": False,
        "runtime_verified": False,
    }


def compile_binaries(output: Path, compiler: str | None = None, proxy: Path | None = None):
    """Compile distributable binaries without requiring commercial game files."""
    compiler = compiler or shutil.which("x86_64-w64-mingw32-clang")
    if not compiler:
        raise RuntimeError("Set --windows-cc to an LLVM-MinGW x64 Clang compiler")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    common = [
        str(compiler),
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wl,--no-insert-timestamp",
        "-I",
        str(WINDOWS_NATIVE),
    ]
    proxy = acquire(output / "winmm.dll", proxy)
    configuration = output / "winmm.ini"
    patch_configuration = output / "dd_engine_patch.ini"
    for source, target in (
        (PATCH_ROOT / "scripts/windows/winmm.ini", configuration),
        (CONFIG_SCRIPTS / patch_configuration.name, patch_configuration),
    ):
        target.write_text(source.read_text(encoding="utf-8"), encoding="utf-8", newline="\r\n")
    library = output / "dd_engine_patch.dll"
    subprocess.run(
        [*common, "-shared", str(COMMON_NATIVE / "skin_rig_patch.c"), "-lm", "-o", str(library)],
        check=True,
    )
    return {
        "proxy": str(proxy),
        "configuration": str(configuration),
        "patch_configuration": str(patch_configuration),
        "library": str(library),
    }


def build(game: Path, compiler: str | None = None, proxy: Path | None = None):
    result = validate(game)
    output = Path(tempfile.mkdtemp(prefix="dd-windows-build-"))
    result.update(compile_binaries(output, compiler, proxy))
    result["check_only"] = True
    return result
