/* Reuse the synthetic engine API, exercising both native swap implementations.
 */
#define main basic_skin_cycle_main
#include "skin_cycle_fixture.c"
#undef main

static char mounted[3][0x210] = {"mods/skin/", "mods/base/", "mods/other/"};
static unsigned preferred;
static unsigned mounted_count(void *device) {
  (void)device;
  return 3;
}
static const void *mounted_root(void *device, unsigned index) {
  (void)device;
  assert(index < 3);
  return mounted[index];
}
static void no_base(NativeFileMatch *files, void *device, const char *path,
                    const char *pattern, int details, int order, int flags) {
  (void)device;
  (void)path;
  (void)pattern;
  (void)details;
  (void)order;
  (void)flags;
  names(files, "", 0);
}
static void mounted_palettes(void *archive, NativeFileMatch *files,
                             const char *path, const char *pattern, int details,
                             int order, bool include) {
  (void)path;
  (void)pattern;
  (void)details;
  (void)order;
  (void)include;
  names(files, archive == (void *)1 ? "BC" : "A", archive == (void *)1 ? 2 : 1);
}
static bool native_path(void *unused, int mount, const char *path, char *out,
                        unsigned capacity) {
  (void)unused;
  (void)mount;
  if (*path == '>')
    return dd_copy(out, capacity, path + 1);
  /* Deliberately different from mounted-root enumeration order. The patch
   * must use the native resolver's choice, not infer a root ordering. */
  const unsigned order[] = {preferred, preferred == 1 ? 2U : 1U, 0};
  for (unsigned i = 0; i < 3; ++i) {
    struct stat info;
    assert(path_join(out, capacity, mounted[order[i]], path));
    if (!stat(out, &info) && S_ISREG(info.st_mode))
      return true;
  }
  return false;
}
static void *create_layered(const Handles *handles, int flags) {
  (void)flags;
  assert(handles->end == handles->begin + 3);
  FixtureSprite *sprite = calloc(1, sizeof(*sprite));
  unsigned char *states = calloc(3, 0x30);
  assert(sprite && states);
  for (unsigned i = 0; i < 3; ++i)
    memcpy(states + i * 0x30, handles->begin + i, sizeof(Handle));
  put_pointer(sprite, 0x20, states);
  put_pointer(sprite, 0x28, states + 3 * 0x30);
  strcpy(sprite->animation, "idle");
  sprite->animation_pointer = sprite->animation;
  put_pointer(sprite, 0, &sprite->animation_pointer);
  sprite->walk = 1;
#ifdef _WIN32
  put_pointer(sprite, 0x50, (void *)(uintptr_t)(made + 1));
#endif
  ++made;
  return sprite;
}
#ifndef _WIN32
static void destroy_layered(void **sprite) {
#else
static void destroy_layered(void *queue, const void *value) {
  (void)queue;
  void *const *sprite = value;
#endif
  ++deleted;
  free(pointer_at(*sprite, 0x20));
  free(*sprite);
}
static void layered_textures(void *view, const char *directory) {
  const char *key = resource(NULL, pointer_at(view, 0x20));
  char expected[512];
  if (strstr(key, ".idle.skel") && strstr(key, "mods/skin/")) {
    assert(strstr(key, "mods/skin/heroes/alpha/alpha_") != NULL);
    char suffix = strstr(key, "alpha_B/") ? 'B' : 'C';
    snprintf(expected, sizeof(expected),
             ">mods/skin/heroes/alpha/alpha_%c/anim/", suffix);
  } else {
    unsigned provider = 1;
    if (strstr(key, ".walk.skel"))
      provider = !strcmp(override_route->origin, mounted[0])   ? preferred
                 : !strcmp(override_route->origin, mounted[1]) ? 1
                                                               : 2;
    assert(!strncmp(key + 1, mounted[provider], strlen(mounted[provider])));
    snprintf(expected, sizeof(expected), ">%sheroes/alpha/alpha_A/anim/",
             mounted[provider]);
  }
  assert(!strcmp(directory, expected));
  ++textured;
}
static void layered_check(int requested, int expected) {
  unsigned before = applied;
  change_colour(actor, requested);
  assert(applied == before + 1 && int_at(actor, 0xb68) == expected);
  void *sprite = pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
  const Route *route = find_route(actor, expected);
  assert(route && route->plan->count == 3 && sprite_matches(sprite, route));
  assert(textured == applied * 3);
#ifdef _WIN32
  assert(pointer_at(sprite, 0x50) == callback_owner);
#endif
}
int main(int argc, char **argv) {
  assert(argc == 3 || argc == 4);
  preferred = (unsigned)atoi(argv[1]);
  int refused = atoi(argv[2]);
  assert(preferred == 1 || preferred == 2);
  dd_config.hide_vanilla_skins = argc == 4 ? atoi(argv[3]) : 0;
  runtime_enabled = 1;
#ifdef _WIN32
  preserve_palette_sources = 1;
  total = 4;
#else
  preserve_palette_sources = 0;
  total = 3;
#endif
  for (unsigned i = 0; i < total; ++i)
    snprintf(paths[i], sizeof(paths[i]), "heroes/alpha/alpha_%c/",
             (preserve_palette_sources ? "AABC" : "ABC")[i]);
  void *table[8] = {0};
  table[DD_CLASS_VTABLE_OFFSET / sizeof(void *)] = (void *)fixture_class;
  put_pointer(actor, 0, table);
  for (unsigned i = 0; i < 3; ++i)
    put_pointer(mounted[i], 0x200, (void *)(uintptr_t)(i + 1));
  put_pointer(storage,
#ifdef _WIN32
              0x18,
#else
              0x38,
#endif
              manager_pointer);
  base = 0;
  native_symbols.values[DD_STORAGE] = (uintptr_t)&storage_pointer;
  native_symbols.values[DD_RESOURCE] = (uintptr_t)&manager_pointer;
  native_symbols.values[DD_ACTOR_SPRITE_FIELD] = 0x100;
  native_symbols.values[DD_ANIMATION_LENGTH] = (uintptr_t)length;
  native_symbols.values[DD_SET_TIME] = (uintptr_t)set_time;
  native_symbols.values[DD_DISCARD_SPRITE] = (uintptr_t)destroy_layered;
#ifndef _WIN32
  unsigned step = 42;
  native_symbols.values[DD_STEP_ID] = (uintptr_t)&step;
  native_symbols.values[DD_ADD_STEP] = (uintptr_t)add_step;
  native_symbols.values[DD_SET_SPRITE] = (uintptr_t)replace_sprite;
  native_symbols.values[DD_ANIMATION_NAME] = (uintptr_t)current_animation;
  native_symbols.values[DD_GET_TIME] = (uintptr_t)current_time;
  native_symbols.values[DD_HAS_ANIMATION] = (uintptr_t)has_animation;
#endif
  colour_count = fixture_count;
  colour_path = fixture_path;
  match_device = no_base;
  alternate_count = mounted_count;
  alternate_root = mounted_root;
  match_archive = mounted_palettes;
  destroy_match = destroy_files;
  full_path = native_path;
  native_new = create_layered;
  load_spine = load;
  release_spine = release;
  resource_name = resource;
  get_resource = get;
  original_colour = apply;
  original_override = layered_textures;
  set_animation = play;
  int first = (int)total - 2;
  memcpy(actor + 0xb68, &first, sizeof(first));
  const Route *initial = find_route(actor, first);
  if (refused == 1) {
    assert(!initial && !made);
    clear_routes();
    clear_catalogs();
    puts("CLASS_FALLBACK_REFUSED_OK");
    return 0;
  }
  assert(initial && initial->plan->count == 3);
  put_pointer(actor, DD_ACTOR_SPRITE_OFFSET, new_actor_sprite(NULL, 0, actor));
#ifdef _WIN32
  put_pointer(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET), 0x50, callback_owner);
#endif
  for (unsigned i = 0; i < 3; ++i) {
    layered_check((int)total - 1, (int)total - 1);
    layered_check(first, first);
    if (!refused) {
      layered_check((int)total - 1, (int)total - 1);
      for (unsigned next = 0; next <= (unsigned)first; ++next)
        layered_check((int)next, (int)next);
    }
  }
  void *last = pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
  free(pointer_at(last, 0x20));
  free(last);
  assert(made == deleted + 1);
  clear_routes();
  clear_catalogs();
  puts("CLASS_FALLBACK_OK");
  return 0;
}
