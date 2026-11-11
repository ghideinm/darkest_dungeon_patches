/* Same-name palettes from different providers must retain distinct portraits. */
#define main basic_skin_cycle_main
#include "skin_cycle_fixture.c"
#undef main

static char providers[2][0x210] = {"mods/one/", "mods/two/"};
static unsigned char campaign[0xf00], node[0x80];
static void *head = campaign;
static unsigned portrait_loads, portrait_releases, previews;
static int fail_portrait;
static unsigned provider_count(void *device) {
  (void)device;
  return 2;
}
static const void *provider_root(void *device, unsigned index) {
  (void)device;
  assert(index < 2);
  return providers[index];
}
static void palettes(void *archive, NativeFileMatch *files, const char *path,
                     const char *pattern, int details, int order, bool include) {
  (void)archive;
  (void)path;
  (void)pattern;
  (void)details;
  (void)order;
  (void)include;
  names(files, "A", 1);
}
#ifndef _WIN32
static void *assets(void) { return campaign; }
#endif
static const char *portrait_name(void *manager, const Handle *handle) {
  return handle->generation < 0 ? NULL : resource(manager, handle);
}
static void portrait_load(Handle *out, const char *key, unsigned flags) {
  assert(!flags);
  ++portrait_loads;
  if (fail_portrait == 1)
    return;
  load(out, fail_portrait == 2 ? "hash_alias.png" : key, 0, NULL);
}
static void portrait_release(Handle *handle) {
  if (handle->generation >= 0) {
    if (strstr(resource(NULL, handle), ".png"))
      ++portrait_releases;
  }
  *handle = (Handle){0, -1, NULL};
}
static void preview(void *panel, void *target) {
  assert(panel == klass && target == actor);
  ++previews;
}
static void check_portraits(const Handle *handles, unsigned count) {
  assert(count == (preserve_palette_sources ? 2U : 1U));
  for (unsigned i = 0; i < count; ++i) {
    char expected[256];
    snprintf(expected, sizeof(expected),
             ">%sheroes/alpha/alpha_A/alpha_portrait_roster.png",
             providers[1 - i]);
    assert(!strcmp(resource(NULL, handles + i), expected));
  }
}
int main(int argc, char **argv) {
  assert(argc == 2);
  char *args[] = {argv[0], "0", "0"};
  assert(!basic_skin_cycle_main(3, args)); /* Bind the native sprite APIs. */
  void *table[8] = {0};
  table[DD_CLASS_VTABLE_OFFSET / sizeof(void *)] = (void *)fixture_class;
  put_pointer(actor, 0, table);
#ifndef _WIN32
  unsigned step = 42;
  native_symbols.values[DD_STEP_ID] = (uintptr_t)&step;
#endif
  official = 0;
#ifdef _WIN32
  preserve_palette_sources = 1;
#else
  preserve_palette_sources = 0;
#endif
  dd_config.hide_vanilla_skins = 1;
  total = preserve_palette_sources ? 2 : 1;
  for (unsigned i = 0; i < total; ++i)
    strcpy(paths[i], "heroes/alpha/alpha_A/");
  for (unsigned i = 0; i < 2; ++i)
    put_pointer(providers[i], 0x200, (void *)(uintptr_t)(i + 1));
  alternate_count = provider_count;
  alternate_root = provider_root;
  match_archive = palettes;
  native_symbols.values[DD_RESOURCE] = (uintptr_t)&manager_pointer;
#ifdef _WIN32
  native_symbols.values[DD_PREVIEW_ROOT] = (uintptr_t)&head;
  put_pointer(campaign, 8, node);
#else
  (void)head;
  native_symbols.values[DD_ASSETS] = (uintptr_t)assets;
  put_pointer(campaign, 0xe88, node);
#endif
  int first = 0;
  memcpy(actor + 0xb68, &first, sizeof(first));
  void *sprite = new_actor_sprite(NULL, 0, actor);
  assert(sprite);
  put_pointer(actor, DD_ACTOR_SPRITE_OFFSET, sprite);
  void *preview_sprite = make_sprite(klass, find_route(actor, 0));
  put_pointer(node, 0x60, preview_sprite);
  Handle handles[2];
  for (unsigned i = 0; i < total; ++i)
    load(handles + i, "heroes/alpha/alpha_A/alpha_portrait_roster.png", 0, NULL);
  Handles view = {handles, handles + total, handles + total};
  memcpy(node + 0x28, &view, sizeof(view));
  load_texture = portrait_load;
  resource_name = portrait_name;
  original_preview = preview;
  /* Late loading: native cache already aliases all identical logical paths. */
  Handle before[2];
  memcpy(before, handles, sizeof(before));
  fail_portrait = atoi(argv[1]);
  release_spine = portrait_release;
  sync_portraits(actor);
  if (fail_portrait) {
    assert(!memcmp(before, handles, total * sizeof(*handles)));
    assert(portrait_loads == total);
    assert(portrait_releases == (fail_portrait == 2 ? total : 0));
    fail_portrait = 0;
  }
  unsigned old_releases = portrait_releases;
  change_preview(klass, actor); /* Existing matching rig still repairs portraits. */
  check_portraits(handles, total);
  assert(portrait_releases == old_releases + (unsigned)(atoi(argv[1]) ? total : 0));
  unsigned loaded = portrait_loads, released = portrait_releases;
  change_colour(actor, (int)total - 1);
  change_preview(klass, actor);
  change_colour(actor, 0);
  check_portraits(handles, total);
  assert(portrait_loads == loaded && portrait_releases == released);
  assert(previews == 2);
  /* No writes or loads when the native vector no longer matches the catalogue. */
  view.end = handles;
  memcpy(node + 0x28, &view, sizeof(view));
  sync_portraits(actor);
  assert(portrait_loads == loaded);
  for (unsigned i = 0; i < total; ++i)
    portrait_release(handles + i);
  assert(portrait_releases == released + total);
  free(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET));
  free(pointer_at(node, 0x60));
  clear_routes();
  clear_catalogs();
  puts("PORTRAIT_SOURCES_OK");
  return 0;
}
