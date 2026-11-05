#ifndef DD_WINDOWS_IMAGE_H
#define DD_WINDOWS_IMAGE_H
#include "../common/address_profile.h"
#include <stdlib.h>
#include <windows.h>
enum { DD_IMAGE_RANGES = 96 };
typedef struct {
  DdRange ranges[DD_IMAGE_RANGES];
  unsigned char *owned[DD_IMAGE_RANGES];
  size_t count;
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
         pe->FileHeader.NumberOfSections <= 32;
}
static inline void dd_windows_image_close(DdWindowsImage *image) {
  for (size_t i = 0; i < image->count; ++i)
    free(image->owned[i]);
  if (image->view)
    UnmapViewOfFile(image->view);
  if (image->mapping)
    CloseHandle(image->mapping);
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
    image->ranges[image->count++] = (DdRange){
        bytes + s->PointerToRawData, s->SizeOfRawData, s->VirtualAddress,
        memory, dd_section_flags(s->Characteristics)};
  }
  return 1;
}
static inline int dd_windows_remote_image(HANDLE process, uintptr_t base,
                                          DdWindowsImage *image) {
  IMAGE_DOS_HEADER dos;
  IMAGE_NT_HEADERS64 pe;
  IMAGE_SECTION_HEADER sections[32];
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
    if (flags & DD_CODE) {
      image->owned[i] = malloc(memory);
      /* Include the allocation in cleanup even when the read fails. */
      image->count = i + 1;
      if (!image->owned[i] ||
          !ReadProcessMemory(process, (void *)(base + s->VirtualAddress),
                             image->owned[i], memory, &read) ||
          read != memory)
        return 0;
    }
    image->ranges[i] = (DdRange){image->owned[i], flags & DD_CODE ? memory : 0,
                                 s->VirtualAddress, memory, flags};
    image->count = i + 1;
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
        if (image->count == DD_IMAGE_RANGES)
          return 0;
        image->ranges[image->count++] =
            (DdRange){(const void *)at, next - at, at - base, next - at,
                      dd_section_flags(s->Characteristics)};
      }
      at = next;
    }
  }
  return 1;
}
#endif
