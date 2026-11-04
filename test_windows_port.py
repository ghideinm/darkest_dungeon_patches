"""Identity checks and isolated Windows storage instructions, without game entry."""
import ctypes
import hashlib
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

import windows_port


class WindowsProfileTest(unittest.TestCase):
    def setUp(self):
        self.game = Path(os.environ.get(
            'DD_WINDOWS_GAME', '/nonexistent/windows/game.exe'))
        if not self.game.is_file():
            self.skipTest('Set DD_WINDOWS_GAME to the verified Windows x64 EXE')

    def test_verifies_windows_instruction_profile(self):
        result = windows_port.validate(self.game)
        self.assertEqual(result['resource_patch_sites'], 3)
        self.assertEqual(result['rig_hook_sites'], 4)
        self.assertFalse(result['disk_binary_modified'])
        self.assertFalse(result['runtime_verified'])

    def test_rejects_altered_or_linux_program(self):
        with tempfile.TemporaryDirectory(prefix='dd-windows-reject-') as root:
            path = Path(root) / 'Darkest.exe'
            data = bytearray(self.game.read_bytes())
            data[-1] ^= 1
            path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                windows_port.validate(path)
            path.write_bytes(b'\x7fELF')
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                windows_port.validate(path)

    def test_windows_native_storage_provider_and_explicit_path(self):
        """Run only IO_GetFileInfo with MS ABI callbacks on this x86-64 host.

        Executable memory contains the verified 372-byte Windows function. Its
        formatting/import calls and virtual file-size method use controlled
        callbacks; no entrypoint, loader, graphics, storage thread or save runs.
        """
        import mmap
        import platform
        if platform.system() != 'Linux' or platform.machine() != 'x86_64':
            self.skipTest('Isolated MS ABI runner requires Linux x86-64')
        data = self.game.read_bytes()
        identity, _, _ = windows_port.profile()
        self.assertEqual(hashlib.sha256(data).hexdigest(), identity)
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        count = struct.unpack_from('<H', data, pe + 6)[0]
        code = None
        for i in range(count):
            at = pe + 264 + i * 40
            rva, size, offset = struct.unpack_from('<III', data, at + 12)
            if rva <= 0x248740 and 0x2488b4 <= rva + size:
                code = bytearray(data[offset + 0x248740 - rva:
                                      offset + 0x2488b4 - rva])
        self.assertIsNotNone(code)
        source = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MS __attribute__((ms_abi))
typedef void (MS *Query)(void *, void *, const char *, void *);
static const char *existing;
static unsigned calls;
static unsigned MS file_size(void *unused, const char *path) {
  (void)unused; ++calls;
  return !strcmp(path, existing) ? 17 : 0;
}
static void *MS format_path(char *out, const char *format, const char *root,
                           const char *name) {
  (void)format; snprintf(out, 512, "%s%s", root, name); return out;
}
static void *MS archive_find(void *unused, const char *name) {
  (void)unused; (void)name; return NULL;
}
static int MS copy_path(char *out, size_t capacity, const char *source,
                        size_t count) {
  (void)count; snprintf(out, capacity, "%s", source); return 0;
}
uintptr_t helper(unsigned i) {
  const uintptr_t functions[] = {(uintptr_t)format_path,
    (uintptr_t)archive_find, (uintptr_t)copy_path};
  return functions[i];
}
int exercise(uintptr_t address) {
  unsigned char roots[2][0x210] = {{0}};
  strcpy((char *)roots[0], "mods/selected/");
  strcpy((char *)roots[1], "mods/other/");
  uintptr_t table[10] = {0}, device[12] = {0};
  table[4] = (uintptr_t)file_size;
  device[0] = (uintptr_t)table;
  device[1] = (uintptr_t)roots;
  device[2] = (uintptr_t)(roots + 2);
  strcpy((char *)device + 0x20, "data");
  Query query = (Query)address;
  struct {const char *request, *found, *actual; unsigned count;} cases[] = {
    {"heroes/a.png", "mods/selected/heroes/a.png", "mods/selected/heroes/a.png", 1},
    {"heroes/a.png", "mods/other/heroes/a.png", "mods/other/heroes/a.png", 2},
    {">heroes/a.png", "heroes/a.png", "heroes/a.png", 1},
    {">mods/selected/heroes/a.png", "mods/selected/heroes/a.png", "mods/selected/heroes/a.png", 1}
  };
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
    unsigned char info[0x448] = {0};
    existing = cases[i].found; calls = 0;
    query(NULL, device, cases[i].request, info);
    unsigned size; memcpy(&size, info + 0x440, 4);
    if (size != 17 || strcmp((char *)info + 0x200, cases[i].actual) ||
        calls != cases[i].count || strcmp((char *)info + 0x400, "data"))
      return (int)i + 1;
  }
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='dd-windows-native-') as root:
            c = Path(root) / 'callbacks.c'
            library = Path(root) / 'callbacks.so'
            c.write_text(source)
            subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Werror', '-shared', '-fPIC', str(c), '-o',
                            str(library)], check=True)
            callbacks = ctypes.CDLL(str(library))
            callbacks.helper.argtypes = [ctypes.c_uint]
            callbacks.helper.restype = ctypes.c_size_t
            callbacks.exercise.argtypes = [ctypes.c_size_t]
            callbacks.exercise.restype = ctypes.c_int
            mapping = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE)
            address = ctypes.addressof(ctypes.c_char.from_buffer(mapping))
            # Redirect near calls to ABI-preserving absolute jump stubs.
            for slot, (offset, size, helper) in enumerate([
                    (0x92, 5, 0), (0xb2, 5, 1), (0x110, 6, 2),
                    (0x139, 6, 2), (0x16d, 7, 2)]):
                location = 512 + slot * 16
                stub = b'\x48\xb8' + struct.pack('<Q', callbacks.helper(helper)) + b'\xff\xe0'
                mapping[location:location + len(stub)] = stub
                # Last instruction is a tail jump; preserve that distinction.
                opcode = b'\xe9' if slot == 4 else b'\xe8'
                code[offset:offset + size] = opcode + struct.pack(
                    '<i', location - offset - 5) + b'\x90' * (size - 5)
            # RIP-relative format literal used by the redirected formatter.
            struct.pack_into('<i', code, 0x84, 1024 - (0x81 + 7))
            mapping[1024:1030] = b'%s%s\0\0'
            mapping[:len(code)] = code
            libc = ctypes.CDLL(None)
            libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
            self.assertEqual(libc.mprotect(address, 4096, mmap.PROT_READ | mmap.PROT_EXEC), 0)
            # A subprocess contains any fault in the isolated foreign code.
            child = os.fork()
            if child == 0:
                import resource
                resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
                os._exit(callbacks.exercise(address))
            _, status = os.waitpid(child, 0)
            mapping.close()
            self.assertTrue(os.WIFEXITED(status), f'Windows resolver fault: {status}')
            self.assertEqual(os.WEXITSTATUS(status), 0)

    def test_windows_bootstrap_on_synthetic_process(self):
        """Exercise entry breakpoint, DLL readiness, writes and detach in Wine.

        The child is our own empty program with sparse signature fixtures. It
        contains no complete game function and never executes game entry code.
        Explicit tool paths opt in; ordinary regressions need neither Wine nor
        a Windows compiler.
        """
        compiler = os.environ.get('DD_WINDOWS_CC')
        runner = os.environ.get('DD_WINDOWS_RUNNER')
        if not compiler or not runner:
            self.skipTest('Set DD_WINDOWS_CC and DD_WINDOWS_RUNNER for PE bootstrap testing')
        with tempfile.TemporaryDirectory(prefix='dd-windows-bootstrap-') as root:
            root = Path(root)
            child = root / 'child.c'
            child.write_text('int wmain(void) {return 0;}\n')
            common = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
            executable = root / 'fixture.exe'
            subprocess.run([*common, '-municode', str(child), '-o', str(executable)],
                           check=True)
            data = bytearray(executable.read_bytes())
            pe = struct.unpack_from('<I', data, 0x3c)[0]
            count = struct.unpack_from('<H', data, pe + 6)[0]
            optional_size = struct.unpack_from('<H', data, pe + 20)[0]
            entry = struct.unpack_from('<I', data, pe + 40)[0]
            base = struct.unpack_from('<Q', data, pe + 48)[0]
            alignment = struct.unpack_from('<I', data, pe + 60)[0]
            header_size = struct.unpack_from('<I', data, pe + 84)[0]
            section = pe + 24 + optional_size + count * 40
            self.assertLessEqual(section + 40, header_size)
            raw = (len(data) + alignment - 1) // alignment * alignment
            data.extend(b'\0' * (raw - len(data)))
            rva, size = 0x200000, 0xa00000
            payload = bytearray(size)
            _, image_size, tables = windows_port.profile()
            for sites in tables.values():
                for at, expected, _ in sites:
                    payload[at - rva:at - rva + len(expected)] = expected
            # The guarded CRT prologue calls a local RET and then jumps to our
            # original empty-program CRT. No commercial entry body is copied.
            guarded = bytes.fromhex('4883ec28e8c70c00004883c428e96efeffff')
            payload[0xb938e0-rva:0xb938e0-rva+len(guarded)] = guarded
            payload[0xb945b0-rva] = 0xc3
            jump = b'\x48\xb8' + struct.pack('<Q', base + entry) + b'\xff\xe0'
            payload[0xb93760-rva:0xb93760-rva+len(jump)] = jump
            data.extend(payload)
            struct.pack_into('<8sIIIIIIHHI', data, section, b'.ddtest\0', size,
                             rva, size, raw, 0, 0, 0, 0, 0x60000020)
            struct.pack_into('<H', data, pe + 6, count + 1)
            struct.pack_into('<I', data, pe + 40, 0xb938e0)
            struct.pack_into('<I', data, pe + 80, image_size)
            flags = struct.unpack_from('<H', data, pe + 94)[0]
            struct.pack_into('<H', data, pe + 94, flags & ~0x40)
            executable.write_bytes(data)
            project = Path(__file__).resolve().parent
            helper = root / 'bootstrap.c'
            helper.write_text('''#define wmain production_wmain
#include "''' + str(project / 'windows_launcher.c') + '''"
#undef wmain
int wmain(int argc, wchar_t **argv) {
  if (argc != 3 && argc != 4) return 2;
  wchar_t game[32768], library[32768], directory[32768], command[32768]={0};
  if (!GetFullPathNameW(argv[1],32768,game,NULL) ||
      !GetFullPathNameW(argv[2],32768,library,NULL) ||
      !GetCurrentDirectoryW(32768,directory)) return 3;
  if (!append_argument(command,32768,game)) return 4;
  return launch_game(game,command,directory,library,1,1,argc == 4) ? 0 : 125;
}
''')
            launcher = root / 'bootstrap.exe'
            library = root / 'dd_skin_rig_patch.dll'
            subprocess.run([*common, '-municode', str(helper), '-lbcrypt',
                            '-o', str(launcher)], check=True)
            subprocess.run([*common, '-shared', str(project / 'skin_rig_patch.c'),
                            '-lm', '-o', str(library)], check=True)
            environment = dict(os.environ, WINEDEBUG='-all',
                               WINEDLLOVERRIDES='winemenubuilder.exe=d')
            result = subprocess.run([runner, str(launcher), 'fixture.exe',
                                     'dd_skin_rig_patch.dll'], cwd=root,
                                    env=environment, capture_output=True,
                                    text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('DD_WINDOWS_RESOURCE_APPLIED', result.stderr)
            self.assertIn('DD_WINDOWS_RENDER_APPLIED', result.stderr)
            self.assertIn('DD_WINDOWS_LAUNCHED', result.stderr)
            diagnosis = subprocess.run([runner, str(launcher), 'fixture.exe',
                                        'dd_skin_rig_patch.dll', 'diagnose'],
                                       cwd=root, env=environment, capture_output=True,
                                       text=True, timeout=60)
            self.assertEqual(diagnosis.returncode, 125, diagnosis.stdout + diagnosis.stderr)
            self.assertIn('DD_WINDOWS_GAME_EXIT code=0', diagnosis.stderr)


if __name__ == '__main__':
    unittest.main()
