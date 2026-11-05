#ifndef DD_LINUX_PATCH_H
#define DD_LINUX_PATCH_H
#include "linux_image.h"
#include <sys/mman.h>
#include <unistd.h>
typedef struct {
  uintptr_t address;
  size_t length;
  const unsigned char *expected, *replacement;
} DdMemoryPatch;
static inline int dd_linux_patch(const DdLinuxImage *image,
                                 const DdMemoryPatch *patches, size_t count) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size < 16 || (page_size & (page_size - 1)))
    return 1;
  uintptr_t pages[64];
  size_t page_count = 0;
  for (size_t i = 0; i < count; ++i) {
    const DdMemoryPatch *p = patches + i;
    if (!p->length || p->address < image->base ||
        !dd_contains(image->ranges, image->count, p->address - image->base,
                     p->length, DD_CODE) ||
        p->length > UINTPTR_MAX - p->address)
      return 2;
    if (memcmp((void *)p->address, p->expected, p->length))
      return 3;
    uintptr_t first = p->address & ~((uintptr_t)page_size - 1);
    uintptr_t last = (p->address + p->length - 1) & ~((uintptr_t)page_size - 1);
    for (uintptr_t page = first;; page += (uintptr_t)page_size) {
      size_t j = 0;
      while (j < page_count && pages[j] != page)
        ++j;
      if (j == page_count) {
        if (page_count == sizeof(pages) / sizeof(*pages))
          return 4;
        pages[page_count++] = page;
      }
      if (page == last)
        break;
    }
  }
  size_t writable = 0;
  for (; writable < page_count; ++writable)
    if (mprotect((void *)pages[writable], (size_t)page_size,
                 PROT_READ | PROT_WRITE))
      break;
  if (writable != page_count) {
    for (size_t i = 0; i < writable; ++i)
      if (mprotect((void *)pages[i], (size_t)page_size, PROT_READ | PROT_EXEC))
        _exit(125);
    return 5;
  }
  for (size_t i = 0; i < count; ++i)
    memcpy((void *)patches[i].address, patches[i].replacement,
           patches[i].length);
  for (size_t i = 0; i < page_count; ++i) {
    __builtin___clear_cache((char *)pages[i], (char *)(pages[i] + page_size));
    if (mprotect((void *)pages[i], (size_t)page_size, PROT_READ | PROT_EXEC))
      _exit(125);
  }
  return 0;
}
#endif
