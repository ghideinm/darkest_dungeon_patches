"""Delayed canvas rebuilds use only synthetic targets and graphics APIs."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from darkest_dungeon_patches.paths import WINDOWS_NATIVE


class WindowsCanvasTest(unittest.TestCase):
    def test_transaction_rolls_back_and_preserves_target_identity(self):
        with tempfile.TemporaryDirectory(prefix="dd-canvas-core-") as directory:
            binary = Path(directory) / "canvas"
            subprocess.run(
                [
                    "clang",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(WINDOWS_NATIVE),
                    str(Path(__file__).with_name("canvas_fixture.c")),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("CANVAS_TRANSACTION_OK", result.stdout)

    def test_windows_render_thread_rebuild_and_graphics_state(self):
        compiler, runner = (os.getenv(n) for n in ("DD_WINDOWS_CC", "DD_WINDOWS_RUNNER"))
        if not compiler or not runner:
            self.skipTest("Set DD_WINDOWS_CC and DD_WINDOWS_RUNNER for Windows canvas fixtures")
        with tempfile.TemporaryDirectory(prefix="dd-canvas-frame-") as directory:
            binary = Path(directory) / "canvas.exe"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(WINDOWS_NATIVE),
                    str(Path(__file__).with_name("windows_canvas_fixture.c")),
                    "-lm",
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            result = subprocess.run(
                [runner, str(binary)], capture_output=True, text=True, timeout=60
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("DELAYED_CANVAS_OK", result.stdout)
