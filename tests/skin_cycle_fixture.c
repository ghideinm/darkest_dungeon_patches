/* Production routing and sprite swaps with synthetic resources/native APIs. */
#include "skin_rig_patch.c"
#include <assert.h>

typedef struct {
  unsigned char bytes[0x100], state[0x30];
  char animation[32];
  const char *animation_pointer;
  int walk;
} FixtureSprite;
static unsigned char klass[0x700], actor[0xc00], storage[0x50];
static void *storage_pointer = storage, *manager_pointer = (void *)1;
static char root[0x210] = "mods/one/";
static char paths[4][512], keys[256][512];
static unsigned total, key_count, made, deleted, applied, textured;
static int official, refuse_create, refuse_idle;
static float last_time;
#ifdef _WIN32
static void *const callback_owner = (void *)0x123456;
#endif

static void put_pointer(void *target, size_t offset, const void *value) {
  memcpy((char *)target + offset, &value, sizeof(value));
}
static void *fixture_class(void *unused) {
  (void)unused;
  return klass;
}
static unsigned fixture_count(void *unused) {
  (void)unused;
  return total;
}
static const char *fixture_path(void *unused, unsigned index) {
  (void)unused;
  assert(index < total);
  return paths[index];
}
static void names(NativeFileMatch *files, const char *text, unsigned count) {
  files->begin = calloc(count ? count : 1, sizeof(*files->begin));
  assert(files->begin);
  for (unsigned i = 0; i < count; ++i)
    snprintf(files->begin[i], 256, "alpha_%c", text[i]);
  files->end = files->capacity = files->begin + count;
}
static void base_files(NativeFileMatch *files, void *device, const char *path,
                       const char *pattern, int details, int order, int flags) {
  (void)device;
  (void)path;
  (void)pattern;
  (void)details;
  (void)order;
  (void)flags;
  names(files, "Z", (unsigned)official);
}
static unsigned roots(void *device) {
  (void)device;
  return 1;
}
static const void *root_at(void *device, unsigned index) {
  (void)device;
  assert(!index);
  return root;
}
static void mod_files(void *archive, NativeFileMatch *files, const char *path,
                      const char *pattern, int details, int order,
                      bool include) {
  (void)archive;
  (void)path;
  (void)pattern;
  (void)details;
  (void)order;
  (void)include;
  names(files, "ABC", 3);
}
static void destroy_files(NativeFileMatch *files) {
  free(files->begin);
  *files = (NativeFileMatch){0};
}
static bool resolve_file(void *unused, int mount, const char *path, char *out,
                         unsigned capacity) {
  (void)unused;
  (void)mount;
  assert(*path == '>');
  return dd_copy(out, capacity, path + 1);
}
static void load(Handle *out, const char *key, int defaults,
                 const void *callback) {
  (void)defaults;
  (void)callback;
  assert(key_count < 256);
  assert(dd_copy(keys[key_count], sizeof(keys[0]), key));
  *out = (Handle){(int)key_count++, 1, NULL};
}
static void release(Handle *handle) { assert(handle->index >= 0); }
static void *get(void *unused, const Handle *handle) {
  (void)unused;
  return handle->index >= 0 ? manager_pointer : NULL;
}
static const char *resource(void *unused, const Handle *handle) {
  (void)unused;
  assert(handle->index >= 0 && (unsigned)handle->index < key_count);
  return keys[handle->index];
}
static void *create(const Handles *handles, int flags) {
  (void)flags;
  assert(handles->end == handles->begin + 1);
  if (refuse_create)
    return NULL;
  FixtureSprite *sprite = calloc(1, sizeof(*sprite));
  assert(sprite);
  memcpy(sprite->state, handles->begin, sizeof(Handle));
  put_pointer(sprite, 0x20, sprite->state);
  put_pointer(sprite, 0x28, sprite->state + sizeof(sprite->state));
  strcpy(sprite->animation, "idle");
  sprite->animation_pointer = sprite->animation;
  put_pointer(sprite, 0, &sprite->animation_pointer);
  sprite->walk = strstr(resource(NULL, handles->begin), "alpha_C/") != NULL;
#ifdef _WIN32
  put_pointer(sprite, 0x50, (void *)(uintptr_t)(made + 1));
#endif
  ++made;
  return sprite;
}
static bool has_animation(void *value, const char *name) {
  FixtureSprite *sprite = value;
  return (!strcmp(name, "idle") && !refuse_idle) ||
         (!strcmp(name, "walk") && sprite->walk);
}
static float length(void *sprite, const char *name) {
  return has_animation(sprite, name) ? 2.0f : 0;
}
static void play(void *value, const char *name, int loop, int force) {
  (void)loop;
  (void)force;
  assert(has_animation(value, name));
  FixtureSprite *sprite = value;
  assert(dd_copy(sprite->animation, sizeof(sprite->animation), name));
}
static void set_time(void *sprite, float time) {
  (void)sprite;
  assert(isfinite(time) && time >= 0 && time < 2);
  last_time = time;
}
#ifndef _WIN32
static const char *current_animation(void *value) {
  return ((FixtureSprite *)value)->animation;
}
static float current_time(void *sprite) {
  (void)sprite;
  return 0.25f;
}
static void replace_sprite(void *target, void *sprite) {
  put_pointer(target, DD_ACTOR_SPRITE_OFFSET, sprite);
}
static void add_step(void *sprite, unsigned id, void *callback) {
  (void)sprite;
  assert(id == 42 && pointer_at(callback, 0) == actor);
}
static void destroy_sprite(void **sprite) {
#else
static void destroy_sprite(void *queue, const void *value) {
  (void)queue;
  void *const *sprite = value;
#endif
  ++deleted;
  free(*sprite);
}
static void textures(void *view, const char *directory) {
  const Handle *handle = pointer_at(view, 0x20);
  const char *key = resource(NULL, handle);
  /* Check that every swap binds the new rig's own skin texture directory. */
  char expected[512];
  assert(snprintf(expected, sizeof(expected), ">%salpha.sprite.idle.skel",
                  directory + 1) < (int)sizeof(expected));
  assert(!strcmp(key, expected));
  ++textured;
}
static void apply(void *target, int index) {
  assert(index >= 0 && (unsigned)index < total);
  override_textures(pointer_at(target, DD_ACTOR_SPRITE_OFFSET), paths[index]);
  memcpy((char *)target + 0xb68, &index, sizeof(index));
  ++applied;
}
static void check(int requested, int expected) {
  unsigned before = applied;
  change_colour(actor, requested);
  assert(applied == before + 1);
  assert(int_at(actor, 0xb68) == expected);
  FixtureSprite *sprite = pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
#ifdef _WIN32
  assert(pointer_at(sprite, 0x50) == callback_owner);
#endif
  const Route *route = find_route(actor, expected);
  assert(route && sprite_matches(sprite, route));
  assert(textured == applied);
}
int main(int argc, char **argv) {
  assert(argc == 3);
  official = atoi(argv[1]);
  dd_config.hide_vanilla_skins = atoi(argv[2]);
  runtime_enabled = preserve_palette_sources = 1;
  total = 3 + (unsigned)official;
  for (unsigned i = 0; i < total; ++i)
    snprintf(paths[i], sizeof(paths[i]), "heroes/alpha/alpha_%c/",
             official && !i ? 'Z' : 'A' + (int)i - official);
  void *table[8] = {0};
  table[DD_CLASS_VTABLE_OFFSET / sizeof(void *)] = (void *)fixture_class;
  put_pointer(actor, 0, table);
  put_pointer(root, 0x200, manager_pointer);
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
  native_symbols.values[DD_DISCARD_SPRITE] = (uintptr_t)destroy_sprite;
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
  match_device = base_files;
  alternate_count = roots;
  alternate_root = root_at;
  match_archive = mod_files;
  destroy_match = destroy_files;
  full_path = resolve_file;
  native_new = create;
  load_spine = load;
  release_spine = release;
  resource_name = resource;
  get_resource = get;
  original_colour = apply;
  original_override = textures;
  set_animation = play;
  int first = official && dd_config.hide_vanilla_skins ? 1 : 0;
  memcpy(actor + 0xb68, &first, sizeof(first));
  const Route *initial = find_route(actor, first);
  assert(initial);
  put_pointer(actor, DD_ACTOR_SPRITE_OFFSET, make_sprite(klass, initial));
#ifdef _WIN32
  put_pointer(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET), 0x50, callback_owner);
#endif
  for (unsigned round = 0; round < 3; ++round) {
    for (unsigned i = (unsigned)first + 1; i < total; ++i)
      check((int)i, (int)i);
    FixtureSprite *last = pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
    play(last, "walk", 1, 1); /* First rig has idle, but no walk. */
    check(0, first);          /* Actual button wraps to zero before the hook. */
    assert(!strcmp(
        ((FixtureSprite *)pointer_at(actor, DD_ACTOR_SPRITE_OFFSET))->animation,
        "idle"));
    assert(last_time == 0);
  }
  check((int)total - 1, (int)total - 1);
  check((int)total, first); /* Also accept a caller passing last + 1. */
  check((int)total * 3 + first, first);
  check(first ? first - 1 : (int)total - 1,
        (int)total - 1); /* Reverse wrap, with masking on/off. */
  check((int)total - 2, (int)total - 2);
  void *old = pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
  int saved = int_at(actor, 0xb68);
  put_pointer(old, 0x38,
              manager_pointer); /* Pending callbacks stay protected. */
  change_colour(actor, first);
  assert(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET) == old);
  assert(int_at(actor, 0xb68) == saved);
  put_pointer(old, 0x38, NULL);
  strcpy(((FixtureSprite *)old)->animation, "attack");
  change_colour(actor, first);
  assert(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET) == old);
  assert(int_at(actor, 0xb68) == saved);
  strcpy(((FixtureSprite *)old)->animation, "idle");
  refuse_create = 1;
  change_colour(actor, first);
  assert(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET) == old);
  assert(int_at(actor, 0xb68) == saved);
  refuse_create = 0;
  refuse_idle = 1;
  change_colour(actor, first);
  assert(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET) == old);
  assert(int_at(actor, 0xb68) == saved);
  refuse_idle = 0;
  check(first, first); /* Rejection must not leave the cycle stuck. */
  free(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET));
  assert(made == deleted + 1);
  clear_routes();
  clear_catalogs();
  puts("SKIN_CYCLE_OK");
  return 0;
}
