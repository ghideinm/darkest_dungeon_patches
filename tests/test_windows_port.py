"""Code-only Windows instruction checks and DLL loading regressions."""

import hashlib
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from address_fixtures import make_fixture, write_pe

from darkest_dungeon_patches import windows_port, windows_proxy
from darkest_dungeon_patches.paths import (
    COMMON_NATIVE,
    WINDOWS_NATIVE,
)


class ProxyPayloadTest(unittest.TestCase):
    def test_local_payload_integrity_and_rejected_tampering(self):
        with tempfile.TemporaryDirectory(prefix="dd-proxy-bytes-") as directory:
            root = Path(directory)
            data = b"own redistributable proxy fixture"
            source = root / "proxy.dll"
            source.write_bytes(data)
            with patch.object(windows_proxy, "PROXY_SHA256", hashlib.sha256(data).hexdigest()):
                target = windows_proxy.acquire(root / "winmm.dll", source)
                self.assertEqual(target.read_bytes(), data)
                source.write_bytes(data + b"changed")
                with self.assertRaisesRegex(ValueError, "checksum"):
                    windows_proxy.acquire(root / "bad.dll", source)
            self.assertFalse((root / "bad.dll").exists())

    def test_download_rejects_untrusted_archive_before_extraction(self):
        import io

        with tempfile.TemporaryDirectory(prefix="dd-proxy-download-") as directory:
            with patch.dict(os.environ, {}, clear=True):
                with patch("urllib.request.urlopen", return_value=io.BytesIO(b"own wrong archive")):
                    with self.assertRaisesRegex(ValueError, "archive checksum"):
                        windows_proxy.acquire(Path(directory) / "winmm.dll")


class WindowsProfileTest(unittest.TestCase):
    def test_offline_check_accepts_moved_code_and_data(self):
        with tempfile.TemporaryDirectory(prefix="dd-pe-profile-unit-") as directory:
            root = Path(directory)
            for code, data in ((0x200000, 0x300000), (0x650000, 0x840000)):
                fixture = make_fixture(root, "windows", code, data)
                path = root / "fixture.exe"
                write_pe(path, fixture)
                with patch.object(windows_port, "signatures", return_value=fixture.features):
                    result = windows_port.validate(path)
                    self.assertTrue(result["compatible"], result["compatibility_issues"])
                    self.assertEqual(result["resolved_symbols"], fixture.symbols)
                    fixture.code[fixture.symbols["RESOURCE_0"] - code] ^= 1
                    write_pe(path, fixture)
                    result = windows_port.validate(path)
                    self.assertFalse(result["compatible"])
                raw = bytearray(path.read_bytes())
                struct.pack_into("<H", raw, 132, 0x14C)
                path.write_bytes(raw)
                with self.assertRaisesRegex(ValueError, "architecture"):
                    windows_port.validate(path)


class WindowsDllRuntimeTest(unittest.TestCase):
    def test_actor_failure_falls_back_and_native_vectors_exceed_old_limits(self):
        compiler, runner = (os.getenv(n) for n in ("DD_WINDOWS_CC", "DD_WINDOWS_RUNNER"))
        if not compiler or not runner:
            self.skipTest("Set DD_WINDOWS_CC and DD_WINDOWS_RUNNER for native fixtures")
        with tempfile.TemporaryDirectory(prefix="dd-windows-actor-") as directory:
            root = Path(directory)
            source = root / "actor.c"
            source.write_text(
                '#include "'
                + (COMMON_NATIVE / "skin_rig_patch.c").as_posix()
                + '"\n#include <assert.h>\n'
                + r"""
static unsigned char klass[0x700];
static const Handles *expected_original;
static unsigned released, fallback_calls;
static void *get_class(void *actor) {(void)actor;return klass;}
static void *fallback(const Handles *handles,int flag) {
  if(!flag) return NULL; /* Simulate a failed routed sprite constructor. */
  assert(handles == expected_original && flag == 7);
  ++fallback_calls;return (void*)0x1234;
}
static void load(Handle *out,const char *key,int defaults,const void *callback) {
  (void)key;(void)defaults;(void)callback;*out=(Handle){0,1,NULL};
}
static void release_handle(Handle *handle) {assert(handle->index==0);++released;}
static const char *registered_name(void *manager,const Handle *handle) {
  (void)manager;(void)handle;return ">heroes/alpha/anim/alpha.skel";
}
static int loaded;
static void *get(void *manager,const Handle *handle) {
  (void)manager;(void)handle;return loaded ? (void*)1 : NULL;
}
int main(void) {
  void *table[8]={0};table[6]=(void*)get_class;
  unsigned char actor[0xc00]={0};
  void *table_address=table;
  memcpy(actor,&table_address,sizeof(table_address));
  char (*palettes)[512]=calloc(600,sizeof(*palettes));assert(palettes);
  void *end=palettes+600;
  memcpy(klass+0x5e0,&palettes,sizeof(palettes));
  memcpy(klass+0x5e8,&end,sizeof(end));
  assert(windows_colour_count(klass)==600);
  SkinSource choices[4]={0};
  strcpy(choices[1].origin,"C:/GAME/DLC/official/");
  strcpy(choices[2].origin,"C:/Game/mods/dlc_theme/");
  strcpy(choices[3].origin,"mods/other/");
  SkinCatalog catalog={.count=4,.entries=choices};
  assert(palette_official_source(choices[1].origin,"c:/game"));
  assert(!palette_official_source(choices[2].origin,"c:/game"));
  assert(visible_catalog_colour(&catalog,0,3,"c:/game")==2);
  assert(visible_catalog_colour(&catalog,1,2,"c:/game")==3);
  assert(visible_catalog_colour(&catalog,2,3,"c:/game")==2);
  unsigned char device[0x20]={0};
  void *roots=calloc(4200,0x210);assert(roots);end=(char*)roots+4200*0x210;
  memcpy(device+8,&roots,sizeof(roots));memcpy(device+16,&end,sizeof(end));
  assert(windows_alternate_count(device)==4200);
  end=(char*)roots+1;memcpy(device+16,&end,sizeof(end));
  assert(windows_alternate_count(device)==UINT_MAX);
  free(roots);
  end=palettes+1;memcpy(klass+0x5e8,&end,sizeof(end));
  strcpy(palettes[0],"heroes/alpha/alpha_A/");
  colour_count=windows_colour_count;colour_path=windows_colour_path;
  native_new=fallback;
  Handle handle={42,9,NULL};Handles original={&handle,&handle+1,&handle+1};
  expected_original=&original;
  assert(new_actor_sprite(&original,7,actor)==(void*)0x1234);
  assert(fallback_calls==1);
  RigPlan *plan=calloc(1,sizeof(*plan));assert(plan);
  assert(add_rig_key(plan,">heroes/alpha/anim/alpha.skel"));
  Route route={.plan=plan};
  strcpy(route.skin,"heroes/alpha/alpha_A");
  strcpy(route.hero,"heroes/alpha");
  assert(append_route(&route));
  void *manager=(void*)1;base=(uintptr_t)&manager;
  native_symbols.values[DD_RESOURCE]=0;
  load_spine=load;release_spine=release_handle;
  resource_name=registered_name;get_resource=get;
  assert(new_actor_sprite(&original,7,actor)==(void*)0x1234);
  assert(fallback_calls==2 && released==1);
  loaded=1;
  assert(new_actor_sprite(&original,7,actor)==(void*)0x1234);
  assert(fallback_calls==3 && released==2);
  clear_routes();free(palettes);
  puts("OWN_ACTOR_FALLBACK_OK");return 0;
}
"""
            )
            binary = root / "actor.exe"
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
                    str(source),
                    "-lm",
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            result = subprocess.run(
                [runner, str(binary)],
                cwd=root,
                env=dict(os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d"),
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("OWN_ACTOR_FALLBACK_OK", result.stdout)

    def test_dll_and_winmm_loading_and_unmodified_fallback_on_own_executable(self):
        compiler, runner, proxy = (
            os.getenv(n) for n in ("DD_WINDOWS_CC", "DD_WINDOWS_RUNNER", "DD_WINDOWS_PROXY")
        )
        if not compiler or not runner:
            self.skipTest("Set DD_WINDOWS_CC and DD_WINDOWS_RUNNER for native fixtures")
        with tempfile.TemporaryDirectory(prefix="dd-native-dll-") as directory:
            root = Path(directory)
            fixture = make_fixture(root, "windows", 0x650000, 0x840000)
            # A wildcard allows the instruction guard to be tested independently of scanning.
            text = fixture.header.read_text()
            text = text.replace('"resource_0", "b9 00 00 00 01 ', '"resource_0", "b9 ?? ?? ?? ?? ')
            fixture.header.write_text(text)
            common = [
                compiler,
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(WINDOWS_NATIVE),
                '-DDD_ADDRESS_TEST_PROFILE="' + str(fixture.header) + '"',
            ]
            child = root / "child.c"
            child.write_text(r"""#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static HMODULE loaded_plugin;
static DWORD WINAPI load_plugin(void *path) {
 loaded_plugin=LoadLibraryW(path);return loaded_plugin ? 0 : 1;
}
static DWORD WINAPI worker(void *unused) {(void)unused;return timeGetTime() ? 0 : 1;}
int wmain(void) {
 wchar_t path[4096];
 HMODULE plugin;
 if(GetEnvironmentVariableW(L"DD_FIXTURE_DLL_PATH",path,4096)) {
   HANDLE loader=CreateThread(NULL,0,load_plugin,path,0,NULL);
   if(!loader)return 19;
   WaitForSingleObject(loader,INFINITE);CloseHandle(loader);
   plugin=loaded_plugin;
 } else plugin=GetModuleHandleW(L"dd_engine_patch.dll");
 if(!plugin)return 10;
 volatile LONG *ready=(void*)GetProcAddress(plugin,"dd_windows_ready");
 const unsigned char *image=(const void*)GetModuleHandleW(NULL);
 int original=GetEnvironmentVariableW(L"DD_EXPECT_ORIGINAL",NULL,0)!=0;
 if(!ready || *ready!=(original?0:1))return 11;
 uint32_t expected=original?DD_EXPECT_CAP:
   GetEnvironmentVariableW(L"DD_EXPECT_CONFIGURED",NULL,0)?0x4000000:0x2000000;
 const unsigned sites[]={DD_RESOURCE_RVA,DD_RESOURCE_RVA_1,DD_RESOURCE_RVA_2};
 for(unsigned i=0;i<3;++i){uint32_t cap;memcpy(&cap,image+sites[i],4);if(cap!=expected)return 12;}
 if(image[DD_COLOUR_RVA]!=(original?0x48:0xe9))return 13;
 if(image[DD_RENDER_RVA]!=(original?0xbf:0x90))return 14;
 if(image[DD_PALETTE_RVA]!=(original?0x48:0xe9))return 18;
 MEMORY_BASIC_INFORMATION info;
 if(!VirtualQuery(image+DD_RESOURCE_RVA,&info,sizeof(info)) || info.Protect!=PAGE_EXECUTE_READ)return 15;
 void (*init)(void)=(void*)GetProcAddress(plugin,"InitializeASI");init();
 if(*ready!=(original?0:1))return 16;
 HANDLE thread=CreateThread(NULL,65536,worker,NULL,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
 if(!thread)return 17;
 WaitForSingleObject(thread,INFINITE);CloseHandle(thread);
 puts("OWN_DLL_FIXTURE_OK");return 0;
}
""")
            definitions = [
                f"-DDD_RESOURCE_RVA={fixture.symbols['RESOURCE_0'] + 1}",
                f"-DDD_RESOURCE_RVA_1={fixture.symbols['RESOURCE_1'] + 7}",
                f"-DDD_RESOURCE_RVA_2={fixture.symbols['RESOURCE_2'] + 4}",
                f"-DDD_COLOUR_RVA={fixture.symbols['HOOK_COLOUR']}",
                f"-DDD_RENDER_RVA={fixture.symbols['RENDER_0']}",
                f"-DDD_PALETTE_RVA={fixture.symbols['FULL_PATH']}",
            ]
            binary = root / "fixture.exe"
            subprocess.run(
                [
                    *common,
                    *definitions,
                    "-DDD_EXPECT_CAP=0x1000000",
                    "-municode",
                    str(child),
                    "-lwinmm",
                    "-Wl,--file-alignment,4096",
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            data = bytearray(binary.read_bytes())
            pe = struct.unpack_from("<I", data, 60)[0]
            count = struct.unpack_from("<H", data, pe + 6)[0]
            optional = struct.unpack_from("<H", data, pe + 20)[0]
            section = pe + 24 + optional + count * 40
            self.assertLessEqual(section + 80, struct.unpack_from("<I", data, pe + 84)[0])
            raw = (len(data) + 4095) & ~4095
            data.extend(b"\0" * (raw - len(data)))
            for i, (name, rva, payload, flags) in enumerate(
                [
                    (b".ddcode", fixture.code_address, fixture.code, 0x60000020),
                    (b".dddata", fixture.data_address, fixture.data, 0xC0000040),
                ]
            ):
                size = (len(payload) + 4095) & ~4095
                offset = len(data)
                data.extend(payload)
                data.extend(b"\0" * (size - len(payload)))
                struct.pack_into(
                    "<8sIIIIIIHHI",
                    data,
                    section + i * 40,
                    name,
                    size,
                    rva,
                    size,
                    offset,
                    0,
                    0,
                    0,
                    0,
                    flags,
                )
            struct.pack_into("<H", data, pe + 6, count + 2)
            struct.pack_into("<I", data, pe + 80, fixture.data_address + 4096)
            environment = dict(os.environ, WINEDEBUG="-all")
            for key in (
                "DD_PATCH_CONFIG",
                "DD_RIG_MAP",
                "DD_PATCH_LOG",
                "DD_LOADBUFFER_PATCH",
                "DD_NATIVE_RENDER",
                "DD_RIG_RUNTIME",
                "DD_EXPECT_ORIGINAL",
                "DD_EXPECT_CONFIGURED",
                "DD_FIXTURE_DLL_PATH",
            ):
                environment.pop(key, None)
            modes = ["dll"]
            if proxy:
                windows_proxy.acquire(root / "winmm.dll", Path(proxy))
                (root / "winmm.ini").write_bytes(
                    (windows_port.PATCH_ROOT / "scripts/windows/winmm.ini").read_bytes()
                )
                modes.append("winmm")
            for mode in modes:
                module_dir = root / "mods/引擎补丁 with spaces" if mode == "dll" else root
                module_dir.mkdir(parents=True, exist_ok=True)
                library = module_dir / "dd_engine_patch.dll"
                mode_environment = dict(
                    environment,
                    WINEDLLOVERRIDES=("winmm=n,b;" if mode == "winmm" else "")
                    + "winemenubuilder.exe=d",
                )
                if mode == "dll":
                    mode_environment["DD_FIXTURE_DLL_PATH"] = str(library)
                for case in ("matching", "configured", "signature", "guard", "rollback"):
                    with self.subTest(mode=mode, case=case):
                        configuration = module_dir / "dd_engine_patch.ini"
                        log = module_dir / "dd_engine_patch.log"
                        configuration.unlink(missing_ok=True)
                        log.unlink(missing_ok=True)
                        if case == "configured":
                            configuration.write_text(
                                "[memory]\nresource_buffer_mib=64\ncache_initial_capacity=3\n"
                                "[logging]\nlog_to_file=1\n"
                            )
                        candidate = bytearray(data)
                        if case == "signature":
                            candidate[
                                raw + fixture.symbols["RESOURCE_0"] - fixture.code_address + 5
                            ] ^= 1
                        elif case == "guard":
                            # Keep the original value; change another guarded instruction with a wildcard.
                            fixture.header.write_text(
                                text.replace(
                                    '"resource_0", "b9 ?? ?? ?? ?? ',
                                    '"resource_0", "?? ?? ?? ?? ?? ',
                                )
                            )
                            candidate[
                                raw + fixture.symbols["RESOURCE_0"] - fixture.code_address
                            ] = 0xB8
                        source = COMMON_NATIVE / "skin_rig_patch.c"
                        if case == "rollback":
                            fixture.header.write_text(text)
                            source = root / "fault.c"
                            source.write_text(
                                """#include <windows.h>
#include <stdint.h>
#include <string.h>
static BOOL fault_protect(void *address,SIZE_T length,DWORD protect,DWORD *previous) {
 static int failed;
 unsigned char *base=(void*)GetModuleHandleW(NULL);
 uint32_t value;memcpy(&value,base+"""
                                + str(fixture.symbols["RESOURCE_0"] + 1)
                                + ''',4);
 if(!failed && protect==PAGE_EXECUTE_READ && value==0x2000000){failed=1;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
 return VirtualProtect(address,length,protect,previous);
}
#define VirtualProtect fault_protect
#include "'''
                                + str(COMMON_NATIVE / "skin_rig_patch.c")
                                + '"\n'
                            )
                        subprocess.run(
                            [*common, "-shared", str(source), "-lm", "-o", str(library)], check=True
                        )
                        binary.write_bytes(candidate)
                        result = subprocess.run(
                            [runner, str(binary)],
                            cwd=root,
                            env=(
                                dict(mode_environment, DD_EXPECT_CONFIGURED="1")
                                if case == "configured"
                                else mode_environment
                                if case == "matching"
                                else dict(mode_environment, DD_EXPECT_ORIGINAL="1")
                            ),
                            capture_output=True,
                            text=True,
                            timeout=60,
                        )
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn("OWN_DLL_FIXTURE_OK", result.stdout)
                        if case == "configured":
                            self.assertIn(
                                "buffer_mib=64 cache_initial=3 log_to_file=1", log.read_text()
                            )
                            self.assertIn("DD_PATCH_APPLIED", log.read_text())
                        else:
                            self.assertFalse(list(module_dir.glob("*.log")))


if __name__ == "__main__":
    unittest.main()
