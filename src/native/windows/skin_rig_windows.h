/* Included by skin_rig_patch.c after the shared source resolver. */
#include "windows_image.h"
#include "windows_patch.h"
#include "windows_profile.h"

static int windows_log(int descriptor, const char *format, ...) {
  (void)descriptor;
  char line[4096];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (length < 0)
    return length;
  OutputDebugStringA(line);
  return length;
}

static unsigned windows_colour_count(void *klass) {
  uintptr_t begin = (uintptr_t)pointer_at(klass, 0x5e0);
  uintptr_t end = (uintptr_t)pointer_at(klass, 0x5e8);
  if (end < begin || (end - begin) % 512 || (end - begin) / 512 > 512)
    return 0;
  return (unsigned)((end - begin) / 512);
}
static const char *windows_colour_path(void *klass, unsigned index) {
  return index < windows_colour_count(klass)
             ? (const char *)pointer_at(klass, 0x5e0) + index * 512
             : NULL;
}
static unsigned windows_alternate_count(void *device) {
  uintptr_t begin = (uintptr_t)pointer_at(device, 8);
  uintptr_t end = (uintptr_t)pointer_at(device, 16);
  if (end < begin || (end - begin) % 0x210 || (end - begin) / 0x210 > 4096)
    return 4097;
  return (unsigned)((end - begin) / 0x210);
}
static const void *windows_alternate_root(void *device, unsigned index) {
  return index < windows_alternate_count(device)
             ? (const char *)pointer_at(device, 8) + index * 0x210
             : NULL;
}
static void windows_match_device(NativeFileMatch *files, void *device,
                                 const char *directory, const char *pattern,
                                 int details, int order, int flags) {
  void (*find)(void *, NativeFileMatch *, const char *, const char *, int, int,
               bool) = pointer_at(pointer_at(device, 0), 0x48);
  find(device, files, directory, pattern, details, order, (flags & 4) != 0);
}
static PaletteRootMatch original_find_root;
static void windows_find_root(void *storage, NativeFileMatch *files,
                              void *device, unsigned root,
                              const char *directory, const char *pattern,
                              int details, int order, int flags) {
  void (*append)(NativeFileMatch *, const void *) =
      (void *)native_address(DD_APPEND_MATCH);
  match_palette_root(original_find_root, append, destroy_match, storage, files,
                     device, root, directory, pattern, details, order, flags);
}
static void windows_match_files(NativeFileMatch *files, int mount,
                                const char *directory, const char *pattern,
                                int details, int order, int flags) {
  (void)mount;
  void *device = data_device();
  windows_match_device(files, device, directory, pattern, details, order,
                       flags);
  unsigned count = windows_alternate_count(device);
  void (*find_root)(void *, NativeFileMatch *, void *, unsigned, const char *,
                    const char *, int, int, int) =
      (void *)(native_address(DD_FULL_PATH));
  void *storage = *(void **)(native_address(DD_STORAGE));
  for (unsigned i = count; i-- > 0;)
    find_root(storage, files, device, i, directory, pattern, details, order,
              flags);
}
static bool windows_full_path(void *storage, int mount, const char *request,
                              char *out, unsigned capacity) {
  (void)mount;
  unsigned char info[0x448] = {0};
  void (*query)(void *, void *, const char *, void *) =
      (void *)(native_address(DD_QUERY_FILE));
  query(storage, data_device(), request, info);
  if (!int_at(info, 0x440) || !memchr(info + 0x200, 0, 512))
    return false;
  const char *actual = (const char *)info + 0x200;
  if (strlen(actual) >= capacity)
    return false;
  strcpy(out, actual);
  for (char *p = out; *p; ++p)
    if (*p == '\\')
      *p = '/';
  return true;
}
static const char *windows_resource_name(void *manager, const Handle *handle) {
  if (!manager || handle->index < 0 || handle->generation < 0 ||
      (unsigned)handle->index >= (unsigned)int_at(manager, 0xa0))
    return NULL;
  const char *entry =
      (const char *)pointer_at(manager, 0x98) + (size_t)handle->index * 0x120;
  if (int_at(entry, 0x114) != handle->generation ||
      !memchr(entry + 0x10, 0, 256))
    return NULL;
  return entry + 0x10;
}
static void windows_release_spine(Handle *handle) {
  void *manager = *(void **)(native_address(DD_RESOURCE));
  void (*release)(void *, Handle *) =
      (void *)(native_address(DD_RELEASE_SPINE));
  if (manager && handle->index >= 0 && handle->generation >= 0)
    release(manager, handle);
  *handle = (Handle){0, -1, NULL};
}
static void discard_sprite(void *sprite) {
  /* Native deferred deletion queue, also used by Hero::LoadAssetsInternal. */
  void (*append)(void *, const void *) =
      (void *)(native_address(DD_DISCARD_SPRITE));
  append((void *)(native_address(DD_DEFERRED_LIST)), &sprite);
}
static const char *animation_name(void *sprite) {
  const void *animation = sprite ? pointer_at(sprite, 0) : NULL;
  return animation ? pointer_at(animation, 0) : "";
}
static float windows_animation_time(void *sprite) {
  unsigned index = (unsigned)int_at(sprite, 0xc);
  const char *first = pointer_at(sprite, 0x20);
  const char *end = pointer_at(sprite, 0x28);
  if (!first || end < first || index >= (size_t)(end - first) / 0x30)
    return 0;
  void *state = pointer_at(first + index * 0x30, 0x18);
  void *tracks = state ? pointer_at(state, 0x20) : NULL;
  void *track = tracks ? pointer_at(tracks, 0) : NULL;
  float time = 0;
  if (track)
    memcpy(&time, (char *)track + 0x28, sizeof(time));
  return time;
}
static int windows_has_animation(void *sprite, const char *name) {
  float (*length)(void *, const char *) =
      (void *)(native_address(DD_ANIMATION_LENGTH));
  return length(sprite, name) > 0;
}
static void start_animation(void *sprite, const char *name, float time) {
  const char *chosen = name && *name ? name : "idle";
  set_animation(sprite, chosen, 1, 1);
  float (*length)(void *, const char *) =
      (void *)(native_address(DD_ANIMATION_LENGTH));
  float seconds = length(sprite, chosen);
  void (*set_time)(void *, float) = (void *)(native_address(DD_SET_TIME));
  if (isfinite(time) && time >= 0 && seconds > 0)
    set_time(sprite, fmodf(time, seconds));
}
static void *new_actor_sprite(const Handles *original, int flag, void *actor) {
  const Route *route = find_route(actor, int_at(actor, 0xb68));
  if (!route && runtime_actor(actor))
    _exit(125);
  void *sprite = route ? make_sprite(actor_class(actor), route)
                       : native_new(original, flag);
  if (!sprite && runtime_actor(actor))
    _exit(125);
  dprintf(2, "DD_RIG_ACTOR_INIT actor=%p sprite=%p skin=%s\n", actor, sprite,
          route ? route->skin : "native");
  return sprite;
}
static void change_colour(void *actor, int colour) {
  const Route *route = find_route(actor, colour);
  void *old = pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
  if (!route && runtime_actor(actor))
    return;
  if (!route || !old || sprite_matches(old, route)) {
    apply_colour(actor, colour, route);
    return;
  }
  char name[256];
  snprintf(name, sizeof(name), "%s", animation_name(old));
  int stable = !strcmp(name, "idle") || !strcmp(name, "walk") ||
               !strcmp(name, "combat") || !*name;
  if (!stable || pointer_at(old, 0x38) != pointer_at(old, 0x40)) {
    dprintf(2, "DD_RIG_SKIN_REFUSED_BUSY actor=%p animation=%s\n", actor, name);
    return;
  }
  float time = *name ? windows_animation_time(old) : 0;
  void *sprite = make_sprite(actor_class(actor), route);
  if (!sprite)
    return;
  if (!windows_has_animation(sprite, *name ? name : "idle")) {
    discard_sprite(sprite);
    dprintf(2, "DD_RIG_SKIN_REFUSED_ANIMATION actor=%p animation=%s\n", actor,
            name);
    return;
  }
  /* MSVC's two event hash containers own heap sentinels, with no embedded
   * self pointers. Transfer their ownership as a pair, including dirty state,
   * so the native captured-Hero step callback survives without fabricating a
   * std::function using another compiler's C++ ABI. New empty containers go to
   * the old sprite and are reclaimed through native deferred deletion. */
  unsigned char callbacks[0x88];
  memcpy(callbacks, (char *)old + 0x50, sizeof(callbacks));
  memcpy((char *)old + 0x50, (char *)sprite + 0x50, sizeof(callbacks));
  memcpy((char *)sprite + 0x50, callbacks, sizeof(callbacks));
  memcpy((char *)actor + DD_ACTOR_SPRITE_OFFSET, &sprite, sizeof(sprite));
  apply_colour(actor, colour, route);
  start_animation(sprite, name, time);
  discard_sprite(old);
  dprintf(2, "DD_RIG_ACTOR_SWAP actor=%p skin=%s source=%s\n", actor,
          route->skin, route->origin);
}
static void change_preview(void *panel, void *actor) {
  const Route *route = find_route(actor, int_at(actor, 0xb68));
  if (!route && runtime_actor(actor))
    return;
  if (!route) {
    apply_preview(panel, actor, route);
    return;
  }
  void *head = *(void **)(native_address(DD_PREVIEW_ROOT));
  void *node = head ? pointer_at(head, 8) : NULL;
  unsigned id = (unsigned)int_at(actor_class(actor), 0xd4);
  for (unsigned depth = 0; node && depth < 64; ++depth) {
    if (*(unsigned char *)((char *)node + 0x19))
      break;
    unsigned key = (unsigned)int_at(node, 0x20);
    if (key != id) {
      node = pointer_at(node, id < key ? 0 : 0x10);
      continue;
    }
    void *old = pointer_at(node, 0x60);
    if (old && !sprite_matches(old, route)) {
      void *sprite = make_sprite(actor_class(actor), route);
      if (!sprite)
        return;
      memcpy((char *)node + 0x60, &sprite, sizeof(sprite));
      apply_preview(panel, actor, route);
      start_animation(sprite, "idle", 0);
      discard_sprite(old);
      dprintf(2, "DD_RIG_PREVIEW_SWAP actor=%p skin=%s source=%s\n", actor,
              route->skin, route->origin);
      return;
    }
    break;
  }
  apply_preview(panel, actor, route);
}

static void windows_absolute_jump(unsigned char *out, void *destination) {
  out[0] = 0x48;
  out[1] = 0xb8;
  uintptr_t value = (uintptr_t)destination;
  memcpy(out + 2, &value, 8);
  out[10] = 0xff;
  out[11] = 0xe0;
}
static unsigned char *windows_near_page(void) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  uintptr_t granularity = info.dwAllocationGranularity;
  uintptr_t aligned = base & ~(granularity - 1);
  for (uintptr_t offset = granularity; offset < 0x70000000;
       offset += granularity) {
    void *address = VirtualAlloc((void *)(aligned + offset), 4096,
                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (address)
      return address;
  }
  return NULL;
}
/* Available to debuggers and code tests, without persistent diagnostics. */
__declspec(dllexport) const char dd_patch_identity[] =
    "DarkestDungeonPatches.Win64.ProxyPlugin.v1";
__declspec(dllexport) volatile LONG dd_windows_ready;
__declspec(dllexport) struct {
  DWORD size;
  volatile LONG state;
  DWORD requested, applied, error;
} dd_patch_status = {sizeof(dd_patch_status), 0, 0, 0, 0};

static int windows_option(const char *name) {
  char value[16];
  DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
  return !length || length >= sizeof(value) || strcmp(value, "0");
}
static int windows_guard_sites(const DdWindowsImage *image,
                               const WindowsSite *sites, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const WindowsSite *site = sites + i;
    uintptr_t address = native_address(site->symbol);
    if (!dd_contains(image->ranges, image->count, address - base, site->length,
                     DD_CODE) ||
        !dd_guard((void *)address, site->expected, site->length, address,
                  native_address(DD_NATIVE_NEW)))
      return 0;
  }
  return 1;
}
static int windows_rig_plan(DdWindowsPatches *plan, unsigned char **allocated) {
  runtime_enabled = 1;
  preserve_palette_sources = 1;
  const char *map = getenv("DD_RIG_MAP");
  if (map && *map) {
    FILE *file = fopen(map, "r");
    if (!file)
      return 0;
    int valid = read_routes(file);
    fclose(file);
    if (!valid)
      return 0;
  }
  native_new = (void *)(native_address(DD_NATIVE_NEW));
  resource_name = windows_resource_name;
  set_animation = (void *)(native_address(DD_SET_ANIMATION));
  match_files = windows_match_files;
  match_device = windows_match_device;
  match_archive = (void *)(native_address(DD_MATCH_ARCHIVE));
  destroy_match = (void *)(native_address(DD_DESTROY_MATCH));
  colour_count = windows_colour_count;
  colour_path = windows_colour_path;
  alternate_count = windows_alternate_count;
  alternate_root = windows_alternate_root;
  full_path = windows_full_path;
  load_spine = (void *)(native_address(DD_LOAD_SPINE));
  release_spine = windows_release_spine;
  get_resource = (void *)(native_address(DD_GET_RESOURCE));
  unsigned char *stubs = windows_near_page();
  if (!stubs)
    return 0;
  *allocated = stubs;
  memcpy(stubs, "\x49\x89\xd8", 3); /* r8 = captured Hero in rbx */
  windows_absolute_jump(stubs + 3, new_actor_sprite);
  void *hooks[] = {NULL, change_colour, change_preview, override_textures,
                   windows_find_root};
  size_t hook_count = sizeof(windows_hook_sites) / sizeof(*windows_hook_sites);
  for (size_t i = 1; i < hook_count; ++i) {
    const WindowsSite *site = windows_hook_sites + i;
    size_t location = i * 128;
    memcpy(stubs + location, (void *)native_address(site->symbol),
           site->length);
    windows_absolute_jump(
        stubs + location + site->length,
        (void *)(native_address(site->symbol) + site->length));
    windows_absolute_jump(stubs + location + 64, hooks[i]);
  }
  original_colour = (void *)(stubs + 128);
  original_preview = (void *)(stubs + 256);
  original_override = (void *)(stubs + 384);
  original_find_root = (void *)(stubs + 512);
  DWORD previous;
  if (!VirtualProtect(stubs, 4096, PAGE_EXECUTE_READ, &previous) ||
      !FlushInstructionCache(GetCurrentProcess(), stubs, 4096))
    return 0;
  for (size_t i = 0; i < hook_count; ++i) {
    const WindowsSite *site = windows_hook_sites + i;
    unsigned char *address = (void *)(native_address(site->symbol));
    unsigned char *target = i ? stubs + i * 128 + 64 : stubs;
    int64_t delta =
        (int64_t)(uintptr_t)target - (int64_t)(uintptr_t)(address + 5);
    if (delta < INT32_MIN || delta > INT32_MAX)
      return 0;
    unsigned char replacement[16];
    memset(replacement, 0x90, sizeof(replacement));
    replacement[0] = i ? 0xe9 : 0xe8;
    int32_t displacement = (int32_t)delta;
    memcpy(replacement + 1, &displacement, 4);
    if (!windows_plan_edit(plan, address, replacement, site->length))
      return 0;
  }
  return 1;
}

__declspec(dllexport) void InitializeASI(void) {
  if (InterlockedCompareExchange(&dd_patch_status.state, 1, 0))
    return;
  base = (uintptr_t)GetModuleHandleW(NULL);
  unsigned groups =
      (windows_option("DD_LOADBUFFER_PATCH") ? DD_GROUP_RESOURCE : 0) |
      (windows_option("DD_NATIVE_RENDER") ? DD_GROUP_RENDER : 0) |
      (windows_option("DD_RIG_RUNTIME") ? DD_GROUP_RIG : 0);
  dd_patch_status.requested = groups;
  DdWindowsImage image = {0};
  const DdSignature *failed = NULL;
  int matches = 0;
  dd_patch_status.error = 1;
  if (!dd_windows_loaded_image(base, &image) ||
      !dd_resolve_profile(image.ranges, image.count, groups, &native_symbols,
                          &failed, &matches)) {
    dprintf(2, "DD_PATCH_SKIPPED feature=%s matches=%d\n",
            failed ? failed->name : "image_sections", matches);
    goto refused;
  }
  dd_patch_status.error = 2;
#define CHECK_SITES(table)                                                     \
  windows_guard_sites(&image, table, sizeof(table) / sizeof(*table))
  if (((groups & DD_GROUP_RESOURCE) && !CHECK_SITES(windows_resource_sites)) ||
      ((groups & DD_GROUP_RENDER) && !CHECK_SITES(windows_render_sites)) ||
      ((groups & DD_GROUP_RIG) &&
       (!native_fields_valid() || !CHECK_SITES(windows_hook_sites) ||
        !CHECK_SITES(windows_api_sites))))
    goto refused;
#undef CHECK_SITES
  DdWindowsPatches plan = {0};
  unsigned char *stubs = NULL;
  dd_patch_status.error = 3;
  if ((groups & DD_GROUP_RIG) && !windows_rig_plan(&plan, &stubs))
    goto rollback;
  if (groups & DD_GROUP_RESOURCE) {
    const uint32_t cap = DD_WINDOWS_NEW_CAP;
    for (size_t i = 0; i < sizeof(windows_resource_sites) / sizeof(WindowsSite);
         ++i) {
      const WindowsSite *site = windows_resource_sites + i;
      if (!windows_plan_edit(
              &plan, (void *)(native_address(site->symbol) + site->operand),
              &cap, sizeof(cap)))
        goto rollback;
    }
  }
  if (groups & DD_GROUP_RENDER)
    for (size_t i = 0; i < sizeof(windows_render_sites) / sizeof(WindowsSite);
         ++i) {
      const WindowsSite *site = windows_render_sites + i;
      if (!windows_plan_edit(&plan, (void *)native_address(site->symbol),
                             windows_render_replacements[i], site->length))
        goto rollback;
    }
  dd_patch_status.error = 4;
  if (!windows_commit(&plan))
    goto rollback;
  dd_patch_status.applied = groups;
  dd_patch_status.error = 0;
  InterlockedExchange(&dd_windows_ready, 1);
  InterlockedExchange(&dd_patch_status.state, 2);
  dprintf(2, "DD_PATCH_APPLIED groups=%u platform=windows_x64\n", groups);
  return;
rollback:
  if (stubs)
    (void)VirtualFree(stubs, 0, MEM_RELEASE);
refused:
  InterlockedExchange(&dd_patch_status.state, -1);
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reserved;
  (void)reason;
  return TRUE;
}
