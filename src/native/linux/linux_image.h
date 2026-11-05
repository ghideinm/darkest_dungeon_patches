#ifndef DD_LINUX_IMAGE_H
#define DD_LINUX_IMAGE_H
#include "../common/address_profile.h"
#include <link.h>
enum { DD_IMAGE_RANGES = 32 };
typedef struct {
  uintptr_t base;
  DdRange ranges[DD_IMAGE_RANGES];
  size_t count;
} DdLinuxImage;
static inline int dd_linux_image_callback(struct dl_phdr_info *info,
                                          size_t size, void *opaque) {
  (void)size;
  if (info->dlpi_name && *info->dlpi_name)
    return 0;
  DdLinuxImage *image = opaque;
  image->base = info->dlpi_addr;
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) *p = info->dlpi_phdr + i;
    if (p->p_type != PT_LOAD)
      continue;
    if (image->count == DD_IMAGE_RANGES)
      return 1;
    image->ranges[image->count++] =
        (DdRange){(const void *)(image->base + p->p_vaddr), p->p_filesz,
                  p->p_vaddr, p->p_memsz,
                  ((p->p_flags & PF_X) ? DD_CODE : 0) |
                      ((p->p_flags & PF_W) ? DD_WRITE : 0) |
                      ((p->p_flags & PF_R) ? DD_READ : 0)};
  }
  return 1;
}
static inline int dd_linux_image(DdLinuxImage *image) {
  return dl_iterate_phdr(dd_linux_image_callback, image) && image->count;
}
#endif
