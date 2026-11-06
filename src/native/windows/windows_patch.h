#ifndef DD_WINDOWS_PATCH_H
#define DD_WINDOWS_PATCH_H
#include <stdint.h>
#include <string.h>
#include <windows.h>

enum { DD_WINDOWS_EDITS = 16, DD_WINDOWS_PAGES = 32 };
typedef struct {
  unsigned char *address;
  size_t length;
  unsigned char before[16], after[16];
} DdWindowsEdit;
typedef struct {
  void *address;
  DWORD protection;
} DdWindowsPage;
typedef struct {
  DdWindowsEdit edits[DD_WINDOWS_EDITS];
  DdWindowsPage pages[DD_WINDOWS_PAGES];
  size_t count, page_count, page_size;
} DdWindowsPatches;

static int windows_plan_edit(DdWindowsPatches *plan, void *address,
                             const void *replacement, size_t length) {
  if (!length || length > 16 || plan->count == DD_WINDOWS_EDITS)
    return 0;
  uintptr_t start = (uintptr_t)address;
  for (size_t i = 0; i < plan->count; ++i) {
    const DdWindowsEdit *other = plan->edits + i;
    uintptr_t at = (uintptr_t)other->address;
    if (start < at + other->length && at < start + length)
      return 0;
  }
  DdWindowsEdit *edit = plan->edits + plan->count++;
  edit->address = address;
  edit->length = length;
  memcpy(edit->before, address, length);
  memcpy(edit->after, replacement, length);
  return 1;
}
static int windows_restore_pages(const DdWindowsPatches *plan, size_t count) {
  int valid = 1;
  for (size_t i = 0; i < count; ++i) {
    DWORD ignored;
    if (!VirtualProtect(plan->pages[i].address, plan->page_size,
                        plan->pages[i].protection, &ignored))
      valid = 0;
  }
  return valid;
}
static int windows_commit(DdWindowsPatches *plan) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  plan->page_size = info.dwPageSize;
  /* Several hooks share a page. Save and restore its protection only once. */
  for (size_t i = 0; i < plan->count; ++i) {
    const DdWindowsEdit *edit = plan->edits + i;
    uintptr_t first = (uintptr_t)edit->address & ~(plan->page_size - 1);
    uintptr_t last =
        ((uintptr_t)edit->address + edit->length - 1) & ~(plan->page_size - 1);
    for (uintptr_t page = first;; page += plan->page_size) {
      size_t j = 0;
      while (j < plan->page_count && plan->pages[j].address != (void *)page)
        ++j;
      if (j == plan->page_count) {
        if (j == DD_WINDOWS_PAGES)
          return 0;
        plan->pages[plan->page_count++].address = (void *)page;
      }
      if (page == last)
        break;
    }
  }
  for (size_t i = 0; i < plan->page_count; ++i) {
    DdWindowsPage *page = plan->pages + i;
    if (!VirtualProtect(page->address, plan->page_size, PAGE_EXECUTE_READWRITE,
                        &page->protection)) {
      (void)windows_restore_pages(plan, i);
      return 0;
    }
  }
  int valid = 1;
  for (size_t i = 0; i < plan->count; ++i) {
    const DdWindowsEdit *edit = plan->edits + i;
    memcpy(edit->address, edit->after, edit->length);
    if (!FlushInstructionCache(GetCurrentProcess(), edit->address,
                               edit->length) ||
        memcmp(edit->address, edit->after, edit->length))
      valid = 0;
  }
  if (valid && windows_restore_pages(plan, plan->page_count))
    return 1;
  /* An initialization failure must not leave resource and rig hooks mixed. */
  int writable = 1;
  for (size_t i = 0; i < plan->page_count; ++i) {
    DWORD ignored;
    if (!VirtualProtect(plan->pages[i].address, plan->page_size,
                        PAGE_EXECUTE_READWRITE, &ignored))
      writable = 0;
  }
  if (writable)
    for (size_t i = 0; i < plan->count; ++i) {
      const DdWindowsEdit *edit = plan->edits + i;
      memcpy(edit->address, edit->before, edit->length);
      (void)FlushInstructionCache(GetCurrentProcess(), edit->address,
                                  edit->length);
    }
  (void)windows_restore_pages(plan, plan->page_count);
  return 0;
}
#endif
