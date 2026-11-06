"""Acquire the pinned, redistributable x64 WinMM proxy at build time only."""

from __future__ import annotations

import hashlib
import io
import os
import urllib.request
import zipfile
from pathlib import Path

PROXY_VERSION = "10.0.2"
PROXY_URL = (
    "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/"
    "v10.0.2/Ultimate-ASI-Loader-NoPDB_x64.zip"
)
ARCHIVE_SHA256 = "fe964adf4de7f467ff39c440a4059de02e37d0574a62944eef2f55a35edafa2f"
PROXY_SHA256 = "57ec56390c92a18b82f58855e63c93985e254d1e29d5f75cbc28dc08f96c8a12"


def acquire(output: Path, source: Path | None = None) -> Path:
    source = source or (
        Path(os.environ["DD_WINDOWS_PROXY"]) if os.getenv("DD_WINDOWS_PROXY") else None
    )
    if source:
        data = Path(source).read_bytes()
    else:
        request = urllib.request.Request(PROXY_URL, headers={"User-Agent": "DarkestDungeonPatches"})
        with urllib.request.urlopen(request, timeout=30) as response:
            archive = response.read(16 * 1024 * 1024)
        if hashlib.sha256(archive).hexdigest() != ARCHIVE_SHA256:
            raise ValueError("ASI loader archive checksum mismatch")
        with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
            data = bundle.read("dinput8.dll")
    if hashlib.sha256(data).hexdigest() != PROXY_SHA256:
        raise ValueError("ASI loader DLL checksum mismatch")
    output = Path(output)
    output.write_bytes(data)
    return output
