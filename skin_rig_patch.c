#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Linux28212 only. Launcher verifies the full executable hash. */
typedef struct {
  int index, generation;
  void *cached;
} Handle;
typedef struct {
  Handle *begin, *end, *capacity;
} Handles;
typedef struct {
  char keys[256][256];
  size_t count;
} RigPlan;
typedef struct {
  char skin[256], hero[256], group[64], prefix[160];
  RigPlan *plan;
  int tested;
} Route;
static Route routes[128];
static size_t route_count;

static int path_join(char *out, size_t capacity, const char *a, const char *b) {
  size_t first = strlen(a), second = strlen(b);
  if (first >= capacity || second >= capacity - first)
    return 0;
  memmove(out, a, first);
  memmove(out + first, b, second + 1);
  return 1;
}
static int rig_files(const char *directory, char names[256][256],
                     size_t *count) {
  *count = 0;
  DIR *dir = opendir(directory);
  if (!dir)
    return errno == ENOENT ? 0 : -1;
  int valid = 1;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    size_t n = strlen(entry->d_name);
    if (n < 6 || strcmp(entry->d_name + n - 5, ".skel"))
      continue;
    /* Copies produced by the optional legacy packager are not source rigs. */
    if (strstr(entry->d_name, ".rig_vanilla.") ||
        strstr(entry->d_name, ".sprite.rig_"))
      continue;
    char key[256], atlas[512];
    struct stat info;
    if (*count == 256 || n >= 256 ||
        snprintf(key, sizeof(key), ">%s/%s", directory, entry->d_name) >=
            (int)sizeof(key) ||
        stat(key + 1, &info) || !S_ISREG(info.st_mode) || info.st_size == 0 ||
        info.st_size > 0x2000000 ||
        snprintf(atlas, sizeof(atlas), "%s/%.*s.atlas", directory, (int)n - 5,
                 entry->d_name) >= (int)sizeof(atlas) ||
        stat(atlas, &info) || !S_ISREG(info.st_mode) || info.st_size == 0) {
      valid = 0;
      break;
    }
    memcpy(names[(*count)++], entry->d_name, n + 1);
  }
  closedir(dir);
  /* Native class resources are ordered; make repeated discovery deterministic.
   */
  for (size_t i = 1; i < *count; ++i)
    for (size_t j = i; j && strcmp(names[j - 1], names[j]) > 0; --j) {
      char swap[256];
      memcpy(swap, names[j], sizeof(swap));
      memcpy(names[j], names[j - 1], sizeof(swap));
      memcpy(names[j - 1], swap, sizeof(swap));
    }
  return valid ? (*count ? 1 : 0) : -1;
}
static int image_name(const char *name) {
  const char *extension = strrchr(name, '.');
  return extension &&
         (!strcasecmp(extension, ".png") || !strcasecmp(extension, ".tga"));
}
static int atlas_uses_file(const char *atlas, const char *file) {
  FILE *input = fopen(atlas, "r");
  if (!input)
    return -1;
  char line[1024], page[1024] = {0};
  int found = 0, pages = 0, valid = 1;
  while (fgets(line, sizeof(line), input)) {
    if (!strchr(line, '\n') && !feof(input)) {
      valid = 0;
      break;
    }
    size_t length = strcspn(line, "\r\n");
    if (length >= sizeof(line)) {
      valid = 0;
      break;
    }
    line[length] = 0;
    if (*page && (!strncmp(line, "size:", 5) || !strncmp(line, "format:", 7) ||
                  !strncmp(line, "filter:", 7))) {
      ++pages;
      const char *leaf = strrchr(page, '/');
      leaf = leaf ? leaf + 1 : page;
      if (!strcmp(leaf, file))
        found = 1;
    }
    if (*line && !strchr(" \t", line[0]) && image_name(line))
      memcpy(page, line, strlen(line) + 1);
    else
      *page = 0;
  }
  valid = valid && !ferror(input) && pages > 0;
  fclose(input);
  return valid ? found : -1;
}
static int add_rig_key(RigPlan *plan, const char *key) {
  const char *leaf = strrchr(key, '/');
  if (!leaf || strlen(key) >= sizeof(plan->keys[0]))
    return 0;
  for (size_t i = 0; i < plan->count; ++i) {
    if (!strcmp(plan->keys[i], key))
      return 1;
    const char *other = strrchr(plan->keys[i], '/');
    if (other && !strcmp(other, leaf)) {
      dprintf(2, "DD_RIG_RESOURCE_CONFLICT first=%s other=%s\n", plan->keys[i],
              key);
      return 0;
    }
  }
  if (plan->count == 256)
    return 0;
  memcpy(plan->keys[plan->count++], key, strlen(key) + 1);
  return 1;
}
static void sort_plan(RigPlan *plan) {
  for (size_t i = 1; i < plan->count; ++i)
    for (size_t j = i; j && strcmp(plan->keys[j - 1], plan->keys[j]) > 0; --j) {
      char swap[256];
      memcpy(swap, plan->keys[j], sizeof(swap));
      memcpy(plan->keys[j], plan->keys[j - 1], sizeof(swap));
      memcpy(plan->keys[j - 1], swap, sizeof(swap));
    }
}
#ifndef DD_PATCH_SELFTEST
static int same_plan(const RigPlan *a, const RigPlan *b) {
  if (!a || !b || a->count != b->count)
    return 0;
  for (size_t i = 0; i < a->count; ++i)
    if (strcmp(a->keys[i], b->keys[i]))
      return 0;
  return 1;
}
#endif
static int matching_rigs(const char *directory, const char *file,
                         RigPlan *plan) {
  char names[256][256];
  size_t count;
  int available = rig_files(directory, names, &count);
  if (available != 1)
    return available;
  for (size_t i = 0; i < count; ++i) {
    char atlas[512], key[256];
    size_t length = strlen(names[i]);
    int n = snprintf(atlas, sizeof(atlas), "%s/%.*s.atlas", directory,
                     (int)length - 5, names[i]);
    if (n < 0 || (size_t)n >= sizeof(atlas))
      return -1;
    int uses = atlas_uses_file(atlas, file);
    if (uses < 0)
      return -1;
    if (!uses)
      continue;
    n = snprintf(key, sizeof(key), ">%s/%s", directory, names[i]);
    if (n < 0 || (size_t)n >= sizeof(key) || !add_rig_key(plan, key))
      return -1;
  }
  return plan->count ? 1 : 0;
}
/* Match each texture to its atlas and skeleton. Independent actions may come
 * from different providers, including official DLC. */
static int source_rigs(const char *skin, const char *file, const char *full,
                       RigPlan *plan) {
  char tail[512], origin[512], hero[256], candidate[512];
  if (strncmp(skin, "heroes/", 7) ||
      !path_join(tail, sizeof(tail), skin, "/anim/") ||
      !path_join(tail, sizeof(tail), tail, file))
    return -1;
  size_t n = strlen(full), length = strlen(tail);
  if (n < length || strcmp(full + n - length, tail) ||
      n - length >= sizeof(origin))
    return -1;
  memcpy(origin, full, n - length);
  origin[n - length] = 0;
  if (*origin && origin[strlen(origin) - 1] != '/')
    return -1;
  const char *separator = strchr(skin + 7, '/');
  if (!separator || (size_t)(separator - skin) >= sizeof(hero))
    return -1;
  memcpy(hero, skin, (size_t)(separator - skin));
  hero[separator - skin] = 0;
  for (int level = 0; level < 3; ++level) {
    const char *root = level == 2 ? "" : origin;
    const char *relative = level == 0 ? skin : hero;
    char directory[512];
    if (!path_join(directory, sizeof(directory), root, relative) ||
        !path_join(candidate, sizeof(candidate), directory, "/anim"))
      return -1;
    int found = matching_rigs(candidate, file, plan);
    if (found < 0)
      return -1; /* Broken local rigs must not silently fall through. */
    if (found)
      return 1;
  }
  return 0; /* An unrelated PNG in anim/ does not select a body skeleton. */
}
#ifdef DD_PATCH_SELFTEST
static int source_rig(const char *skin, const char *file, const char *full,
                      char out[512]) {
  RigPlan plan = {0};
  if (source_rigs(skin, file, full, &plan) != 1)
    return 0;
  sort_plan(&plan);
  memcpy(out, plan.keys[0] + 1, strlen(plan.keys[0]));
  *strrchr(out, '/') = 0;
  return 1;
}
#endif

static int same_hero(const Route *a, const Route *b) {
  return a && b && !strcmp(a->hero, b->hero);
}
static int same_group(const Route *a, const Route *b) {
  return same_hero(a, b) && !strcmp(a->group, b->group);
}
static const Route *route_for_name(const char *name, const Route *scope) {
  if (!name || !scope)
    return NULL;
  const char *hero_path = strstr(name, "heroes/");
  if (hero_path && (hero_path == name || hero_path[-1] == '/')) {
    size_t n = strlen(scope->hero);
    if (strncmp(hero_path, scope->hero, n) || hero_path[n] != '/')
      return NULL;
  }
  const char *leaf = strrchr(name, '/');
  leaf = leaf ? leaf + 1 : name;
  const Route *best = NULL;
  size_t length = 0;
  for (size_t i = 0; i < route_count; ++i) {
    size_t n = strlen(routes[i].prefix);
    if (same_hero(scope, routes + i) && n > length &&
        !strncmp(leaf, routes[i].prefix, n)) {
      best = routes + i;
      length = n;
    }
  }
  return best;
}
static int valid_token(const char *text) {
  if (!((*text >= 'A' && *text <= 'Z') || (*text >= 'a' && *text <= 'z') ||
        (*text >= '0' && *text <= '9') || *text == '_'))
    return 0;
  for (const char *p = text; *p; ++p)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-'))
      return 0;
  return 1;
}
static int read_routes(FILE *file) {
  route_count = 0;
  char line[1024];
  while (fgets(line, sizeof(line), file)) {
    if (!strchr(line, '\n') && !feof(file))
      return 0;
    const char *p = line;
    while (*p && strchr(" \t\r\n\v\f", *p))
      ++p;
    if (!*p || *p == '#')
      continue;
    Route route = {0};
    char extra;
    if (route_count == 128 ||
        sscanf(p, "%255s %63s %159s %c", route.skin, route.group, route.prefix,
               &extra) != 3 ||
        strncmp(route.skin, "heroes/", 7) || !valid_token(route.group) ||
        !valid_token(route.prefix))
      return 0;
    char *separator = strchr(route.skin + 7, '/');
    if (!separator || !valid_token(separator + 1))
      return 0;
    size_t length = (size_t)(separator - route.skin);
    memcpy(route.hero, route.skin, length);
    if (!valid_token(route.hero + 7))
      return 0;
    for (size_t i = 0; i < route_count; ++i) {
      if (!strcmp(routes[i].skin, route.skin))
        return 0;
      if (same_hero(routes + i, &route) &&
          ((!strcmp(routes[i].group, route.group) &&
            strcmp(routes[i].prefix, route.prefix)) ||
           (!strcmp(routes[i].prefix, route.prefix) &&
            strcmp(routes[i].group, route.group))))
        return 0;
    }
    routes[route_count++] = route;
  }
  return !ferror(file) && route_count > 0;
}

#ifndef DD_PATCH_SELFTEST
static uintptr_t base;
static void (*original_colour)(void *, int);
static void (*original_preview)(void *, void *);
static void (*original_override)(void *, const char *);
static void *(*native_new)(const Handles *, int);
static const char *(*resource_name)(void *, const Handle *);
static void (*set_animation)(void *, const char *, int, int);
static int runtime_enabled;
typedef struct {
  char (*begin)[256], (*end)[256], (*capacity)[256];
} NativeFileMatch;
static void (*match_files)(NativeFileMatch *, int, const char *, const char *,
                           int, int, int);
static void (*destroy_match)(NativeFileMatch *);
static bool (*full_path)(void *, int, const char *, char *, unsigned);
/* C++ nontrivial Handle return uses an explicit sret pointer on this ABI. */
static void (*load_spine)(Handle *, const char *, int, const void *);
static void (*release_spine)(Handle *);
static void *(*get_resource)(void *, const Handle *);

static void *pointer_at(const void *p, size_t offset) {
  void *v;
  memcpy(&v, (const char *)p + offset, sizeof(v));
  return v;
}
static int int_at(const void *p, size_t offset) {
  int v;
  memcpy(&v, (const char *)p + offset, 4);
  return v;
}
static void *actor_class(void *actor) {
  void *table = pointer_at(actor, 0);
  void *(*get_class)(void *) = pointer_at(table, 0x38);
  return get_class(actor);
}
static int runtime_actor(void *actor) {
  unsigned (*count)(void *) = (void *)(base + 0xd7b130);
  const char *(*path)(void *, unsigned) = (void *)(base + 0xd7b150);
  void *klass = actor_class(actor);
  if (!count(klass))
    return 0;
  const char *skin = path(klass, 0);
  return skin && !strncmp(skin, "heroes/", 7);
}
static const Route *runtime_route(const char *skin) {
  char palette[512];
  RigPlan plan = {0};
  if (strlen(skin) >= sizeof(routes[0].skin) ||
      !path_join(palette, sizeof(palette), skin, "/anim/") ||
      strncmp(skin, "heroes/", 7))
    return NULL;
  const char *separator = strchr(skin + 7, '/');
  if (!separator || strchr(separator + 1, '/'))
    return NULL;
  NativeFileMatch files = {0};
  match_files(&files, 1, palette, ".*\\.(png|PNG|tga|TGA)", 0, 0, 2);
  ptrdiff_t count = files.begin ? files.end - files.begin : 0;
  int valid = count > 0 && count <= 4096;
  void *storage = *(void **)(base + 0x1d744f0);
  for (ptrdiff_t i = 0; valid && i < count; ++i) {
    char request[512], actual[512] = {0};
    RigPlan candidates = {0};
    const char *file = files.begin[i];
    if (!memchr(file, 0, 256) || strchr(file, '/') || strchr(file, '\\') ||
        !path_join(request, sizeof(request), palette, file) ||
        !full_path(storage, 1, request, actual, sizeof(actual))) {
      valid = 0;
      break;
    }
    int found = source_rigs(skin, file, actual, &candidates);
    if (found < 0) {
      dprintf(2, "DD_RIG_FILE_REFUSED skin=%s file=%s source=%s\n", skin, file,
              actual);
      valid = 0;
      break;
    }
    if (!found) {
      dprintf(2, "DD_RIG_FILE_UNUSED skin=%s file=%s source=%s\n", skin, file,
              actual);
      continue;
    }
    for (size_t j = 0; valid && j < candidates.count; ++j) {
      dprintf(2, "DD_RIG_FILE_SOURCE skin=%s file=%s source=%s rig=%s\n", skin,
              file, actual, candidates.keys[j]);
      valid = add_rig_key(&plan, candidates.keys[j]);
    }
  }
  destroy_match(&files);
  if (!valid || !plan.count) {
    dprintf(2, "DD_RIG_SOURCE_REFUSED skin=%s\n", skin);
    return NULL;
  }
  sort_plan(&plan);
  for (size_t i = 0; i < route_count; ++i)
    if (!strcmp(routes[i].skin, skin) && same_plan(routes[i].plan, &plan))
      return routes + i;
  if (route_count == 128) {
    dprintf(2, "DD_RIG_SOURCE_CACHE_FULL skin=%s\n", skin);
    return NULL;
  }
  RigPlan *owned = malloc(sizeof(*owned));
  if (!owned)
    return NULL;
  memcpy(owned, &plan, sizeof(plan));
  Route *route = routes + route_count++;
  route->plan = owned;
  snprintf(route->skin, sizeof(route->skin), "%s", skin);
  memcpy(route->hero, skin, (size_t)(separator - skin));
  memcpy(route->group, "runtime", sizeof("runtime"));
  dprintf(2, "DD_RIG_SOURCE_RESOLVED skin=%s resources=%zu first=%s\n", skin,
          plan.count, plan.keys[0]);
  return route;
}

static const Route *find_route(void *actor, int colour) {
  void *klass = actor_class(actor);
  unsigned (*count)(void *) = (void *)(base + 0xd7b130);
  const char *(*path)(void *, unsigned) = (void *)(base + 0xd7b150);
  if (colour < 0)
    colour = 0;
  if ((unsigned)colour >= count(klass))
    return NULL;
  const char *skin = path(klass, (unsigned)colour);
  if (!skin)
    return NULL;
  size_t skin_length = strlen(skin);
  while (skin_length &&
         (skin[skin_length - 1] == '/' || skin[skin_length - 1] == '\\'))
    --skin_length;
  if (runtime_enabled) {
    char normalized[256];
    if (skin_length >= sizeof(normalized))
      return NULL;
    memcpy(normalized, skin, skin_length);
    normalized[skin_length] = 0;
    return runtime_route(normalized);
  }
  for (size_t i = 0; i < route_count; ++i)
    if (strlen(routes[i].skin) == skin_length &&
        !strncmp(skin, routes[i].skin, skin_length))
      return routes + i;
  return NULL;
}
static const Route *resource_route(const Handle *handle, const Route *scope) {
  void *manager = *(void **)(base + 0x31c2b58);
  const char *name = resource_name(manager, handle);
  return route_for_name(name, scope);
}
static size_t select_handles(void *klass, const Route *route, Handle *out,
                             size_t limit) {
  Handles *all = (void *)((char *)klass + 0xe0);
  if (!all->begin || all->end < all->begin || all->end - all->begin > 512)
    return 0;
  size_t count = 0;
  for (const Handle *h = all->begin; h < all->end; ++h) {
    if (!same_group(route, resource_route(h, route)))
      continue;
    if (count == limit)
      return 0;
    out[count++] = *h;
  }
  return count;
}
static void *make_sprite(void *klass, const Route *route) {
  Handle selected[256];
  if (route->plan) {
    size_t count = route->plan->count, loaded = 0;
    void *manager = *(void **)(base + 0x31c2b58);
    const uint64_t empty_callback[4] __attribute__((aligned(16))) = {0};
    int valid = 1;
    for (size_t i = 0; valid && i < count; ++i) {
      const char *key = route->plan->keys[i];
      load_spine(selected + i, key, 0, empty_callback);
      ++loaded;
      const char *registered = resource_name(manager, selected + i);
      /* Refuse native hash aliases instead of reusing another source's rig. */
      valid = registered && !strcmp(registered, key) &&
              get_resource(manager, selected + i);
    }
    Handles view = {selected, selected + loaded, selected + loaded};
    void *sprite = valid ? native_new(&view, 0) : NULL;
    for (size_t i = 0; i < loaded; ++i)
      release_spine(selected + i);
    dprintf(2, "DD_RIG_RUNTIME_CREATED sprite=%p first=%s resources=%zu\n",
            sprite, route->plan->keys[0], loaded);
    return sprite;
  }
  size_t count = select_handles(klass, route, selected, 256);
  if (!count) {
    dprintf(2, "DD_RIG_UNAVAILABLE group=%s class=%p\n", route->group, klass);
    return NULL;
  }
  Handles view = {selected, selected + count, selected + count};
  void *sprite = native_new(&view, 0);
  dprintf(2, "DD_RIG_CREATED sprite=%p group=%s resources=%zu\n", sprite,
          route->group, count);
  return sprite;
}
static int sprite_matches(void *sprite, const Route *route) {
  if (!sprite)
    return 0;
  const Handle *first = pointer_at(sprite, 0x20);
  if (!first || (void *)first == pointer_at(sprite, 0x28))
    return 0;
  if (route->plan) {
    uintptr_t begin = (uintptr_t)first;
    uintptr_t end = (uintptr_t)pointer_at(sprite, 0x28);
    /* PerSpineDataState is 0x30 bytes, with its Handle at offset zero. */
    if (end < begin || (end - begin) % 0x30 ||
        (end - begin) / 0x30 != route->plan->count)
      return 0;
    void *manager = *(void **)(base + 0x31c2b58);
    for (size_t i = 0; i < route->plan->count; ++i) {
      const Handle *handle = (const void *)(begin + i * 0x30);
      const char *name = resource_name(manager, handle);
      if (!name || strcmp(name, route->plan->keys[i]))
        return 0;
    }
    return 1;
  }
  return same_group(resource_route(first, route), route);
}
/* Native colour paths can contain repeated separators. The OS accepts them,
 * but the Mod storage index uses exact logical names. Preserve the final slash:
 * OverrideAtlasTextures concatenates its directory and page name with %s%s. */
static void override_textures(void *sprite, const char *directory) {
  if (!runtime_enabled || !directory || strncmp(directory, "heroes/", 7)) {
    original_override(sprite, directory);
    return;
  }
  char normalized[512];
  size_t count = 0;
  for (const char *p = directory; *p; ++p) {
    if (*p == '/' && count && normalized[count - 1] == '/')
      continue;
    if (count == sizeof(normalized) - 1) {
      dprintf(2, "DD_RIG_TEXTURE_PATH_REFUSED reason=too_long\n");
      return;
    }
    normalized[count++] = *p;
  }
  normalized[count] = 0;
  if (strcmp(directory, normalized))
    dprintf(2, "DD_RIG_TEXTURE_PATH_NORMALIZED from=%s to=%s\n", directory,
            normalized);
  original_override(sprite, normalized);
}
#ifdef DD_RIG_RUNTIME_TEST
/* Offline adapter: exercise discovery and native ownership calls with fake
 * APIs. */
void dd_runtime_test_bind(
    uintptr_t image_base,
    void (*match)(NativeFileMatch *, int, const char *, const char *, int, int,
                  int),
    void (*destroy)(NativeFileMatch *),
    bool (*resolve)(void *, int, const char *, char *, unsigned),
    void (*load)(Handle *, const char *, int, const void *),
    void (*release)(Handle *), void *(*get)(void *, const Handle *),
    void *(*create)(const Handles *, int),
    const char *(*name)(void *, const Handle *)) {
  base = image_base;
  for (size_t i = 0; i < route_count; ++i)
    free(routes[i].plan);
  route_count = 0;
  memset(routes, 0, sizeof(routes));
  match_files = match;
  destroy_match = destroy;
  full_path = resolve;
  load_spine = load;
  release_spine = release;
  get_resource = get;
  native_new = create;
  resource_name = name;
}
void *dd_runtime_test_sprite(const char *skin) {
  const Route *route = runtime_route(skin);
  return route ? make_sprite(NULL, route) : NULL;
}
int dd_runtime_test_matches(void *sprite, const char *skin) {
  const Route *route = runtime_route(skin);
  return route && sprite_matches(sprite, route);
}
void dd_runtime_test_override(void *sprite, const char *directory,
                              void (*native)(void *, const char *)) {
  runtime_enabled = 1;
  original_override = native;
  override_textures(sprite, directory);
}
#endif
static void discard_sprite(void *sprite) {
  void (*destroy)(void **) = (void *)(base + 0xa6f170);
  destroy(&sprite); /* native deferred deletion, not a foreign free */
}
static const char *animation_name(void *sprite) {
  if (!sprite || !pointer_at(sprite, 0))
    return "";
  const char *(*name)(void *) = (void *)(base + 0xa70690);
  return name(sprite);
}
static void start_animation(void *sprite, const char *name, float time) {
  set_animation(sprite, name && *name ? name : "idle", 1, 1);
  float (*length)(void *, const char *) = (void *)(base + 0xa706a0);
  float seconds = length(sprite, name && *name ? name : "idle");
  void (*set_time)(void *, float) = (void *)(base + 0xa704c0);
  if (isfinite(time) && time >= 0 && seconds > 0)
    set_time(sprite, fmodf(time, seconds));
}
static void native_two_sprite_test(void *klass, const Route *scope) {
  if (scope->tested)
    return;
  const Route *a = NULL, *b = NULL;
  for (size_t i = 0; i < route_count; ++i) {
    if (!same_hero(scope, routes + i))
      continue;
    if (!a)
      a = routes + i;
    else if (!same_group(a, routes + i)) {
      b = routes + i;
      break;
    }
  }
  if (!a || !b)
    return;
  Handle probe[256];
  if (!select_handles(klass, a, probe, 256) ||
      !select_handles(klass, b, probe, 256))
    return;
  for (size_t i = 0; i < route_count; ++i)
    if (same_hero(scope, routes + i))
      routes[i].tested = 1;
  void *left = make_sprite(klass, a), *right = make_sprite(klass, b);
  if (left && right) {
    set_animation(left, "idle", 1, 1);
    set_animation(right, "idle", 1, 1);
    void *replacement = make_sprite(klass, b);
    int pass = replacement && left != right && replacement != right &&
               sprite_matches(left, a) && sprite_matches(right, b) &&
               sprite_matches(replacement, b);
    if (replacement) {
      set_animation(replacement, "idle", 1, 1);
      discard_sprite(replacement);
    }
    dprintf(
        2,
        "DD_RIG_TWO_SPRITE_TEST %s left=%p:%s right=%p:%s right_unchanged=%d\n",
        pass ? "PASS" : "FAIL", left, a->group, right, b->group,
        sprite_matches(right, b));
  }
  if (left)
    discard_sprite(left);
  if (right)
    discard_sprite(right);
}
static void *new_actor_sprite(const Handles *original, int flag, void *actor) {
  const Route *route = find_route(actor, int_at(actor, 0xb68));
  if (!route && runtime_enabled && runtime_actor(actor))
    _exit(125);
  if (!route)
    return native_new(original, flag);
  void *klass = actor_class(actor);
  void *sprite = make_sprite(klass, route);
  if (!sprite && runtime_enabled && runtime_actor(actor))
    _exit(125);
  if (!sprite)
    return native_new(original, flag);
  dprintf(2, "DD_RIG_ACTOR_INIT actor=%p sprite=%p skin=%s group=%s\n", actor,
          sprite, route->skin, route->group);
  if (!runtime_enabled)
    native_two_sprite_test(klass, route);
  return sprite;
}
static void change_colour(void *actor, int colour) {
  const Route *route = find_route(actor, colour);
  void *old = pointer_at(actor, 0x1070);
  if (!route && runtime_enabled && runtime_actor(actor))
    return;
  if (!route || !old || sprite_matches(old, route)) {
    original_colour(actor, colour);
    return;
  }
  char name[256];
  snprintf(name, sizeof(name), "%s", animation_name(old));
  int stable = !strcmp(name, "idle") || !strcmp(name, "walk") ||
               !strcmp(name, "combat") || !name[0];
  int callbacks_empty = pointer_at(old, 0x38) == pointer_at(old, 0x40);
  if (!stable || !callbacks_empty) {
    dprintf(2, "DD_RIG_SKIN_REFUSED_BUSY actor=%p anim=%s callbacks_empty=%d\n",
            actor, name, callbacks_empty);
    return; /* keep both old skin and old rig; do not mix incompatible data */
  }
  float (*get_time)(void *) = (void *)(base + 0xa70490);
  float time = name[0] ? get_time(old) : 0;
  void *sprite = make_sprite(actor_class(actor), route);
  if (!sprite)
    return;
  bool (*has_animation)(void *, const char *) = (void *)(base + 0xa70a60);
  if (!has_animation(sprite, name[0] ? name : "idle")) {
    discard_sprite(sprite);
    dprintf(2, "DD_RIG_SKIN_REFUSED_ANIMATION actor=%p animation=%s\n", actor,
            name);
    return;
  }
  void (*set_sprite)(void *, void *) = (void *)(base + 0xd660c0);
  set_sprite(actor, sprite);
  /* Exactly the same captured-Hero step callback constructed by
   * Hero::LoadAssetsInternal. */
  uint64_t callback[4] __attribute__((aligned(16))) = {
      (uintptr_t)actor, 0, base + 0xf00060, base + 0xf00050};
  void (*add_step)(void *, unsigned, void *) = (void *)(base + 0xa70ce0);
  add_step(sprite, *(unsigned *)(base + 0x37be400), callback);
  original_colour(actor, colour);
  start_animation(sprite, name, time);
  discard_sprite(old);
  dprintf(2,
          "DD_RIG_ACTOR_SWAP actor=%p old=%p new=%p skin=%s group=%s anim=%s\n",
          actor, old, sprite, route->skin, route->group, name);
}
static void change_preview(void *panel, void *actor) {
  const Route *route = find_route(actor, int_at(actor, 0xb68));
  if (!route && runtime_enabled && runtime_actor(actor))
    return;
  if (!route) {
    original_preview(panel, actor);
    return;
  }
  void *(*assets)(void) = (void *)(base + 0x1170d60);
  void *campaign = assets(), *klass = actor_class(actor);
  if (!campaign) {
    original_preview(panel, actor);
    return;
  }
  unsigned id = (unsigned)int_at(klass, 0xd4);
  void *node = pointer_at(campaign, 0xe88), *found = NULL;
  for (int depth = 0; node && depth < 64; ++depth) {
    unsigned key = (unsigned)int_at(node, 0x20);
    if (key == id) {
      found = node;
      break;
    }
    node = pointer_at(node, id < key ? 0x10 : 0x18);
  }
  if (found) {
    void *old = pointer_at(found, 0x60);
    if (old && !sprite_matches(old, route)) {
      void *sprite = make_sprite(klass, route);
      if (sprite) {
        memcpy((char *)found + 0x60, &sprite, sizeof(sprite));
        original_preview(panel, actor);
        start_animation(sprite, "idle", 0);
        discard_sprite(old);
        dprintf(2, "DD_RIG_PREVIEW_SWAP hero=%p old=%p new=%p group=%s\n",
                actor, old, sprite, route->group);
        return;
      }
    }
  }
  original_preview(panel, actor);
}

static int main_image(struct dl_phdr_info *info, size_t size, void *unused) {
  (void)size;
  (void)unused;
  if (info->dlpi_name && *info->dlpi_name)
    return 0;
  base = info->dlpi_addr;
  return 1;
}
static void jump_absolute(unsigned char *out, void *destination) {
  out[0] = 0x48;
  out[1] = 0xb8;
  uintptr_t value = (uintptr_t)destination;
  memcpy(out + 2, &value, 8);
  out[10] = 0xff;
  out[11] = 0xe0;
}
static void install_jump(uintptr_t site, void *stub) {
  int64_t delta = (uintptr_t)stub - (site + 5);
  if (delta < INT32_MIN || delta > INT32_MAX)
    _exit(125);
  unsigned char *p = (void *)site;
  p[0] = 0xe9;
  int32_t offset = (int32_t)delta;
  memcpy(p + 1, &offset, 4);
}
__attribute__((constructor)) static void init(void) {
  const char *map = getenv("DD_RIG_MAP");
  const char *runtime = getenv("DD_RIG_RUNTIME");
  runtime_enabled = runtime && !strcmp(runtime, "1");
  if ((!map || !*map) && !runtime_enabled)
    return;
  char executable[4096];
  ssize_t n = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (n <= 0 || n >= (ssize_t)sizeof(executable) - 1)
    return;
  executable[n] = 0;
  const char *name = strrchr(executable, '/');
  if (!name || strcmp(name + 1, "darkest.bin.x86_64"))
    return;
  if (!dl_iterate_phdr(main_image, NULL))
    _exit(125);
  if (!runtime_enabled) {
    FILE *file = fopen(map, "r");
    if (!file)
      _exit(125);
    int valid = read_routes(file);
    fclose(file);
    if (!valid)
      _exit(125);
  }
  uintptr_t sites[4] = {base + 0xefc214, base + 0xd66e80, base + 0x11a8400,
                        base + 0xa70e10};
  const unsigned char expected[4][10] = {
      {0xe8, 0xf7, 0x2e, 0xb7, 0xff},
      {0x41, 0x57, 0x41, 0x56, 0x53},
      {0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x08, 0x02, 0, 0},
      {0x55, 0x41, 0x57, 0x41, 0x56}};
  size_t lengths[4] = {5, 5, 10, 5};
  size_t site_count = runtime_enabled ? 4 : 3;
  for (size_t i = 0; i < site_count; ++i)
    if (memcmp((void *)sites[i], expected[i], lengths[i])) {
      dprintf(2, "DD_RIG_PATCH_REFUSED site=%zx\n", sites[i]);
      _exit(125);
    }
  if (runtime_enabled) {
    const struct {
      uintptr_t address;
      const char *expected;
      size_t length;
    } api[] = {{0xa5a7d0, "\x55\x53\x48\x81\xec\x48\x04\x00\x00", 9},
               {0xc27c90, "\x55\x41\x57\x41\x56\x41\x55\x41\x54\x53", 10},
               {0xc27d90, "\x48\x89\xf8\x48\x8b\x3f\x48\x85\xff", 9},
               {0xc3a260, "\x41\x57\x41\x56\x41\x54\x53", 7},
               {0xa63a60, "\x83\x3f\x00\x78\x38", 5},
               {0xc3f3f0, "\x41\x56\x53\x50\x48\x63\x06", 7},
               {0xa70a60, "\x55\x41\x57\x41\x56\x41\x55\x41\x54\x53\x50", 11}};
    for (size_t i = 0; i < sizeof(api) / sizeof(*api); ++i)
      if (memcmp((void *)(base + api[i].address), api[i].expected,
                 api[i].length)) {
        dprintf(2, "DD_RIG_API_REFUSED site=%zx\n", api[i].address);
        _exit(125);
      }
  }
  unsigned char *stubs = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (stubs == MAP_FAILED)
    _exit(125);
  /* Preserve original calling convention; RBX at this call site is the Hero. */
  memcpy(stubs, "\x48\x89\xda", 3);
  jump_absolute(stubs + 3, new_actor_sprite);
  memcpy(stubs + 64, expected[1], 5);
  jump_absolute(stubs + 69, (void *)(sites[1] + 5));
  jump_absolute(stubs + 96, change_colour);
  memcpy(stubs + 128, expected[2], 10);
  jump_absolute(stubs + 138, (void *)(sites[2] + 10));
  jump_absolute(stubs + 160, change_preview);
  if (runtime_enabled) {
    memcpy(stubs + 192, expected[3], 5);
    jump_absolute(stubs + 197, (void *)(sites[3] + 5));
    jump_absolute(stubs + 224, override_textures);
    original_override = (void *)(stubs + 192);
  }
  original_colour = (void *)(stubs + 64);
  original_preview = (void *)(stubs + 128);
  native_new = (void *)(base + 0xa6f110);
  resource_name = (void *)(base + 0xc3e910);
  set_animation = (void *)(base + 0xa6f2f0);
  match_files = (void *)(base + 0xc27c90);
  destroy_match = (void *)(base + 0xc27d90);
  full_path = (void *)(base + 0xa5a7d0);
  load_spine = (void *)(base + 0xc3a260);
  release_spine = (void *)(base + 0xa63a60);
  get_resource = (void *)(base + 0xc3f3f0);
  if (mprotect(stubs, 4096, PROT_READ | PROT_EXEC))
    _exit(125);
  long page_size = sysconf(_SC_PAGESIZE);
  uintptr_t pages[4];
  for (size_t i = 0; i < site_count; ++i) {
    pages[i] = sites[i] & ~((uintptr_t)page_size - 1);
    if (mprotect((void *)pages[i], (size_t)page_size, PROT_READ | PROT_WRITE))
      _exit(125);
  }
  install_jump(
      sites[0],
      stubs); /* CALL must stay CALL, keeping the native return address. */
  *(unsigned char *)sites[0] = 0xe8;
  install_jump(sites[1], stubs + 96);
  install_jump(sites[2], stubs + 160);
  if (runtime_enabled)
    install_jump(sites[3], stubs + 224);
  for (size_t i = 0; i < site_count; ++i) {
    __builtin___clear_cache((char *)pages[i], (char *)(pages[i] + page_size));
    if (mprotect((void *)pages[i], (size_t)page_size, PROT_READ | PROT_EXEC))
      _exit(125);
  }
  dprintf(2,
          "DD_RIG_PATCH_APPLIED mode=%s routes=%zu body=instance "
          "preview=selected_hero\n",
          runtime_enabled ? "runtime_source" : "map", route_count);
}
#else
static int test_map(const char *text) {
  FILE *file = fmemopen((void *)text, strlen(text), "r");
  if (!file)
    return 0;
  int result = read_routes(file);
  fclose(file);
  return result;
}
int main(int argc, char **argv) {
  if (argc == 5 && !strcmp(argv[1], "--source-rig")) {
    char directory[512];
    if (!source_rig(argv[2], argv[3], argv[4], directory))
      return 4;
    puts(directory);
    return 0;
  }
  if (!test_map("  # shared group names and prefixes across classes\n\n"
                "heroes/alpha/alpha_A vanilla common.rig_vanilla.\n"
                "heroes/alpha/alpha_B custom common.\n"
                "heroes/alpha/alpha_C vanilla common.rig_vanilla.\n"
                "heroes/beta/beta_A vanilla common.rig_vanilla.\n"
                "heroes/beta/beta_B custom common.\n"))
    return 1;
  if (same_group(routes, routes + 3) || !same_group(routes, routes + 2) ||
      route_for_name("anim/common.rig_vanilla.idle.skel", routes + 4) !=
          routes + 3 ||
      route_for_name("common.idle.skel", routes) != routes + 1 ||
      route_for_name("heroes/alpha/anim/common.idle.skel", routes + 3) ||
      route_for_name("heroes/beta/anim/common.idle.skel", routes + 3) !=
          routes + 4 ||
      route_for_name("unmapped.idle.skel", routes))
    return 2;
  const char *invalid[] = {"heroes/alpha/alpha_A vanilla common. extra\n",
                           "heroes/alpha/../alpha_A vanilla common.\n",
                           "heroes/alpha/alpha_A vanilla common.\n"
                           "heroes/alpha/alpha_A custom other.\n",
                           "heroes/alpha/alpha_A vanilla common.\n"
                           "heroes/alpha/alpha_B custom common.\n",
                           "heroes/alpha/alpha_A vanilla common.\n"
                           "heroes/alpha/alpha_B vanilla other.\n"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
    if (test_map(invalid[i]))
      return 3;
  puts("RIG_SELFTEST_PASS: hero-scoped groups, longest prefixes and invalid "
       "maps");
  return 0;
}
#endif
