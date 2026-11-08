"""Offline counterpart of the native signature resolver; never executes game code."""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass
from pathlib import Path

from .paths import LINUX_NATIVE, WINDOWS_NATIVE


@dataclass(frozen=True)
class ImageRange:
    address: int
    data: bytes
    memory_size: int
    flags: int


@dataclass(frozen=True)
class Signature:
    symbol: str
    pattern: str
    group: int
    capture: int
    kind: int
    adjustment: int
    target_flags: int


def signatures(platform: str) -> list[Signature]:
    root = {"linux": LINUX_NATIVE, "windows": WINDOWS_NATIVE}[platform]
    source = (root / f"{platform}_signatures.h").read_text()
    rows = []
    for match in re.finditer(
        r'\{"[^"]+",\s*((?:"[^"]*"\s*)+),\s*DD_(\w+),\s*'
        r"(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(\d+)\}",
        source,
    ):
        pattern, symbol, *values = match.groups()
        rows.append(
            Signature(symbol, "".join(re.findall(r'"([^"]*)"', pattern)), *map(int, values))
        )
    if not rows:
        raise ValueError("Empty instruction signature profile")
    return rows


def read_image(path: Path, platform: str) -> list[ImageRange]:
    data = path.read_bytes()
    ranges = []
    if platform == "linux":
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
        for i in range(count):
            kind, flags, pos, address, _, size, memory, _ = struct.unpack_from(
                "<IIQQQQQQ", data, offset + entry_size * i
            )
            if pos + size > len(data) or size > memory:
                raise ValueError("Truncated ELF segment")
            if kind == 1:
                ranges.append(ImageRange(address, data[pos : pos + size], memory, flags))
    else:
        if len(data) < 64 or data[:2] != b"MZ":
            raise ValueError("Invalid Windows DOS header")
        pe = struct.unpack_from("<I", data, 60)[0]
        if pe < 64 or pe + 264 > len(data) or data[pe : pe + 4] != b"PE\0\0":
            raise ValueError("Invalid Windows PE header")
        machine, count = struct.unpack_from("<HH", data, pe + 4)
        optional = struct.unpack_from("<H", data, pe + 20)[0]
        if (
            machine != 0x8664
            or optional != 240
            or not 0 < count <= 96
            or struct.unpack_from("<H", data, pe + 24)[0] != 0x20B
        ):
            raise ValueError("Unsupported Windows PE architecture or layout")
        image_size = struct.unpack_from("<I", data, pe + 80)[0]
        for i in range(count):
            at = pe + 24 + optional + 40 * i
            if at + 40 > len(data):
                raise ValueError("Truncated Windows section table")
            _, memory, address, size, pos, _, _, _, _, flags = struct.unpack_from(
                "<8sIIIIIIHHI", data, at
            )
            if pos + size > len(data) or address + max(memory, size) > image_size:
                raise ValueError("Truncated Windows section")
            permissions = (
                (1 if flags & 0x20000000 else 0)
                | (2 if flags & 0x80000000 else 0)
                | (4 if flags & 0x40000000 else 0)
            )
            ranges.append(
                ImageRange(address, data[pos : pos + size], max(memory, size), permissions)
            )
    return ranges


def bytes_at(ranges: list[ImageRange], address: int, length: int) -> bytes | None:
    for region in ranges:
        offset = address - region.address
        if 0 <= offset and offset + length <= len(region.data):
            return region.data[offset : offset + length]
    return None


def resolve(ranges: list[ImageRange], profile: list[Signature], groups: int = 7):
    resolved, issues = {}, []
    for feature in profile:
        if not feature.group & groups:
            continue
        tokens = feature.pattern.split()
        expression = re.compile(
            b"".join(
                b"." if token == "??" else re.escape(bytes.fromhex(token)) for token in tokens
            ),
            re.S,
        )
        hits = []
        for region in ranges:
            if not region.flags & 1:
                continue
            start = 0
            while (hit := expression.search(region.data, start)) is not None:
                hits.append((region.address + hit.start(), hit.group()))
                if len(hits) > 1:
                    break
                start = hit.start() + 1
            if len(hits) > 1:
                break
        if len(hits) != 1:
            issues.append(f"{feature.symbol}: matches={len(hits)}")
            continue
        address, raw = hits[0]
        target = address
        if feature.kind:
            width = 8 if feature.kind == 3 else 4
            if feature.kind > 5 or feature.capture + width > len(raw):
                raise ValueError("Invalid signature capture")
            target = int.from_bytes(
                raw[feature.capture : feature.capture + width],
                "little",
                signed=feature.kind in (1, 5),
            )
            if feature.kind in (1, 5):
                target += address + feature.capture + 4
        target += feature.adjustment
        length = 1 if feature.target_flags == 1 else 8
        if feature.kind == 5:
            setter = bytes_at(ranges, target, 8)
            if not setter or setter[:3] != b"\x48\x89\xb7" or setter[7] != 0xC3:
                issues.append(f"{feature.symbol}: unsupported native field accessor")
                continue
            target = int.from_bytes(setter[3:7], "little")
        if feature.target_flags == 8:
            valid = feature.kind in (4, 5) and 8 <= target <= 0x10000 and target % 8 == 0
        else:
            valid = any(
                region.flags & feature.target_flags == feature.target_flags
                and region.address <= target
                and target + length <= region.address + region.memory_size
                for region in ranges
            )
        if not valid:
            issues.append(f"{feature.symbol}: target outside required sections or field bounds")
            continue
        resolved[feature.symbol] = target
    if (
        "ACTOR_SPRITE_FIELD" in resolved
        and "ACTOR_SPRITE_VERIFY" in resolved
        and resolved["ACTOR_SPRITE_FIELD"] != resolved["ACTOR_SPRITE_VERIFY"]
    ):
        issues.append("ACTOR_SPRITE_FIELD: independent field references disagree")
    return resolved, issues


def check_instructions(ranges, resolved, entries):
    issues = []
    for symbol, expected, _ in entries:
        if symbol not in resolved:
            continue
        at = resolved[symbol]
        actual = bytes_at(ranges, at, len(expected))
        valid = actual == expected
        if len(expected) == 5 and expected[0] == 0xE8 and actual and actual[0] == 0xE8:
            valid = at + 5 + int.from_bytes(actual[1:], "little", signed=True) == resolved.get(
                "NATIVE_NEW"
            )
        if not valid:
            issues.append(f"{symbol}: unexpected instruction")
    return issues
