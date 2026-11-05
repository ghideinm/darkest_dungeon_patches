"""Generate test-owned instruction features and independent code/data layouts."""

import hashlib
import re
import struct
from dataclasses import dataclass
from pathlib import Path

from darkest_dungeon_patches.address_scan import ImageRange, Signature, signatures
from darkest_dungeon_patches.paths import COMMON_NATIVE
from darkest_dungeon_patches.windows_port import profile

LINUX_GUARDS = {
    **{f"RESOURCE_{i}": bytes([0xB9 if i == 6 else 0xBF, 0, 0, 0x60, 0]) for i in range(7)},
    **dict(
        zip(
            (f"RENDER_{i}" for i in range(7)),
            map(
                bytes.fromhex,
                [
                    "bd80070000",
                    "41be38040000",
                    "0f47e8",
                    "bf80070000",
                    "be38040000",
                    "bf80070000",
                    "be38040000",
                ],
            ),
        )
    ),
    "HOOK_NEW": bytes.fromhex("e8f72eb7ff"),
    "HOOK_COLOUR": bytes.fromhex("4157415653"),
    "HOOK_PREVIEW": bytes.fromhex("4156534881ec08020000"),
    "HOOK_TEXTURE": bytes.fromhex("5541574156"),
}


@dataclass
class Fixture:
    code_address: int
    data_address: int
    code: bytearray
    data: bytearray
    symbols: dict
    features: list[Signature]
    header: Path

    def ranges(self):
        return [
            ImageRange(self.code_address, bytes(self.code), len(self.code), 5),
            ImageRange(self.data_address, bytes(self.data), len(self.data), 6),
        ]


def make_fixture(
    root: Path, platform: str, code_address: int, data_address: int, actor_offset: int | None = None
) -> Fixture:
    source = signatures(platform)
    guard = (
        LINUX_GUARDS
        if platform == "linux"
        else {name: expected for rows in profile().values() for name, expected, _ in rows}
    )
    symbols = {}
    code_index, data_index = 0, 0
    for entry in source:
        if entry.target_flags == 1:
            symbols[entry.symbol] = code_address + 0x100 + code_index * 256
            code_index += 1
        elif entry.target_flags == 8:
            symbols[entry.symbol] = (
                actor_offset
                if actor_offset is not None
                else (0x1070 if platform == "linux" else 0x1050)
            )
        else:
            symbols[entry.symbol] = data_address + data_index * 16
            data_index += 1
    code = bytearray(b"\xcc" * (0x200 + len(source) * 256))
    data = bytearray(4096)
    features, rows = [], []
    data_index = code_index
    for entry in source:
        if entry.target_flags == 1:
            at = symbols[entry.symbol]
            raw = bytearray(guard.get(entry.symbol, b"\xc3"))
            pattern = [f"{b:02x}" for b in raw]
            if entry.symbol == "HOOK_NEW":
                struct.pack_into("<i", raw, 1, symbols["NATIVE_NEW"] - at - 5)
                pattern[1:5] = ["??"] * 4
            capture, kind = 0, 0
        elif entry.target_flags == 8:
            at = code_address + 0x100 + data_index * 256
            data_index += 1
            raw = bytearray(b"\x48\x8b\x80" + struct.pack("<I", symbols[entry.symbol]))
            pattern = ["48", "8b", "80", "??", "??", "??", "??"]
            capture, kind = 3, 4
        else:
            at = code_address + 0x100 + data_index * 256
            data_index += 1
            raw = bytearray(b"\x48\x8d\x05" + struct.pack("<i", symbols[entry.symbol] - at - 7))
            pattern = ["48", "8d", "05", "??", "??", "??", "??"]
            capture, kind = 3, 1
        # These marker bytes are our own test data, never executed as code.
        marker = hashlib.blake2s(("dd fixture " + entry.symbol).encode(), digest_size=16).digest()
        raw.extend(marker)
        pattern.extend(f"{b:02x}" for b in marker)
        offset = at - code_address
        code[offset : offset + len(raw)] = raw
        feature = Signature(
            entry.symbol, " ".join(pattern), entry.group, capture, kind, 0, entry.target_flags
        )
        features.append(feature)
        rows.append(
            '{"'
            + entry.symbol.lower()
            + '", "'
            + feature.pattern
            + '", DD_'
            + entry.symbol
            + ", "
            + ",".join(map(str, [entry.group, capture, kind, 0, entry.target_flags]))
            + "},"
        )
    header = root / (platform + "_test_profile.h")
    header.write_text(
        '#include "'
        + str(COMMON_NATIVE / "address_symbols.h")
        + '"\nstatic const DdSignature dd_signatures[] = {\n'
        + "\n".join(rows)
        + "\n};\n"
    )
    return Fixture(code_address, data_address, code, data, symbols, features, header)


def symbol_order():
    source = (COMMON_NATIVE / "address_symbols.h").read_text()
    return re.findall(r"^\s*DD_(\w+),", source, re.M)


def write_pe(path: Path, fixture: Fixture):
    code_size = (len(fixture.code) + 4095) & ~4095
    data_size = (len(fixture.data) + 4095) & ~4095
    data = bytearray(4096 + code_size + data_size)
    data[:2] = b"MZ"
    pe = 128
    struct.pack_into("<I", data, 60, pe)
    data[pe : pe + 4] = b"PE\0\0"
    struct.pack_into("<HH", data, pe + 4, 0x8664, 2)
    struct.pack_into("<H", data, pe + 20, 240)
    struct.pack_into("<H", data, pe + 24, 0x20B)
    struct.pack_into("<I", data, pe + 80, fixture.data_address + data_size)
    for i, (name, address, size, offset, flags) in enumerate(
        [
            (b".code", fixture.code_address, code_size, 4096, 0x60000020),
            (b".data", fixture.data_address, data_size, 4096 + code_size, 0xC0000040),
        ]
    ):
        struct.pack_into(
            "<8sIIIIIIHHI",
            data,
            pe + 264 + i * 40,
            name,
            size,
            address,
            size,
            offset,
            0,
            0,
            0,
            0,
            flags,
        )
    data[4096 : 4096 + len(fixture.code)] = fixture.code
    data[4096 + code_size : 4096 + code_size + len(fixture.data)] = fixture.data
    path.write_bytes(data)
