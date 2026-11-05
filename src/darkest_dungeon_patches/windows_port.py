"""Build the Windows x64 port and report instruction compatibility; never starts a game."""

from __future__ import annotations

import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from .paths import COMMON_NATIVE, WINDOWS_NATIVE, WINDOWS_SCRIPTS

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
            r"\{\s*(0x[0-9a-f]+),\s*(\d+),\s*(\d+),\s*\{([^}]+)\}\}", match[1], re.S
        ):
            expected = bytes(int(value.strip(), 16) for value in raw.split(","))
            if len(expected) != int(length):
                raise ValueError("Windows instruction length is inconsistent")
            entries.append((int(rva, 16), expected, int(operand)))
        if not entries:
            raise ValueError("Windows instruction table is empty")
        tables[name] = entries
    return tables


def validate(game: Path):
    data = Path(game).read_bytes()
    tables = profile()
    if data[:2] != b"MZ" or len(data) < 64:
        raise ValueError("Invalid Windows DOS header")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe < 64 or pe + 264 > len(data) or data[pe : pe + 4] != b"PE\0\0":
        raise ValueError("Invalid Windows PE header")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    if (
        machine != 0x8664
        or not 0 < count <= 32
        or optional_size != 240
        or struct.unpack_from("<H", data, pe + 24)[0] != 0x20B
    ):
        raise ValueError("Unsupported Windows PE architecture or layout")
    sections = []
    for i in range(count):
        position = pe + 24 + optional_size + i * 40
        if position + 40 > len(data):
            raise ValueError("Truncated Windows section table")
        _, _, rva, size, offset, _, _, _, _, flags = struct.unpack_from(
            "<8sIIIIIIHHI", data, position
        )
        if offset + size > len(data):
            raise ValueError("Truncated Windows section")
        if flags & 0x20000000:
            sections.append((rva, rva + size, offset))
    issues = []
    for entries in tables.values():
        for rva, expected, _ in entries:
            section = next(
                (s for s in sections if s[0] <= rva and rva + len(expected) <= s[1]), None
            )
            if section is None:
                issues.append(f"Windows code section missing at {rva:x}")
                continue
            offset = section[2] + rva - section[0]
            if data[offset : offset + len(expected)] != expected:
                issues.append(f"Unexpected Windows instruction at {rva:x}")
    return {
        "compatible": not issues,
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


def compile_binaries(output: Path, compiler: str | None = None):
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
    launcher = output / "dd_windows_launcher.exe"
    library = output / "dd_skin_rig_patch.dll"
    subprocess.run(
        [*common, "-municode", str(HERE / "windows_launcher.c"), "-o", str(launcher)],
        check=True,
    )
    subprocess.run(
        [*common, "-shared", str(COMMON_NATIVE / "skin_rig_patch.c"), "-lm", "-o", str(library)],
        check=True,
    )
    setup = output / "dd_patch_setup.exe"
    with tempfile.TemporaryDirectory(prefix="dd-setup-payload-") as directory:
        temporary = Path(directory)
        header = []
        for name, payload in (
            ("setup_script", (WINDOWS_SCRIPTS / "windows_steam.ps1").read_bytes()),
            ("setup_launcher", launcher.read_bytes()),
            ("setup_library", library.read_bytes()),
        ):
            rows = [
                ",".join(f"0x{byte:02x}" for byte in payload[i : i + 32])
                for i in range(0, len(payload), 32)
            ]
            header.append(
                f"static const unsigned char {name}[] = {{\n" + ",\n".join(rows) + "\n};\n"
            )
        (temporary / "windows_setup_payload.h").write_text("".join(header))
        manifest = temporary / "setup.manifest"
        manifest.write_text("""<?xml version="1.0" encoding="UTF-8"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
 <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>
  <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
 </requestedPrivileges></security></trustInfo>
</assembly>
""")
        resource = temporary / "setup.rc"
        resource.write_text('1 24 "' + manifest.as_posix() + '"\n')
        windres = Path(compiler).resolve().parent / "x86_64-w64-mingw32-windres"
        if not windres.is_file():
            # Resolve the command path before following LLVM-MinGW's symlink.
            selected = shutil.which(str(compiler))
            windres = Path(selected or compiler).absolute().parent / "x86_64-w64-mingw32-windres"
        compiled_resource = temporary / "setup_resource.o"
        subprocess.run(
            [str(windres), "-i", str(resource), "-o", str(compiled_resource)], check=True
        )
        subprocess.run(
            [
                *common,
                "-municode",
                "-mwindows",
                "-I",
                str(temporary),
                str(HERE / "windows_setup.c"),
                str(compiled_resource),
                "-lshell32",
                "-luser32",
                "-o",
                str(setup),
            ],
            check=True,
        )
    return {"launcher": str(launcher), "library": str(library), "setup": str(setup)}


def build(game: Path, compiler: str | None = None):
    result = validate(game)
    output = Path(tempfile.mkdtemp(prefix="dd-windows-build-"))
    result.update(compile_binaries(output, compiler))
    result["check_only"] = True
    return result
