#define _GNU_SOURCE
#include "linux_patch.h"
#include "linux_runtime.h"
#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Candidate instruction offsets. Only immediate operands change. */
enum { OLD_CAP = 0x600000, NEW_CAP = 0x2000000, SITE_COUNT = 7 };
#ifdef DD_PATCH_SELFTEST
static const uintptr_t sites[SITE_COUNT] = {0x0ffe, 0x1100, 0x1110, 0x1120,
                                            0x1130, 0x1140, 0x1150};
#endif
struct image {
  DdLinuxImage native;
  DdResolution symbols;
};
static uintptr_t site_address(const struct image *image, DdSymbol symbol) {
  return image->native.base + image->symbols.values[symbol];
}
static int apply_patch(const struct image *image) {
  unsigned char expected[SITE_COUNT][5], replacement[SITE_COUNT][5];
  DdMemoryPatch patches[SITE_COUNT];
  const uint32_t cap = NEW_CAP;
  for (size_t i = 0; i < SITE_COUNT; ++i) {
    const unsigned char original[5] = {i == 6 ? 0xb9 : 0xbf, 0, 0, 0x60, 0};
    memcpy(expected[i], original, 5);
    memcpy(replacement[i], original, 5);
    memcpy(replacement[i] + 1, &cap, 4);
    patches[i] = (DdMemoryPatch){site_address(image, DD_RESOURCE_0 + i), 5,
                                 expected[i], replacement[i]};
  }
  return dd_linux_patch(&image->native, patches, SITE_COUNT);
}

/* Darkest::Start: follow the fitted physical canvas, retaining logical UI
 * coordinates. Existing comparisons/cmov instructions become harmless no-ops.
 */
#ifdef DD_PATCH_SELFTEST
static const uintptr_t render_sites[2] = {0x2ffd, 0x3010};
static const uintptr_t dungeon_site = 0x4000;
static const uintptr_t gui_sites[4] = {0x3030, 0x3040, 0x3050, 0x3060};
#endif
static const size_t render_lengths[2] = {5, 6};
static const unsigned char render_expected[2][8] = {
    {0xbd, 0x80, 0x07, 0, 0}, {0x41, 0xbe, 0x38, 0x04, 0, 0}};
static const unsigned char render_replacement[2][8] = {
    {0x44, 0x89, 0xf5, 0x90, 0x90},      /* mov r14d,ebp */
    {0x45, 0x89, 0xfe, 0x90, 0x90, 0x90} /* mov r15d,r14d */
};
static const unsigned char dungeon_expected[3] = {0x0f, 0x47, 0xe8};
/* mov eax,ebp; nop: use physical main-target width, keeping the logical canvas.
 */
static const unsigned char dungeon_replacement[3] = {0x89, 0xc5, 0x90};
static const unsigned char gui_expected[4][5] = {{0xbf, 0x80, 0x07, 0, 0},
                                                 {0xbe, 0x38, 0x04, 0, 0},
                                                 {0xbf, 0x80, 0x07, 0, 0},
                                                 {0xbe, 0x38, 0x04, 0, 0}};
/* Use saved physical main width and r14d main height. Logical UI stays
 * 1920x1080. */
static const unsigned char gui_replacement[4][5] = {
    {0x8b, 0x7c, 0x24, 0x1c, 0x90},
    {0x44, 0x89, 0xf6, 0x90, 0x90},
    {0x8b, 0x7c, 0x24, 0x1c, 0x90},
    {0x44, 0x89, 0xf6, 0x90, 0x90}};

static int apply_render_patch(const struct image *image) {
  DdMemoryPatch patches[7];
  for (size_t i = 0; i < 2; ++i)
    patches[i] =
        (DdMemoryPatch){site_address(image, DD_RENDER_0 + i), render_lengths[i],
                        render_expected[i], render_replacement[i]};
  patches[2] = (DdMemoryPatch){site_address(image, DD_RENDER_2), 3,
                               dungeon_expected, dungeon_replacement};
  for (size_t i = 0; i < 4; ++i)
    patches[i + 3] = (DdMemoryPatch){site_address(image, DD_RENDER_3 + i), 5,
                                     gui_expected[i], gui_replacement[i]};
  int result = dd_linux_patch(&image->native, patches, 7);
  return result == 3 ? 22 : result ? 21 : 0;
}

#ifndef DD_PATCH_SELFTEST
__attribute__((constructor)) static void initialize(void) {
  const char *enabled = getenv("DD_LOADBUFFER_PATCH");
  if (!enabled || strcmp(enabled, "1"))
    return;
  char executable[4096];
  ssize_t length =
      readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (length < 0 || (size_t)length >= sizeof(executable) - 1)
    return;
  executable[length] = '\0';
  const char *name = strrchr(executable, '/');
  if (!name || strcmp(name + 1, "darkest.bin.x86_64"))
    return;
  dd_linux_log();
  struct image image = {0};
  if (!dd_linux_image(&image.native)) {
    dprintf(STDERR_FILENO, "DD_LOADBUFFER_PATCH_SKIPPED reason=main_image\n");
    return;
  }
  const char *render = getenv("DD_NATIVE_RENDER");
  int render_enabled = render && !strcmp(render, "1");
  const DdSignature *failed = NULL;
  int matches = 0;
  int resources_found =
      dd_resolve_profile(image.native.ranges, image.native.count,
                         DD_GROUP_RESOURCE, &image.symbols, &failed, &matches);
  if (!resources_found)
    dprintf(2, "DD_LOADBUFFER_PATCH_SKIPPED feature=%s matches=%d\n",
            failed->name, matches);
  int render_found = 0;
  if (render_enabled) {
    render_found =
        dd_resolve_profile(image.native.ranges, image.native.count,
                           DD_GROUP_RENDER, &image.symbols, &failed, &matches);
    if (!render_found)
      dprintf(2, "DD_NATIVE_RENDER_PATCH_SKIPPED feature=%s matches=%d\n",
              failed->name, matches);
  }
  int result = resources_found ? apply_patch(&image) : 0;
  if (result) {
    dprintf(STDERR_FILENO, "DD_LOADBUFFER_PATCH_SKIPPED code=%d\n", result);
  } else if (resources_found)
    dprintf(STDERR_FILENO,
            "DD_LOADBUFFER_PATCH_APPLIED cap=%u buffers=6 sites=7\n", NEW_CAP);
  if (render_found) {
    result = apply_render_patch(&image);
    if (result) {
      dprintf(STDERR_FILENO, "DD_NATIVE_RENDER_PATCH_SKIPPED code=%d\n",
              result);
    } else
      dprintf(STDERR_FILENO, "DD_NATIVE_RENDER_PATCH_APPLIED cap=game_canvas "
                             "sites=7 dungeon=physical_width gui=main_size\n");
  }
}

#else
int main(void) {
  const size_t length = 0x6000;
  unsigned char *memory = mmap(NULL, length, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED)
    return 10;
  struct image image = {0};
  image.native.base = (uintptr_t)memory;
  image.native.ranges[0] =
      (DdRange){memory, length, 0, length, DD_CODE | DD_READ};
  image.native.count = 1;
  for (size_t i = 0; i < SITE_COUNT; ++i)
    image.symbols.values[DD_RESOURCE_0 + i] = sites[i];
  for (size_t i = 0; i < 2; ++i)
    image.symbols.values[DD_RENDER_0 + i] = render_sites[i];
  image.symbols.values[DD_RENDER_2] = dungeon_site;
  for (size_t i = 0; i < 4; ++i)
    image.symbols.values[DD_RENDER_3 + i] = gui_sites[i];
  for (size_t i = 0; i < SITE_COUNT; ++i) {
    unsigned char bytes[5] = {i == 6 ? 0xb9 : 0xbf, 0, 0, 0x60, 0};
    memcpy(memory + sites[i], bytes, 5);
  }
  memory[sites[6]] ^= 1;
  if (apply_patch(&image) != 3)
    return 11;
  for (size_t i = 0; i < SITE_COUNT; ++i) {
    uint32_t value;
    memcpy(&value, memory + sites[i] + 1, 4);
    if (value != OLD_CAP)
      return 12;
  }
  memory[sites[6]] ^= 1;
  if (apply_patch(&image))
    return 13;
  if (apply_patch(&image) != 3)
    return 14;
  for (size_t i = 0; i < 2; ++i)
    memcpy(memory + render_sites[i], render_expected[i], render_lengths[i]);
  memcpy(memory + dungeon_site, dungeon_expected, 3);
  for (size_t i = 0; i < 4; ++i)
    memcpy(memory + gui_sites[i], gui_expected[i], 5);
  memory[render_sites[1]] ^= 1;
  if (apply_render_patch(&image) != 22)
    return 16;
  if (memcmp(memory + render_sites[0], render_expected[0], render_lengths[0]))
    return 17;
  memory[render_sites[1]] ^= 1;
  memory[dungeon_site] ^= 1;
  if (apply_render_patch(&image) != 22)
    return 21;
  if (memcmp(memory + render_sites[0], render_expected[0], render_lengths[0]))
    return 22;
  memory[dungeon_site] ^= 1;
  memory[gui_sites[3]] ^= 1;
  if (apply_render_patch(&image) != 22)
    return 24;
  if (memcmp(memory + render_sites[0], render_expected[0], render_lengths[0]))
    return 25;
  memory[gui_sites[3]] ^= 1;
  if (apply_render_patch(&image))
    return 18;
  if (memcmp(memory + dungeon_site, dungeon_replacement, 3))
    return 23;
  for (size_t i = 0; i < 4; ++i)
    if (memcmp(memory + gui_sites[i], gui_replacement[i], 5))
      return 26;
  for (size_t i = 0; i < 2; ++i)
    if (memcmp(memory + render_sites[i], render_replacement[i],
               render_lengths[i]))
      return 19;
  if (apply_render_patch(&image) != 22)
    return 20;
  if (munmap(memory, length))
    return 15;
  puts("PATCH_SELFTEST_PASS: loader and render operands; mismatch changes "
       "nothing; repeat refused");
  return 0;
}
#endif
