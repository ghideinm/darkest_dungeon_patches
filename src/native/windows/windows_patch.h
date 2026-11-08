#ifndef DD_WINDOWS_PATCH_H
#define DD_WINDOWS_PATCH_H
#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

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
static int windows_write_plan(DdWindowsPatches *plan) {
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

typedef struct {
  HANDLE handle;
  int suspended;
} DdWindowsThread;
typedef struct {
  DdWindowsThread *threads;
  size_t count, capacity;
} DdWindowsThreads;

static void windows_resume_threads(DdWindowsThreads *peers) {
  for (size_t i = 0; i < peers->count; ++i) {
    if (peers->threads[i].suspended)
      (void)ResumeThread(peers->threads[i].handle);
    CloseHandle(peers->threads[i].handle);
  }
  free(peers->threads);
}

/* A generic DLL loader can enter while the main thread is already reading
 * resources. Freeze peers for the guarded write/rollback only. Collect handles
 * and allocate storage first: never enter the C heap while peers are stopped.
 */
static int windows_suspend_threads(const DdWindowsPatches *plan,
                                   DdWindowsThreads *peers) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return 0;
  THREADENTRY32 entry = {0};
  entry.dwSize = sizeof(entry);
  DWORD process = GetCurrentProcessId(), current = GetCurrentThreadId();
  int valid = Thread32First(snapshot, &entry) != 0;
  while (valid) {
    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != current) {
      if (peers->count == peers->capacity) {
        size_t capacity = peers->capacity ? peers->capacity * 2 : 8;
        if (capacity < peers->capacity ||
            capacity > SIZE_MAX / sizeof(*peers->threads)) {
          valid = 0;
          break;
        }
        void *grown =
            realloc(peers->threads, capacity * sizeof(*peers->threads));
        if (!grown) {
          valid = 0;
          break;
        }
        peers->threads = grown;
        peers->capacity = capacity;
      }
      HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                     THREAD_QUERY_LIMITED_INFORMATION,
                                 FALSE, entry.th32ThreadID);
      if (!thread) {
        /* A thread that exited since the snapshot no longer needs freezing. */
        if (GetLastError() != ERROR_INVALID_PARAMETER) {
          valid = 0;
          break;
        }
      } else
        peers->threads[peers->count++] = (DdWindowsThread){thread, 0};
    }
    if (!Thread32Next(snapshot, &entry)) {
      valid = GetLastError() == ERROR_NO_MORE_FILES;
      break;
    }
  }
  CloseHandle(snapshot);
  if (!valid)
    return 0;
  for (size_t i = 0; i < peers->count; ++i) {
    DdWindowsThread *thread = peers->threads + i;
    if (SuspendThread(thread->handle) == (DWORD)-1) {
      DWORD exit_code;
      if (GetExitCodeThread(thread->handle, &exit_code) &&
          exit_code != STILL_ACTIVE)
        continue;
      return 0;
    }
    thread->suspended = 1;
    CONTEXT context = {0};
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread->handle, &context))
      return 0;
    for (size_t j = 0; j < plan->count; ++j) {
      uintptr_t start = (uintptr_t)plan->edits[j].address;
      /* At the first byte the incoming state also fits the replacement. A
       * thread midway through a replaced prologue must finish before retry.
       */
      if (context.Rip > start && context.Rip < start + plan->edits[j].length)
        return 0;
    }
  }
  return 1;
}

static int windows_commit(DdWindowsPatches *plan) {
  DdWindowsThreads peers = {0};
  int frozen = windows_suspend_threads(plan, &peers);
  int committed = frozen && windows_write_plan(plan);
  windows_resume_threads(&peers);
  return committed;
}
#endif
