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
  char palettes[256][512];
  size_t count;
} RigPlan;
typedef struct {
  char skin[256], hero[256], group[64], prefix[160];
  char origin[512];
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
      char palette[512];
      memcpy(palette, plan->palettes[j], sizeof(palette));
      memcpy(plan->palettes[j], plan->palettes[j - 1], sizeof(palette));
      memcpy(plan->palettes[j - 1], palette, sizeof(palette));
    }
}
#ifndef DD_PATCH_SELFTEST
static int same_plan(const RigPlan *a, const RigPlan *b) {
  if (!a || !b || a->count != b->count)
    return 0;
  for (size_t i = 0; i < a->count; ++i)
    if (strcmp(a->keys[i], b->keys[i]) ||
        strcmp(a->palettes[i], b->palettes[i]))
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
static unsigned (*colour_count)(void *);
static const char *(*colour_path)(void *, unsigned);
typedef struct {
  char (*begin)[256], (*end)[256], (*capacity)[256];
} NativeFileMatch;
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
  void *(*get_class)(void *) = pointer_at(table, 0x38);
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
static SkinCatalog catalogs[128];
static size_t catalog_count;

static void *data_device(void) {
  void *storage = *(void **)(base + 0x1d744f0);
  return storage ? pointer_at(storage, 0x38) : NULL;
}
static uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t count) {
  const unsigned char *p = bytes;
  for (size_t i = 0; i < count; ++i)
    hash = (hash ^ p[i]) * UINT64_C(1099511628211);
  return hash;
}
static int add_catalog_files(SkinCatalog *catalog, size_t limit,
                             const NativeFileMatch *files, const char *hero,
                             const char *origin) {
  ptrdiff_t count = files->begin ? files->end - files->begin : 0;
  if (count < 0 || count > 4096)
    return 0;
  for (ptrdiff_t i = 0; i < count; ++i) {
    const char *raw = files->begin[i];
    if (!memchr(raw, 0, 256))
      return 0;
    size_t j;
    /* This is the native details=0/flags=0 duplicate rule, before trimming.
     * A directory name ending in '/' is distinct from one without it. */
    for (j = 0; j < catalog->count; ++j)
      if (!strcmp(catalog->entries[j].raw, raw))
        break;
    if (j != catalog->count)
      continue;
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
  if (!count || count > 512 || !device)
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
  int n = snprintf(pattern, sizeof(pattern), "(.*)%s_([A-Z])(\\\\\\\\|/)",
                   hero + 7);
  if (n < 0 || (size_t)n >= sizeof(pattern))
    return NULL;
  uint64_t signature = UINT64_C(14695981039346656037);
  for (unsigned i = 0; i < count; ++i) {
    const char *path = colour_path(klass, i);
    if (!path || strnlen(path, 512) == 512)
      return NULL;
    signature = hash_bytes(signature, path, strlen(path) + 1);
  }
  unsigned roots = alternate_count(device);
  if (roots > 4096)
    return NULL;
  for (unsigned i = 0; i < roots; ++i) {
    const char *root = alternate_root(device, i);
    if (!root || !memchr(root, 0, 512))
      return NULL;
    signature = hash_bytes(signature, root, strlen(root) + 1);
    signature = hash_bytes(signature, root + 0x200, 16);
  }
  for (size_t i = 0; i < catalog_count; ++i)
    if (catalogs[i].klass == klass && catalogs[i].signature == signature)
      return catalogs + i;
  SkinCatalog catalog = {klass, signature, 0,
                         calloc(count, sizeof(SkinSource))};
  if (!catalog.entries)
    return NULL;
  NativeFileMatch files = {0};
  match_device(&files, device, directory, pattern, 0, 0, 0);
  int valid = add_catalog_files(&catalog, count, &files, hero, "");
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
    valid = add_catalog_files(&catalog, count, &files, hero, origin);
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
  if (!valid || catalog_count == 128) {
    dprintf(2, "DD_RIG_PALETTE_CATALOG_REFUSED class=%p native=%u found=%zu\n",
            klass, count, catalog.count);
    free(catalog.entries);
    return NULL;
  }
  for (size_t i = 0; i < catalog.count; ++i)
    dprintf(2, "DD_RIG_PALETTE_ENTRY class=%p index=%zu skin=%s source=%s\n",
            klass, i, catalog.entries[i].skin,
            *catalog.entries[i].origin ? catalog.entries[i].origin : "base");
  catalogs[catalog_count] = catalog;
  return catalogs + catalog_count++;
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
    size_t count = (size_t)(files->end - files->begin), i;
    for (i = 0; i < count; ++i)
      if (!strcmp(files->begin[i], entry->d_name))
        break;
    if (i < count)
      continue;
    if (count == 4096 || strlen(entry->d_name) >= 256) {
      valid = 0;
      break;
    }
    snprintf(*files->end++, 256, "%s", entry->d_name);
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
    if (!strcmp(plan->keys[i], key)) {
      if (*plan->palettes[i] && strcmp(plan->palettes[i], directory)) {
        dprintf(2, "DD_RIG_TEXTURE_SOURCE_CONFLICT rig=%s\n", key);
        return 0;
      }
      memcpy(plan->palettes[i], directory, strlen(directory) + 1);
      return 1;
    }
  return 0;
}
static const Route *runtime_route(const char *skin, const char *origin) {
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
  int valid = 1;
  if (!origin)
    match_files(&files, 1, palette, ".*\\.(png|PNG|tga|TGA)", 0, 0, 2);
  else {
    files.begin = calloc(4096, sizeof(*files.begin));
    if (!files.begin)
      return NULL;
    files.end = files.begin;
    files.capacity = files.begin + 4096;
    valid = append_palette_files(&files, origin, palette) &&
            append_palette_files(&files, "", palette);
    void *device = alternate_count ? data_device() : NULL;
    unsigned roots = device ? alternate_count(device) : 0;
    for (unsigned i = roots; valid && i-- > 0;) {
      const char *root = alternate_root(device, i);
      if (root && !strncmp(root, "dlc/", 4))
        valid = append_palette_files(&files, root, palette);
    }
  }
  ptrdiff_t count = files.begin ? files.end - files.begin : 0;
  valid = valid && count > 0 && count <= 4096;
  void *storage = *(void **)(base + 0x1d744f0);
  for (ptrdiff_t i = 0; valid && i < count; ++i) {
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
      valid = add_rig_key(&plan, candidates.keys[j]) &&
              record_palette(&plan, candidates.keys[j], actual);
    }
  }
  if (origin)
    free(files.begin);
  else
    destroy_match(&files);
  if (!valid || !plan.count) {
    dprintf(2, "DD_RIG_SOURCE_REFUSED skin=%s\n", skin);
    return NULL;
  }
  sort_plan(&plan);
  for (size_t i = 0; i < route_count; ++i)
    if (!strcmp(routes[i].skin, skin) &&
        !strcmp(routes[i].origin, origin ? origin : "") &&
        same_plan(routes[i].plan, &plan))
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
  snprintf(route->origin, sizeof(route->origin), "%s", origin ? origin : "");
  memcpy(route->hero, skin, (size_t)(separator - skin));
  memcpy(route->group, "runtime", sizeof("runtime"));
  dprintf(2,
          "DD_RIG_SOURCE_RESOLVED skin=%s source=%s resources=%zu first=%s\n",
          skin, origin && *origin ? origin : "base", plan.count, plan.keys[0]);
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
    const SkinSource *source = catalog->entries + colour;
    return runtime_route(source->skin, source->origin);
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
static void override_each(void *sprite, const RigPlan *plan) {
  unsigned char view[0x30] = {0};
  unsigned char *states = pointer_at(sprite, 0x20);
  for (size_t i = 0; i < plan->count; ++i) {
    void *begin = states + i * 0x30, *end = states + (i + 1) * 0x30;
    /* The native loop only reads this vector pair. Operate on the original
     * PerSpineDataState so native texture ownership/refcounts stay intact. */
    memcpy(view + 0x20, &begin, sizeof(begin));
    memcpy(view + 0x28, &end, sizeof(end));
    original_override(view, plan->palettes[i]);
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
static void apply_colour(void *actor, int colour, const Route *route) {
  const Route *previous = override_route;
  override_route = route;
  original_colour(actor, colour);
  override_route = previous;
}
static void apply_preview(void *panel, void *actor, const Route *route) {
  const Route *previous = override_route;
  override_route = route;
  original_preview(panel, actor);
  override_route = previous;
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
  for (size_t i = 0; i < catalog_count; ++i)
    free(catalogs[i].entries);
  catalog_count = 0;
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
void *dd_runtime_test_sprite(const char *skin) {
  const Route *route = runtime_route(skin, NULL);
  return route ? make_sprite(NULL, route) : NULL;
}
int dd_runtime_test_matches(void *sprite, const char *skin) {
  const Route *route = runtime_route(skin, NULL);
  return route && sprite_matches(sprite, route);
}
void *dd_runtime_test_sprite_at(const char *skin, const char *origin) {
  const Route *route = runtime_route(skin, origin);
  return route ? make_sprite(NULL, route) : NULL;
}
const char *dd_runtime_test_texture(const char *skin, const char *origin,
                                    unsigned index) {
  const Route *route = runtime_route(skin, origin);
  return route && index < route->plan->count ? route->plan->palettes[index]
                                             : NULL;
}
void dd_runtime_test_bind_sources(
    unsigned (*count)(void *), const char *(*path)(void *, unsigned),
    void (*match)(NativeFileMatch *, void *, const char *, const char *, int,
                  int, int),
    unsigned (*roots)(void *), const void *(*root)(void *, unsigned),
    void (*archive)(void *, NativeFileMatch *, const char *, const char *, int,
                    int, bool)) {
  colour_count = count;
  colour_path = path;
  match_device = match;
  alternate_count = roots;
  alternate_root = root;
  match_archive = archive;
}
const char *dd_runtime_test_source(void *klass, unsigned index) {
  const SkinCatalog *catalog = source_catalog(klass);
  return catalog && index < catalog->count ? catalog->entries[index].origin
                                           : NULL;
}
void dd_runtime_test_override(void *sprite, const char *directory,
                              void (*native)(void *, const char *)) {
  runtime_enabled = 1;
  original_override = native;
  override_textures(sprite, directory);
}
void dd_runtime_test_override_each(void *sprite, const char **directories,
                                   unsigned count,
                                   void (*native)(void *, const char *)) {
  if (count > 256)
    return;
  RigPlan plan = {0};
  plan.count = count;
  for (unsigned i = 0; i < count; ++i)
    snprintf(plan.palettes[i], sizeof(plan.palettes[i]), "%s", directories[i]);
  original_override = native;
  override_each(sprite, &plan);
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
  apply_colour(actor, colour, route);
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
    apply_preview(panel, actor, route);
    return;
  }
  void *(*assets)(void) = (void *)(base + 0x1170d60);
  void *campaign = assets(), *klass = actor_class(actor);
  if (!campaign) {
    apply_preview(panel, actor, route);
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
        apply_preview(panel, actor, route);
        start_animation(sprite, "idle", 0);
        discard_sprite(old);
        dprintf(2, "DD_RIG_PREVIEW_SWAP hero=%p old=%p new=%p group=%s\n",
                actor, old, sprite, route->group);
        return;
      }
    }
  }
  apply_preview(panel, actor, route);
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
               {0xa70a60, "\x55\x41\x57\x41\x56\x41\x55\x41\x54\x53\x50", 11},
               {0xc27c30, "\x41\x56\x53\x50", 4},
               {0xa40930, "\x48\x8b\x47\x10\x48\x2b\x47\x08", 8},
               {0xa40950, "\x89\xf1\x48\x8b\x47\x08", 6},
               {0xa41700, "\x55\x41\x57\x41\x56\x41\x55\x41\x54\x53", 10},
               {0xd7b130, "\x48\x8b\x87\x08\x06\x00\x00", 7},
               {0xd7b150, "\x89\xf0\x48\xc1\xe0\x09", 6}};
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
  colour_count = (void *)(base + 0xd7b130);
  colour_path = (void *)(base + 0xd7b150);
  match_device = (void *)(base + 0xc27c30);
  alternate_count = (void *)(base + 0xa40930);
  alternate_root = (void *)(base + 0xa40950);
  match_archive = (void *)(base + 0xa41700);
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
