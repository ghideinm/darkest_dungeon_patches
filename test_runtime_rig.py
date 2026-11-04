"""Runtime source policy and isolated native resolver tests; never starts the game."""
import ctypes
import io
import mmap
import os
from pathlib import Path
import platform
import struct
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

import launch
import manage
from paths import DEFAULT_GAME_ROOT


class RuntimeRigFixture:
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='dd-runtime-test-')
        cls.binary = Path(cls.temporary.name) / 'rig_selftest'
        subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DDD_PATCH_SELFTEST', str(Path(__file__).with_name('skin_rig_patch.c')),
                        '-ldl', '-lm', '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.tree = tempfile.TemporaryDirectory(prefix='dd-runtime-source-')
        self.addCleanup(self.tree.cleanup)
        self.root = Path(self.tree.name)
        self.skin = 'heroes/alpha/alpha_E'

    def rig(self, directory, valid=True, action='idle', pages=('idle.png', 'walk.png')):
        path = self.root / directory
        path.mkdir(parents=True, exist_ok=True)
        (path / f'alpha.sprite.{action}.skel').write_bytes(b'fixture, not a parsed skeleton')
        if valid:
            (path / f'alpha.sprite.{action}.atlas').write_text(''.join(
                f'\n{page}\nsize: 8,8\nformat: RGBA8888\nregion\n  rotate: false\n'
                for page in pages))

    def resolve(self, full='mods/chosen/heroes/alpha/alpha_E/anim/idle.png'):
        return subprocess.run([str(self.binary), '--source-rig', self.skin, 'idle.png', full],
                              cwd=self.root, capture_output=True, text=True)


class RuntimeRigTest(RuntimeRigFixture, unittest.TestCase):
    def test_prefers_skin_local_rig_from_actual_provider(self):
        self.rig('mods/chosen/heroes/alpha/alpha_E/anim')
        self.rig('mods/chosen/heroes/alpha/anim')
        self.rig('mods/unselected/heroes/alpha/alpha_E/anim')
        result = self.resolve()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'mods/chosen/heroes/alpha/alpha_E/anim')

    def test_searches_only_same_provider_then_vanilla(self):
        self.rig('mods/unselected/heroes/alpha/anim')
        self.rig('heroes/alpha/anim')
        self.assertEqual(self.resolve().stdout.strip(), 'heroes/alpha/anim')
        self.rig('mods/chosen/heroes/alpha/anim')
        self.assertEqual(self.resolve().stdout.strip(), 'mods/chosen/heroes/alpha/anim')

    def test_broken_local_rig_does_not_fall_through(self):
        self.rig('mods/chosen/heroes/alpha/alpha_E/anim', valid=False)
        self.rig('heroes/alpha/anim')
        self.assertEqual(self.resolve().returncode, 4)

    def test_refuses_wrong_source_and_ignores_generated_copies(self):
        self.rig('mods/chosen/heroes/alpha/anim')
        self.assertEqual(self.resolve('mods/chosen/heroes/beta/beta_E/anim/idle.png').returncode, 4)
        root = self.root / 'mods/chosen/heroes/alpha/anim'
        (root / 'alpha.sprite.rig_vanilla.idle.skel').write_bytes(b'legacy isolated rig')
        self.assertEqual(self.resolve().stdout.strip(), 'mods/chosen/heroes/alpha/anim')

    def test_manager_launch_needs_no_config_package_or_map(self):
        with (patch('sys.argv', ['manage.py', 'launch', '--game-root', str(self.root),
                                 '--config', str(self.root / 'absent.json')]),
              patch('manage.subprocess.run') as run):
            manage.main()
        command = run.call_args.args[0]
        self.assertIn('--runtime-rig', command)
        self.assertNotIn('--rig-map', command)

    def test_launcher_checks_runtime_without_map(self):
        game = self.root / 'game'
        game.touch()
        output = io.StringIO()
        with (patch('sys.argv', ['launch.py', '--game', str(game), '--check', '--runtime-rig']),
              patch('launch.validate') as validate,
              patch('launch.build_library', return_value=Path('/tmp/fixture.so')),
              redirect_stdout(output)):
            launch.main()
        validate.assert_called_once_with(game, False, False, True)
        self.assertIn('"rig_mode": "runtime_source"', output.getvalue())
        self.assertIn('"rig_map": null', output.getvalue())


class NativeSourceResolverTest(unittest.TestCase):
    """Run only IO_GetFileInfo instructions, with fake devices and host callbacks.

    No entrypoint, game initialization, storage thread, save or graphics code runs.
    Relative calls are redirected to controlled callbacks; the original resolver's
    provider-order loop and '>' branch execute unchanged.
    """

    @classmethod
    def setUpClass(cls):
        game = DEFAULT_GAME_ROOT / '_linux/darkest.bin.x86_64'
        if platform.machine() != 'x86_64' or not game.is_file():
            raise unittest.SkipTest('Verified Linux x86-64 game binary unavailable')
        launch.validate(game, runtime_rig=True)
        data = game.read_bytes()
        offset = struct.unpack_from('<Q', data, 32)[0]
        size, count = struct.unpack_from('<HH', data, 54)
        start, end = 0xa5c2d0, 0xa5c3d2
        code = None
        for i in range(count):
            kind, flags, pos, address, _, length, _, _ = struct.unpack_from(
                '<IIQQQQQQ', data, offset + size * i)
            if kind == 1 and flags & 1 and address <= start < end <= address + length:
                code = bytearray(data[pos + start - address:pos + end - address])
        if code is None:
            raise AssertionError('Resolver code segment unavailable')
        cls.blocks = []
        cls.providers = []
        cls.existing = set()
        cls.calls = []
        cls.name = ctypes.create_string_buffer(b'fixture_device')
        ptr = ctypes.c_void_p

        def callback(result, args, function):
            wrapped = ctypes.CFUNCTYPE(result, *args)(function)
            cls.blocks.append(wrapped)
            return ctypes.cast(wrapped, ptr).value

        def assign(target, source):
            text = ctypes.string_at(source)
            ctypes.memmove(target, text + b'\0', len(text) + 1)
            return target

        def format_path(target, unused_format, directory, name):
            text = ctypes.string_at(directory) + ctypes.string_at(name)
            ctypes.memmove(target, text + b'\0', len(text) + 1)
            return target

        def file_size(unused_device, path):
            name = ctypes.string_at(path).decode()
            cls.calls.append(name)
            return 17 if name in cls.existing else 0

        targets = {
            0xa40930: callback(ctypes.c_uint, [ptr], lambda device: len(cls.providers)),
            0xa40950: callback(ptr, [ptr, ctypes.c_uint], lambda device, i: ctypes.addressof(cls.providers[i])),
            0xc33010: callback(ptr, [ptr, ptr, ptr, ptr], format_path),
            0xc32ca0: callback(ptr, [ptr, ptr], assign),
            0xc2e1a0: callback(ptr, [ptr, ptr], assign),
            0xa40590: callback(ptr, [ptr], lambda device: ctypes.addressof(cls.name)),
            0xa416c0: callback(ctypes.c_uint, [ptr, ptr], lambda archive, name: 0),
        }
        file_size_address = callback(ctypes.c_uint, [ptr, ptr], file_size)
        cls.vtable = (ptr * 12)()
        cls.vtable[5] = file_size_address
        cls.device = (ptr * 1)(ctypes.addressof(cls.vtable))
        cls.mapping = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE)
        base = ctypes.addressof(ctypes.c_char.from_buffer(cls.mapping))
        trampoline = {}
        for i, (target, replacement) in enumerate(targets.items()):
            location = 512 + i * 16
            trampoline[target] = base + location
            cls.mapping[location:location + 12] = b'\x48\xb8' + struct.pack('<Q', replacement) + b'\xff\xe0'
        sites = [0xa5c2f5, 0xa5c316, 0xa5c32e, 0xa5c342, 0xa5c382,
                 0xa5c39e, 0xa5c3ad, 0xa5c3cd]
        for site in sites:
            index = site - start
            if code[index] not in (0xe8, 0xe9):
                raise AssertionError('Unexpected native call instruction')
            target = site + 5 + struct.unpack_from('<i', code, index + 1)[0]
            struct.pack_into('<i', code, index + 1, trampoline[target] - (base + index + 5))
        cls.mapping[:len(code)] = code
        libc = ctypes.CDLL(None, use_errno=True)
        libc.mprotect.argtypes = [ptr, ctypes.c_size_t, ctypes.c_int]
        if libc.mprotect(base, 4096, mmap.PROT_READ | mmap.PROT_EXEC):
            raise OSError(ctypes.get_errno(), 'mprotect failed')
        cls.native = ctypes.CFUNCTYPE(None, ptr, ptr, ptr, ptr)(base)

    @classmethod
    def tearDownClass(cls):
        cls.mapping.close()

    def setUp(self):
        self.providers.clear()
        self.existing.clear()
        self.calls.clear()
        for name in [b'mods/first/', b'mods/second/']:
            entry = ctypes.create_string_buffer(0x210)
            ctypes.memmove(entry, name + b'\0', len(name) + 1)
            self.providers.append(entry)

    def resolve(self, request):
        info = ctypes.create_string_buffer(0x444)
        path = ctypes.create_string_buffer(request.encode())
        self.native(None, ctypes.addressof(self.device), ctypes.addressof(path), ctypes.addressof(info))
        return ctypes.string_at(ctypes.addressof(info) + 0x200).decode(), struct.unpack_from('<I', info, 0x440)[0]

    def test_native_provider_precedence_changes_with_active_order(self):
        name = 'heroes/alpha/alpha_E/anim/idle.png'
        self.existing.update({'mods/first/' + name, 'mods/second/' + name, name})
        self.assertEqual(self.resolve(name), ('mods/first/' + name, 17))
        self.providers.reverse()
        self.assertEqual(self.resolve(name), ('mods/second/' + name, 17))

    def test_native_explicit_source_bypasses_mod_search(self):
        name = 'heroes/alpha/anim/alpha.sprite.idle.skel'
        self.existing.update({'mods/first/' + name, name})
        self.assertEqual(self.resolve('>' + name), (name, 17))
        self.assertEqual(self.calls, [name])

    def test_native_missing_file_is_not_a_valid_source(self):
        _, size = self.resolve('heroes/alpha/alpha_E/anim/missing.png')
        self.assertEqual(size, 0)


class RuntimeLoadingTest(RuntimeRigFixture, unittest.TestCase):
    """Exercise the production discovery/loading path with controlled native APIs."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        library = Path(cls.temporary.name) / 'runtime_fixture.so'
        subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DDD_RIG_RUNTIME_TEST', '-fPIC', '-shared',
                        str(Path(__file__).with_name('skin_rig_patch.c')), '-ldl', '-lm',
                        '-o', str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.library.dd_runtime_test_bind.argtypes = [ctypes.c_size_t] + [ctypes.c_void_p] * 8
        cls.library.dd_runtime_test_bind.restype = None
        cls.library.dd_runtime_test_sprite.argtypes = [ctypes.c_char_p]
        cls.library.dd_runtime_test_sprite.restype = ctypes.c_void_p
        cls.library.dd_runtime_test_matches.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        cls.library.dd_runtime_test_matches.restype = ctypes.c_int

    def setUp(self):
        super().setUp()
        previous = Path.cwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, previous)
        ptr = ctypes.c_void_p

        class Handle(ctypes.Structure):
            _fields_ = [('index', ctypes.c_int), ('generation', ctypes.c_int), ('cached', ptr)]

        class Vector(ctypes.Structure):
            _fields_ = [('begin', ptr), ('end', ptr), ('capacity', ptr)]

        self.callbacks, self.keys, self.loaded, self.released = [], [], [], []
        self.created, self.matches, self.destroyed = [], [], []
        self.sources = {'idle.png': 'mods/chosen/', 'walk.png': 'mods/chosen/'}
        self.alias = False
        self.loaded_ok = True
        entries = ((ctypes.c_char * 256) * 2)()
        entries[0].value, entries[1].value = b'idle.png', b'walk.png'
        self.entries = entries

        def callback(result, args, function):
            wrapped = ctypes.CFUNCTYPE(result, *args)(function)
            self.callbacks.append(wrapped)
            return wrapped

        def match(target, mount, path, pattern, details, depth, flags):
            self.matches.append((mount, path, details, depth, flags))
            vector = target.contents
            vector.begin = ctypes.addressof(entries)
            vector.end = vector.capacity = vector.begin + 512

        def destroy(target):
            self.destroyed.append(True)
            target.contents.begin = target.contents.end = target.contents.capacity = None

        def resolve(storage, mount, request, output, capacity):
            file = request.decode().rsplit('/', 1)[1]
            source = self.sources.get(file)
            if source is None:
                return False
            text = (source + request.decode()).encode()
            if len(text) >= capacity:
                return False
            ctypes.memmove(output, text + b'\0', len(text) + 1)
            return True

        def load(target, key, defaults, function):
            target.contents.index = len(self.keys)
            target.contents.generation = 1
            target.contents.cached = None
            self.keys.append(ctypes.create_string_buffer(key))
            self.loaded.append((key.decode(), defaults, ctypes.string_at(function, 32)))

        def release(target):
            self.released.append(target.contents.index)

        def name(manager, target):
            if self.alias:
                return ctypes.addressof(entries)
            return ctypes.addressof(self.keys[target.contents.index])

        def create(vector, flag):
            count = (vector.contents.end - vector.contents.begin) // ctypes.sizeof(Handle)
            self.created.append(count)
            return 0x1234

        match_callback = callback(None, [ctypes.POINTER(Vector), ctypes.c_int, ctypes.c_char_p,
                                        ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int], match)
        destroy_callback = callback(None, [ctypes.POINTER(Vector)], destroy)
        resolve_callback = callback(ctypes.c_bool, [ptr, ctypes.c_int, ctypes.c_char_p, ptr,
                                                  ctypes.c_uint], resolve)
        load_callback = callback(None, [ctypes.POINTER(Handle), ctypes.c_char_p, ctypes.c_int, ptr], load)
        release_callback = callback(None, [ctypes.POINTER(Handle)], release)
        get_callback = callback(ptr, [ptr, ctypes.POINTER(Handle)],
                                lambda manager, handle: 1 if self.loaded_ok else None)
        create_callback = callback(ptr, [ctypes.POINTER(Vector), ctypes.c_int], create)
        name_callback = callback(ptr, [ptr, ctypes.POINTER(Handle)], name)
        delta = 0x31c2b58 - 0x1d744f0
        self.managers = mmap.mmap(-1, delta + 16)
        self.addCleanup(self.managers.close)
        address = ctypes.addressof(ctypes.c_char.from_buffer(self.managers))
        ctypes.c_void_p.from_buffer(self.managers, 0).value = 0x10
        ctypes.c_void_p.from_buffer(self.managers, delta).value = 0x20
        self.library.dd_runtime_test_bind(address - 0x1d744f0, match_callback, destroy_callback,
                                          resolve_callback, load_callback, release_callback,
                                          get_callback, create_callback, name_callback)

    def test_loads_source_keys_and_releases_temporary_native_handles(self):
        self.rig('mods/chosen/heroes/alpha/anim')
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        self.assertEqual(self.loaded, [('>mods/chosen/heroes/alpha/anim/alpha.sprite.idle.skel',
                                       0, bytes(32))])
        self.assertEqual(self.released, [0])
        self.assertEqual(self.created, [1])
        self.assertEqual(self.destroyed, [True])
        self.assertEqual(self.matches[0], (1, b'heroes/alpha/alpha_E/anim/', 0, 0, 2))

    def test_conflicting_texture_sources_never_load_or_create_sprite(self):
        self.rig('mods/chosen/heroes/alpha/anim')
        self.rig('mods/other/heroes/alpha/anim')
        self.sources['walk.png'] = 'mods/other/'
        self.assertIsNone(self.library.dd_runtime_test_sprite(self.skin.encode()))
        self.assertEqual(self.loaded, [])
        self.assertEqual(self.created, [])
        self.assertEqual(self.destroyed, [True])

    def test_independent_actions_compose_dlc_and_base_skeletons(self):
        self.rig('dlc/official/heroes/alpha/anim', action='dead', pages=('dead.png',))
        self.rig('heroes/alpha/anim', action='walk', pages=('walk.png',))
        self.entries[0].value = b'dead.png'
        self.sources = {'dead.png': 'dlc/official/', 'walk.png': ''}
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        self.assertEqual([entry[0] for entry in self.loaded], [
            '>dlc/official/heroes/alpha/anim/alpha.sprite.dead.skel',
            '>heroes/alpha/anim/alpha.sprite.walk.skel'])
        self.assertEqual(self.created, [2])
        self.assertEqual(self.released, [0, 1])

    def test_existing_highwayman_c_dlc_dead_and_base_walk(self):
        base = DEFAULT_GAME_ROOT / 'heroes/highwayman/anim/highwayman.sprite.walk.skel'
        dead = DEFAULT_GAME_ROOT / 'dlc/1117860_arena_mp/heroes/highwayman/anim/highwayman.sprite.dead.skel'
        if not base.is_file() or not dead.is_file():
            self.skipTest('Installed base and official DLC assets unavailable')
        os.chdir(DEFAULT_GAME_ROOT)  # Read existing atlases only; factory calls remain mocked.
        self.skin = 'heroes/highwayman/highwayman_C'
        self.entries[0].value = b'highwayman.sprite.dead.png'
        self.entries[1].value = b'highwayman.sprite.walk.png'
        self.sources = {'highwayman.sprite.dead.png': 'dlc/1117860_arena_mp/',
                        'highwayman.sprite.walk.png': ''}
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        self.assertEqual([entry[0] for entry in self.loaded], [
            '>dlc/1117860_arena_mp/heroes/highwayman/anim/highwayman.sprite.dead.skel',
            '>heroes/highwayman/anim/highwayman.sprite.walk.skel'])
        self.assertEqual(self.created, [2])

    def test_unrelated_palette_image_does_not_select_extra_skeletons(self):
        self.rig('mods/chosen/heroes/alpha/anim', pages=('idle.png',))
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        self.assertEqual(self.created, [1])

    def test_sprite_identity_checks_every_action_source(self):
        self.rig('dlc/official/heroes/alpha/anim', action='dead', pages=('dead.png',))
        self.rig('heroes/alpha/anim', action='walk', pages=('walk.png',))
        self.entries[0].value = b'dead.png'
        self.sources = {'dead.png': 'dlc/official/', 'walk.png': ''}
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        states = (ctypes.c_ubyte * 0x60)()
        for i in range(2):
            ctypes.c_int.from_buffer(states, i * 0x30).value = i
            ctypes.c_int.from_buffer(states, i * 0x30 + 4).value = 1
        sprite = (ctypes.c_ubyte * 0x40)()
        ctypes.c_void_p.from_buffer(sprite, 0x20).value = ctypes.addressof(states)
        ctypes.c_void_p.from_buffer(sprite, 0x28).value = ctypes.addressof(states) + 0x60
        self.assertEqual(self.library.dd_runtime_test_matches(ctypes.addressof(sprite),
                                                             self.skin.encode()), 1)
        self.rig('mods/other/heroes/alpha/anim', action='walk', pages=('walk.png',))
        self.sources['walk.png'] = 'mods/other/'
        self.assertEqual(self.library.dd_runtime_test_matches(ctypes.addressof(sprite),
                                                             self.skin.encode()), 0)

    def test_native_cache_alias_and_failed_load_release_handle(self):
        self.rig('mods/chosen/heroes/alpha/anim')
        self.alias = True
        self.assertIsNone(self.library.dd_runtime_test_sprite(self.skin.encode()))
        self.assertEqual(self.released, [0])
        self.assertEqual(self.created, [])
        self.alias = False
        self.loaded_ok = False
        self.assertIsNone(self.library.dd_runtime_test_sprite(self.skin.encode()))
        self.assertEqual(self.released, [0, 1])
        self.assertEqual(self.created, [])


if __name__ == '__main__':
    unittest.main()
