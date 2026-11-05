"""Build Linux libraries and inspect instruction compatibility without a version gate."""

from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from .paths import COMMON_NATIVE, LINUX_NATIVE

SITES = [0xC3C288, 0xC3C343, 0xC3C3FA, 0xC3C4B1, 0xC3C568, 0xC3C61F, 0xC3F77F]
RENDER_SITES = [
    (0xCA13CA, bytes.fromhex("bd 80 07 00 00")),
    (0xCA13D3, bytes.fromhex("41 be 38 04 00 00")),
    (0x10AF426, bytes.fromhex("0f 47 e8")),
    (0xCA1485, bytes.fromhex("bf 80 07 00 00")),
    (0xCA148A, bytes.fromhex("be 38 04 00 00")),
    (0xCA14A6, bytes.fromhex("bf 80 07 00 00")),
    (0xCA14AB, bytes.fromhex("be 38 04 00 00")),
]
RIG_SITES = [
    (0xEFC214, bytes.fromhex("e8 f7 2e b7 ff")),
    (0xD66E80, bytes.fromhex("41 57 41 56 53")),
    (0x11A8400, bytes.fromhex("41 56 53 48 81 ec 08 02 00 00")),
]
RUNTIME_SITES = [
    (0xA5A7D0, bytes.fromhex("55 53 48 81 ec 48 04 00 00")),
    (0xC27C90, bytes.fromhex("55 41 57 41 56 41 55 41 54 53")),
    (0xC27D90, bytes.fromhex("48 89 f8 48 8b 3f 48 85 ff")),
    (0xC3A260, bytes.fromhex("41 57 41 56 41 54 53")),
    (0xA63A60, bytes.fromhex("83 3f 00 78 38")),
    (0xC3F3F0, bytes.fromhex("41 56 53 50 48 63 06")),
    (0xA70A60, bytes.fromhex("55 41 57 41 56 41 55 41 54 53 50")),
    (0xA70E10, bytes.fromhex("55 41 57 41 56")),
    (0xC27C30, bytes.fromhex("41 56 53 50")),
    (0xA40930, bytes.fromhex("48 8b 47 10 48 2b 47 08")),
    (0xA40950, bytes.fromhex("89 f1 48 8b 47 08")),
    (0xA41700, bytes.fromhex("55 41 57 41 56 41 55 41 54 53")),
    (0xD7B130, bytes.fromhex("48 8b 87 08 06 00 00")),
    (0xD7B150, bytes.fromhex("89 f0 48 c1 e0 09")),
]


def validate(game, native_render=False, rig_routing=False, runtime_rig=False):
    data = game.read_bytes()
    if (
        len(data) < 64
        or data[:6] != b"\x7fELF\x02\x01"
        or struct.unpack_from("<H", data, 18)[0] != 62
    ):
        raise ValueError("Expected a Linux x86-64 ELF executable")
    offset = struct.unpack_from("<Q", data, 32)[0]
    entry_size, count = struct.unpack_from("<HH", data, 54)
    if entry_size < 56 or offset + entry_size * count > len(data):
        raise ValueError("Truncated ELF program headers")
    segments = []
    for i in range(count):
        kind, flags, pos, address, _, size, _, _ = struct.unpack_from(
            "<IIQQQQQQ", data, offset + entry_size * i
        )
        if pos + size > len(data):
            raise ValueError("Truncated ELF segment")
        if kind == 1 and flags & 1:
            segments.append((address, address + size, pos))
    sites = [
        (address, bytes([0xB9 if i == 6 else 0xBF, 0, 0, 0x60, 0]))
        for i, address in enumerate(SITES)
    ]
    extra_sites = (
        (RENDER_SITES if native_render else [])
        + (RIG_SITES if rig_routing or runtime_rig else [])
        + (RUNTIME_SITES if runtime_rig else [])
    )
    issues = []
    for address, expected in sites + extra_sites:
        segment = next(
            (s for s in segments if s[0] <= address and address + len(expected) <= s[1]), None
        )
        if segment is None:
            issues.append(f"Code segment missing at {address:x}")
            continue
        start, _, pos = segment
        if data[pos + address - start : pos + address - start + len(expected)] != expected:
            issues.append(f"Unexpected instruction at {address:x}")
    return {"compatible": not issues, "compatibility_issues": issues}


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
    digest.update((LINUX_NATIVE / "linux_runtime.h").read_bytes())
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
