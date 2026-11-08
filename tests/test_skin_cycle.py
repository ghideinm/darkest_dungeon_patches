"""Cross-platform skin cycles through production rig creation and swapping."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from darkest_dungeon_patches.paths import COMMON_NATIVE, WINDOWS_NATIVE


class SkinCycleTest(unittest.TestCase):
    def run_cycle(self, windows):
        compiler = os.getenv("DD_WINDOWS_CC") if windows else "clang"
        runner = os.getenv("DD_WINDOWS_RUNNER") if windows else None
        if windows and not (compiler and runner):
            self.skipTest("Set DD_WINDOWS_CC and DD_WINDOWS_RUNNER for Windows skin cycles")
        with tempfile.TemporaryDirectory(prefix="dd-skin-cycle-") as directory:
            root = Path(directory)
            for origin, suffix in [("", "Z")] + [("mods/one", c) for c in "ABC"]:
                anim = root / origin / f"heroes/alpha/alpha_{suffix}/anim"
                anim.mkdir(parents=True)
                (anim / "idle.png").write_bytes(b"own synthetic texture")
                (anim / "alpha.sprite.idle.skel").write_bytes(b"own synthetic skeleton")
                (anim / "alpha.sprite.idle.atlas").write_text(
                    "idle.png\nsize: 8,8\nformat: RGBA8888\nregion\n  rotate: false\n"
                )
            binary = root / ("cycle.exe" if windows else "cycle")
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-O1",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-DDD_RIG_RUNTIME_TEST",
                    "-I",
                    str(COMMON_NATIVE),
                    "-I",
                    str(WINDOWS_NATIVE),
                    str(Path(__file__).with_name("skin_cycle_fixture.c")),
                    *([] if windows else ["-fsanitize=address,undefined", "-ldl"]),
                    "-lm",
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            environment = dict(
                os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d"
            )
            for official in (0, 1):
                for hide in (0, 1):
                    with self.subTest(windows=windows, official=official, hide=hide):
                        result = subprocess.run(
                            ([runner] if windows else []) + [str(binary), str(official), str(hide)],
                            cwd=root,
                            env=environment,
                            capture_output=True,
                            text=True,
                            timeout=60,
                        )
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn("SKIN_CYCLE_OK", result.stdout)

    def test_linux_skin_cycles(self):
        self.run_cycle(False)

    def test_windows_skin_cycles(self):
        self.run_cycle(True)
