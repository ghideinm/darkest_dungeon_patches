"""Self-contained config parsing, discovery and logging regressions."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from darkest_dungeon_patches.paths import COMMON_NATIVE, CONFIG_SCRIPTS


class PatchConfigTest(unittest.TestCase):
    def compile(self, root, windows=False):
        source = root / "config.c"
        source.write_text(r"""#define _GNU_SOURCE
#include "patch_config.h"
int main(void) {
  dd_runtime_init((const void *)main);
  dd_patch_log(2,"OWN_CONFIG_LOG_MARKER\n");
  printf("%u %zu %d\n",dd_config.resource_buffer_mib,
         dd_config.cache_initial_capacity,dd_config.log_to_file);
  return 0;
}
""")
        binary = root / ("config.exe" if windows else "config")
        compiler = os.environ["DD_WINDOWS_CC"] if windows else "clang"
        subprocess.run(
            [
                compiler,
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(COMMON_NATIVE),
                str(source),
                *([] if windows else ["-ldl"]),
                "-o",
                str(binary),
            ],
            check=True,
        )
        return binary

    def run_fixture(self, binary, cwd, windows=False, **overrides):
        environment = dict(os.environ, WINEDEBUG="-all")
        for name in ("DD_PATCH_CONFIG", "DD_PATCH_LOG"):
            environment.pop(name, None)
        environment.update(overrides)
        result = subprocess.run(
            [*([os.environ["DD_WINDOWS_RUNNER"]] if windows else []), str(binary)],
            cwd=cwd,
            env=environment,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def exercise_module_config(self, windows):
        with tempfile.TemporaryDirectory(prefix="dd-config-") as directory:
            root = Path(directory)
            module = root / "补丁目录"
            module.mkdir()
            work = root / "cwd"
            work.mkdir()
            # A configuration in the working directory must not shadow the module's.
            (work / "dd_engine_patch.ini").write_text("[memory]\nresource_buffer_mib=99\n")
            binary = self.compile(module, windows)
            result = self.run_fixture(binary, work, windows)
            self.assertEqual(result.stdout.strip(), "32 16 0")
            config = module / "dd_engine_patch.ini"
            config.write_bytes((CONFIG_SCRIPTS / config.name).read_bytes())
            result = self.run_fixture(binary, work, windows)
            self.assertEqual(result.stdout.strip(), "32 16 0")
            self.assertFalse(list(module.glob("*.log")))
            config.write_text(
                "\ufeff[memory]\r\nresource_buffer_mib=64\r\ncache_initial_capacity=3\r\n"
                "[logging]\r\nlog_to_file=1\r\nlog_file=诊断.log\r\n"
            )
            for _ in range(2):
                result = self.run_fixture(binary, work, windows)
                self.assertEqual(result.stdout.strip(), "64 3 1")
            log = module / "诊断.log"
            self.assertEqual(log.read_text().count("OWN_CONFIG_LOG_MARKER"), 2)
            before = log.read_bytes()
            config.write_text("[logging]\nlog_to_file=0\nlog_file=诊断.log\n")
            result = self.run_fixture(binary, work, windows)
            self.assertEqual(result.stdout.strip(), "32 16 0")
            self.assertEqual(log.read_bytes(), before)
            config.write_text("[logging]\nlog_to_file=1\nlog_file=missing/diagnostic.log\n")
            result = self.run_fixture(binary, work, windows)
            self.assertEqual(result.stdout.strip(), "32 16 1")
            override = module / "指定配置"
            override.mkdir()
            custom = override / "settings.ini"
            custom.write_text(
                "[memory]\nresource_buffer_mib=48\n[logging]\nlog_to_file=1\n"
                "log_file=指定日志.log\n[skins]\nhide_vanilla_skins=1\n"
            )
            result = self.run_fixture(binary, work, windows, DD_PATCH_CONFIG=str(custom))
            self.assertEqual(result.stdout.strip(), "48 16 1")
            self.assertIn("OWN_CONFIG_LOG_MARKER", (override / "指定日志.log").read_text())
            self.assertIn("hide_vanilla_skins=1", (override / "指定日志.log").read_text())

    def test_module_relative_config_and_utf8_logs(self):
        self.exercise_module_config(False)

    def test_windows_module_relative_config_and_utf8_logs(self):
        if not all(os.getenv(n) for n in ("DD_WINDOWS_CC", "DD_WINDOWS_RUNNER")):
            self.skipTest("Set DD_WINDOWS_CC and DD_WINDOWS_RUNNER for native fixtures")
        self.exercise_module_config(True)

    def test_explicit_config_and_legacy_log_fallback(self):
        with tempfile.TemporaryDirectory(prefix="dd-config-override-") as directory:
            root = Path(directory)
            binary = self.compile(root)
            log = root / "legacy.log"
            result = self.run_fixture(binary, root, DD_PATCH_LOG=str(log))
            self.assertEqual(result.stdout.strip(), "32 16 1")
            self.assertIn("OWN_CONFIG_LOG_MARKER", log.read_text())
            settings = root / "settings"
            settings.mkdir()
            config = settings / "custom.ini"
            config.write_text(
                "[memory]\nresource_buffer_mib=48\ncache_initial_capacity=7\n"
                "[logging]\nlog_to_file=1\nlog_file=custom.log\n"
            )
            result = self.run_fixture(binary, root, DD_PATCH_CONFIG=str(config))
            self.assertEqual(result.stdout.strip(), "48 7 1")
            self.assertIn("OWN_CONFIG_LOG_MARKER", (settings / "custom.log").read_text())
            before = log.read_bytes()
            config.write_text("[logging]\nlog_to_file=0\n")
            self.run_fixture(binary, root, DD_PATCH_CONFIG=str(config), DD_PATCH_LOG=str(log))
            self.assertEqual(log.read_bytes(), before)

    def test_invalid_values_retain_defaults_and_valid_duplicates(self):
        with tempfile.TemporaryDirectory(prefix="dd-config-invalid-") as directory:
            root = Path(directory)
            binary = self.compile(root)
            config = root / "dd_engine_patch.ini"
            config.write_text(
                "[memory]\nresource_buffer_mib=64\nresource_buffer_mib=-1\n"
                "cache_initial_capacity=0\ncache_initial_capacity=184467440737095516160\n"
                "[logging]\nlog_to_file=2\nlog_file=\n"
            )
            result = self.run_fixture(binary, root)
            self.assertEqual(result.stdout.strip(), "64 16 0")
            self.assertIn("DD_PATCH_CONFIG_INVALID fields=5", result.stderr)
            for value in ("0", "5", "2048", "32junk", "32.5", "999999999999999999999999999"):
                config.write_text("[memory]\nresource_buffer_mib=" + value + "\n")
                self.assertEqual(self.run_fixture(binary, root).stdout.strip(), "32 16 0")
            config.write_text("[memory]\n" + "x" * 9000 + "\nresource_buffer_mib=48\n")
            result = self.run_fixture(binary, root)
            self.assertEqual(result.stdout.strip(), "48 16 0")
            self.assertIn("DD_PATCH_CONFIG_INVALID fields=1", result.stderr)

    def test_skin_visibility_option_is_explicit_and_validated(self):
        with tempfile.TemporaryDirectory(prefix="dd-config-skins-") as directory:
            root = Path(directory)
            binary = self.compile(root)
            config = root / "dd_engine_patch.ini"
            for value in ("0", "1", "2", "-1", "true"):
                config.write_text("[skins]\nhide_vanilla_skins=" + value + "\n")
                result = self.run_fixture(binary, root)
                self.assertIn("hide_vanilla_skins=" + ("1" if value == "1" else "0"), result.stderr)
                self.assertEqual(
                    "DD_PATCH_CONFIG_INVALID" in result.stderr, value not in ("0", "1")
                )
