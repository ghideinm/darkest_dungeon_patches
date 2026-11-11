#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <stdarg.h>
#include <windows.h>
#define DD_CLASS_VTABLE_OFFSET 0x30
#define DD_FUNCTION_WORDS 8
#else
#include "../linux/linux_patch.h"
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#define DD_CLASS_VTABLE_OFFSET 0x38
#define DD_FUNCTION_WORDS 4
#endif

#include "address_profile.h"
#include "palette_sources.h"
#include "patch_config.h"
#define dprintf dd_patch_log

/* Original instructions are checked before native hooks run. */
typedef struct {
  int index, generation;
  void *cached;
} Handle;
typedef struct {
  Handle *begin, *end, *capacity;
} Handles;
typedef struct {
  char key[256], palette[512];
} RigEntry;
typedef struct {
  RigEntry *entries;
  size_t count, capacity;
} RigPlan;
typedef struct {
  char skin[256], hero[256], group[64], prefix[160];
  char origin[512];
  RigPlan *plan;
  int tested;
} Route;
/* Only pointer tables move on growth: active routes retain stable addresses. */
static Route **routes;
static size_t route_count, route_capacity;

static void *grow_array(void *data, size_t *capacity, size_t count,
                        size_t stride, size_t initial) {
  if (count <= *capacity)
    return data;
  size_t maximum = PTRDIFF_MAX / stride;
  if (count > maximum)
    return NULL;
  size_t next =
      *capacity ? *capacity : (initial && initial < maximum ? initial : count);
  while (next < count)
    next = next > maximum / 2 ? count : next * 2;
  void *grown = realloc(data, next * stride);
  if (grown)
    *capacity = next;
  return grown;
}
/* Explicit constructor fallbacks stay native across every later hook. Actors
 * created before a delayed load may be adopted separately, once a safe swap is
 * possible. A tracking allocation failure disables adoption of unknown actors:
 * they might instead be constructor fallbacks that could not be recorded.
 */
typedef struct ActorSprite {
  void *actor, *sprite;
  int routed, pending;
  struct ActorSprite *next;
} ActorSprite;
static ActorSprite *actor_sprites;
#ifndef DD_PATCH_SELFTEST
static int actor_tracking_failed;
static ActorSprite *actor_sprite_slot(void *actor, int create) {
  for (ActorSprite *entry = actor_sprites; entry; entry = entry->next)
    if (entry->actor == actor)
      return entry;
  if (!create)
    return NULL;
  ActorSprite *entry = calloc(1, sizeof(*entry));
  if (entry) {
    entry->actor = actor;
    entry->next = actor_sprites;
    actor_sprites = entry;
  } else
    actor_tracking_failed = 1;
  return entry;
}
#endif
static void clear_routes(void) {
#ifndef DD_PATCH_SELFTEST
  actor_tracking_failed = 0;
#endif
  while (actor_sprites) {
    ActorSprite *next = actor_sprites->next;
    free(actor_sprites);
    actor_sprites = next;
  }
  for (size_t i = 0; i < route_count; ++i) {
    if (routes[i]->plan) {
      free(routes[i]->plan->entries);
      free(routes[i]->plan);
    }
    free(routes[i]);
  }
  free(routes);
  routes = NULL;
  route_count = route_capacity = 0;
}
static Route *append_route(const Route *value) {
  if (route_count == SIZE_MAX)
    return NULL;
  Route **grown = grow_array(routes, &route_capacity, route_count + 1,
                             sizeof(*routes), dd_config.cache_initial_capacity);
  if (!grown)
    return NULL;
  routes = grown;
  Route *route = malloc(sizeof(*route));
  if (!route)
    return NULL;
  *route = *value;
  routes[route_count++] = route;
  return route;
}

static int path_join(char *out, size_t capacity, const char *a, const char *b) {
  size_t first = strlen(a), second = strlen(b);
  if (first >= capacity || second >= capacity - first)
    return 0;
  memmove(out, a, first);
  memmove(out + first, b, second + 1);
  return 1;
}
static int append_file_name(NativeFileMatch *files, const char *name) {
  size_t count, capacity;
  if (!file_match_count(files, &count) ||
      strlen(name) >= sizeof(*files->begin) || count == SIZE_MAX)
    return 0;
  capacity = files->begin ? (size_t)(files->capacity - files->begin) : 0;
  void *grown =
      grow_array(files->begin, &capacity, count + 1, sizeof(*files->begin), 16);
  if (!grown)
    return 0;
  files->begin = grown;
  files->end = files->begin + count + 1;
  files->capacity = files->begin + capacity;
  snprintf(files->begin[count], sizeof(*files->begin), "%s", name);
  return 1;
}
static int compare_names(const void *a, const void *b) { return strcmp(a, b); }
static int rig_files(const char *directory, NativeFileMatch *files) {
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
    if (n >= 256 ||
        snprintf(key, sizeof(key), ">%s/%s", directory, entry->d_name) >=
            (int)sizeof(key) ||
        stat(key + 1, &info) || !S_ISREG(info.st_mode) || info.st_size == 0 ||
        (uint64_t)info.st_size > dd_rig_file_limit() ||
        snprintf(atlas, sizeof(atlas), "%s/%.*s.atlas", directory, (int)n - 5,
                 entry->d_name) >= (int)sizeof(atlas) ||
        stat(atlas, &info) || !S_ISREG(info.st_mode) || info.st_size == 0 ||
        !append_file_name(files, entry->d_name)) {
      valid = 0;
      break;
    }
  }
  closedir(dir);
  size_t count;
  if (!file_match_count(files, &count))
    return -1;
  if (count && files->begin)
    qsort(files->begin, count, sizeof(*files->begin), compare_names);
  return valid ? (count ? 1 : 0) : -1;
}
static int image_name(const char *name) {
  const char *extension = strrchr(name, '.');
  return extension &&
         (!strcasecmp(extension, ".png") || !strcasecmp(extension, ".tga"));
}
typedef struct {
  char **names;
  size_t count, capacity;
} AtlasPages;
static void clear_atlas_pages(AtlasPages *pages) {
  for (size_t i = 0; i < pages->count; ++i)
    free(pages->names[i]);
  free(pages->names);
}
static int append_atlas_page(AtlasPages *pages, const char *name) {
  if (pages->count == SIZE_MAX)
    return 0;
  char **grown = grow_array(pages->names, &pages->capacity, pages->count + 1,
                            sizeof(*pages->names), 16);
  if (!grown)
    return 0;
  pages->names = grown;
  size_t length = strlen(name) + 1;
  char *owned = malloc(length);
  if (!owned)
    return 0;
  memcpy(owned, name, length);
  pages->names[pages->count++] = owned;
  return 1;
}
static int atlas_pages(const char *atlas, AtlasPages *files) {
  FILE *input = fopen(atlas, "r");
  if (!input)
    return -1;
  char line[1024], page[1024] = {0};
  int valid = 1;
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
      if (!append_atlas_page(files, page)) {
        valid = 0;
        break;
      }
    }
    if (*line && !strchr(" \t", line[0]) && image_name(line))
      memcpy(page, line, strlen(line) + 1);
    else
      *page = 0;
  }
  valid = valid && !ferror(input) && files->count;
  fclose(input);
  return valid ? 1 : -1;
}
static int atlas_uses_file(const char *atlas, const char *file) {
  AtlasPages files = {0};
  int result = atlas_pages(atlas, &files);
  if (result == 1) {
    result = 0;
    for (size_t i = 0; i < files.count; ++i) {
      const char *page = files.names[i], *leaf = strrchr(page, '/');
      if (!strcmp(leaf ? leaf + 1 : page, file))
        result = 1;
    }
  }
  clear_atlas_pages(&files);
  return result;
}
static int add_rig_key(RigPlan *plan, const char *key) {
  const char *leaf = strrchr(key, '/');
  if (!leaf || strlen(key) >= sizeof(plan->entries[0].key))
    return 0;
  for (size_t i = 0; i < plan->count; ++i) {
    if (!strcmp(plan->entries[i].key, key))
      return 1;
    const char *other = strrchr(plan->entries[i].key, '/');
    if (other && !strcmp(other, leaf)) {
      dprintf(2, "DD_RIG_RESOURCE_CONFLICT first=%s other=%s\n",
              plan->entries[i].key, key);
      return 0;
    }
  }
  if (plan->count == SIZE_MAX)
    return 0;
  RigEntry *grown = grow_array(plan->entries, &plan->capacity, plan->count + 1,
                               sizeof(*plan->entries), 16);
  if (!grown)
    return 0;
  plan->entries = grown;
  RigEntry *entry = plan->entries + plan->count++;
  *entry = (RigEntry){0};
  memcpy(entry->key, key, strlen(key) + 1);
  return 1;
}
static int compare_rigs(const void *a, const void *b) {
  return strcmp(((const RigEntry *)a)->key, ((const RigEntry *)b)->key);
}
static void sort_plan(RigPlan *plan) {
  if (plan->count)
    qsort(plan->entries, plan->count, sizeof(*plan->entries), compare_rigs);
}
#ifndef DD_PATCH_SELFTEST
static int same_plan(const RigPlan *a, const RigPlan *b) {
  if (!a || !b || a->count != b->count)
    return 0;
  for (size_t i = 0; i < a->count; ++i)
    if (strcmp(a->entries[i].key, b->entries[i].key) ||
        strcmp(a->entries[i].palette, b->entries[i].palette))
      return 0;
  return 1;
}
#endif
static int matching_rigs(const char *directory, const char *file,
                         RigPlan *plan) {
  NativeFileMatch files = {0};
  int result = rig_files(directory, &files);
  if (result != 1) {
    free(files.begin);
    return result;
  }
  size_t count = (size_t)(files.end - files.begin);
  for (size_t i = 0; i < count; ++i) {
    char atlas[512], key[256];
    const char *name = files.begin[i];
    size_t length = strlen(name);
    int n = snprintf(atlas, sizeof(atlas), "%s/%.*s.atlas", directory,
                     (int)length - 5, name);
    if (n < 0 || (size_t)n >= sizeof(atlas)) {
      result = -1;
      break;
    }
    int uses = atlas_uses_file(atlas, file);
    if (uses < 0) {
      result = -1;
      break;
    }
    if (!uses)
      continue;
    n = snprintf(key, sizeof(key), ">%s/%s", directory, name);
    if (n < 0 || (size_t)n >= sizeof(key) || !add_rig_key(plan, key)) {
      result = -1;
      break;
    }
  }
  free(files.begin);
  return result < 0 ? -1 : (plan->count ? 1 : 0);
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
  if (source_rigs(skin, file, full, &plan) != 1 || !plan.count ||
      !plan.entries) {
    free(plan.entries);
    return 0;
  }
  sort_plan(&plan);
  memcpy(out, plan.entries[0].key + 1, strlen(plan.entries[0].key));
  *strrchr(out, '/') = 0;
  free(plan.entries);
  return 1;
}
#endif

static int same_hero(const Route *a, const Route *b) {
  return a && b && !strcmp(a->hero, b->hero);
}
static int same_group(const Route *a, const Route *b) {
  if (!a || !b)
    return 0;
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
    size_t n = strlen(routes[i]->prefix);
    if (same_hero(scope, routes[i]) && n > length &&
        !strncmp(leaf, routes[i]->prefix, n)) {
      best = routes[i];
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
  clear_routes();
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
    if (sscanf(p, "%255s %63s %159s %c", route.skin, route.group, route.prefix,
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
      if (!strcmp(routes[i]->skin, route.skin))
        return 0;
      if (same_hero(routes[i], &route) &&
          ((!strcmp(routes[i]->group, route.group) &&
            strcmp(routes[i]->prefix, route.prefix)) ||
           (!strcmp(routes[i]->prefix, route.prefix) &&
            strcmp(routes[i]->group, route.group))))
        return 0;
    }
    if (!append_route(&route))
      return 0;
  }
  return !ferror(file) && route_count > 0;
}

#ifndef DD_PATCH_SELFTEST
static uintptr_t base;
static DdResolution native_symbols;
#define DD_ACTOR_SPRITE_OFFSET (native_symbols.values[DD_ACTOR_SPRITE_FIELD])
static int native_fields_valid(void) {
  if (native_symbols.values[DD_ACTOR_SPRITE_FIELD] !=
      native_symbols.values[DD_ACTOR_SPRITE_VERIFY]) {
    dprintf(2, "DD_RIG_PATCH_SKIPPED reason=actor_sprite_field_mismatch\n");
    return 0;
  }
  return 1;
}
static uintptr_t native_address(DdSymbol symbol) {
  return base + native_symbols.values[symbol];
}
static void (*original_colour)(void *, int);
static void (*original_preview)(void *, void *);
static void (*original_override)(void *, const char *);
static void *(*native_new)(const Handles *, int);
static const char *(*resource_name)(void *, const Handle *);
static void (*set_animation)(void *, const char *, int, int);
static int runtime_enabled;
static unsigned (*colour_count)(void *);
static const char *(*colour_path)(void *, unsigned);
static int preserve_palette_sources;
static void (*match_files)(NativeFileMatch *, int, const char *, const char *,
                           int, int, int);
static void (*destroy_match)(NativeFileMatch *);
static void (*match_device)(NativeFileMatch *, void *, const char *,
                            const char *, int, int, int);
static unsigned (*alternate_count)(void *);
static const void *(*alternate_root)(void *, unsigned);
static void (*match_archive)(void *, NativeFileMatch *, const char *,
                             const char *, int, int, bool);
static bool (*full_path)(void *, int, const char *, char *, unsigned);
/* C++ nontrivial Handle return uses an explicit sret pointer on this ABI. */
static void (*load_spine)(Handle *, const char *, int, const void *);
static void (*release_spine)(Handle *);
static void (*load_texture)(Handle *, const char *, unsigned);
static void *(*get_resource)(void *, const Handle *);
static _Thread_local const Route *override_route;

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
  void *(*get_class)(void *) = pointer_at(table, DD_CLASS_VTABLE_OFFSET);
  return get_class(actor);
}
typedef struct {
  char raw[256], skin[256], origin[512];
} SkinSource;
typedef struct {
  void *klass;
  uint64_t signature;
  size_t count;
  SkinSource *entries;
} SkinCatalog;
static SkinCatalog **catalogs;
static size_t catalog_count, catalog_capacity;
#ifdef DD_RIG_RUNTIME_TEST
static void change_colour(void *, int);
static void clear_catalogs(void) {
  for (size_t i = 0; i < catalog_count; ++i) {
    free(catalogs[i]->entries);
    free(catalogs[i]);
  }
  free(catalogs);
  catalogs = NULL;
  catalog_count = catalog_capacity = 0;
}
#endif

static void *data_device(void) {
  void *storage = *(void **)(native_address(DD_STORAGE));
#ifdef _WIN32
  return storage ? pointer_at(storage, 0x18) : NULL;
#else
  return storage ? pointer_at(storage, 0x38) : NULL;
#endif
}
static uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t count) {
  const unsigned char *p = bytes;
  for (size_t i = 0; i < count; ++i)
    hash = (hash ^ p[i]) * UINT64_C(1099511628211);
  return hash;
}
static int add_catalog_files(SkinCatalog *catalog, size_t limit,
                             const NativeFileMatch *files, const char *hero,
                             const char *origin, const char *game_root) {
  size_t count;
  if (!file_match_count(files, &count))
    return 0;
  for (size_t i = 0; i < count; ++i) {
    const char *raw = files->begin[i];
    if (!memchr(raw, 0, 256))
      return 0;
    size_t j;
    /* Linux follows the native raw-name merge. Windows preserves each root
     * during enumeration, so its identity includes both source and name. */
    for (j = 0; j < catalog->count; ++j)
      if (!strcmp(catalog->entries[j].raw, raw) &&
          (!preserve_palette_sources ||
           !strcmp(catalog->entries[j].origin, origin)))
        break;
    if (j != catalog->count) {
      /* Linux merges identical raw names. When masking official choices,
       * route that shared index to the first (highest-priority) Mod provider.
       */
      if (dd_config.hide_vanilla_skins && !preserve_palette_sources &&
          *game_root &&
          palette_official_source(catalog->entries[j].origin, game_root) &&
          !palette_official_source(origin, game_root))
        snprintf(catalog->entries[j].origin, sizeof(catalog->entries[j].origin),
                 "%s", origin);
      continue;
    }
    if (catalog->count == limit)
      return 0;
    SkinSource *entry = catalog->entries + catalog->count++;
    snprintf(entry->raw, sizeof(entry->raw), "%s", raw);
    snprintf(entry->origin, sizeof(entry->origin), "%s", origin);
    int n = snprintf(entry->skin, sizeof(entry->skin), "%s/%s", hero, raw);
    if (n < 0 || (size_t)n >= sizeof(entry->skin))
      return 0;
    size_t length = strlen(entry->skin);
    while (length && strchr("/\\", entry->skin[length - 1]))
      entry->skin[--length] = 0;
  }
  return 1;
}
static const SkinCatalog *source_catalog(void *klass) {
  unsigned count = colour_count(klass);
  void *device = data_device();
  if (!count || !device)
    return NULL;
  const char *first = colour_path(klass, 0);
  if (!first || strncmp(first, "heroes/", 7))
    return NULL;
  const char *end = strchr(first + 7, '/');
  char hero[256], directory[512], pattern[512];
  if (!end || (size_t)(end - first) >= sizeof(hero))
    return NULL;
  memcpy(hero, first, (size_t)(end - first));
  hero[end - first] = 0;
  if (!path_join(directory, sizeof(directory), hero, "/"))
    return NULL;
#ifdef _WIN32
  int n =
      snprintf(pattern, sizeof(pattern), "(.*)%s_([A-Z])(\\\\|/)", hero + 7);
#else
  int n = snprintf(pattern, sizeof(pattern), "(.*)%s_([A-Z])(\\\\\\\\|/)",
                   hero + 7);
#endif
  if (n < 0 || (size_t)n >= sizeof(pattern))
    return NULL;
  uint64_t signature = UINT64_C(14695981039346656037);
  signature = hash_bytes(signature, &dd_config.hide_vanilla_skins,
                         sizeof(dd_config.hide_vanilla_skins));
  for (unsigned i = 0; i < count; ++i) {
    const char *path = colour_path(klass, i);
    if (!path || strnlen(path, 512) == 512)
      return NULL;
    signature = hash_bytes(signature, path, strlen(path) + 1);
  }
  unsigned roots = alternate_count(device);
  if (roots == UINT_MAX)
    return NULL;
  for (unsigned i = 0; i < roots; ++i) {
    const char *root = alternate_root(device, i);
    if (!root || !memchr(root, 0, 512))
      return NULL;
    signature = hash_bytes(signature, root, strlen(root) + 1);
    signature = hash_bytes(signature, root + 0x200, 16);
  }
  for (size_t i = 0; i < catalog_count; ++i)
    if (catalogs[i]->klass == klass && catalogs[i]->signature == signature)
      return catalogs[i];
  SkinCatalog catalog = {klass, signature, 0,
                         calloc(count, sizeof(SkinSource))};
  if (!catalog.entries) {
    dprintf(2, "DD_RIG_PALETTE_CATALOG_REFUSED class=%p reason=allocation\n",
            klass);
    return NULL;
  }
  NativeFileMatch files = {0};
  char game_root[4096] = {0};
  if (!getcwd(game_root, sizeof(game_root)))
    *game_root = 0;
  match_device(&files, device, directory, pattern, 0, 0, 0);
  int valid = add_catalog_files(&catalog, count, &files, hero, "", game_root);
  destroy_match(&files);
  /* IO_FindFiles enumerates the base device first, then alternate roots in
   * reverse order. Keep the origin before its duplicate-name merge loses it. */
  for (unsigned i = roots; valid && i-- > 0;) {
    const void *root = alternate_root(device, i);
    const char *origin = root;
    void *archive = pointer_at(root, 0x200);
    void *delegate = pointer_at(root, 0x208);
    files = (NativeFileMatch){0};
    if (archive)
      match_archive(archive, &files, directory, pattern, 0, 0, false);
    else {
      char full[1024];
      if (!path_join(full, sizeof(full), origin, directory)) {
        valid = 0;
        break;
      }
      match_device(&files, delegate ? delegate : device, full, pattern, 0, 0,
                   0);
    }
    valid = add_catalog_files(&catalog, count, &files, hero, origin, game_root);
    destroy_match(&files);
  }
  if (catalog.count != count)
    valid = 0;
  for (size_t i = 0; valid && i < catalog.count; ++i) {
    char expected[512];
    n = snprintf(expected, sizeof(expected), "%s/%s/", hero,
                 catalog.entries[i].raw);
    valid = n > 0 && (size_t)n < sizeof(expected) &&
            !strcmp(expected, colour_path(klass, (unsigned)i));
  }
  if (!valid) {
    dprintf(2, "DD_RIG_PALETTE_CATALOG_REFUSED class=%p native=%u found=%zu\n",
            klass, count, catalog.count);
    free(catalog.entries);
    return NULL;
  }
  for (size_t i = 0; i < catalog.count; ++i)
    dprintf(2, "DD_RIG_PALETTE_ENTRY class=%p index=%zu skin=%s source=%s\n",
            klass, i, catalog.entries[i].skin,
            *catalog.entries[i].origin ? catalog.entries[i].origin : "base");
  if (catalog_count == SIZE_MAX) {
    free(catalog.entries);
    return NULL;
  }
  SkinCatalog **grown =
      grow_array(catalogs, &catalog_capacity, catalog_count + 1,
                 sizeof(*catalogs), dd_config.cache_initial_capacity);
  if (!grown) {
    free(catalog.entries);
    return NULL;
  }
  catalogs = grown;
  SkinCatalog *owned = malloc(sizeof(*owned));
  if (!owned) {
    free(catalog.entries);
    return NULL;
  }
  *owned = catalog;
  catalogs[catalog_count++] = owned;
  return owned;
}
static int visible_catalog_colour(const SkinCatalog *catalog, int requested,
                                  int current, const char *game_root) {
  size_t count = catalog->count;
  if (!count || count > INT_MAX)
    return requested;
  size_t index =
      requested >= 0 && (size_t)requested < count ? (size_t)requested : 0;
  int backwards = current >= 0 && (size_t)current < count && requested >= 0 &&
                  (size_t)requested < count &&
                  index == ((size_t)current + count - 1) % count;
  for (size_t checked = 0; checked < count; ++checked) {
    if (!palette_official_source(catalog->entries[index].origin, game_root))
      return (int)index;
    index = backwards ? (index ? index - 1 : count - 1) : (index + 1) % count;
  }
  return requested; /* An unmodded class retains every official choice. */
}
static int masked_catalog_colour(void *actor, const SkinCatalog *catalog,
                                 int requested) {
  if (!dd_config.hide_vanilla_skins)
    return requested;
  char game_root[4096];
  if (!getcwd(game_root, sizeof(game_root)))
    return requested;
  int selected = visible_catalog_colour(catalog, requested,
                                        int_at(actor, 0xb68), game_root);
  if (selected != requested)
    dprintf(2, "DD_RIG_COLOUR_MASKED actor=%p requested=%d selected=%d\n",
            actor, requested, selected);
  return selected;
}
static int skin_colour(void *actor, int requested) {
  /* Keep cycling independent of masking. Preserve negative native sentinel
   * values, but normalize a caller's last+1 before resolving its skeleton. */
  unsigned count = colour_count(actor_class(actor));
  if (requested >= 0 && count && count <= INT_MAX &&
      (unsigned)requested >= count) {
    int wrapped = requested % (int)count;
    dprintf(2, "DD_RIG_COLOUR_WRAPPED actor=%p requested=%d selected=%d\n",
            actor, requested, wrapped);
    requested = wrapped;
  }
  if (!runtime_enabled || !dd_config.hide_vanilla_skins)
    return requested;
  const SkinCatalog *catalog = source_catalog(actor_class(actor));
  return catalog ? masked_catalog_colour(actor, catalog, requested) : requested;
}
static int runtime_actor(void *actor) {
  void *klass = actor_class(actor);
  if (!colour_count(klass))
    return 0;
  const char *skin = colour_path(klass, 0);
  return skin && !strncmp(skin, "heroes/", 7);
}
static int append_palette_files(NativeFileMatch *files, const char *origin,
                                const char *palette) {
  char directory[1024];
  if (!path_join(directory, sizeof(directory), origin, palette))
    return 0;
  DIR *dir = opendir(directory);
  if (!dir)
    return errno == ENOENT;
  int valid = 1;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (!image_name(entry->d_name))
      continue;
    size_t count = files->begin ? (size_t)(files->end - files->begin) : 0, i;
    for (i = 0; i < count; ++i)
      if (!strcmp(files->begin[i], entry->d_name))
        break;
    if (i < count)
      continue;
    if (!append_file_name(files, entry->d_name)) {
      valid = 0;
      break;
    }
  }
  closedir(dir);
  return valid;
}
static int disk_texture(char out[512], const char *origin,
                        const char *request) {
  if (!path_join(out, 512, origin, request))
    return -1;
  struct stat info;
  if (stat(out, &info))
    return errno == ENOENT ? 0 : -1;
  return S_ISREG(info.st_mode) && info.st_size > 0 ? 1 : -1;
}
static int pinned_texture(char out[512], const char *origin,
                          const char *request) {
  int found = *origin ? disk_texture(out, origin, request) : 0;
  if (found)
    return found;
  void *device = alternate_count ? data_device() : NULL;
  unsigned count = device ? alternate_count(device) : 0;
  if (count == UINT_MAX)
    return -1;
  /* Official DLC augments the baseline. Other user Mods must not supply
   * missing textures for a different, independently selected skin entry. */
  for (unsigned i = count; i-- > 0;) {
    const char *root = alternate_root(device, i);
    if (root && !strncmp(root, "dlc/", 4)) {
      found = disk_texture(out, root, request);
      if (found)
        return found;
    }
  }
  return disk_texture(out, "", request);
}
static int record_palette(RigPlan *plan, const char *key, const char *actual) {
  const char *slash = strrchr(actual, '/');
  if (!slash || strlen(actual) + 1 >= 256)
    return 0;
  char directory[512];
  int n = snprintf(directory, sizeof(directory), ">%.*s/",
                   (int)(slash - actual), actual);
  if (n < 0 || (size_t)n >= sizeof(directory))
    return 0;
  for (size_t i = 0; i < plan->count; ++i)
    if (!strcmp(plan->entries[i].key, key)) {
      if (*plan->entries[i].palette &&
          strcmp(plan->entries[i].palette, directory)) {
        dprintf(2, "DD_RIG_TEXTURE_SOURCE_CONFLICT rig=%s\n", key);
        return 0;
      }
      memcpy(plan->entries[i].palette, directory, strlen(directory) + 1);
      return 1;
    }
  return 0;
}
/* A class present in the base game or official DLC retains its existing skin
 * isolation. Only a class supplied by user Mods needs a mounted Mod baseline.
 */
static int mod_class(const char *hero, void *device, unsigned roots) {
  struct stat info;
  if (!stat(hero, &info))
    return S_ISDIR(info.st_mode) ? 0 : -1;
  if (errno != ENOENT)
    return -1;
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd)))
    return -1;
  for (unsigned i = 0; i < roots; ++i) {
    const char *root = alternate_root(device, i);
    char path[1024];
    if (!root || !memchr(root, 0, 512))
      return -1;
    if (!*root || !palette_official_source(root, cwd))
      continue;
    if (!path_join(path, sizeof(path), root, hero))
      return -1;
    if (!stat(path, &info))
      return S_ISDIR(info.st_mode) ? 0 : -1;
    if (errno != ENOENT)
      return -1;
  }
  return 1;
}
/* Enumerate names only. A broken lower-priority copy must not invalidate an
 * action already supplied by the selected skin or the game's winning Mod. */
static int append_rig_names(NativeFileMatch *files, const char *directory) {
  DIR *dir = opendir(directory);
  if (!dir)
    return errno == ENOENT;
  int valid = 1;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    size_t length = strlen(entry->d_name);
    if (length < 6 || strcmp(entry->d_name + length - 5, ".skel") ||
        strstr(entry->d_name, ".rig_vanilla.") ||
        strstr(entry->d_name, ".sprite.rig_"))
      continue;
    size_t count = files->begin ? (size_t)(files->end - files->begin) : 0, i;
    for (i = 0; i < count; ++i)
      if (!strcmp(files->begin[i], entry->d_name))
        break;
    if (i == count && !append_file_name(files, entry->d_name)) {
      valid = 0;
      break;
    }
  }
  closedir(dir);
  return valid;
}
static int has_rig_leaf(const RigPlan *plan, const char *leaf) {
  for (size_t i = 0; i < plan->count; ++i) {
    const char *other = strrchr(plan->entries[i].key, '/');
    if (other && !strcmp(other + 1, leaf))
      return 1;
  }
  return 0;
}
static int rig_atlas(char atlas[512], const char *key) {
  size_t length = strlen(key);
  struct stat info;
  if (length < 6 || key[0] != '>' || strcmp(key + length - 5, ".skel") ||
      stat(key + 1, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      (uint64_t)info.st_size > dd_rig_file_limit())
    return 0;
  int n = snprintf(atlas, 512, "%.*s.atlas", (int)length - 6, key + 1);
  return n > 0 && n < 512 && !stat(atlas, &info) && S_ISREG(info.st_mode) &&
         info.st_size > 0;
}
/* Native path lookup is authoritative for mounted-root precedence. Directory
 * enumeration below only discovers possible names, never picks a provider. */
static int mounted_path(char out[512], void *storage, const char *request) {
  if (!full_path(storage, 1, request, out, 512))
    return 0;
  size_t length = strlen(out), suffix = strlen(request);
  if (length < suffix || (length > suffix && out[length - suffix - 1] != '/'))
    return -1;
  for (size_t i = 0; i < suffix; ++i)
    if (palette_path_char((unsigned char)out[length - suffix + i]) !=
        palette_path_char((unsigned char)request[i]))
      return -1;
  struct stat info;
  return !stat(out, &info) && S_ISREG(info.st_mode) && info.st_size > 0 ? 1
                                                                        : -1;
}
static int class_rig_layer(RigPlan *plan, const char *origin,
                           const char *relative, void *storage, void *device,
                           unsigned roots) {
  NativeFileMatch files = {0};
  char directory[1024];
  int valid =
      path_join(directory, sizeof(directory), origin ? origin : "", relative) &&
      append_rig_names(&files, directory);
  if (!origin) {
    for (unsigned i = 0; valid && i < roots; ++i) {
      const char *root = alternate_root(device, i);
      valid = root && memchr(root, 0, 512) &&
              path_join(directory, sizeof(directory), root, relative) &&
              append_rig_names(&files, directory);
    }
  }
  for (char (*file)[256] = files.begin; valid && file != files.end; ++file) {
    if (has_rig_leaf(plan, *file))
      continue;
    char request[512], actual[512], key[513], atlas[512];
    valid = path_join(request, sizeof(request), relative, *file);
    if (valid)
      valid = origin ? path_join(actual, sizeof(actual), origin, request)
                     : mounted_path(actual, storage, request) == 1;
    if (valid)
      valid = snprintf(key, sizeof(key), ">%s", actual) < (int)sizeof(key) &&
              rig_atlas(atlas, key) && add_rig_key(plan, key);
    if (!valid)
      dprintf(2, "DD_RIG_CLASS_FALLBACK_REFUSED action=%s directory=%s\n",
              *file, relative);
  }
  free(files.begin);
  return valid;
}
static int rig_from_origin(const char *key, const char *origin,
                           const char *hero) {
  char prefix[1024];
  if (!*origin || !path_join(prefix, sizeof(prefix), origin, hero) ||
      !path_join(prefix, sizeof(prefix), prefix, "/"))
    return 0;
  size_t length = strlen(prefix);
  if (*key != '>' || strlen(key + 1) < length)
    return 0;
  for (size_t i = 0; i < length; ++i)
    if (palette_path_char((unsigned char)key[i + 1]) !=
        palette_path_char((unsigned char)prefix[i]))
      return 0;
  return 1;
}
static int class_texture(char actual[512], const char *origin, const char *hero,
                         const char *palette, const char *page, const char *key,
                         void *storage, const SkinCatalog *catalog) {
  char request[512];
  if (!path_join(request, sizeof(request), palette, page))
    return -1;
  int found = disk_texture(actual, origin, request);
  if (found)
    return found;
  found = mounted_path(actual, storage, request);
  if (found || !catalog)
    return found;
  /* New palette letters need not duplicate the base Mod's action images.
   * Prefer a baseline palette of the action provider, then native catalogue
   * order. Each logical candidate is still resolved by the game itself. */
  for (int pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i < catalog->count; ++i) {
      const SkinSource *entry = catalog->entries + i;
      if (rig_from_origin(key, entry->origin, hero) != !pass)
        continue;
      char directory[512];
      if (!path_join(directory, sizeof(directory), entry->skin, "/anim/") ||
          !path_join(request, sizeof(request), directory, page))
        return -1;
      found = mounted_path(actual, storage, request);
      if (found)
        return found;
    }
  return 0;
}
static int class_rig_plan(const char *skin, const char *origin,
                          const char *hero, const char *palette, void *device,
                          unsigned roots, const SkinCatalog *catalog,
                          RigPlan *plan) {
  void *storage = *(void **)(native_address(DD_STORAGE));
  char common[512];
  if (!path_join(common, sizeof(common), hero, "/anim/") ||
      !class_rig_layer(plan, origin, palette, storage, device, roots) ||
      !class_rig_layer(plan, origin, common, storage, device, roots) ||
      !class_rig_layer(plan, NULL, palette, storage, device, roots) ||
      !class_rig_layer(plan, NULL, common, storage, device, roots))
    return 0;
  /* Some custom classes put their baseline actions beside palette A (or the
   * provider's first native palette), rather than in the class-wide folder. */
  for (size_t i = 0; catalog && i < catalog->count; ++i) {
    size_t previous;
    for (previous = 0; previous < i; ++previous)
      if (!strcmp(catalog->entries[previous].origin,
                  catalog->entries[i].origin))
        break;
    if (previous != i)
      continue;
    char baseline[512];
    if (!path_join(baseline, sizeof(baseline), catalog->entries[i].skin,
                   "/anim/") ||
        !class_rig_layer(plan, NULL, baseline, storage, device, roots))
      return 0;
  }
  for (size_t i = 0; i < plan->count;) {
    RigEntry *entry = plan->entries + i;
    char atlas[512];
    AtlasPages pages = {0};
    int valid = rig_atlas(atlas, entry->key) && atlas_pages(atlas, &pages) == 1;
    size_t found = 0;
    for (size_t j = 0; valid && j < pages.count; ++j) {
      const char *page = pages.names[j];
      char actual[512], directory[513];
      int resolved = class_texture(actual, origin, hero, palette, page,
                                   entry->key, storage, catalog);
      if (resolved < 0) {
        valid = 0;
        break;
      }
      if (!resolved)
        continue;
      ++found;
      size_t length = strlen(actual), suffix = strlen(page);
      valid = length >= suffix &&
              snprintf(directory, sizeof(directory), ">%.*s",
                       (int)(length - suffix), actual) < (int)sizeof(directory);
      if (valid && *entry->palette && strcmp(entry->palette, directory))
        valid = 0; /* A native state has one texture directory for its atlas. */
      if (valid)
        valid = dd_copy(entry->palette, sizeof(entry->palette), directory);
    }
    size_t required = pages.count;
    clear_atlas_pages(&pages);
    /* A class-wide directory can also contain effects with their own texture
     * scheme. They are not palette actions. Keep explicit skin overrides and
     * partially missing action atlases strict, rather than hiding bad files. */
    if (valid && !found && !rig_from_origin(entry->key, origin, skin)) {
      dprintf(2, "DD_RIG_CLASS_ACTION_UNUSED skin=%s rig=%s\n", skin,
              entry->key);
      memmove(entry, entry + 1, (plan->count - i - 1) * sizeof(*entry));
      --plan->count;
      continue;
    }
    valid = valid && found == required && found;
    if (!valid) {
      dprintf(2, "DD_RIG_CLASS_TEXTURE_REFUSED skin=%s rig=%s\n", skin,
              entry->key);
      return 0;
    }
    dprintf(2, "DD_RIG_CLASS_ACTION skin=%s rig=%s textures=%s\n", skin,
            entry->key, entry->palette);
    ++i;
  }
  return plan->count > 0;
}
static const Route *runtime_route(const char *skin, const char *origin,
                                  const SkinCatalog *catalog) {
  char palette[512];
  RigPlan plan = {0};
  if (strlen(skin) >= sizeof(routes[0]->skin) ||
      !path_join(palette, sizeof(palette), skin, "/anim/") ||
      strncmp(skin, "heroes/", 7))
    return NULL;
  const char *separator = strchr(skin + 7, '/');
  if (!separator || strchr(separator + 1, '/'))
    return NULL;
  char hero[256];
  size_t hero_length = (size_t)(separator - skin);
  memcpy(hero, skin, hero_length);
  hero[hero_length] = 0;
  if (origin && *origin) {
    void *device = alternate_count ? data_device() : NULL;
    unsigned roots = device ? alternate_count(device) : 0;
    int custom = roots == UINT_MAX ? -1 : mod_class(hero, device, roots);
    if (custom < 0)
      return NULL;
    if (custom) {
      if (!class_rig_plan(skin, origin, hero, palette, device, roots, catalog,
                          &plan)) {
        free(plan.entries);
        return NULL;
      }
      goto resolved;
    }
  }
  NativeFileMatch files = {0};
  int valid = 1;
  if (!origin)
    match_files(&files, 1, palette, ".*\\.(png|PNG|tga|TGA)", 0, 0, 2);
  else {
    valid = append_palette_files(&files, origin, palette) &&
            append_palette_files(&files, "", palette);
    void *device = alternate_count ? data_device() : NULL;
    unsigned roots = device ? alternate_count(device) : 0;
    valid = valid && roots != UINT_MAX;
    for (unsigned i = roots; valid && i-- > 0;) {
      const char *root = alternate_root(device, i);
      if (root && !strncmp(root, "dlc/", 4))
        valid = append_palette_files(&files, root, palette);
    }
  }
  size_t count = 0;
  valid = valid && file_match_count(&files, &count) && count > 0;
  void *storage = *(void **)(native_address(DD_STORAGE));
  for (size_t i = 0; valid && i < count; ++i) {
    char request[512], actual[512] = {0};
    RigPlan candidates = {0};
    const char *file = files.begin[i];
    if (!memchr(file, 0, 256) || strchr(file, '/') || strchr(file, '\\') ||
        !path_join(request, sizeof(request), palette, file)) {
      valid = 0;
      break;
    }
    char pinned[512], explicit_key[513];
    const char *lookup = request;
    if (origin) {
      if (pinned_texture(pinned, origin, request) != 1 ||
          snprintf(explicit_key, sizeof(explicit_key), ">%s", pinned) >=
              (int)sizeof(explicit_key)) {
        valid = 0;
        break;
      }
      lookup = explicit_key;
    }
    if (!full_path(storage, 1, lookup, actual, sizeof(actual))) {
      valid = 0;
      break;
    }
    int found = source_rigs(skin, file, actual, &candidates);
    if (found < 0) {
      dprintf(2, "DD_RIG_FILE_REFUSED skin=%s file=%s source=%s\n", skin, file,
              actual);
      free(candidates.entries);
      valid = 0;
      break;
    }
    if (!found) {
      dprintf(2, "DD_RIG_FILE_UNUSED skin=%s file=%s source=%s\n", skin, file,
              actual);
      free(candidates.entries);
      continue;
    }
    for (size_t j = 0; valid && j < candidates.count; ++j) {
      dprintf(2, "DD_RIG_FILE_SOURCE skin=%s file=%s source=%s rig=%s\n", skin,
              file, actual, candidates.entries[j].key);
      valid = add_rig_key(&plan, candidates.entries[j].key) &&
              record_palette(&plan, candidates.entries[j].key, actual);
    }
    free(candidates.entries);
  }
  if (origin)
    free(files.begin);
  else
    destroy_match(&files);
  if (!valid || !plan.count) {
    dprintf(2, "DD_RIG_SOURCE_REFUSED skin=%s\n", skin);
    free(plan.entries);
    return NULL;
  }
resolved:
  sort_plan(&plan);
  for (size_t i = 0; i < route_count; ++i)
    if (!strcmp(routes[i]->skin, skin) &&
        !strcmp(routes[i]->origin, origin ? origin : "") &&
        same_plan(routes[i]->plan, &plan)) {
      free(plan.entries);
      return routes[i];
    }
  RigPlan *owned = malloc(sizeof(*owned));
  if (!owned) {
    free(plan.entries);
    dprintf(2, "DD_RIG_SOURCE_REFUSED skin=%s reason=allocation\n", skin);
    return NULL;
  }
  *owned = plan;
  Route value = {.plan = owned};
  snprintf(value.skin, sizeof(value.skin), "%s", skin);
  snprintf(value.origin, sizeof(value.origin), "%s", origin ? origin : "");
  memcpy(value.hero, skin, (size_t)(separator - skin));
  memcpy(value.group, "runtime", sizeof("runtime"));
  Route *route = append_route(&value);
  if (!route) {
    free(owned->entries);
    free(owned);
    dprintf(2, "DD_RIG_SOURCE_REFUSED skin=%s reason=allocation\n", skin);
    return NULL;
  }
  dprintf(2,
          "DD_RIG_SOURCE_RESOLVED skin=%s source=%s resources=%zu first=%s\n",
          skin, origin && *origin ? origin : "base", plan.count,
          plan.entries[0].key);
  return route;
}

static const Route *find_route(void *actor, int colour) {
  void *klass = actor_class(actor);
  if (colour < 0)
    colour = 0;
  if ((unsigned)colour >= colour_count(klass))
    return NULL;
  const char *skin = colour_path(klass, (unsigned)colour);
  if (!skin)
    return NULL;
  size_t skin_length = strlen(skin);
  while (skin_length &&
         (skin[skin_length - 1] == '/' || skin[skin_length - 1] == '\\'))
    --skin_length;
  if (runtime_enabled) {
    const SkinCatalog *catalog = source_catalog(klass);
    if (!catalog || (unsigned)colour >= catalog->count)
      return NULL;
    colour = masked_catalog_colour(actor, catalog, colour);
    if (colour < 0 || (size_t)colour >= catalog->count)
      return NULL;
    const SkinSource *source = catalog->entries + colour;
    return runtime_route(source->skin, source->origin, catalog);
  }
  for (size_t i = 0; i < route_count; ++i)
    if (strlen(routes[i]->skin) == skin_length &&
        !strncmp(skin, routes[i]->skin, skin_length))
      return routes[i];
  return NULL;
}
static const Route *resource_route(const Handle *handle, const Route *scope) {
  void *manager = *(void **)(native_address(DD_RESOURCE));
  const char *name = resource_name(manager, handle);
  return route_for_name(name, scope);
}
static size_t select_handles(void *klass, const Route *route, Handles *out) {
  const Handles *all = (const void *)((const char *)klass + 0xe0);
  uintptr_t begin = (uintptr_t)all->begin, end = (uintptr_t)all->end,
            capacity = (uintptr_t)all->capacity;
  if (!begin || end < begin || capacity < end ||
      (end - begin) % sizeof(Handle) || (capacity - begin) % sizeof(Handle))
    return 0;
  size_t total = (end - begin) / sizeof(Handle);
  if (!total)
    return 0;
  out->begin = calloc(total, sizeof(*out->begin));
  if (!out->begin)
    return 0;
  out->end = out->begin;
  out->capacity = out->begin + total;
  for (const Handle *h = all->begin; h < all->end; ++h)
    if (same_group(route, resource_route(h, route)))
      *out->end++ = *h;
  return (size_t)(out->end - out->begin);
}
static void *make_sprite(void *klass, const Route *route) {
  if (route->plan) {
    size_t count = route->plan->count, loaded = 0;
    if (!count)
      return NULL;
    Handle *selected = calloc(count, sizeof(*selected));
    if (!selected) {
      dprintf(2, "DD_RIG_SPRITE_REFUSED skin=%s reason=allocation\n",
              route->skin);
      return NULL;
    }
    void *manager = *(void **)(native_address(DD_RESOURCE));
    const uint64_t empty_callback[DD_FUNCTION_WORDS]
        __attribute__((aligned(16))) = {0};
    int valid = 1;
    for (size_t i = 0; valid && i < count; ++i) {
      const char *key = route->plan->entries[i].key;
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
    free(selected);
    dprintf(2, "DD_RIG_RUNTIME_CREATED sprite=%p first=%s resources=%zu\n",
            sprite, route->plan->entries[0].key, loaded);
    return sprite;
  }
  Handles selected = {0};
  size_t count = select_handles(klass, route, &selected);
  void *sprite = count ? native_new(&selected, 0) : NULL;
  free(selected.begin);
  dprintf(2, "DD_RIG_CREATED sprite=%p group=%s resources=%zu\n", sprite,
          route->group, count);
  return sprite;
}
/* Keep the game's original constructor arguments as a fallback. In particular,
 * a failed custom route must never terminate the process hosting this plugin.
 */
static void *create_actor_sprite(const Handles *original, int flag, void *klass,
                                 const Route **route) {
  void *sprite = *route ? make_sprite(klass, *route) : NULL;
  if (sprite)
    return sprite;
  dprintf(2, "DD_RIG_ACTOR_FALLBACK class=%p reason=%s skin=%s\n", klass,
          *route ? "sprite_load" : "route_unavailable",
          *route ? (*route)->skin : "unknown");
  *route = NULL;
  return native_new(original, flag);
}
static int native_actor_sprite(void *actor) {
  if (!runtime_enabled)
    return 0;
  const ActorSprite *entry = actor_sprite_slot(actor, 0);
  return !entry || !entry->routed ||
         entry->sprite != pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
}
/* A pending entry remains native for preview/fallback purposes until the
 * existing colour hook has validated the route and, for a loaded actor,
 * completed the custom sprite swap. Unloaded town actors have no sprite yet;
 * they can still route their shared preview/portrait cache immediately.
 * Busy animations and transient resource failures can therefore be retried.
 */
static int late_actor_sprite(void *actor) {
  if (!runtime_enabled)
    return 0;
  void *sprite = pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
  ActorSprite *entry = actor_sprite_slot(actor, 0);
  if (entry)
    return entry->pending && entry->sprite == sprite;
  if (actor_tracking_failed)
    return 0;
  entry = actor_sprite_slot(actor, 1);
  if (!entry)
    return 0;
  entry->pending = 1;
  entry->sprite = sprite;
  dprintf(2, "DD_RIG_ACTOR_ADOPT_PENDING actor=%p sprite=%p\n", actor, sprite);
  return 1;
}
static void *construct_actor_sprite(const Handles *original, int flag,
                                    void *actor, const Route **route) {
  ActorSprite *entry = actor_sprite_slot(actor, 1);
  if (entry) {
    entry->routed = 0;
    entry->pending = 0;
    entry->sprite = NULL;
  }
  *route = entry ? find_route(actor, int_at(actor, 0xb68)) : NULL;
  void *sprite = create_actor_sprite(original, flag, actor_class(actor), route);
  if (entry) {
    entry->sprite = sprite;
    entry->routed = sprite && *route;
  }
  return sprite;
}
static void remember_actor_sprite(void *actor, void *sprite) {
  ActorSprite *entry = actor_sprite_slot(actor, 0);
  if (entry) {
    entry->sprite = sprite;
    entry->routed = 1;
    entry->pending = 0;
  }
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
    void *manager = *(void **)(native_address(DD_RESOURCE));
    for (size_t i = 0; i < route->plan->count; ++i) {
      const Handle *handle = (const void *)(begin + i * 0x30);
      const char *name = resource_name(manager, handle);
      if (!name || strcmp(name, route->plan->entries[i].key))
        return 0;
    }
    return 1;
  }
  return same_group(resource_route(first, route), route);
}
static void override_each(void *sprite, const RigPlan *plan) {
  unsigned char view[0x30] = {0};
  unsigned char *states = pointer_at(sprite, 0x20);
  for (size_t i = 0; i < plan->count; ++i) {
    void *begin = states + i * 0x30, *end = states + (i + 1) * 0x30;
    /* The native loop only reads this vector pair. Operate on the original
     * PerSpineDataState so native texture ownership/refcounts stay intact. */
    memcpy(view + 0x20, &begin, sizeof(begin));
    memcpy(view + 0x28, &end, sizeof(end));
    original_override(view, plan->entries[i].palette);
  }
}
/* A skin entry owns a provider, not merely a normalized logical directory.
 * Pin every state's texture request, including baseline/DLC fallbacks, before
 * allowing the native texture loader to handle the original atlas page name. */
static void override_textures(void *sprite, const char *directory) {
  if (!runtime_enabled || !directory) {
    original_override(sprite, directory);
    return;
  }
  if (override_route && override_route->plan) {
    if (!sprite_matches(sprite, override_route)) {
      dprintf(2, "DD_RIG_TEXTURE_PATH_REFUSED reason=wrong_sprite\n");
      return;
    }
    override_each(sprite, override_route->plan);
    dprintf(2, "DD_RIG_TEXTURES_ROUTED skin=%s source=%s resources=%zu\n",
            override_route->skin,
            *override_route->origin ? override_route->origin : "base",
            override_route->plan->count);
    return;
  }
  if (strncmp(directory, "heroes/", 7)) {
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
static void **preview_sprite_slot(void *actor);
/* Native campaign assets cache roster portraits by colour index. Two providers
 * may publish exactly the same logical path, so resolving that path globally
 * loses the selected provider even after the actor's rig has been corrected.
 * Repair every catalogue slot, including an already populated late-load cache,
 * and transfer the new native Handle's reference to the native owning vector.
 */
static void sync_portraits(void *actor) {
  if (!runtime_enabled || !load_texture)
    return;
  void **slot = preview_sprite_slot(actor);
  const SkinCatalog *catalog = slot ? source_catalog(actor_class(actor)) : NULL;
  if (!catalog)
    return;
  const Handles *view = (const void *)((const char *)slot - 0x38);
  uintptr_t begin = (uintptr_t)view->begin, end = (uintptr_t)view->end;
  uintptr_t capacity = (uintptr_t)view->capacity;
  if (!begin || end < begin || capacity < end ||
      (end - begin) % sizeof(Handle) ||
      (end - begin) / sizeof(Handle) != catalog->count ||
      (capacity - begin) % sizeof(Handle))
    return;
  void *manager = *(void **)(native_address(DD_RESOURCE));
  if (!manager)
    return;
  for (size_t i = 0; i < catalog->count; ++i) {
    const SkinSource *source = catalog->entries + i;
    if (!*source->origin)
      continue;
    const char *hero = source->skin + strlen("heroes/");
    const char *separator = strchr(hero, '/');
    char request[512], actual[512], key[256];
    int length =
        separator
            ? snprintf(request, sizeof(request), "%s/%.*s_portrait_roster.png",
                       source->skin, (int)(separator - hero), hero)
            : -1;
    if (length < 0 || (size_t)length >= sizeof(request) ||
        pinned_texture(actual, source->origin, request) != 1)
      continue;
    length = snprintf(key, sizeof(key), ">%s", actual);
    if (length < 0 || (size_t)length >= sizeof(key))
      continue;
    Handle *target = view->begin + i;
    const char *name = resource_name(manager, target);
    if (name && !strcmp(name, key))
      continue;
    Handle loaded = {0, -1, NULL};
    load_texture(&loaded, key, 0);
    name = resource_name(manager, &loaded);
    if (!name || strcmp(name, key)) {
      release_spine(&loaded);
      dprintf(2, "DD_RIG_PORTRAIT_REFUSED index=%zu source=%s\n", i,
              source->origin);
      continue;
    }
    Handle previous = *target;
    *target = loaded;
    release_spine(&previous);
    dprintf(2, "DD_RIG_PORTRAIT_ROUTED index=%zu skin=%s source=%s\n", i,
            source->skin, source->origin);
  }
}
static void apply_colour(void *actor, int colour, const Route *route) {
  const Route *previous = override_route;
  override_route = route;
  original_colour(actor, colour);
  override_route = previous;
  if (route)
    sync_portraits(actor);
}
static void apply_preview(void *panel, void *actor, const Route *route) {
  if (route)
    sync_portraits(actor);
  const Route *previous = override_route;
  override_route = route;
  original_preview(panel, actor);
  override_route = previous;
}
static void discard_sprite(void *sprite);
/* The class preview cache is shared between actors. If it last held a custom
 * sprite, restore the class's native handles before previewing a native actor.
 */
static int restore_native_preview(void **slot, void *klass) {
  ActorSprite *entry = actor_sprite_slot(slot, 0);
  if (!entry || !entry->routed || entry->sprite != *slot)
    return 1;
  const Handles *handles = (const void *)((const char *)klass + 0xe0);
  void *sprite = native_new(handles, 0);
  if (!sprite)
    return 0;
  void *old = *slot;
  *slot = sprite;
  entry->routed = 0;
  entry->sprite = sprite;
  discard_sprite(old);
  return 2;
}
static int remember_custom_preview(void **slot, void *sprite) {
  ActorSprite *entry = actor_sprite_slot(slot, 1);
  if (!entry)
    return 0;
  entry->routed = 1;
  entry->sprite = sprite;
  return 1;
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
  /* Addresses inside the synthetic memory owned by this test adapter. */
  native_symbols.values[DD_STORAGE] = 0x1d744f0;
  native_symbols.values[DD_RESOURCE] = 0x31c2b58;
  clear_routes();
  clear_catalogs();
  dd_config.hide_vanilla_skins = 0;
  runtime_enabled = 0;
  preserve_palette_sources = 0;
  alternate_count = NULL;
  override_route = NULL;
  match_files = match;
  destroy_match = destroy;
  full_path = resolve;
  load_spine = load;
  release_spine = release;
  get_resource = get;
  native_new = create;
  resource_name = name;
}
void *dd_runtime_test_actor_sprite(const char *skin, const Handles *original,
                                   int flag) {
  const Route *route = runtime_route(skin, NULL, NULL);
  return create_actor_sprite(original, flag, NULL, &route);
}
const void *dd_runtime_test_route(const char *skin) {
  return runtime_route(skin, NULL, NULL);
}
int dd_runtime_test_saved_route(const void *saved, const char *skin) {
  const Route *route = saved;
  return route && !strcmp(route->skin, skin) && route->plan &&
         route->plan->count && *route->plan->entries[0].key;
}
void *dd_runtime_test_sprite(const char *skin) {
  const Route *route = runtime_route(skin, NULL, NULL);
  return route ? make_sprite(NULL, route) : NULL;
}
int dd_runtime_test_matches(void *sprite, const char *skin) {
  const Route *route = runtime_route(skin, NULL, NULL);
  return route && sprite_matches(sprite, route);
}
void *dd_runtime_test_sprite_at(const char *skin, const char *origin) {
  const Route *route = runtime_route(skin, origin, NULL);
  return route ? make_sprite(NULL, route) : NULL;
}
const char *dd_runtime_test_texture(const char *skin, const char *origin,
                                    unsigned index) {
  const Route *route = runtime_route(skin, origin, NULL);
  return route && index < route->plan->count
             ? route->plan->entries[index].palette
             : NULL;
}
void dd_runtime_test_bind_sources(
    unsigned (*count)(void *), const char *(*path)(void *, unsigned),
    void (*match)(NativeFileMatch *, void *, const char *, const char *, int,
                  int, int),
    unsigned (*roots)(void *), const void *(*root)(void *, unsigned),
    void (*archive)(void *, NativeFileMatch *, const char *, const char *, int,
                    int, bool),
    int preserve_sources) {
  colour_count = count;
  colour_path = path;
  match_device = match;
  alternate_count = roots;
  alternate_root = root;
  match_archive = archive;
  preserve_palette_sources = preserve_sources;
}
const char *dd_runtime_test_source(void *klass, unsigned index) {
  const SkinCatalog *catalog = source_catalog(klass);
  return catalog && index < catalog->count ? catalog->entries[index].origin
                                           : NULL;
}
void dd_runtime_test_hide_vanilla(int enabled) {
  dd_config.hide_vanilla_skins = enabled;
}
int dd_runtime_test_visible_colour(void *klass, int requested, int current) {
  const SkinCatalog *catalog = source_catalog(klass);
  char game_root[4096];
  if (!catalog || !dd_config.hide_vanilla_skins ||
      !getcwd(game_root, sizeof(game_root)))
    return requested;
  return visible_catalog_colour(catalog, requested, current, game_root);
}
void dd_runtime_test_bind_colour(void (*apply)(void *, int)) {
  original_colour = apply;
  runtime_enabled = 1;
  native_symbols.values[DD_ACTOR_SPRITE_FIELD] = 0x100;
}
void dd_runtime_test_change_colour(void *actor, int colour) {
  /* This adapter tests colour selection without constructing a physical sprite.
   */
  ActorSprite *entry = actor_sprite_slot(actor, 1);
  if (entry) {
    entry->sprite = pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
    entry->routed = 1;
  }
  change_colour(actor, colour);
}
const char *dd_runtime_test_actor_skin(void *actor, int colour) {
  const Route *route = find_route(actor, colour);
  return route ? route->skin : NULL;
}
#endif
#ifdef _WIN32
#include "skin_rig_windows.h"
#else
static void discard_sprite(void *sprite) {
  void (*destroy)(void **) = (void *)(native_address(DD_DISCARD_SPRITE));
  destroy(&sprite); /* native deferred deletion, not a foreign free */
}
static const char *animation_name(void *sprite) {
  if (!sprite || !pointer_at(sprite, 0))
    return "";
  const char *(*name)(void *) = (void *)(native_address(DD_ANIMATION_NAME));
  return name(sprite);
}
static void start_animation(void *sprite, const char *name, float time) {
  set_animation(sprite, name && *name ? name : "idle", 1, 1);
  float (*length)(void *, const char *) =
      (void *)(native_address(DD_ANIMATION_LENGTH));
  float seconds = length(sprite, name && *name ? name : "idle");
  void (*set_time)(void *, float) = (void *)(native_address(DD_SET_TIME));
  if (isfinite(time) && time >= 0 && seconds > 0)
    set_time(sprite, fmodf(time, seconds));
}
static void native_two_sprite_test(void *klass, const Route *scope) {
  if (scope->tested)
    return;
  const Route *a = NULL, *b = NULL;
  for (size_t i = 0; i < route_count; ++i) {
    if (!same_hero(scope, routes[i]))
      continue;
    if (!a)
      a = routes[i];
    else if (!same_group(a, routes[i])) {
      b = routes[i];
      break;
    }
  }
  if (!a || !b)
    return;
  Handles probe = {0};
  size_t first = select_handles(klass, a, &probe);
  free(probe.begin);
  probe = (Handles){0};
  size_t second = select_handles(klass, b, &probe);
  free(probe.begin);
  if (!first || !second)
    return;
  for (size_t i = 0; i < route_count; ++i)
    if (same_hero(scope, routes[i]))
      routes[i]->tested = 1;
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
  const Route *route;
  void *klass = actor_class(actor);
  void *sprite = construct_actor_sprite(original, flag, actor, &route);
  if (!route)
    return sprite;
  dprintf(2, "DD_RIG_ACTOR_INIT actor=%p sprite=%p skin=%s group=%s\n", actor,
          sprite, route->skin, route->group);
  if (!runtime_enabled)
    native_two_sprite_test(klass, route);
  return sprite;
}
static void change_colour(void *actor, int colour) {
  int late = late_actor_sprite(actor);
  if (native_actor_sprite(actor) && !late) {
    apply_colour(actor, colour, NULL);
    return;
  }
  colour = skin_colour(actor, colour);
  const Route *route = find_route(actor, colour);
  void *old = pointer_at(actor, DD_ACTOR_SPRITE_OFFSET);
  if (late && !route) {
    apply_colour(actor, colour, NULL);
    return;
  }
  if (!route && runtime_enabled && runtime_actor(actor))
    return;
  if (!route || !old || sprite_matches(old, route)) {
    if (late && route)
      remember_actor_sprite(actor, old);
    apply_colour(actor, colour, route);
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
  float (*get_time)(void *) = (void *)(native_address(DD_GET_TIME));
  float time = name[0] ? get_time(old) : 0;
  void *sprite = make_sprite(actor_class(actor), route);
  if (!sprite)
    return;
  bool (*has_animation)(void *, const char *) =
      (void *)(native_address(DD_HAS_ANIMATION));
  if (!has_animation(sprite, name[0] ? name : "idle")) {
    if (name[0] && strcmp(name, "idle") && has_animation(sprite, "idle")) {
      dprintf(2, "DD_RIG_SKIN_ANIMATION_FALLBACK actor=%p from=%s to=idle\n",
              actor, name);
      memcpy(name, "idle", sizeof("idle"));
      time = 0;
    } else {
      discard_sprite(sprite);
      dprintf(2, "DD_RIG_SKIN_REFUSED_ANIMATION actor=%p animation=%s\n", actor,
              name);
      return;
    }
  }
  void (*set_sprite)(void *, void *) = (void *)(native_address(DD_SET_SPRITE));
  set_sprite(actor, sprite);
  remember_actor_sprite(actor, sprite);
  /* Exactly the same captured-Hero step callback constructed by
   * Hero::LoadAssetsInternal. */
  uint64_t callback[4] __attribute__((aligned(16))) = {
      (uintptr_t)actor, 0, native_address(DD_CALLBACK_MANAGER),
      native_address(DD_CALLBACK_INVOKER)};
  void (*add_step)(void *, unsigned, void *) =
      (void *)(native_address(DD_ADD_STEP));
  add_step(sprite, *(unsigned *)(native_address(DD_STEP_ID)), callback);
  apply_colour(actor, colour, route);
  start_animation(sprite, name, time);
  discard_sprite(old);
  dprintf(2,
          "DD_RIG_ACTOR_SWAP actor=%p old=%p new=%p skin=%s group=%s anim=%s\n",
          actor, old, sprite, route->skin, route->group, name);
}
static void **preview_sprite_slot(void *actor) {
  void *(*assets)(void) = (void *)(native_address(DD_ASSETS));
  void *campaign = assets(), *klass = actor_class(actor);
  if (!campaign)
    return NULL;
  unsigned id = (unsigned)int_at(klass, 0xd4);
  void *node = pointer_at(campaign, 0xe88);
  for (int depth = 0; node && depth < 64; ++depth) {
    unsigned key = (unsigned)int_at(node, 0x20);
    if (key == id)
      return (void **)((char *)node + 0x60);
    node = pointer_at(node, id < key ? 0x10 : 0x18);
  }
  return NULL;
}
static void change_preview(void *panel, void *actor) {
  if (late_actor_sprite(actor))
    change_colour(actor, int_at(actor, 0xb68));
  int native = native_actor_sprite(actor);
  const Route *route = native ? NULL : find_route(actor, int_at(actor, 0xb68));
  if (!native && !route && runtime_enabled && runtime_actor(actor))
    return;
  if (!native && !route) {
    apply_preview(panel, actor, NULL);
    return;
  }
  void **slot = preview_sprite_slot(actor);
  int restored =
      native && slot ? restore_native_preview(slot, actor_class(actor)) : 1;
  if (!restored)
    return;
  if (route && slot && *slot && !sprite_matches(*slot, route)) {
    void *sprite = make_sprite(actor_class(actor), route);
    if (!sprite)
      return;
    if (!remember_custom_preview(slot, sprite)) {
      discard_sprite(sprite);
      return;
    }
    void *old = *slot;
    *slot = sprite;
    apply_preview(panel, actor, route);
    start_animation(sprite, "idle", 0);
    discard_sprite(old);
    dprintf(2, "DD_RIG_PREVIEW_SWAP actor=%p skin=%s source=%s\n", actor,
            route->skin, route->origin);
    return;
  }
  apply_preview(panel, actor, route);
  if (restored == 2)
    start_animation(*slot, "idle", 0);
}

static void jump_absolute(unsigned char *out, void *destination) {
  out[0] = 0x48;
  out[1] = 0xb8;
  uintptr_t value = (uintptr_t)destination;
  memcpy(out + 2, &value, 8);
  out[10] = 0xff;
  out[11] = 0xe0;
}
static void encode_jump(unsigned char *out, uintptr_t site, void *stub) {
  int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(site + 5);
  out[0] = 0xe9;
  int32_t offset = (int32_t)delta;
  memcpy(out + 1, &offset, 4);
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
  dd_runtime_init((const void *)init);
  DdLinuxImage image = {0};
  if (!dd_linux_image(&image)) {
    dprintf(2, "DD_RIG_PATCH_SKIPPED reason=main_image\n");
    return;
  }
  base = image.base;
  const DdSignature *failed = NULL;
  int matches = 0;
  if (!dd_resolve_profile(image.ranges, image.count, DD_GROUP_RIG,
                          &native_symbols, &failed, &matches)) {
    dprintf(2, "DD_RIG_PATCH_SKIPPED feature=%s matches=%d\n", failed->name,
            matches);
    return;
  }
  if (!native_fields_valid())
    return;
  dprintf(2, "DD_RIG_LAYOUT_RESOLVED actor_sprite_offset=%zx\n",
          (size_t)DD_ACTOR_SPRITE_OFFSET);
  if (!runtime_enabled) {
    if (!map || !*map)
      return;
    FILE *file = fopen(map, "r");
    if (!file) {
      dprintf(2, "DD_RIG_PATCH_SKIPPED reason=route_map\n");
      return;
    }
    int valid = read_routes(file);
    fclose(file);
    if (!valid) {
      dprintf(2, "DD_RIG_PATCH_SKIPPED reason=route_map\n");
      return;
    }
  }
  uintptr_t sites[4] = {
      native_address(DD_HOOK_NEW), native_address(DD_HOOK_COLOUR),
      native_address(DD_HOOK_PREVIEW), native_address(DD_HOOK_TEXTURE)};
  const unsigned char expected[4][10] = {
      {0xe8, 0xf7, 0x2e, 0xb7, 0xff},
      {0x41, 0x57, 0x41, 0x56, 0x53},
      {0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x08, 0x02, 0, 0},
      {0x55, 0x41, 0x57, 0x41, 0x56}};
  size_t lengths[4] = {5, 5, 10, 5};
  size_t site_count = runtime_enabled ? 4 : 3;
  for (size_t i = 0; i < site_count; ++i)
    if (!dd_contains(image.ranges, image.count, sites[i] - base, lengths[i],
                     DD_CODE) ||
        !dd_guard((void *)sites[i], expected[i], lengths[i], sites[i],
                  native_address(DD_NATIVE_NEW))) {
      dprintf(2, "DD_RIG_PATCH_SKIPPED site=%zx\n", sites[i]);
      return;
    }
  unsigned char *stubs = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (stubs == MAP_FAILED) {
    dprintf(2, "DD_RIG_PATCH_SKIPPED reason=stub_allocation\n");
    return;
  }
  const size_t stub_offsets[] = {0, 96, 160, 224};
  for (size_t i = 0; i < site_count; ++i) {
    int64_t delta =
        (int64_t)(uintptr_t)(stubs + stub_offsets[i]) - (int64_t)(sites[i] + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
      dprintf(2, "DD_RIG_PATCH_SKIPPED reason=jump_range\n");
      munmap(stubs, 4096);
      return;
    }
  }
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
  native_new = (void *)(native_address(DD_NATIVE_NEW));
  resource_name = (void *)(native_address(DD_RESOURCE_NAME));
  set_animation = (void *)(native_address(DD_SET_ANIMATION));
  match_files = (void *)(native_address(DD_MATCH_FILES));
  destroy_match = (void *)(native_address(DD_DESTROY_MATCH));
  colour_count = (void *)(native_address(DD_COLOUR_COUNT));
  colour_path = (void *)(native_address(DD_COLOUR_PATH));
  match_device = (void *)(native_address(DD_MATCH_DEVICE));
  alternate_count = (void *)(native_address(DD_ALT_COUNT));
  alternate_root = (void *)(native_address(DD_ALT_ROOT));
  match_archive = (void *)(native_address(DD_MATCH_ARCHIVE));
  full_path = (void *)(native_address(DD_FULL_PATH));
  load_spine = (void *)(native_address(DD_LOAD_SPINE));
  load_texture = (void *)(native_address(DD_LOAD_TEXTURE));
  release_spine = (void *)(native_address(DD_RELEASE_SPINE));
  get_resource = (void *)(native_address(DD_GET_RESOURCE));
  if (mprotect(stubs, 4096, PROT_READ | PROT_EXEC)) {
    dprintf(2, "DD_RIG_PATCH_SKIPPED reason=stub_protection\n");
    munmap(stubs, 4096);
    return;
  }
  unsigned char originals[4][10], replacements[4][10];
  DdMemoryPatch patches[4];
  for (size_t i = 0; i < site_count; ++i) {
    memcpy(originals[i], (void *)sites[i], lengths[i]);
    memcpy(replacements[i], originals[i], lengths[i]);
    encode_jump(replacements[i], sites[i], stubs + stub_offsets[i]);
    if (!i)
      replacements[i][0] = 0xe8;
    patches[i] =
        (DdMemoryPatch){sites[i], lengths[i], originals[i], replacements[i]};
  }
  int patch_result = dd_linux_patch(&image, patches, site_count);
  if (patch_result) {
    dprintf(2, "DD_RIG_PATCH_SKIPPED reason=write_preparation code=%d\n",
            patch_result);
    munmap(stubs, 4096);
    return;
  }
  dprintf(2,
          "DD_RIG_PATCH_APPLIED mode=%s routes=%zu body=instance "
          "preview=selected_hero\n",
          runtime_enabled ? "runtime_source" : "map", route_count);
}
#endif /* platform native hooks */
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
  if (route_count != 5 || route_capacity < 5 || !routes)
    return 1;
  if (same_group(routes[0], routes[3]) || !same_group(routes[0], routes[2]) ||
      route_for_name("anim/common.rig_vanilla.idle.skel", routes[4]) !=
          routes[3] ||
      route_for_name("common.idle.skel", routes[0]) != routes[1] ||
      route_for_name("heroes/alpha/anim/common.idle.skel", routes[3]) ||
      route_for_name("heroes/beta/anim/common.idle.skel", routes[3]) !=
          routes[4] ||
      route_for_name("unmapped.idle.skel", routes[0]))
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
