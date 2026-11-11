"""Cross-platform skin cycles through production rig creation and swapping."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from darkest_dungeon_patches.paths import COMMON_NATIVE, WINDOWS_NATIVE


class SkinCycleTest(unittest.TestCase):
    def compile_fixture(self, root, windows, name):
        compiler = os.getenv("DD_WINDOWS_CC") if windows else "clang"
        runner = os.getenv("DD_WINDOWS_RUNNER") if windows else None
        if windows and not (compiler and runner):
            self.skipTest("Set DD_WINDOWS_CC and DD_WINDOWS_RUNNER for Windows skin cycles")
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
                str(Path(__file__).with_name(name)),
                *([] if windows else ["-fsanitize=address,undefined", "-ldl"]),
                "-lm",
                "-o",
                str(binary),
            ],
            check=True,
        )
        return ([runner] if windows else []) + [str(binary)]

    def run_fixture(self, command, root, args, expected):
        result = subprocess.run(
            command + list(map(str, args)),
            cwd=root,
            env=dict(os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d"),
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(expected, result.stdout)

    def run_cycle(self, windows):
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
            command = self.compile_fixture(root, windows, "skin_cycle_fixture.c")
            for official in (0, 1):
                for hide in (0, 1):
                    with self.subTest(windows=windows, official=official, hide=hide):
                        self.run_fixture(command, root, (official, hide), "SKIN_CYCLE_OK")

    def run_native_fallback(self, windows):
        with tempfile.TemporaryDirectory(prefix="dd-native-fallback-") as directory:
            root = Path(directory)
            for origin, suffix in [("", "Z")] + [("mods/one", c) for c in "ABC"]:
                anim = root / origin / f"heroes/alpha/alpha_{suffix}/anim"
                anim.mkdir(parents=True)
                (anim / "idle.png").write_bytes(b"synthetic texture")
                (anim / "alpha.sprite.idle.skel").write_bytes(b"synthetic skeleton")
                (anim / "alpha.sprite.idle.atlas").write_text("idle.png\nsize: 8,8\n")
            command = self.compile_fixture(root, windows, "native_fallback_fixture.c")
            for failure in (1, 2, 3, 4):
                with self.subTest(windows=windows, failure=failure):
                    self.run_fixture(command, root, (failure,), "NATIVE_FALLBACK_OK")

    def test_linux_native_fallback_colour_and_preview(self):
        self.run_native_fallback(False)

    def test_windows_native_fallback_colour_and_preview(self):
        self.run_native_fallback(True)

    def run_class_fallback(self, windows):
        with tempfile.TemporaryDirectory(prefix="dd-class-fallback-") as directory:
            root = Path(directory)

            def rig(origin, relative, action, pages):
                anim = root / origin / relative / "anim"
                anim.mkdir(parents=True, exist_ok=True)
                (anim / f"alpha.sprite.{action}.skel").write_bytes(b"own synthetic skeleton")
                (anim / f"alpha.sprite.{action}.atlas").write_text(
                    "".join(f"{page}\nsize: 8,8\nformat: RGBA8888\n\n" for page in pages)
                )

            def texture(origin, skin, name):
                path = root / origin / f"heroes/alpha/alpha_{skin}/anim" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"own synthetic texture")

            for skin in "BC":
                rig("mods/skin", f"heroes/alpha/alpha_{skin}", "idle", ["idle.png"])
                texture("mods/skin", skin, "idle.png")
            for action in ("idle", "walk", "dead"):
                rig("mods/base", "heroes/alpha", action, [f"{action}.png"])
                texture("mods/base", "A", f"{action}.png")
            rig("mods/other", "heroes/alpha", "walk", ["walk.png"])
            texture("mods/other", "A", "walk.png")
            # An unmounted Mod and a different class must never supply actions.
            rig("mods/unmounted", "heroes/alpha", "unwanted", ["unwanted.png"])
            rig("mods/other", "heroes/beta", "unwanted", ["unwanted.png"])
            rig("mods/base", "heroes/alpha", "effect", ["effect.png"])
            command = self.compile_fixture(root, windows, "class_fallback_fixture.c")
            for preferred in (1, 2):
                for hide in (0, 1):
                    with self.subTest(windows=windows, priority=preferred, hide=hide):
                        self.run_fixture(command, root, (preferred, 0, hide), "CLASS_FALLBACK_OK")
            lower_atlas = root / "mods/other/heroes/alpha/anim/alpha.sprite.walk.atlas"
            lower_atlas.unlink()
            self.run_fixture(command, root, (1, 2), "CLASS_FALLBACK_OK")
            self.run_fixture(command, root, (2, 1), "CLASS_FALLBACK_REFUSED_OK")
            lower_atlas.write_text("walk.png\nsize: 8,8\n")
            # Base actions may live beside its default palette, not in anim/.
            moved = []
            for action in ("walk", "dead"):
                for extension in ("skel", "atlas"):
                    source = root / f"mods/base/heroes/alpha/anim/alpha.sprite.{action}.{extension}"
                    target = root / f"mods/base/heroes/alpha/alpha_A/anim/{source.name}"
                    source.rename(target)
                    moved.append((source, target))
            other_skel = lower_atlas.with_suffix(".skel")
            other_skel.unlink()
            lower_atlas.unlink()
            self.run_fixture(command, root, (1, 2), "CLASS_FALLBACK_OK")
            for source, target in moved:
                target.rename(source)
            rig("mods/other", "heroes/alpha", "walk", ["walk.png"])
            # A broken selected override must not fall through to a base copy.
            own_atlas = root / "mods/skin/heroes/alpha/alpha_B/anim/alpha.sprite.idle.atlas"
            own_atlas.unlink()
            self.run_fixture(command, root, (1, 1), "CLASS_FALLBACK_REFUSED_OK")
            own_atlas.write_text("idle.png\nsize: 8,8\n")
            # Multiple pages of one atlas need a coherent texture directory.
            rig("mods/base", "heroes/alpha", "walk", ["walk.png", "extra.png"])
            texture("mods/base", "A", "extra.png")
            self.run_fixture(command, root, (1, 2), "CLASS_FALLBACK_OK")
            texture("mods/skin", "B", "walk.png")
            self.run_fixture(command, root, (1, 1), "CLASS_FALLBACK_REFUSED_OK")

    def test_linux_skin_cycles(self):
        self.run_cycle(False)

    def test_windows_skin_cycles(self):
        self.run_cycle(True)

    def test_linux_mod_class_fallback_and_cycles(self):
        self.run_class_fallback(False)

    def test_windows_mod_class_fallback_and_cycles(self):
        self.run_class_fallback(True)
