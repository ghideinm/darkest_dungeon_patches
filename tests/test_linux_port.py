"""Code-only compatibility checks using generated ELF files and programs."""

import hashlib
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from darkest_dungeon_patches.paths import COMMON_NATIVE, LINUX_NATIVE


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

    def test_preload_follows_relocated_code_and_data(self):
        from address_fixtures import make_fixture

        with tempfile.TemporaryDirectory(prefix="dd-moved-elf-") as directory:
            root = Path(directory)
            libraries = [root / "resource.so", root / "rig.so"]
            common = ["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
            previous_header = None
            for index, (code, data) in enumerate(((0x800000, 0x900000), (0x1400000, 0x1900000))):
                fixture = make_fixture(
                    root, "linux", code, data, actor_offset=0x1070 if index == 0 else 0x10E0
                )
                if previous_header is None:
                    previous_header = fixture.header.read_bytes()
                    for source, library in zip(
                        [
                            LINUX_NATIVE / "resource_loading_patch.c",
                            COMMON_NATIVE / "skin_rig_patch.c",
                        ],
                        libraries,
                    ):
                        subprocess.run(
                            [
                                *common,
                                '-DDD_ADDRESS_TEST_PROFILE="' + str(fixture.header) + '"',
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
                else:
                    self.assertEqual(previous_header, fixture.header.read_bytes())
                work = root / str(index)
                work.mkdir()
                expected_capacity = (32 if index == 0 else 64) * 1024 * 1024
                (work / "code.bin").write_bytes(fixture.code)
                (work / "data.bin").write_bytes(fixture.data)
                assembly = work / "sections.s"
                assembly.write_text(
                    '.section .ddsig,"ax",@progbits\n.incbin "'
                    + str(work / "code.bin")
                    + '"\n.section .dddata,"aw",@progbits\n.incbin "'
                    + str(work / "data.bin")
                    + '"\n.section .note.GNU-stack,"",@progbits\n'
                )
                source = work / "main.c"
                source.write_text(
                    "#include <stdio.h>\n#include <stdint.h>\nint main(void){\n"
                    + "".join(
                        "if(*(const uint32_t *)(uintptr_t)"
                        + str(fixture.symbols[f"RESOURCE_{i}"] + 1)
                        + f" != {expected_capacity}) return 2;\n"
                        for i in range(7)
                    )
                    + "".join(
                        "if(*(const unsigned char *)(uintptr_t)"
                        + str(fixture.symbols[name])
                        + " != 0xe9) return 3;\n"
                        for name in ("HOOK_COLOUR", "HOOK_PREVIEW", "HOOK_TEXTURE")
                    )
                    + 'puts("MOVED_FIXTURE_MAIN");return 0;}\n'
                )
                binary = work / "darkest.bin.x86_64"
                subprocess.run(
                    [
                        *common,
                        "-fno-pie",
                        "-no-pie",
                        str(source),
                        str(assembly),
                        f"-Wl,--section-start=.ddsig={code:#x}",
                        f"-Wl,--section-start=.dddata={data:#x}",
                        "-o",
                        str(binary),
                    ],
                    check=True,
                )
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
                        DD_PATCH_LOG="",
                        DD_RESOURCE_BUFFER_MIB="32" if index == 0 else "64",
                        DD_CACHE_INITIAL_CAPACITY="16" if index == 0 else "3",
                    ),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("MOVED_FIXTURE_MAIN", result.stdout)
                self.assertIn(
                    "actor_sprite_offset=" + format(fixture.symbols["ACTOR_SPRITE_FIELD"], "x"),
                    result.stderr,
                )
                for message in (
                    "DD_LOADBUFFER_PATCH_APPLIED",
                    "DD_NATIVE_RENDER_PATCH_APPLIED",
                    "DD_RIG_PATCH_APPLIED",
                ):
                    self.assertIn(message, result.stderr)

                if index == 1:
                    # Mismatched independently extracted offsets must leave
                    # all rig hooks untouched, while our own main still runs.
                    marker = hashlib.blake2s(
                        b"dd fixture ACTOR_SPRITE_VERIFY", digest_size=16
                    ).digest()
                    raw = bytearray(binary.read_bytes())
                    capture = raw.index(marker) - 4
                    struct.pack_into("<I", raw, capture, 0x1100)
                    binary.write_bytes(raw)
                    mismatch = subprocess.run(
                        [str(binary)],
                        capture_output=True,
                        text=True,
                        env=dict(
                            os.environ,
                            LD_PRELOAD=":".join(map(str, libraries)),
                            DD_RIG_RUNTIME="1",
                            DD_NATIVE_RENDER="1",
                            DD_LOADBUFFER_PATCH="1",
                            DD_PATCH_LOG="",
                            DD_RESOURCE_BUFFER_MIB="64",
                            DD_CACHE_INITIAL_CAPACITY="3",
                        ),
                    )
                    self.assertEqual(mismatch.returncode, 3, mismatch.stderr)
                    self.assertIn("actor_sprite_field_mismatch", mismatch.stderr)
                    self.assertNotIn("DD_RIG_PATCH_APPLIED", mismatch.stderr)
