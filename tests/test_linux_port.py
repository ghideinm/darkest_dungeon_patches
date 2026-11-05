"""Code-only compatibility checks using generated ELF files and programs."""

import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from darkest_dungeon_patches import linux_port
from darkest_dungeon_patches.paths import COMMON_NATIVE, LINUX_NATIVE


class LinuxCompatibilityTest(unittest.TestCase):
    def test_changes_outside_patch_sites_are_allowed(self):
        with tempfile.TemporaryDirectory(prefix="dd-elf-fixture-") as directory:
            path = Path(directory) / "fixture"
            data = bytearray(128 + max(linux_port.SITES) + 5)
            data[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<H", data, 18, 62)
            struct.pack_into("<Q", data, 32, 64)
            struct.pack_into("<HH", data, 54, 56, 1)
            struct.pack_into(
                "<IIQQQQQQ", data, 64, 1, 5, 128, 0, 0, len(data) - 128, len(data) - 128, 4096
            )
            for i, address in enumerate(linux_port.SITES):
                data[128 + address : 128 + address + 5] = bytes(
                    [0xB9 if i == 6 else 0xBF, 0, 0, 0x60, 0]
                )
            path.write_bytes(data)
            self.assertTrue(linux_port.validate(path)["compatible"])
            data[120] ^= 1
            path.write_bytes(data)
            self.assertTrue(linux_port.validate(path)["compatible"])
            data[128 + linux_port.SITES[0]] ^= 1
            path.write_bytes(data)
            result = linux_port.validate(path)
            self.assertFalse(result["compatible"])
            self.assertIn("instruction", result["compatibility_issues"][0])


class PreloadCompatibilityTest(unittest.TestCase):
    def test_resource_and_canvas_transactions_on_synthetic_memory(self):
        with tempfile.TemporaryDirectory(prefix="dd-resource-unit-") as directory:
            binary = Path(directory) / "resource_selftest"
            subprocess.run(
                [
                    "clang",
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-DDD_PATCH_SELFTEST",
                    str(LINUX_NATIVE / "resource_loading_patch.c"),
                    "-ldl",
                    "-lm",
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unknown_layout_logs_and_continues_to_main(self):
        with tempfile.TemporaryDirectory(prefix="dd-preload-fixture-") as directory:
            root = Path(directory)
            source = root / "program.c"
            source.write_text(
                '#include <stdio.h>\nint main(void){puts("FIXTURE_MAIN");return 0;}\n'
            )
            # The game basename selects the hooks; this is our own tiny ELF.
            binary = root / "darkest.bin.x86_64"
            libraries = [root / "resource.so", root / "rig.so"]
            common = ["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
            subprocess.run([*common, str(source), "-o", str(binary)], check=True)
            for source, library in zip(
                [LINUX_NATIVE / "resource_loading_patch.c", COMMON_NATIVE / "skin_rig_patch.c"],
                libraries,
            ):
                subprocess.run(
                    [*common, "-fPIC", "-shared", str(source), "-ldl", "-lm", "-o", str(library)],
                    check=True,
                )
            log = root / "patch.log"
            result = subprocess.run(
                [str(binary)],
                capture_output=True,
                text=True,
                env=dict(
                    os.environ,
                    LD_PRELOAD=":".join(map(str, libraries)),
                    DD_RIG_RUNTIME="1",
                    DD_NATIVE_RENDER="1",
                    DD_LOADBUFFER_PATCH="1",
                    DD_PATCH_LOG=str(log),
                ),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("FIXTURE_MAIN", result.stdout)
            for message in (
                "DD_LOADBUFFER_PATCH_SKIPPED",
                "DD_NATIVE_RENDER_PATCH_SKIPPED",
                "DD_RIG_PATCH_SKIPPED",
            ):
                self.assertIn(message, log.read_text())
            self.assertFalse(linux_port.validate(binary)["compatible"])
