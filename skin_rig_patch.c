#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
  char skin[256], hero[256], group[64], prefix[160];
  int tested;
} Route;
static Route routes[128];
static size_t route_count;

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
static void *(*native_new)(const Handles *, int);
static const char *(*resource_name)(void *, const Handle *);
static void (*set_animation)(void *, const char *, int, int);

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
  return same_group(resource_route(first, route), route);
}
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
  if (!route)
    return native_new(original, flag);
  void *klass = actor_class(actor);
  void *sprite = make_sprite(klass, route);
  if (!sprite)
    return native_new(original, flag);
  dprintf(2, "DD_RIG_ACTOR_INIT actor=%p sprite=%p skin=%s group=%s\n", actor,
          sprite, route->skin, route->group);
  native_two_sprite_test(klass, route);
  return sprite;
}
static void change_colour(void *actor, int colour) {
  const Route *route = find_route(actor, colour);
  void *old = pointer_at(actor, 0x1070);
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
  if (!map || !*map)
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
  FILE *file = fopen(map, "r");
  if (!file)
    _exit(125);
  int valid = read_routes(file);
  fclose(file);
  if (!valid)
    _exit(125);
  uintptr_t sites[3] = {base + 0xefc214, base + 0xd66e80, base + 0x11a8400};
  const unsigned char expected[3][10] = {
      {0xe8, 0xf7, 0x2e, 0xb7, 0xff},
      {0x41, 0x57, 0x41, 0x56, 0x53},
      {0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x08, 0x02, 0, 0}};
  size_t lengths[3] = {5, 5, 10};
  for (size_t i = 0; i < 3; ++i)
    if (memcmp((void *)sites[i], expected[i], lengths[i])) {
      dprintf(2, "DD_RIG_PATCH_REFUSED site=%zx\n", sites[i]);
      _exit(125);
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
  original_colour = (void *)(stubs + 64);
  original_preview = (void *)(stubs + 128);
  native_new = (void *)(base + 0xa6f110);
  resource_name = (void *)(base + 0xc3e910);
  set_animation = (void *)(base + 0xa6f2f0);
  if (mprotect(stubs, 4096, PROT_READ | PROT_EXEC))
    _exit(125);
  long page_size = sysconf(_SC_PAGESIZE);
  uintptr_t pages[3];
  for (size_t i = 0; i < 3; ++i) {
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
  for (size_t i = 0; i < 3; ++i) {
    __builtin___clear_cache((char *)pages[i], (char *)(pages[i] + page_size));
    if (mprotect((void *)pages[i], (size_t)page_size, PROT_READ | PROT_EXEC))
      _exit(125);
  }
  dprintf(
      2,
      "DD_RIG_PATCH_APPLIED routes=%zu body=instance preview=selected_hero\n",
      route_count);
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
int main(void) {
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
