#ifndef DD_PALETTE_SOURCES_H
#define DD_PALETTE_SOURCES_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char (*begin)[256], (*end)[256], (*capacity)[256];
} NativeFileMatch;

static inline int file_match_count(const NativeFileMatch *files,
                                   size_t *count) {
  uintptr_t begin = (uintptr_t)files->begin, end = (uintptr_t)files->end,
            capacity = (uintptr_t)files->capacity;
  *count = 0;
  if (!begin)
    return !end && !capacity;
  if (end < begin || capacity < end || (end - begin) % sizeof(*files->begin) ||
      (capacity - begin) % sizeof(*files->begin))
    return 0;
  *count = (end - begin) / sizeof(*files->begin);
  return 1;
}

#if defined(_WIN32) || defined(DD_PALETTE_SOURCES_TEST)
typedef void (*PaletteRootMatch)(void *, NativeFileMatch *, void *, unsigned,
                                 const char *, const char *, int, int, int);

static int palette_source_query(const char *directory, const char *pattern,
                                int details, int order, int flags) {
  if (!directory || !pattern || details || order || flags ||
      strncmp(directory, "heroes/", 7))
    return 0;
  const char *end = strchr(directory + 7, '/');
  if (!end || end == directory + 7 || end[1])
    return 0;
  char expected[512];
  int n = snprintf(expected, sizeof(expected), "(.*)%.*s_([A-Z])(\\\\|/)",
                   (int)(end - directory - 7), directory + 7);
  return n > 0 && (size_t)n < sizeof(expected) && !strcmp(expected, pattern);
}

/* The native root merger discards names already returned by another source.
 * Enumerate palette roots separately and append with the game's allocator so
 * equal logical paths retain distinct palette indices and ownership. */
static void match_palette_root(PaletteRootMatch find,
                               void (*append)(NativeFileMatch *, const void *),
                               void (*destroy)(NativeFileMatch *),
                               void *storage, NativeFileMatch *files,
                               void *device, unsigned root,
                               const char *directory, const char *pattern,
                               int details, int order, int flags) {
  if (!palette_source_query(directory, pattern, details, order, flags)) {
    find(storage, files, device, root, directory, pattern, details, order,
         flags);
    return;
  }
  NativeFileMatch source = {0};
  find(storage, &source, device, root, directory, pattern, details, order,
       flags);
  size_t count;
  int valid = file_match_count(&source, &count);
  for (size_t i = 0; valid && i < count; ++i)
    valid = memchr(source.begin[i], 0, sizeof(source.begin[i])) != NULL;
  if (valid)
    for (size_t i = 0; i < count; ++i)
      append(files, source.begin[i]);
  destroy(&source);
  if (!valid)
    find(storage, files, device, root, directory, pattern, details, order,
         flags);
}
#endif
#endif
