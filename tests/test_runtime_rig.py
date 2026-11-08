"""Runtime source policy and isolated native resolver tests; never starts the game."""

import ctypes
import mmap
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from darkest_dungeon_patches.paths import COMMON_NATIVE


class RuntimeRigFixture:
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="dd-runtime-test-")
        cls.binary = Path(cls.temporary.name) / "rig_selftest"
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-DDD_PATCH_SELFTEST",
                str((COMMON_NATIVE / "skin_rig_patch.c")),
                "-ldl",
                "-lm",
                "-o",
                str(cls.binary),
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.tree = tempfile.TemporaryDirectory(prefix="dd-runtime-source-")
        self.addCleanup(self.tree.cleanup)
        self.root = Path(self.tree.name)
        self.skin = "heroes/alpha/alpha_E"

    def rig(self, directory, valid=True, action="idle", pages=("idle.png", "walk.png")):
        path = self.root / directory
        path.mkdir(parents=True, exist_ok=True)
        (path / f"alpha.sprite.{action}.skel").write_bytes(b"fixture, not a parsed skeleton")
        if valid:
            (path / f"alpha.sprite.{action}.atlas").write_text(
                "".join(
                    f"\n{page}\nsize: 8,8\nformat: RGBA8888\nregion\n  rotate: false\n"
                    for page in pages
                )
            )

    def resolve(self, full="mods/chosen/heroes/alpha/alpha_E/anim/idle.png"):
        return subprocess.run(
            [str(self.binary), "--source-rig", self.skin, "idle.png", full],
            cwd=self.root,
            capture_output=True,
            text=True,
        )


class RuntimeRigTest(RuntimeRigFixture, unittest.TestCase):
    def test_prefers_skin_local_rig_from_actual_provider(self):
        self.rig("mods/chosen/heroes/alpha/alpha_E/anim")
        self.rig("mods/chosen/heroes/alpha/anim")
        self.rig("mods/unselected/heroes/alpha/alpha_E/anim")
        result = self.resolve()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "mods/chosen/heroes/alpha/alpha_E/anim")

    def test_searches_only_same_provider_then_vanilla(self):
        self.rig("mods/unselected/heroes/alpha/anim")
        self.rig("heroes/alpha/anim")
        self.assertEqual(self.resolve().stdout.strip(), "heroes/alpha/anim")
        self.rig("mods/chosen/heroes/alpha/anim")
        self.assertEqual(self.resolve().stdout.strip(), "mods/chosen/heroes/alpha/anim")

    def test_broken_local_rig_does_not_fall_through(self):
        self.rig("mods/chosen/heroes/alpha/alpha_E/anim", valid=False)
        self.rig("heroes/alpha/anim")
        self.assertEqual(self.resolve().returncode, 4)

    def test_refuses_wrong_source_and_ignores_generated_copies(self):
        self.rig("mods/chosen/heroes/alpha/anim")
        self.assertEqual(self.resolve("mods/chosen/heroes/beta/beta_E/anim/idle.png").returncode, 4)
        root = self.root / "mods/chosen/heroes/alpha/anim"
        (root / "alpha.sprite.rig_vanilla.idle.skel").write_bytes(b"legacy isolated rig")
        self.assertEqual(self.resolve().stdout.strip(), "mods/chosen/heroes/alpha/anim")


class PaletteEnumerationTest(unittest.TestCase):
    def test_native_merge_preserves_three_sources_only_for_palette_queries(self):
        with tempfile.TemporaryDirectory(prefix="dd-palette-test-") as directory:
            root = Path(directory)
            source = root / "palette.c"
            source.write_text(r"""#define DD_PALETTE_SOURCES_TEST
#include "palette_sources.h"
#include <assert.h>
#include <stdlib.h>
static unsigned destroyed, appended;
static void append(NativeFileMatch *files, const void *name) {
  size_t count = files->begin ? (size_t)(files->end-files->begin) : 0;
  files->begin = realloc(files->begin, (count+1)*256);
  assert(files->begin);
  memcpy(files->begin[count], name, 256);
  files->end = files->capacity = files->begin+count+1;
  ++appended;
}
static void destroy(NativeFileMatch *files) {
  free(files->begin); *files = (NativeFileMatch){0}; ++destroyed;
}
static void native_merge(void *storage, NativeFileMatch *files, void *device,
                         unsigned root, const char *directory,
                         const char *pattern, int details, int order, int flags) {
  assert(storage == (void*)1 && device == (void*)2 && root < 3);
  (void)directory; (void)pattern; (void)details; (void)order; (void)flags;
  for (unsigned i=0; i<4; ++i) {
    char name[256] = {0}; snprintf(name,sizeof(name),"alpha_%c",'A'+i);
    char (*at)[256] = files->begin;
    while(at != files->end && strcmp(*at,name)) ++at;
    if(at == files->end) append(files,name);
  }
}
int main(void) {
  const char *pattern = "(.*)alpha_([A-Z])(\\\\|/)";
  NativeFileMatch files = {0};
  native_merge((void*)1,&files,(void*)2,0,"heroes/alpha/",pattern,0,0,0);
  for(unsigned root=1;root<3;++root)
    match_palette_root(native_merge,append,destroy,(void*)1,&files,(void*)2,
                        root,"heroes/alpha/",pattern,0,0,0);
  assert(files.end-files.begin == 12 && destroyed == 2 && appended == 20);
  for(unsigned i=0;i<12;++i) assert(files.begin[i][6] == 'A'+i%4);
  destroy(&files);
  const char *directories[] = {"heroes/alpha/anim/","monsters/alpha/",
                               "heroes/alpha/","heroes/alpha/","heroes/alpha/",
                               "heroes/alpha/","heroes/alpha/"};
  for(unsigned i=0;i<7;++i) {
    files = (NativeFileMatch){0};
    native_merge((void*)1,&files,(void*)2,0,"heroes/alpha/",pattern,0,0,0);
    unsigned before = destroyed;
    match_palette_root(native_merge,append,destroy,(void*)1,&files,(void*)2,1,
                        directories[i], i==2 ? "(.*)alpha_([A-Z])" : pattern,
                        i==3, i==4, i==5 ? 4 : i==6 ? 1 : 0);
    assert(files.end-files.begin == 4 && destroyed == before);
    destroy(&files);
  }
  return 0;
}
""")
            binary = root / "palette"
            subprocess.run(
                [
                    "clang",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(COMMON_NATIVE),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


class DynamicStorageTest(unittest.TestCase):
    def test_large_maps_file_lists_and_allocation_cleanup_under_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="dd-storage-test-") as directory:
            root = Path(directory)
            source = root / "storage.c"
            source.write_text(
                '#define DD_RIG_RUNTIME_TEST\n#include "'
                + (COMMON_NATIVE / "skin_rig_patch.c").as_posix()
                + '"\n#include <assert.h>\n'
                + r"""
static const char *resource_key(void *manager,const Handle *handle) {
  (void)manager;(void)handle;return "prefix.idle.skel";
}
int main(void) {
  dd_config.cache_initial_capacity=3;
  Route initial={0};
  assert(append_route(&initial) && route_capacity == 3);
  for (unsigned i=0;i<3;++i) assert(append_route(&initial));
  assert(route_capacity == 6);
  clear_routes();
  FILE *map = tmpfile();
  assert(map);
  for (unsigned i=0;i<320;++i)
    fprintf(map,"heroes/alpha/alpha_%u group prefix.\n",i);
  rewind(map);
  assert(read_routes(map) && route_count == 320);
  fclose(map);
  const Route *saved = routes[0];
  Route extra = {0};
  for (unsigned i=0;i<320;++i) assert(append_route(&extra));
  assert(saved == routes[0] && !strcmp(saved->skin,"heroes/alpha/alpha_0"));
  void *manager=(void*)1;base=(uintptr_t)&manager;
  native_symbols.values[DD_RESOURCE]=0;resource_name=resource_key;
  Handle *handles=calloc(700,sizeof(*handles));assert(handles);
  Handles all={handles,handles+700,handles+700},selected={0};
  unsigned char klass[0xe0+sizeof(Handles)]={0};
  memcpy(klass+0xe0,&all,sizeof(all));
  assert(select_handles(klass,saved,&selected)==700);
  free(selected.begin);free(handles);
  clear_routes();
  clear_routes();
  NativeFileMatch files = {0};
  char name[256];
  for (unsigned i=0;i<4200;++i) {
    snprintf(name,sizeof(name),"image_%u.png",i);
    assert(append_file_name(&files,name));
  }
  size_t count;
  assert(file_match_count(&files,&count) && count == 4200);
  assert(!strcmp(files.begin[4199],"image_4199.png"));
  NativeFileMatch invalid = files;
  invalid.end = (void *)((char *)files.end-1);
  assert(!file_match_count(&invalid,&count));
  invalid = files; invalid.capacity = files.begin;
  assert(!file_match_count(&invalid,&count));
  free(files.begin);
  size_t capacity = 0;
  assert(!grow_array(NULL,&capacity,SIZE_MAX,sizeof(RigEntry),16) && !capacity);
  RigPlan plan = {0};
  for (unsigned i=0;i<300;++i) {
    snprintf(name,sizeof(name),">heroes/alpha/action_%03u.skel",i);
    assert(add_rig_key(&plan,name));
  }
  assert(plan.count == 300);
  free(plan.entries);
  return 0;
}
"""
            )
            binary = root / "storage"
            subprocess.run(
                [
                    "clang",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    str(source),
                    "-ldl",
                    "-lm",
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


class RuntimeLoadingTest(RuntimeRigFixture, unittest.TestCase):
    """Exercise the production discovery/loading path with controlled native APIs."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        library = Path(cls.temporary.name) / "runtime_fixture.so"
        source = Path(cls.temporary.name) / "runtime_fixture.c"
        source.write_text(
            """#define _GNU_SOURCE
#include <stdlib.h>
static int allocation_budget = -1;
static int refuse_allocation(void) {
  if (allocation_budget < 0) return 0;
  if (!allocation_budget) return 1;
  --allocation_budget;
  return 0;
}
static void *fixture_malloc(size_t size) {
  return refuse_allocation() ? NULL : malloc(size);
}
static void *fixture_calloc(size_t count, size_t size) {
  return refuse_allocation() ? NULL : calloc(count, size);
}
static void *fixture_realloc(void *data, size_t size) {
  return refuse_allocation() ? NULL : realloc(data, size);
}
#define malloc fixture_malloc
#define calloc fixture_calloc
#define realloc fixture_realloc
"""
            + '#include "'
            + (COMMON_NATIVE / "skin_rig_patch.c").as_posix()
            + '"\nvoid dd_runtime_test_allocation_budget(int count) {allocation_budget=count;}\n'
        )
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-DDD_RIG_RUNTIME_TEST",
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
        cls.library = ctypes.CDLL(str(library))
        cls.library.dd_runtime_test_bind.argtypes = [ctypes.c_size_t] + [ctypes.c_void_p] * 8
        cls.library.dd_runtime_test_bind.restype = None
        cls.library.dd_runtime_test_sprite.argtypes = [ctypes.c_char_p]
        cls.library.dd_runtime_test_sprite.restype = ctypes.c_void_p
        cls.library.dd_runtime_test_matches.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        cls.library.dd_runtime_test_matches.restype = ctypes.c_int
        cls.library.dd_runtime_test_sprite_at.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        cls.library.dd_runtime_test_sprite_at.restype = ctypes.c_void_p
        cls.library.dd_runtime_test_texture.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        cls.library.dd_runtime_test_texture.restype = ctypes.c_char_p
        cls.library.dd_runtime_test_bind_sources.argtypes = [ctypes.c_void_p] * 6 + [ctypes.c_int]
        cls.library.dd_runtime_test_source.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        cls.library.dd_runtime_test_source.restype = ctypes.c_char_p
        cls.library.dd_runtime_test_actor_sprite.argtypes = [
            ctypes.c_char_p,
            ctypes.c_void_p,
            ctypes.c_int,
        ]
        cls.library.dd_runtime_test_actor_sprite.restype = ctypes.c_void_p
        cls.library.dd_runtime_test_route.argtypes = [ctypes.c_char_p]
        cls.library.dd_runtime_test_route.restype = ctypes.c_void_p
        cls.library.dd_runtime_test_saved_route.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        cls.library.dd_runtime_test_saved_route.restype = ctypes.c_int
        cls.library.dd_runtime_test_allocation_budget.argtypes = [ctypes.c_int]
        cls.library.dd_runtime_test_hide_vanilla.argtypes = [ctypes.c_int]
        cls.library.dd_runtime_test_visible_colour.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
        ]
        cls.library.dd_runtime_test_visible_colour.restype = ctypes.c_int
        cls.library.dd_runtime_test_bind_colour.argtypes = [ctypes.c_void_p]
        cls.library.dd_runtime_test_change_colour.argtypes = [ctypes.c_void_p, ctypes.c_int]
        cls.library.dd_runtime_test_actor_skin.argtypes = [ctypes.c_void_p, ctypes.c_int]
        cls.library.dd_runtime_test_actor_skin.restype = ctypes.c_char_p

    def setUp(self):
        super().setUp()
        previous = Path.cwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, previous)
        ptr = ctypes.c_void_p

        class Handle(ctypes.Structure):
            _fields_ = [("index", ctypes.c_int), ("generation", ctypes.c_int), ("cached", ptr)]

        class Vector(ctypes.Structure):
            _fields_ = [("begin", ptr), ("end", ptr), ("capacity", ptr)]

        self.callbacks, self.keys, self.loaded, self.released = [], [], [], []
        self.created, self.matches, self.destroyed = [], [], []
        self.constructor_calls = []
        self.create_failures = 0
        self.sources = {"idle.png": "mods/chosen/", "walk.png": "mods/chosen/"}
        self.alias = False
        self.loaded_ok = True
        entries = ((ctypes.c_char * 256) * 2)()
        entries[0].value, entries[1].value = b"idle.png", b"walk.png"
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
            if request.startswith(b">"):
                text = request[1:]
            else:
                file = request.decode().rsplit("/", 1)[1]
                source = self.sources.get(file)
                if source is None:
                    return False
                text = (source + request.decode()).encode()
            if len(text) >= capacity:
                return False
            ctypes.memmove(output, text + b"\0", len(text) + 1)
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
            self.constructor_calls.append((vector.contents.begin, count, flag))
            if self.create_failures:
                self.create_failures -= 1
                return None
            return 0x1234

        match_callback = callback(
            None,
            [
                ctypes.POINTER(Vector),
                ctypes.c_int,
                ctypes.c_char_p,
                ctypes.c_char_p,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_int,
            ],
            match,
        )
        destroy_callback = callback(None, [ctypes.POINTER(Vector)], destroy)
        resolve_callback = callback(
            ctypes.c_bool, [ptr, ctypes.c_int, ctypes.c_char_p, ptr, ctypes.c_uint], resolve
        )
        load_callback = callback(
            None, [ctypes.POINTER(Handle), ctypes.c_char_p, ctypes.c_int, ptr], load
        )
        release_callback = callback(None, [ctypes.POINTER(Handle)], release)
        get_callback = callback(
            ptr,
            [ptr, ctypes.POINTER(Handle)],
            lambda manager, handle: 1 if self.loaded_ok else None,
        )
        create_callback = callback(ptr, [ctypes.POINTER(Vector), ctypes.c_int], create)
        name_callback = callback(ptr, [ptr, ctypes.POINTER(Handle)], name)
        delta = 0x31C2B58 - 0x1D744F0
        self.managers = mmap.mmap(-1, delta + 16)
        self.addCleanup(self.managers.close)
        address = ctypes.addressof(ctypes.c_char.from_buffer(self.managers))
        ctypes.c_void_p.from_buffer(self.managers, 0).value = 0x10
        ctypes.c_void_p.from_buffer(self.managers, delta).value = 0x20
        self.library.dd_runtime_test_bind(
            address - 0x1D744F0,
            match_callback,
            destroy_callback,
            resolve_callback,
            load_callback,
            release_callback,
            get_callback,
            create_callback,
            name_callback,
        )
        self.Vector, self.Handle, self.callback = Vector, Handle, callback

    def bind_catalog(self, roots=None, preserve_sources=False, base_names=None):
        ptr = ctypes.c_void_p
        self.base_names = base_names if base_names is not None else [f"alpha_{c}" for c in "ABCD"]
        # Archive-backed Mod indices omit the trailing directory separator;
        # the base Linux directory enumeration includes it. Keep both entries.
        self.root_specs = (
            roots
            if roots is not None
            else [("mods/deepseek/", ["alpha_E"]), ("mods/blood/", [f"alpha_{c}" for c in "ABCD"])]
        )
        self.native_paths = [f"heroes/alpha/alpha_{c}//" for c in "ABCD"] + [
            f"heroes/alpha/alpha_{c}/" for c in "ABCDE"
        ]
        if preserve_sources:
            self.native_paths = [f"heroes/alpha/{name}/" for name in self.base_names] + [
                f"heroes/alpha/{name}/" for _, names in reversed(self.root_specs) for name in names
            ]
        self.path_buffers = [ctypes.create_string_buffer(p.encode()) for p in self.native_paths]
        self.root_buffers = []
        for i, (origin, _) in enumerate(self.root_specs):
            entry = ctypes.create_string_buffer(0x210)
            ctypes.memmove(entry, origin.encode() + b"\0", len(origin) + 1)
            struct.pack_into("<Q", entry, 0x200, i + 1)
            self.root_buffers.append(entry)
        self.storage = ctypes.create_string_buffer(0x40)
        struct.pack_into("<Q", self.storage, 0x38, 0x55)
        ctypes.c_void_p.from_buffer(self.managers, 0).value = ctypes.addressof(self.storage)
        self.catalog_vectors = []

        def fill(target, names):
            entries = ((ctypes.c_char * 256) * len(names))()
            for entry, name in zip(entries, names):
                entry.value = name.encode()
            self.catalog_vectors.append(entries)
            target.contents.begin = ctypes.addressof(entries)
            target.contents.end = target.contents.capacity = ctypes.addressof(entries) + 256 * len(
                names
            )

        def match(target, device, directory, pattern, details, depth, flags):
            self.assertEqual((directory, details, depth, flags), (b"heroes/alpha/", 0, 0, 0))
            fill(target, [name + ("" if preserve_sources else "/") for name in self.base_names])

        def archive(device, target, directory, pattern, details, depth, include):
            fill(target, self.root_specs[device - 1][1])

        callback = self.callback
        self.library.dd_runtime_test_bind_sources(
            callback(ctypes.c_uint, [ptr], lambda klass: len(self.path_buffers)),
            callback(
                ptr, [ptr, ctypes.c_uint], lambda klass, i: ctypes.addressof(self.path_buffers[i])
            ),
            callback(
                None,
                [
                    ctypes.POINTER(self.Vector),
                    ptr,
                    ctypes.c_char_p,
                    ctypes.c_char_p,
                    ctypes.c_int,
                    ctypes.c_int,
                    ctypes.c_int,
                ],
                match,
            ),
            callback(ctypes.c_uint, [ptr], lambda device: len(self.root_buffers)),
            callback(
                ptr, [ptr, ctypes.c_uint], lambda device, i: ctypes.addressof(self.root_buffers[i])
            ),
            callback(
                None,
                [
                    ptr,
                    ctypes.POINTER(self.Vector),
                    ctypes.c_char_p,
                    ctypes.c_char_p,
                    ctypes.c_int,
                    ctypes.c_int,
                    ctypes.c_bool,
                ],
                archive,
            ),
            preserve_sources,
        )

    def texture(self, origin, skin, filename):
        path = self.root / origin / skin / "anim" / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"texture fixture; native texture parsing is mocked")

    def test_source_catalog_and_render_keys_preserve_base_four_mod_four_and_extra_one(self):
        self.bind_catalog()
        expected = [""] * 4 + ["mods/blood/"] * 4 + ["mods/deepseek/"]
        origins = [self.library.dd_runtime_test_source(0x55, i).decode() for i in range(9)]
        self.assertEqual(origins, expected)
        for origin in set(origins):
            self.rig(origin + "heroes/alpha/anim", action="idle", pages=("idle.png",))
            self.rig(origin + "heroes/alpha/anim", action="walk", pages=("walk.png",))
        for i, origin in enumerate(origins):
            colour = "ABCDABCDE"[i]
            skin = f"heroes/alpha/alpha_{colour}"
            for file in ["idle.png", "walk.png"]:
                self.texture(origin, skin, file)
            self.assertEqual(
                self.library.dd_runtime_test_sprite_at(skin.encode(), origin.encode()), 0x1234
            )
            self.assertEqual(
                self.loaded[-2][0], f">{origin}heroes/alpha/anim/alpha.sprite.idle.skel"
            )
            self.assertEqual(
                self.library.dd_runtime_test_texture(skin.encode(), origin.encode(), 0).decode(),
                f">{origin}{skin}/anim/",
            )

    def test_catalog_refuses_unmatched_native_entry_order(self):
        self.bind_catalog()
        self.path_buffers[0], self.path_buffers[1] = self.path_buffers[1], self.path_buffers[0]
        self.assertIsNone(self.library.dd_runtime_test_source(0x55, 0))

    def test_visibility_skips_base_and_dlc_in_both_directions_without_renumbering(self):
        self.bind_catalog(
            roots=[
                ("mods/second/", ["alpha_A"]),
                ("dlc/official/", [f"alpha_{c}" for c in "ABCD"]),
                ("mods/first/", ["alpha_A", "alpha_E"]),
            ],
            preserve_sources=True,
        )
        before = [buffer.raw for buffer in self.path_buffers]
        choose = self.library.dd_runtime_test_visible_colour
        self.assertEqual(choose(0x55, 0, 0), 0)
        self.library.dd_runtime_test_hide_vanilla(1)
        visible = (4, 5, 10)
        for current in range(11):
            for requested in range(11):
                self.assertIn(choose(0x55, requested, current), visible)
        self.assertEqual(choose(0x55, 0, 10), 4)
        self.assertEqual(choose(0x55, 6, 5), 10)
        self.assertEqual(choose(0x55, 3, 4), 10)
        self.assertEqual(choose(0x55, 9, 10), 5)
        self.assertEqual([buffer.raw for buffer in self.path_buffers], before)
        self.assertEqual(
            [self.library.dd_runtime_test_source(0x55, i) for i in visible],
            [b"mods/first/", b"mods/first/", b"mods/second/"],
        )
        self.library.dd_runtime_test_hide_vanilla(0)
        self.assertEqual(choose(0x55, 0, 10), 0)

    def test_visibility_preserves_unmodded_and_dlc_only_classes(self):
        self.library.dd_runtime_test_hide_vanilla(1)
        self.bind_catalog(
            roots=[("mods/unrelated/", []), ("dlc/official/", ["alpha_A", "alpha_B"])],
            preserve_sources=True,
        )
        for i in range(6):
            self.assertEqual(self.library.dd_runtime_test_visible_colour(0x55, i, (i + 5) % 6), i)
        self.bind_catalog(
            roots=[("dlc/official/", ["alpha_A", "alpha_B"])], preserve_sources=True, base_names=[]
        )
        self.assertEqual(self.library.dd_runtime_test_visible_colour(0x55, 0, 1), 0)
        self.assertEqual(self.library.dd_runtime_test_visible_colour(0x55, 1, 0), 1)
        absolute = str(self.root / "dlc" / "official") + "/"
        self.bind_catalog(roots=[(absolute, ["alpha_A"])], preserve_sources=True, base_names=[])
        self.assertEqual(self.library.dd_runtime_test_source(0x55, 0), absolute.encode())
        self.assertEqual(self.library.dd_runtime_test_visible_colour(0x55, 0, 0), 0)

    def test_linux_shared_palette_index_uses_mod_provider_only_when_enabled(self):
        self.bind_catalog(roots=[("mods/second/", ["alpha_A/"]), ("mods/first/", ["alpha_A/"])])
        self.path_buffers = [
            ctypes.create_string_buffer(f"heroes/alpha/alpha_{c}//".encode()) for c in "ABCD"
        ]
        self.assertEqual(self.library.dd_runtime_test_source(0x55, 0), b"")
        self.library.dd_runtime_test_hide_vanilla(1)
        self.assertEqual(self.library.dd_runtime_test_source(0x55, 0), b"mods/first/")
        for i in range(4):
            self.assertEqual(self.library.dd_runtime_test_visible_colour(0x55, i, 0), 0)
        self.library.dd_runtime_test_hide_vanilla(0)
        self.assertEqual(self.library.dd_runtime_test_source(0x55, 0), b"")

    def test_initial_route_preview_and_colour_setter_choose_mod_skin(self):
        self.bind_catalog(
            roots=[("dlc/official/", ["alpha_A"]), ("mods/chosen/", ["alpha_E", "alpha_F"])],
            preserve_sources=True,
        )
        self.library.dd_runtime_test_hide_vanilla(1)
        self.rig("mods/chosen/heroes/alpha/anim", pages=("idle.png",))
        for suffix in "EF":
            self.texture("mods/chosen/", f"heroes/alpha/alpha_{suffix}", "idle.png")
        actor = ctypes.create_string_buffer(0xC00)
        table = (ctypes.c_void_p * 8)()
        get_class = self.callback(ctypes.c_void_p, [ctypes.c_void_p], lambda unused: 0x55)
        table[7] = ctypes.cast(get_class, ctypes.c_void_p).value
        struct.pack_into("<Q", actor, 0, ctypes.addressof(table))
        applied = []

        def apply(target, colour):
            applied.append(colour)
            ctypes.c_int.from_address(target + 0xB68).value = colour

        setter = self.callback(None, [ctypes.c_void_p, ctypes.c_int], apply)
        self.library.dd_runtime_test_bind_colour(setter)
        # Actor creation and previews both use this resolver, while retaining the saved index.
        self.assertEqual(self.library.dd_runtime_test_actor_skin(actor, 0), b"heroes/alpha/alpha_E")
        self.assertEqual(struct.unpack_from("<i", actor, 0xB68)[0], 0)
        for requested, expected in ((0, 4), (5, 5), (6, 4), (3, 5)):
            self.library.dd_runtime_test_change_colour(actor, requested)
            self.assertEqual(applied[-1], expected)
            self.assertEqual(struct.unpack_from("<i", actor, 0xB68)[0], expected)

    def test_windows_catalog_keeps_base_and_two_workshop_sources_with_identical_names(self):
        first = "C:/Steam/steamapps/workshop/content/262060/101/"
        second = "C:/Steam/steamapps/workshop/content/262060/202/"
        self.bind_catalog(
            roots=[
                (second, [f"alpha_{c}" for c in "ABCD"]),
                (first, [f"alpha_{c}" for c in "ABCD"]),
            ],
            preserve_sources=True,
        )
        origins = [self.library.dd_runtime_test_source(0x55, i).decode() for i in range(12)]
        self.assertEqual(origins, [""] * 4 + [first] * 4 + [second] * 4)
        for origin in set(origins):
            self.rig(origin + "heroes/alpha/anim", action="idle", pages=("idle.png",))
        for i, origin in enumerate(origins):
            skin = f"heroes/alpha/alpha_{'ABCD'[i % 4]}"
            self.texture(origin, skin, "idle.png")
            self.assertEqual(
                self.library.dd_runtime_test_sprite_at(skin.encode(), origin.encode()), 0x1234
            )
            self.assertEqual(
                self.loaded[-1][0], f">{origin}heroes/alpha/anim/alpha.sprite.idle.skel"
            )
            self.assertEqual(
                self.library.dd_runtime_test_texture(skin.encode(), origin.encode(), 0).decode(),
                f">{origin}{skin}/anim/",
            )

    def test_pinned_partial_mod_falls_back_to_base_and_dlc_without_other_mod(self):
        self.bind_catalog(roots=[("dlc/official/", [])])
        self.rig("mods/chosen/heroes/alpha/anim", action="idle", pages=("idle.png",))
        self.rig("heroes/alpha/anim", action="walk", pages=("walk.png",))
        self.rig("dlc/official/heroes/alpha/anim", action="dead", pages=("dead.png",))
        self.rig("mods/other/heroes/alpha/anim", action="walk", pages=("walk.png",))
        self.texture("mods/chosen/", self.skin, "idle.png")
        self.texture("", self.skin, "walk.png")
        self.texture("dlc/official/", self.skin, "dead.png")
        self.texture("mods/other/", self.skin, "walk.png")
        self.sources = {"walk.png": "mods/other/"}
        self.assertEqual(
            self.library.dd_runtime_test_sprite_at(self.skin.encode(), b"mods/chosen/"), 0x1234
        )
        self.assertEqual(
            [item[0] for item in self.loaded],
            [
                ">dlc/official/heroes/alpha/anim/alpha.sprite.dead.skel",
                ">heroes/alpha/anim/alpha.sprite.walk.skel",
                ">mods/chosen/heroes/alpha/anim/alpha.sprite.idle.skel",
            ],
        )
        self.assertEqual(
            self.library.dd_runtime_test_texture(self.skin.encode(), b"mods/chosen/", 1),
            b">heroes/alpha/alpha_E/anim/",
        )

    def test_loads_source_keys_and_releases_temporary_native_handles(self):
        self.rig("mods/chosen/heroes/alpha/anim")
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        self.assertEqual(
            self.loaded, [(">mods/chosen/heroes/alpha/anim/alpha.sprite.idle.skel", 0, bytes(32))]
        )
        self.assertEqual(self.released, [0])
        self.assertEqual(self.created, [1])
        self.assertEqual(self.destroyed, [True])
        self.assertEqual(self.matches[0], (1, b"heroes/alpha/alpha_E/anim/", 0, 0, 2))

    def test_conflicting_texture_sources_never_load_or_create_sprite(self):
        self.rig("mods/chosen/heroes/alpha/anim")
        self.rig("mods/other/heroes/alpha/anim")
        self.sources["walk.png"] = "mods/other/"
        self.assertIsNone(self.library.dd_runtime_test_sprite(self.skin.encode()))
        self.assertEqual(self.loaded, [])
        self.assertEqual(self.created, [])
        self.assertEqual(self.destroyed, [True])

    def test_independent_actions_compose_dlc_and_base_skeletons(self):
        self.rig("dlc/official/heroes/alpha/anim", action="dead", pages=("dead.png",))
        self.rig("heroes/alpha/anim", action="walk", pages=("walk.png",))
        self.entries[0].value = b"dead.png"
        self.sources = {"dead.png": "dlc/official/", "walk.png": ""}
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        self.assertEqual(
            [entry[0] for entry in self.loaded],
            [
                ">dlc/official/heroes/alpha/anim/alpha.sprite.dead.skel",
                ">heroes/alpha/anim/alpha.sprite.walk.skel",
            ],
        )
        self.assertEqual(self.created, [2])
        self.assertEqual(self.released, [0, 1])

    def test_unrelated_palette_image_does_not_select_extra_skeletons(self):
        self.rig("mods/chosen/heroes/alpha/anim", pages=("idle.png",))
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        self.assertEqual(self.created, [1])

    def test_sprite_identity_checks_every_action_source(self):
        self.rig("dlc/official/heroes/alpha/anim", action="dead", pages=("dead.png",))
        self.rig("heroes/alpha/anim", action="walk", pages=("walk.png",))
        self.entries[0].value = b"dead.png"
        self.sources = {"dead.png": "dlc/official/", "walk.png": ""}
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        states = (ctypes.c_ubyte * 0x60)()
        for i in range(2):
            ctypes.c_int.from_buffer(states, i * 0x30).value = i
            ctypes.c_int.from_buffer(states, i * 0x30 + 4).value = 1
        sprite = (ctypes.c_ubyte * 0x40)()
        ctypes.c_void_p.from_buffer(sprite, 0x20).value = ctypes.addressof(states)
        ctypes.c_void_p.from_buffer(sprite, 0x28).value = ctypes.addressof(states) + 0x60
        self.assertEqual(
            self.library.dd_runtime_test_matches(ctypes.addressof(sprite), self.skin.encode()), 1
        )
        self.rig("mods/other/heroes/alpha/anim", action="walk", pages=("walk.png",))
        self.sources["walk.png"] = "mods/other/"
        self.assertEqual(
            self.library.dd_runtime_test_matches(ctypes.addressof(sprite), self.skin.encode()), 0
        )

    def test_native_cache_alias_and_failed_load_release_handle(self):
        self.rig("mods/chosen/heroes/alpha/anim")
        self.alias = True
        self.assertIsNone(self.library.dd_runtime_test_sprite(self.skin.encode()))
        self.assertEqual(self.released, [0])
        self.assertEqual(self.created, [])
        self.alias = False
        self.loaded_ok = False
        self.assertIsNone(self.library.dd_runtime_test_sprite(self.skin.encode()))
        self.assertEqual(self.released, [0, 1])
        self.assertEqual(self.created, [])

    def original_handles(self):
        handles = (self.Handle * 1)()
        handles[0].index, handles[0].generation = 42, 9
        address = ctypes.addressof(handles)
        return handles, self.Vector(
            address, address + ctypes.sizeof(handles), address + ctypes.sizeof(handles)
        )

    def test_route_growth_preserves_live_addresses_and_existing_sources(self):
        self.rig("mods/chosen/heroes/alpha/anim")
        saved = self.library.dd_runtime_test_route(self.skin.encode())
        self.assertTrue(saved)
        for i in range(300):
            skin = f"heroes/alpha/alpha_{i:03d}".encode()
            self.assertEqual(self.library.dd_runtime_test_sprite(skin), 0x1234)
        self.assertEqual(self.library.dd_runtime_test_route(self.skin.encode()), saved)
        self.assertEqual(self.library.dd_runtime_test_saved_route(saved, self.skin.encode()), 1)

    def test_catalog_growth_and_more_than_512_palettes(self):
        self.bind_catalog(roots=[], preserve_sources=True)
        for klass in range(160):
            self.assertEqual(self.library.dd_runtime_test_source(0x1000 + klass, 3), b"")
        self.assertEqual(self.library.dd_runtime_test_source(0x1000, 0), b"")
        roots = [(f"mods/provider_{i}/", [f"alpha_{c}" for c in "ABCD"]) for i in range(140)]
        self.bind_catalog(roots=roots, preserve_sources=True)
        self.assertEqual(self.library.dd_runtime_test_source(0x55, 563), b"mods/provider_0/")

    def test_more_than_256_skeletons_load_and_release_every_handle(self):
        for i in range(270):
            self.rig("mods/chosen/heroes/alpha/anim", action=f"action_{i:03d}")
        self.assertEqual(self.library.dd_runtime_test_sprite(self.skin.encode()), 0x1234)
        self.assertEqual(self.created, [270])
        self.assertEqual(len(self.loaded), 270)
        self.assertEqual(self.released, list(range(270)))

    def test_route_refusal_and_load_failure_use_original_constructor_arguments(self):
        handles, original = self.original_handles()
        self.assertEqual(
            self.library.dd_runtime_test_actor_sprite(
                self.skin.encode(), ctypes.byref(original), 7
            ),
            0x1234,
        )
        self.assertEqual(self.constructor_calls[-1], (ctypes.addressof(handles), 1, 7))
        self.rig("mods/chosen/heroes/alpha/anim")
        self.loaded_ok = False
        self.assertEqual(
            self.library.dd_runtime_test_actor_sprite(
                self.skin.encode(), ctypes.byref(original), 7
            ),
            0x1234,
        )
        self.assertEqual(self.constructor_calls[-1], (ctypes.addressof(handles), 1, 7))
        self.assertEqual(self.released, [0])
        self.loaded_ok = True
        self.create_failures = 1
        self.assertEqual(
            self.library.dd_runtime_test_actor_sprite(
                self.skin.encode(), ctypes.byref(original), 7
            ),
            0x1234,
        )
        self.assertEqual(self.constructor_calls[-1], (ctypes.addressof(handles), 1, 7))
        self.assertEqual(self.constructor_calls[-2][2], 0)

    def test_allocation_failures_fall_back_and_allow_later_retry(self):
        self.rig("mods/chosen/heroes/alpha/anim")
        handles, original = self.original_handles()
        saved = self.library.dd_runtime_test_route(self.skin.encode())
        for budget in range(18):
            skin = f"heroes/alpha/alpha_failure_{budget}".encode()
            self.library.dd_runtime_test_allocation_budget(budget)
            try:
                self.assertEqual(
                    self.library.dd_runtime_test_actor_sprite(skin, ctypes.byref(original), 7),
                    0x1234,
                )
            finally:
                self.library.dd_runtime_test_allocation_budget(-1)
            begin, count, flag = self.constructor_calls[-1]
            if flag == 7:
                self.assertEqual((begin, count), (ctypes.addressof(handles), 1))
            self.assertEqual(self.library.dd_runtime_test_saved_route(saved, self.skin.encode()), 1)
            self.assertEqual(self.library.dd_runtime_test_sprite(skin), 0x1234)

    def test_catalog_allocation_failure_leaves_existing_entries_usable(self):
        self.bind_catalog(roots=[], preserve_sources=True)
        for klass in range(16):
            self.assertEqual(self.library.dd_runtime_test_source(0x1000 + klass, 0), b"")
        # Entries, pointer-table growth and the stable catalog node each allocate.
        for budget in range(3):
            self.library.dd_runtime_test_allocation_budget(budget)
            try:
                self.assertIsNone(self.library.dd_runtime_test_source(0x2000, 0))
            finally:
                self.library.dd_runtime_test_allocation_budget(-1)
            self.assertEqual(self.library.dd_runtime_test_source(0x1000, 0), b"")
        self.assertEqual(self.library.dd_runtime_test_source(0x2000, 0), b"")


if __name__ == "__main__":
    unittest.main()
