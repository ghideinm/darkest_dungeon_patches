"""Linux environment and Windows INI configuration regressions."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from darkest_dungeon_patches.paths import COMMON_NATIVE, CONFIG_SCRIPTS

CONFIG_ENVIRONMENT = (
    "DD_PATCH_CONFIG",
    "DD_PATCH_LOG",
    "DD_RESOURCE_BUFFER_MIB",
    "DD_CACHE_INITIAL_CAPACITY",
    "DD_HIDE_VANILLA_SKINS",
    "DD_LOG_TO_FILE",
    "DD_LOG_FILE",
)


class PatchConfigTest(unittest.TestCase):
    def compile(self, root, windows=False):
        source = root / "config.c"
        source.write_text(r"""#define _GNU_SOURCE
#include "patch_config.h"
int main(void) {
  dd_runtime_init((const void *)main);
  dd_patch_log(2,"OWN_CONFIG_LOG_MARKER\n");
  printf("%u %zu %d %d\n",dd_config.resource_buffer_mib,
         dd_config.cache_initial_capacity,dd_config.log_to_file,
         dd_config.hide_vanilla_skins);
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
                "-o",
                str(binary),
            ],
            check=True,
        )
        return binary

    def run_fixture(self, binary, cwd, windows=False, **overrides):
        environment = dict(os.environ, WINEDEBUG="-all")
        for name in CONFIG_ENVIRONMENT:
            environment.pop(name, None)
        environment.update(overrides)
        result = subprocess.run(
            [*([os.environ["DD_WINDOWS_RUNNER"]] if windows else []), str(binary)],
            cwd=cwd,
            env=environment,
            capture_output=True,
            text=True,
            timeout=60 if windows else 5,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def require_windows(self):
        if not all(os.getenv(n) for n in ("DD_WINDOWS_CC", "DD_WINDOWS_RUNNER")):
            self.skipTest("Set DD_WINDOWS_CC and DD_WINDOWS_RUNNER for native fixtures")

    def test_linux_uses_environment_and_never_opens_ini(self):
        with tempfile.TemporaryDirectory(prefix="dd-env-only-") as directory:
            root = Path(directory)
            module, work = root / "module", root / "cwd"
            module.mkdir()
            work.mkdir()
            binary = self.compile(module)
            # Opening either FIFO for reading would block the child and fail its timeout.
            os.mkfifo(module / "dd_engine_patch.ini")
            os.mkfifo(root / "explicit.ini")
            (work / "dd_engine_patch.ini").write_text(
                "[memory]\nresource_buffer_mib=99\n[logging]\nlog_to_file=1\n"
            )
            result = self.run_fixture(binary, work, DD_PATCH_CONFIG=str(root / "explicit.ini"))
            self.assertEqual(result.stdout.strip(), "32 16 0 1")
            result = self.run_fixture(
                binary,
                work,
                DD_PATCH_CONFIG=str(root / "explicit.ini"),
                DD_RESOURCE_BUFFER_MIB="64",
                DD_CACHE_INITIAL_CAPACITY="3",
                DD_HIDE_VANILLA_SKINS="0",
            )
            self.assertEqual(result.stdout.strip(), "64 3 0 0")
            self.assertFalse(list(work.glob("*.log")))

    def test_linux_utf8_log_append_disable_and_unavailable_directory(self):
        with tempfile.TemporaryDirectory(prefix="dd-env-log-") as directory:
            root = Path(directory)
            module, work = root / "module", root / "日志目录"
            module.mkdir()
            work.mkdir()
            binary = self.compile(module)
            for _ in range(2):
                result = self.run_fixture(binary, work, DD_LOG_TO_FILE="1", DD_LOG_FILE="诊断.log")
                self.assertEqual(result.stdout.strip(), "32 16 1 1")
            log = work / "诊断.log"
            self.assertEqual(log.read_text().count("OWN_CONFIG_LOG_MARKER"), 2)
            before = log.read_bytes()
            self.run_fixture(binary, work, DD_LOG_TO_FILE="0", DD_LOG_FILE="诊断.log")
            self.assertEqual(log.read_bytes(), before)
            self.assertFalse(list(module.glob("*.log")))
            result = self.run_fixture(
                binary, work, DD_LOG_TO_FILE="1", DD_LOG_FILE="missing/diagnostic.log"
            )
            self.assertEqual(result.stdout.strip(), "32 16 1 1")
            self.assertIn("DD_PATCH_LOG_UNAVAILABLE", result.stderr)

    def test_linux_legacy_log_path_and_explicit_switch_precedence(self):
        with tempfile.TemporaryDirectory(prefix="dd-env-legacy-") as directory:
            root = Path(directory)
            binary = self.compile(root)
            legacy = root / "legacy.log"
            result = self.run_fixture(binary, root, DD_PATCH_LOG=str(legacy))
            self.assertEqual(result.stdout.strip(), "32 16 1 1")
            before = legacy.read_bytes()
            result = self.run_fixture(binary, root, DD_PATCH_LOG=str(legacy), DD_LOG_TO_FILE="0")
            self.assertEqual(result.stdout.strip(), "32 16 0 1")
            self.assertEqual(legacy.read_bytes(), before)
            canonical = root / "selected.log"
            self.run_fixture(binary, root, DD_PATCH_LOG=str(legacy), DD_LOG_FILE=str(canonical))
            self.assertFalse(canonical.exists())
            self.run_fixture(
                binary,
                root,
                DD_PATCH_LOG=str(legacy),
                DD_LOG_FILE=str(canonical),
                DD_LOG_TO_FILE="1",
            )
            self.assertIn("OWN_CONFIG_LOG_MARKER", canonical.read_text())
            self.assertEqual(legacy.read_bytes(), before)

    def test_linux_invalid_environment_retains_defaults(self):
        with tempfile.TemporaryDirectory(prefix="dd-env-invalid-") as directory:
            root = Path(directory)
            binary = self.compile(root)
            for value in ("0", "5", "2048", "32junk", "32.5", "999999999999999999999999999"):
                result = self.run_fixture(binary, root, DD_RESOURCE_BUFFER_MIB=value)
                self.assertEqual(result.stdout.strip(), "32 16 0 1")
                self.assertIn("DD_PATCH_CONFIG_INVALID fields=1", result.stderr)
            self.assertEqual(
                self.run_fixture(binary, root, DD_RESOURCE_BUFFER_MIB="6").stdout.strip(),
                "6 16 0 1",
            )
            result = self.run_fixture(
                binary,
                root,
                DD_CACHE_INITIAL_CAPACITY="0",
                DD_HIDE_VANILLA_SKINS="2",
                DD_LOG_TO_FILE="true",
                DD_LOG_FILE="",
            )
            self.assertEqual(result.stdout.strip(), "32 16 0 1")
            self.assertIn("DD_PATCH_CONFIG_INVALID fields=4", result.stderr)
            for overrides in (
                {"DD_CACHE_INITIAL_CAPACITY": "18446744073709551615"},
                {"DD_LOG_FILE": "x" * 5000},
            ):
                result = self.run_fixture(binary, root, **overrides)
                self.assertEqual(result.stdout.strip(), "32 16 0 1")
                self.assertIn("DD_PATCH_CONFIG_INVALID fields=1", result.stderr)
            for value in ("0", "1", "2", "-1", "true"):
                result = self.run_fixture(binary, root, DD_HIDE_VANILLA_SKINS=value)
                self.assertEqual(result.stdout.strip(), "32 16 0 " + ("0" if value == "0" else "1"))

    def test_windows_module_ini_utf8_logs_and_explicit_path(self):
        self.require_windows()
        with tempfile.TemporaryDirectory(prefix="dd-win-config-") as directory:
            root = Path(directory)
            module, work = root / "补丁目录", root / "cwd"
            module.mkdir()
            work.mkdir()
            (work / "dd_engine_patch.ini").write_text("[memory]\nresource_buffer_mib=99\n")
            binary = self.compile(module, True)
            self.assertEqual(self.run_fixture(binary, work, True).stdout.strip(), "32 16 0 1")
            config = module / "dd_engine_patch.ini"
            config.write_bytes((CONFIG_SCRIPTS / config.name).read_bytes())
            self.assertEqual(self.run_fixture(binary, work, True).stdout.strip(), "32 16 0 1")
            self.assertFalse(list(module.glob("*.log")))
            config.write_text(
                "\ufeff[memory]\r\nresource_buffer_mib=64\r\ncache_initial_capacity=3\r\n[logging]\r\nlog_to_file=1\r\nlog_file=诊断.log\r\n"
            )
            for _ in range(2):
                result = self.run_fixture(
                    binary,
                    work,
                    True,
                    DD_RESOURCE_BUFFER_MIB="128",
                    DD_CACHE_INITIAL_CAPACITY="99",
                    DD_HIDE_VANILLA_SKINS="0",
                    DD_LOG_TO_FILE="0",
                )
                self.assertEqual(result.stdout.strip(), "64 3 1 1")
            log = module / "诊断.log"
            self.assertEqual(log.read_text().count("OWN_CONFIG_LOG_MARKER"), 2)
            before = log.read_bytes()
            config.write_text("[logging]\nlog_to_file=0\nlog_file=诊断.log\n")
            self.assertEqual(self.run_fixture(binary, work, True).stdout.strip(), "32 16 0 1")
            self.assertEqual(log.read_bytes(), before)
            override = module / "指定配置"
            override.mkdir()
            custom = override / "settings.ini"
            custom.write_text(
                "[memory]\nresource_buffer_mib=48\n[skins]\nhide_vanilla_skins=0\n[logging]\nlog_to_file=1\nlog_file=指定日志.log\n"
            )
            result = self.run_fixture(binary, work, True, DD_PATCH_CONFIG=str(custom))
            self.assertEqual(result.stdout.strip(), "48 16 1 0")
            self.assertIn("OWN_CONFIG_LOG_MARKER", (override / "指定日志.log").read_text())
            custom.write_text("[logging]\nlog_to_file=1\nlog_file=missing/diagnostic.log\n")
            self.assertEqual(
                self.run_fixture(binary, work, True, DD_PATCH_CONFIG=str(custom)).stdout.strip(),
                "32 16 1 1",
            )

    def test_windows_invalid_ini_retains_defaults_and_valid_duplicates(self):
        self.require_windows()
        with tempfile.TemporaryDirectory(prefix="dd-win-invalid-") as directory:
            root = Path(directory)
            binary = self.compile(root, True)
            config = root / "dd_engine_patch.ini"
            config.write_text(
                "[memory]\nresource_buffer_mib=64\nresource_buffer_mib=-1\ncache_initial_capacity=0\ncache_initial_capacity=184467440737095516160\n[logging]\nlog_to_file=2\nlog_file=\n"
            )
            self.assertEqual(self.run_fixture(binary, root, True).stdout.strip(), "64 16 0 1")
            for value in ("0", "6", "2048", "32junk", "32.5"):
                config.write_text("[memory]\nresource_buffer_mib=" + value + "\n")
                self.assertEqual(self.run_fixture(binary, root, True).stdout.strip(), "32 16 0 1")
            config.write_text("[memory]\n" + "x" * 9000 + "\nresource_buffer_mib=48\n")
            self.assertEqual(self.run_fixture(binary, root, True).stdout.strip(), "48 16 0 1")

    def test_windows_visibility_default_and_explicit_switch(self):
        self.require_windows()
        with tempfile.TemporaryDirectory(prefix="dd-win-skins-") as directory:
            root = Path(directory)
            binary = self.compile(root, True)
            config = root / "dd_engine_patch.ini"
            self.assertEqual(self.run_fixture(binary, root, True).stdout.strip(), "32 16 0 1")
            config.write_text("[skins]\n")
            self.assertEqual(self.run_fixture(binary, root, True).stdout.strip(), "32 16 0 1")
            for value in ("0", "1", "2", "-1", "true"):
                config.write_text("[skins]\nhide_vanilla_skins=" + value + "\n")
                result = self.run_fixture(binary, root, True)
                self.assertEqual(result.stdout.strip(), "32 16 0 " + ("0" if value == "0" else "1"))
