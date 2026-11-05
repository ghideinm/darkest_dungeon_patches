"""Compatibility, setup and injection checks using only generated programs/data."""

import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from darkest_dungeon_patches import windows_port
from darkest_dungeon_patches.paths import COMMON_NATIVE, WINDOWS_NATIVE, WINDOWS_SCRIPTS


class WindowsSteamSetupTest(unittest.TestCase):
    def run_powershell(self, script, root):
        runner = os.environ.get("DD_WINDOWS_POWERSHELL")
        if not runner:
            self.skipTest("Set DD_WINDOWS_POWERSHELL for Steam setup regressions")
        path = root / "test.ps1"
        path.write_text(script)
        result = subprocess.run(
            [runner, "-NoLogo", "-NoProfile", "-File", str(path)],
            capture_output=True,
            text=True,
            timeout=40,
            env=dict(
                os.environ,
                XDG_CACHE_HOME=str(root / ".cache"),
                XDG_CONFIG_HOME=str(root / ".config"),
                XDG_DATA_HOME=str(root / ".data"),
            ),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        import json

        return json.loads(result.stdout)

    def test_lossless_vdf_roundtrip_and_wrapper_arguments(self):
        import json

        with tempfile.TemporaryDirectory(prefix="dd-steam-ps-") as directory:
            root = Path(directory)
            source = WINDOWS_SCRIPTS / "windows_steam.ps1"
            text = (
                '// keep comments\n"UserLocalConfigStore" { "Software" { "Valve" { '
                '"Steam" { "apps" { "999" { "LaunchOptions" "keep" } '
                '"262060" { "Playtime" "42" "LaunchOptions" "-old" } } } } } }'
            )
            launcher = "C:\\用户 游戏\\补丁\\dd_windows_launcher.exe"
            value = '"' + launcher + '" --steam %command%'
            fixture = root / "input.json"
            fixture.write_text(
                json.dumps(
                    {"text": text, "launcher": launcher, "options": value}, ensure_ascii=False
                )
            )
            script = (
                ". '" + str(source) + "' -InstallRoot '" + str(root) + "'\n"
                "$x=Get-Content -Raw '"
                + str(fixture)
                + "'|ConvertFrom-Json\n"
                + r"""
$after=Set-SteamLaunchOptions $x.text $x.options
$missing=$x.text.Replace('"LaunchOptions" "-old"','')
$inserted=Set-SteamLaunchOptions $missing $x.options
$withoutApp=$missing.Replace('"262060" { "Playtime" "42"  }','')
$newApp=Set-SteamLaunchOptions $withoutApp $x.options
$failures=0
foreach($bad in @('"broken" {',$x.text.Replace('"LaunchOptions" "-old"','"LaunchOptions" "x" "LaunchOptions" "y"'))){
 try{Get-SteamLaunchOptions $bad|Out-Null}catch{$failures++}
}
try{Get-WrappedLaunchOptions $x.launcher '%command% %command%'|Out-Null}catch{$failures++}
[ordered]@{after=$after;value=(Get-SteamLaunchOptions $after);inserted=(Get-SteamLaunchOptions $inserted);new_app=(Get-SteamLaunchOptions $newApp);wrapper=(Get-WrappedLaunchOptions $x.launcher '-old');nested=(Get-WrappedLaunchOptions $x.launcher 'outer %command% -old');empty=(Get-WrappedLaunchOptions $x.launcher $null);failures=$failures}|ConvertTo-Json
"""
            )
            result = self.run_powershell(script, root)
            self.assertEqual(
                result["after"],
                text.replace(
                    '"LaunchOptions" "-old"',
                    '"LaunchOptions" ' + json.dumps(value, ensure_ascii=False),
                ),
            )
            for key in ("value", "inserted", "new_app", "empty"):
                self.assertEqual(result[key], value)
            self.assertEqual(result["wrapper"], value + " -old")
            self.assertEqual(result["nested"], "outer " + value + " -old")
            self.assertEqual(result["failures"], 3)

    def test_install_update_rollback_conflict_and_uninstall(self):
        import json

        with tempfile.TemporaryDirectory(prefix="dd-steam-setup with spaces-") as directory:
            root = Path(directory)
            package, client, installed = root / "package", root / "steam", root / "installed"
            package.mkdir()
            setup = package / "windows_steam.ps1"
            setup.write_bytes((WINDOWS_SCRIPTS / "windows_steam.ps1").read_bytes())
            native = package / "dd_windows_launcher.exe"
            native.write_text("#!/bin/sh\nexit 0\n")
            native.chmod(0o700)
            (package / "dd_skin_rig_patch.dll").write_bytes(b"fixture library")
            config = client / "userdata/123/config/localconfig.vdf"
            config.parent.mkdir(parents=True)
            original = (
                '"UserLocalConfigStore" { "Software" { "Valve" { "Steam" { '
                '"apps" { "262060" { "LaunchOptions" "-old" } } } } } }'
            )
            config.write_bytes(b"\xef\xbb\xbf" + original.encode())
            game_root = root / "game"
            game = game_root / "_windows/win64/Darkest.exe"
            game.parent.mkdir(parents=True)
            header = bytearray(512)
            header[:2] = b"MZ"
            struct.pack_into("<I", header, 0x3C, 128)
            header[128:132] = b"PE\0\0"
            struct.pack_into("<H", header, 132, 0x8664)
            struct.pack_into("<H", header, 152, 0x20B)
            game.write_bytes(header)
            paths = root / "paths.json"
            paths.write_text(
                json.dumps(
                    dict(
                        setup=str(setup),
                        steam=str(client),
                        installed=str(installed),
                        game=str(game_root),
                        binary=str(game),
                        config=str(config),
                        package=str(package),
                    )
                )
            )
            script = (
                "$x=Get-Content -Raw '"
                + str(paths)
                + "'|ConvertFrom-Json\n"
                + r"""
. $x.setup -SteamRoot $x.steam -SteamUser 123 -GameRoot $x.game -InstallRoot $x.installed
function Get-Process {param($Name,$ErrorAction) if($script:busy){[pscustomobject]@{Id=42}}}
$script:busy=$true;$blocked=$false
try{Invoke-SteamSetup|Out-Null}catch{$blocked=-not(Test-Path -LiteralPath $x.installed)}
$script:busy=$false
$first=Invoke-SteamSetup
$firstBytes=[IO.File]::ReadAllBytes($x.config)
$statePath=Join-Path $x.installed 'steam_install.json'
$stateBytes=[IO.File]::ReadAllBytes($statePath)
$again=Invoke-SteamSetup
$same=$first.launch_options -ceq $again.launch_options
$saved=[IO.File]::ReadAllBytes((Join-Path $x.installed 'dd_windows_launcher.exe'))
# Simulate a config-write failure after updating binaries and state.
$realWrite=(Get-Item Function:Write-SteamConfig).ScriptBlock
function Write-SteamConfig {throw 'fixture disk failure'}
$source=Join-Path $x.package 'dd_windows_launcher.exe'
[IO.File]::WriteAllText($source,"#!/bin/sh`n# update`nexit 0`n")
$failed=$false
try{Invoke-SteamSetup|Out-Null}catch{$failed=$true}
$rolledBack=([Convert]::ToBase64String($saved) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $x.installed 'dd_windows_launcher.exe')))) -and ([Convert]::ToBase64String($stateBytes) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($statePath))) -and ([Convert]::ToBase64String($firstBytes) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($x.config)))
Set-Item Function:Write-SteamConfig $realWrite
$snapshot=Read-SteamConfig $x.config
[IO.File]::AppendAllText($x.config,"`n// external edit")
$race=$false
try{Write-SteamConfig $x.config $snapshot 'changed'}catch{$race=[IO.File]::ReadAllText($x.config).EndsWith('// external edit')}
[IO.File]::WriteAllBytes($x.config,$firstBytes)
$modified=Set-SteamLaunchOptions ([IO.File]::ReadAllText($x.config)) 'manual'
[IO.File]::WriteAllText($x.config,$modified)
$Action='Uninstall';$conflict=$false
try{Invoke-SteamSetup|Out-Null}catch{$conflict=(Get-SteamLaunchOptions ([IO.File]::ReadAllText($x.config))) -eq 'manual'}
[IO.File]::WriteAllBytes($x.config,$firstBytes)
$removed=Invoke-SteamSetup
[ordered]@{blocked=$blocked;same=$same;failed=$failed;rollback=$rolledBack;race=$race;conflict=$conflict;restored=(Get-SteamLaunchOptions ([IO.File]::ReadAllText($x.config)));bom=([IO.File]::ReadAllBytes($x.config)[0] -eq 239);state_removed=(-not(Test-Path -LiteralPath $statePath));enabled=$removed.enabled}|ConvertTo-Json
"""
            )
            result = self.run_powershell(script, root)
            for key in (
                "blocked",
                "same",
                "failed",
                "rollback",
                "race",
                "conflict",
                "bom",
                "state_removed",
            ):
                self.assertTrue(result[key], key)
            self.assertEqual(result["restored"], "-old")
            self.assertFalse(result["enabled"])


class WindowsProfileTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dd-pe-profile-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.executable = Path(self.temporary.name) / "fixture.exe"
        self.tables = windows_port.profile()
        self.image_size = 0x1000000
        rva = 0x200000
        end = max(at + len(expected) for rows in self.tables.values() for at, expected, _ in rows)
        size = (end - rva + 0xFFF) & ~0xFFF
        self.data = bytearray(512 + size)
        self.data[:2] = b"MZ"
        pe = 0x80
        struct.pack_into("<I", self.data, 0x3C, pe)
        self.data[pe : pe + 4] = b"PE\0\0"
        struct.pack_into("<HH", self.data, pe + 4, 0x8664, 1)
        struct.pack_into("<H", self.data, pe + 20, 240)
        struct.pack_into("<H", self.data, pe + 24, 0x20B)
        struct.pack_into("<I", self.data, pe + 80, self.image_size)
        struct.pack_into(
            "<8sIIIIIIHHI",
            self.data,
            pe + 264,
            b".fixture",
            size,
            rva,
            size,
            512,
            0,
            0,
            0,
            0,
            0x60000020,
        )
        for rows in self.tables.values():
            for at, expected, _ in rows:
                offset = 512 + at - rva
                self.data[offset : offset + len(expected)] = expected
        self.executable.write_bytes(self.data)

    def validate(self):
        return windows_port.validate(self.executable)

    def test_accepts_complete_instruction_fixture(self):
        result = self.validate()
        self.assertTrue(result["compatible"])
        self.assertEqual(result["resource_patch_sites"], 3)
        self.assertEqual(result["rig_hook_sites"], 4)
        self.assertFalse(result["disk_binary_modified"])
        self.assertFalse(result["runtime_verified"])

    def test_accepts_changes_outside_patch_sites_and_image_size(self):
        self.data[-1] ^= 1
        self.executable.write_bytes(self.data)
        struct.pack_into("<I", self.data, 0x80 + 80, self.image_size + 4096)
        self.executable.write_bytes(self.data)
        self.assertTrue(self.validate()["compatible"])

    def test_rejects_wrong_architecture(self):
        struct.pack_into("<H", self.data, 0x84, 0x14C)
        self.executable.write_bytes(self.data)
        with self.assertRaisesRegex(ValueError, "architecture"):
            self.validate()

    def test_reports_mismatched_instruction_without_blocking(self):
        at, _, _ = self.tables["resource"][0]
        self.data[512 + at - 0x200000] ^= 1
        self.executable.write_bytes(self.data)
        result = self.validate()
        self.assertFalse(result["compatible"])
        self.assertIn("instruction", result["compatibility_issues"][0])


class WindowsBootstrapTest(unittest.TestCase):
    def test_windows_bootstrap_on_synthetic_process(self):
        """Exercise entry breakpoint, DLL readiness, writes and detach in Wine.

        The child is our own program with sparse signature fixtures. It
        contains no complete game function and executes only its own code.
        Explicit tool paths opt in; ordinary regressions need neither Wine nor
        a Windows compiler.
        """
        compiler = os.environ.get("DD_WINDOWS_CC")
        runner = os.environ.get("DD_WINDOWS_RUNNER")
        if not compiler or not runner:
            self.skipTest("Set DD_WINDOWS_CC and DD_WINDOWS_RUNNER for PE bootstrap testing")
        with tempfile.TemporaryDirectory(prefix="dd-windows-bootstrap-") as root:
            root = Path(root)
            child = root / "child.c"
            child.write_text("""#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
static DWORD WINAPI worker(void *argument) {
  (void)argument; return 0;
}
int wmain(void) {
  if (GetEnvironmentVariableW(L"DD_FIXTURE_EXPECT_ORIGINAL", NULL, 0)) {
    const unsigned char *image = (const void *)GetModuleHandleW(NULL);
    unsigned cap;
    memcpy(&cap, image + 0x37ffc1, sizeof(cap));
    if (cap != 0x1000000) return 5;
  }
  HANDLE thread = CreateThread(NULL, 65536, worker, NULL,
                               STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (!thread) return 2;
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
  HANDLE marker = CreateFileW(L"worker_completed", GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (marker == INVALID_HANDLE_VALUE) return 3;
  DWORD written;
  int ok = WriteFile(marker, "ok", 2, &written, NULL) && written == 2;
  CloseHandle(marker);
  if (!ok) return 4;
  return 0;
}
""")
            common = [
                compiler,
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(WINDOWS_NATIVE),
            ]
            executable = root / "fixture.exe"
            subprocess.run([*common, "-municode", str(child), "-o", str(executable)], check=True)
            data = bytearray(executable.read_bytes())
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            count = struct.unpack_from("<H", data, pe + 6)[0]
            optional_size = struct.unpack_from("<H", data, pe + 20)[0]
            alignment = struct.unpack_from("<I", data, pe + 60)[0]
            header_size = struct.unpack_from("<I", data, pe + 84)[0]
            section = pe + 24 + optional_size + count * 40
            self.assertLessEqual(section + 40, header_size)
            raw = (len(data) + alignment - 1) // alignment * alignment
            data.extend(b"\0" * (raw - len(data)))
            rva, size = 0x200000, 0xA00000
            payload = bytearray(size)
            tables = windows_port.profile()
            for sites in tables.values():
                for at, expected, _ in sites:
                    payload[at - rva : at - rva + len(expected)] = expected
            data.extend(payload)
            struct.pack_into(
                "<8sIIIIIIHHI",
                data,
                section,
                b".ddtest\0",
                size,
                rva,
                size,
                raw,
                0,
                0,
                0,
                0,
                0x60000020,
            )
            struct.pack_into("<H", data, pe + 6, count + 1)
            struct.pack_into("<I", data, pe + 80, rva + size)
            flags = struct.unpack_from("<H", data, pe + 94)[0]
            struct.pack_into("<H", data, pe + 94, flags & ~0x40)
            executable.write_bytes(data)
            project = WINDOWS_NATIVE
            helper = root / "bootstrap.c"
            helper.write_text(
                '''#define wmain production_wmain
#include "'''
                + str(project / "windows_launcher.c")
                + """"
#undef wmain
int wmain(int argc, wchar_t **argv) {
  if (argc != 3) return 2;
  wchar_t game[32768], library[32768], directory[32768], command[32768]={0};
  if (!GetFullPathNameW(argv[1],32768,game,NULL) ||
      !GetFullPathNameW(argv[2],32768,library,NULL) ||
      !GetCurrentDirectoryW(32768,directory)) return 3;
  if (!append_argument(command,32768,game)) return 4;
  return launch_game(game,command,directory,library,1,1) ? 0 : 125;
}
"""
            )
            launcher = root / "bootstrap.exe"
            library = root / "dd_skin_rig_patch.dll"
            subprocess.run([*common, "-municode", str(helper), "-o", str(launcher)], check=True)
            subprocess.run(
                [
                    *common,
                    "-shared",
                    str(COMMON_NATIVE / "skin_rig_patch.c"),
                    "-lm",
                    "-o",
                    str(library),
                ],
                check=True,
            )
            environment = dict(
                os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d"
            )
            result = subprocess.run(
                [runner, str(launcher), "fixture.exe", "dd_skin_rig_patch.dll"],
                cwd=root,
                env=environment,
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("DD_WINDOWS_RESOURCE_APPLIED", result.stderr)
            self.assertIn("DD_WINDOWS_RENDER_APPLIED", result.stderr)
            self.assertIn("DD_WINDOWS_LAUNCHED", result.stderr)
            import time

            marker = root / "worker_completed"
            deadline = time.monotonic() + 5
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertEqual(marker.read_bytes(), b"ok")

            # Exercise the production entry point with an arbitrary generated
            # EXE, then changed instructions and DLL failure after writes.
            production = root / "dd_windows_launcher.exe"
            subprocess.run(
                [*common, "-municode", str(project / "windows_launcher.c"), "-o", str(production)],
                check=True,
            )
            for failure in ("matching", "instruction", "library"):
                with self.subTest(failure=failure):
                    marker.unlink()
                    candidate = bytearray(data)
                    if failure == "instruction":
                        candidate[raw + tables["resource"][0][0] - rva] ^= 1
                    elif failure == "library":
                        failed = root / "failed_library.c"
                        failed.write_text("""#include <windows.h>
__declspec(dllexport) volatile LONG dd_windows_ready;
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module; (void)reserved;
  return reason != DLL_PROCESS_ATTACH;
}
""")
                        subprocess.run(
                            [*common, "-shared", str(failed), "-o", str(library)], check=True
                        )
                    executable.write_bytes(candidate)
                    result = subprocess.run(
                        [runner, str(production), "--game", str(executable)],
                        cwd=root,
                        env=(
                            environment
                            if failure == "matching"
                            else dict(environment, DD_FIXTURE_EXPECT_ORIGINAL="1")
                        ),
                        capture_output=True,
                        text=True,
                        timeout=60,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    if failure == "matching":
                        self.assertIn("DD_WINDOWS_LAUNCHED", result.stderr)
                        self.assertNotIn("DD_WINDOWS_PATCH_SKIPPED", result.stderr)
                    else:
                        self.assertIn("DD_WINDOWS_PATCH_SKIPPED", result.stderr)
                        self.assertIn("DD_WINDOWS_UNMODIFIED_LAUNCHED", result.stderr)
                    if failure == "library":
                        self.assertIn("DD_WINDOWS_RESOURCE_APPLIED", result.stderr)
                        self.assertIn("DD_WINDOWS_DLL_REFUSED", result.stderr)
                    deadline = time.monotonic() + 5
                    while not marker.exists() and time.monotonic() < deadline:
                        time.sleep(0.01)
                    self.assertEqual(marker.read_bytes(), b"ok")


if __name__ == "__main__":
    unittest.main()
