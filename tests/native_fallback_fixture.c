/* Exercise constructor fallback through both production colour/preview hooks.
 */
#define _GNU_SOURCE
#include <stdlib.h>
static int refuse_tracking;
static void *fixture_calloc(size_t count, size_t size);
#define calloc fixture_calloc
#define main basic_skin_cycle_main
#include "skin_cycle_fixture.c"
#undef main
#undef calloc
static void *fixture_calloc(size_t count, size_t size) {
  return refuse_tracking && size == sizeof(ActorSprite) ? NULL
                                                        : calloc(count, size);
}

static int failure;
static unsigned native_created, previews, native_textures;
static Handles native_handles;
static unsigned char preview_node[0x80], preview_campaign[0xf00];
static void *preview_head = preview_campaign;
static unsigned restored_previews;
#ifndef _WIN32
static void *fixture_assets(void) { return preview_campaign; }
#endif
static bool checked_path(void *storage_arg, int mount, const char *path,
                         char *out, unsigned capacity) {
  return failure == 1 ? false
                      : resolve_file(storage_arg, mount, path, out, capacity);
}
static void *checked_get(void *manager, const Handle *handle) {
  return failure == 2 ? NULL : get(manager, handle);
}
static void *checked_create(const Handles *handles, int flag) {
  if (handles == &native_handles) {
    assert(flag == 7);
    ++native_created;
  } else if (handles == (const Handles *)(klass + 0xe0)) {
    assert(flag == 0);
    ++restored_previews;
  } else if (failure == 3)
    return NULL;
  return create(handles, flag);
}
static void checked_textures(void *view, const char *directory) {
  if (override_route) {
    textures(view, directory);
    return;
  }
  assert(!strcmp(directory, paths[0]));
  ++native_textures;
}
static void checked_colour(void *target, int index) {
  assert(!override_route && index == 0);
  override_textures(pointer_at(target, DD_ACTOR_SPRITE_OFFSET), paths[index]);
  memcpy((char *)target + 0xb68, &index, sizeof(index));
  ++applied;
}
static void checked_preview(void *panel, void *target) {
  assert(panel == klass);
  (void)target;
  override_textures(pointer_at(preview_node, 0x60), paths[0]);
  ++previews;
}
static void check_native(void) {
  unsigned before = applied, preview_before = previews, keys_before = key_count;
  change_colour(actor, 0);
  change_preview(klass, actor);
  assert(applied == before + 1 && previews == preview_before + 1);
  /* Native fallback must neither load custom rigs nor mask native requests. */
  assert(key_count == keys_before);
  assert(int_at(actor, 0xb68) == 0);
}
static void late_preview(void *panel, void *target) {
  assert(panel == klass && target == actor);
  const Route *route = find_route(target, int_at(target, 0xb68));
  assert(route && sprite_matches(pointer_at(preview_node, 0x60), route));
  ++previews;
}
static void late_colour(void *target, int colour) {
  if (pointer_at(target, DD_ACTOR_SPRITE_OFFSET))
    apply(target, colour);
  else {
    /* Native town heroes change the index without loading their actor sprite. */
    memcpy((char *)target + 0xb68, &colour, sizeof(colour));
    ++applied;
  }
}
static void check_late_actor(int mode) {
  clear_routes();
  clear_catalogs();
  failure = refuse_tracking = 0;
  original_colour = late_colour;
  original_override = textures;
  original_preview = late_preview;
  int colour = 0;
  memcpy(actor + 0xb68, &colour, sizeof(colour));
  void *sprite = mode == 2 ? NULL : create(&native_handles, 0);
  put_pointer(actor, DD_ACTOR_SPRITE_OFFSET, sprite);
#ifdef _WIN32
  if (sprite)
    put_pointer(sprite, 0x50, callback_owner);
#endif
  put_pointer(preview_node, 0x60, create(&native_handles, 0));
  assert(!actor_sprite_slot(actor, 0) && native_actor_sprite(actor));
  if (mode)
    change_preview(klass, actor);
  else {
    unsigned before = applied;
    put_pointer(sprite, 0x38, manager_pointer);
    change_colour(actor, 0);
    assert(native_actor_sprite(actor) && applied == before);
    assert(actor_sprite_slot(actor, 0)->pending);
    assert(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET) == sprite);
    put_pointer(sprite, 0x38, NULL);
    failure = 2; /* A failed adoption retains its native sprite and can retry. */
    change_colour(actor, 0);
    assert(native_actor_sprite(actor) && applied == before);
    assert(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET) == sprite);
    failure = 0;
    change_colour(actor, 0);
  }
  assert(!native_actor_sprite(actor));
  assert(!actor_sprite_slot(actor, 0)->pending);
  assert(int_at(actor, 0xb68) == 1); /* Official choices remain masked. */
  if (mode == 2)
    assert(!pointer_at(actor, DD_ACTOR_SPRITE_OFFSET));
  else {
    assert(sprite_matches(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET),
                          find_route(actor, 1)));
#ifdef _WIN32
    assert(pointer_at(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET), 0x50) ==
           callback_owner);
#endif
  }
  change_colour(actor, 2);
  assert(int_at(actor, 0xb68) == 2);
  change_preview(klass, actor);
  if (mode == 2) {
    /* Loading its real assets later must retain the newly selected skin. */
    assert(!pointer_at(actor, DD_ACTOR_SPRITE_OFFSET));
    put_pointer(actor, DD_ACTOR_SPRITE_OFFSET,
                new_actor_sprite(&native_handles, 7, actor));
    assert(!native_actor_sprite(actor));
    assert(sprite_matches(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET),
                          find_route(actor, 2)));
  }
  free(pointer_at(actor, DD_ACTOR_SPRITE_OFFSET));
  free(pointer_at(preview_node, 0x60));
}
int main(int argc, char **argv) {
  assert(argc == 2);
  failure = atoi(argv[1]);
  assert(failure >= 1 && failure <= 4);
  refuse_tracking = failure == 4;
  official = 1;
  total = 4;
  runtime_enabled = preserve_palette_sources = 1;
  dd_config.hide_vanilla_skins = 1;
  for (unsigned i = 0; i < total; ++i)
    snprintf(paths[i], sizeof(paths[i]), "heroes/alpha/alpha_%c/",
             !i ? 'Z' : 'A' + (int)i - 1);
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
  full_path = checked_path;
  native_new = checked_create;
  load_spine = load;
  release_spine = release;
  resource_name = resource;
  get_resource = checked_get;
  original_colour = checked_colour;
  original_override = checked_textures;
  set_animation = play;
  int first = official && dd_config.hide_vanilla_skins ? 1 : 0;
  memcpy(actor + 0xb68, &first, sizeof(first));

  original_preview = checked_preview;
  Handle handle = {0, 1, NULL};
  strcpy(keys[key_count++], "heroes/alpha/anim/alpha.sprite.idle.skel");
  native_handles = (Handles){&handle, &handle + 1, &handle + 1};
  memcpy(klass + 0xe0, &native_handles, sizeof(native_handles));
#ifdef _WIN32
  native_symbols.values[DD_PREVIEW_ROOT] = (uintptr_t)&preview_head;
  put_pointer(preview_campaign, 8, preview_node);
#else
  (void)preview_head;
  native_symbols.values[DD_ASSETS] = (uintptr_t)fixture_assets;
  put_pointer(preview_campaign, 0xe88, preview_node);
#endif
  put_pointer(preview_node, 0x60, create(&native_handles, 0));
  void *sprite = new_actor_sprite(&native_handles, 7, actor);
  assert(sprite && native_created == 1);
  put_pointer(actor, DD_ACTOR_SPRITE_OFFSET, sprite);
  assert(native_actor_sprite(actor));
  check_native();
  /* Another actor can use a custom route even while this actor is native. */
  unsigned char other[sizeof(actor)];
  memcpy(other, actor, sizeof(other));
  failure = 0;
  refuse_tracking = 0;
  void *custom = new_actor_sprite(&native_handles, 7, other);
  put_pointer(other, DD_ACTOR_SPRITE_OFFSET, custom);
  assert(custom && !native_actor_sprite(other));
  change_preview(klass, other); /* The shared cache now holds a custom rig. */
  assert(sprite_matches(pointer_at(preview_node, 0x60), find_route(other, 0)));
  check_native();
  assert(restored_previews == 1);
  free(custom);
  free(sprite);
  /* Rebuilding at the same actor address must replace the old fallback state.
   */
  custom = new_actor_sprite(&native_handles, 7, actor);
  put_pointer(actor, DD_ACTOR_SPRITE_OFFSET, custom);
  assert(custom && !native_actor_sprite(actor));
  free(custom);
  failure = 2;
  sprite = new_actor_sprite(&native_handles, 7, actor);
  put_pointer(actor, DD_ACTOR_SPRITE_OFFSET, sprite);
  assert(sprite && native_created == 2 && native_actor_sprite(actor));
  check_native();
  assert(native_textures == 6);
  free(sprite);
  free(pointer_at(preview_node, 0x60));
  clear_routes();
  clear_catalogs();
  check_late_actor(0);
  check_late_actor(1);
  check_late_actor(2);
  clear_routes();
  clear_catalogs();
  puts("NATIVE_FALLBACK_OK");
  return 0;
}
