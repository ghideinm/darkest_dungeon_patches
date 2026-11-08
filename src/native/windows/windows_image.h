#ifndef DD_WINDOWS_IMAGE_H
#define DD_WINDOWS_IMAGE_H
#include "../common/address_profile.h"
#include <stdlib.h>
#include <windows.h>
/* Windows' PE loader supports at most 96 sections. A section can contain an
 * unbounded number of VirtualQuery regions after copy-on-write page changes.
 */
enum { DD_PE_SECTIONS = 96 };
typedef struct {
  DdRange *ranges;
  unsigned char *owned[DD_PE_SECTIONS];
  size_t count, capacity, owned_count;
  void *view;
  HANDLE mapping;
} DdWindowsImage;
static inline unsigned dd_section_flags(DWORD flags) {
  return ((flags & IMAGE_SCN_MEM_EXECUTE) ? DD_CODE : 0) |
         ((flags & IMAGE_SCN_MEM_WRITE) ? DD_WRITE : 0) |
         ((flags & IMAGE_SCN_MEM_READ) ? DD_READ : 0);
}
static inline int dd_windows_pe(const IMAGE_NT_HEADERS64 *pe) {
  return pe->Signature == IMAGE_NT_SIGNATURE &&
         pe->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
         pe->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
         pe->FileHeader.SizeOfOptionalHeader ==
             sizeof(IMAGE_OPTIONAL_HEADER64) &&
         pe->FileHeader.NumberOfSections > 0 &&
         pe->FileHeader.NumberOfSections <= DD_PE_SECTIONS;
}
static inline void dd_windows_image_close(DdWindowsImage *image) {
  for (size_t i = 0; i < image->owned_count; ++i)
    free(image->owned[i]);
  free(image->ranges);
  if (image->view)
    UnmapViewOfFile(image->view);
  if (image->mapping)
    CloseHandle(image->mapping);
  memset(image, 0, sizeof(*image));
}
static inline int dd_windows_add_range(DdWindowsImage *image, DdRange range) {
  if (image->count == image->capacity) {
    size_t capacity = image->capacity ? image->capacity * 2 : 8;
    if (capacity < image->capacity ||
        capacity > SIZE_MAX / sizeof(*image->ranges))
      return 0;
    void *grown = realloc(image->ranges, capacity * sizeof(*image->ranges));
    if (!grown)
      return 0;
    image->ranges = grown;
    image->capacity = capacity;
  }
  image->ranges[image->count++] = range;
  return 1;
}
static inline int dd_windows_add_loaded_range(DdWindowsImage *image,
                                              DdRange range) {
  if (image->count) {
    DdRange *previous = image->ranges + image->count - 1;
    if (previous->flags == range.flags &&
        previous->size == previous->memory_size &&
        previous->address + previous->size == range.address &&
        previous->bytes + previous->size == range.bytes) {
      previous->size += range.size;
      previous->memory_size += range.memory_size;
      return 1;
    }
  }
  return dd_windows_add_range(image, range);
}
static inline int dd_windows_file_image(HANDLE file, DdWindowsImage *image) {
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 64)
    return 0;
  image->mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!image->mapping)
    return 0;
  image->view = MapViewOfFile(image->mapping, FILE_MAP_READ, 0, 0, 0);
  if (!image->view)
    return 0;
  const unsigned char *bytes = image->view;
  const IMAGE_DOS_HEADER *dos = image->view;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 ||
      (uint64_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) >
          (uint64_t)size.QuadPart)
    return 0;
  const IMAGE_NT_HEADERS64 *pe =
      (const void *)(bytes + (unsigned)dos->e_lfanew);
  if (!dd_windows_pe(pe) ||
      (uint64_t)dos->e_lfanew + sizeof(*pe) +
              pe->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) >
          (uint64_t)size.QuadPart)
    return 0;
  const IMAGE_SECTION_HEADER *sections = (const void *)(pe + 1);
  for (size_t i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
    const IMAGE_SECTION_HEADER *s = sections + i;
    if ((uint64_t)s->PointerToRawData + s->SizeOfRawData >
        (uint64_t)size.QuadPart)
      return 0;
    size_t memory = s->Misc.VirtualSize > s->SizeOfRawData ? s->Misc.VirtualSize
                                                           : s->SizeOfRawData;
    if ((uint64_t)s->VirtualAddress + memory > pe->OptionalHeader.SizeOfImage)
      return 0;
    if (!dd_windows_add_range(
            image, (DdRange){bytes + s->PointerToRawData, s->SizeOfRawData,
                             s->VirtualAddress, memory,
                             dd_section_flags(s->Characteristics)}))
      return 0;
  }
  return 1;
}
static inline int dd_windows_remote_image(HANDLE process, uintptr_t base,
                                          DdWindowsImage *image) {
  IMAGE_DOS_HEADER dos;
  IMAGE_NT_HEADERS64 pe;
  IMAGE_SECTION_HEADER sections[DD_PE_SECTIONS];
  SIZE_T read;
  if (!ReadProcessMemory(process, (void *)base, &dos, sizeof(dos), &read) ||
      read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < 64 ||
      !ReadProcessMemory(process, (void *)(base + (unsigned)dos.e_lfanew), &pe,
                         sizeof(pe), &read) ||
      read != sizeof(pe) || !dd_windows_pe(&pe) ||
      !ReadProcessMemory(
          process, (void *)(base + (unsigned)dos.e_lfanew + sizeof(pe)),
          sections, pe.FileHeader.NumberOfSections * sizeof(*sections),
          &read) ||
      read != pe.FileHeader.NumberOfSections * sizeof(*sections))
    return 0;
  for (size_t i = 0; i < pe.FileHeader.NumberOfSections; ++i) {
    const IMAGE_SECTION_HEADER *s = sections + i;
    size_t memory = s->Misc.VirtualSize > s->SizeOfRawData ? s->Misc.VirtualSize
                                                           : s->SizeOfRawData;
    if ((uint64_t)s->VirtualAddress + memory > pe.OptionalHeader.SizeOfImage)
      return 0;
    unsigned flags = dd_section_flags(s->Characteristics);
    unsigned char *owned = NULL;
    if (flags & DD_CODE) {
      owned = malloc(memory);
      /* Include the allocation in cleanup even when the read fails. */
      image->owned[image->owned_count++] = owned;
      if (!owned ||
          !ReadProcessMemory(process, (void *)(base + s->VirtualAddress), owned,
                             memory, &read) ||
          read != memory)
        return 0;
    }
    if (!dd_windows_add_range(image,
                              (DdRange){owned, flags & DD_CODE ? memory : 0,
                                        s->VirtualAddress, memory, flags}))
      return 0;
  }
  return 1;
}
static inline int dd_windows_loaded_image(uintptr_t base,
                                          DdWindowsImage *image) {
  const IMAGE_DOS_HEADER *dos = (const void *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64)
    return 0;
  const IMAGE_NT_HEADERS64 *pe = (const void *)(base + (unsigned)dos->e_lfanew);
  if (!dd_windows_pe(pe))
    return 0;
  const IMAGE_SECTION_HEADER *sections = (const void *)(pe + 1);
  for (size_t i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
    const IMAGE_SECTION_HEADER *s = sections + i;
    size_t memory = s->Misc.VirtualSize > s->SizeOfRawData ? s->Misc.VirtualSize
                                                           : s->SizeOfRawData;
    if ((uint64_t)s->VirtualAddress + memory > pe->OptionalHeader.SizeOfImage)
      return 0;
    uintptr_t at = base + s->VirtualAddress, end = at + memory;
    while (at < end) {
      MEMORY_BASIC_INFORMATION region;
      if (!VirtualQuery((void *)at, &region, sizeof(region)))
        return 0;
      uintptr_t next = (uintptr_t)region.BaseAddress + region.RegionSize;
      if (next <= at)
        return 0;
      if (next > end)
        next = end;
      if (region.State == MEM_COMMIT &&
          !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        if (!dd_windows_add_loaded_range(
                image,
                (DdRange){(const void *)at, next - at, at - base, next - at,
                          dd_section_flags(s->Characteristics)}))
          return 0;
      }
      at = next;
    }
  }
  return 1;
}
#endif
