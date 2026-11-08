#ifndef DD_WINDOWS_RESOURCE_H
#define DD_WINDOWS_RESOURCE_H

static void *(*windows_buffer_realloc)(void *, size_t);
static size_t (*windows_buffer_size)(void *);

/* The native dispatcher has removed this buffer from its free queue. No read
 * owns it until the following native submission; grow it here even when the
 * DLL was loaded after the pool constructor. Keep the pool's owning pointer
 * and this job's borrowed pointer in sync for native return/free operations.
 */
static void windows_prepare_buffer(void *manager, void *job) {
  void *buffer = pointer_at(job, 0);
  void **owner = NULL;
  for (size_t i = 0; i < DD_WINDOWS_RESOURCE_BUFFERS; ++i) {
    void **slot = (void **)((char *)manager + 0x20 + i * sizeof(void *));
    if (*slot == buffer) {
      owner = slot;
      break;
    }
  }
  uint32_t capacity = DD_WINDOWS_OLD_CAP;
  if (buffer && owner) {
    size_t allocated = windows_buffer_size(buffer);
    uint32_t requested = dd_resource_buffer_bytes();
    if (allocated != SIZE_MAX && allocated < requested) {
      void *grown = windows_buffer_realloc(buffer, requested);
      if (grown) {
        *owner = grown;
        memcpy(job, &grown, sizeof(grown));
        allocated = requested;
      } else {
        dprintf(2, "DD_RESOURCE_GROW_FAILED requested=%u available=%zu\n",
                requested, allocated);
      }
    }
    if (allocated != SIZE_MAX)
      capacity = allocated < requested ? (uint32_t)allocated : requested;
  }
  memcpy((char *)job + sizeof(void *), &capacity, sizeof(capacity));
}

static int windows_resource_plan(DdWindowsPatches *plan,
                                 unsigned char **allocated) {
  HMODULE allocator_module = NULL;
  void *allocator = *(void **)(native_address(DD_RESOURCE_ALLOCATOR));
  if (!allocator ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (const wchar_t *)allocator, &allocator_module))
    return 0;
  windows_buffer_realloc = (void *)GetProcAddress(allocator_module, "realloc");
  windows_buffer_size = (void *)GetProcAddress(allocator_module, "_msize");
  if (!windows_buffer_realloc || !windows_buffer_size)
    return 0;

  unsigned char *stub = windows_near_page();
  if (!stub)
    return 0;
  *allocated = stub;
  /* Preserve flags, all volatile GPRs and XMM0..5. The native dispatcher's
   * stack is 16-byte aligned here; reserve the Windows ABI shadow space.
   */
  static const unsigned char before[] = {
      0x9c, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52,
      0x41, 0x53, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00};
  size_t at = sizeof(before);
  memcpy(stub, before, at);
  for (unsigned i = 0; i < 6; ++i) {
    const unsigned char save[] = {0x0f, 0x29, (unsigned char)(0x44 + i * 8),
                                  0x24, (unsigned char)(0x20 + i * 16)};
    memcpy(stub + at, save, sizeof(save));
    at += sizeof(save);
  }
  static const unsigned char arguments[] = {
      0x4c, 0x89, 0xf1,                        /* rcx = pool in r14 */
      0x48, 0x8d, 0x94, 0x17, 0x08, 0x01, 0, 0 /* rdx = job buffer */
  };
  memcpy(stub + at, arguments, sizeof(arguments));
  at += sizeof(arguments);
  stub[at++] = 0x48;
  stub[at++] = 0xb8;
  uintptr_t target = (uintptr_t)windows_prepare_buffer;
  memcpy(stub + at, &target, sizeof(target));
  at += sizeof(target);
  stub[at++] = 0xff;
  stub[at++] = 0xd0;
  for (unsigned i = 0; i < 6; ++i) {
    const unsigned char restore[] = {0x0f, 0x28, (unsigned char)(0x44 + i * 8),
                                     0x24, (unsigned char)(0x20 + i * 16)};
    memcpy(stub + at, restore, sizeof(restore));
    at += sizeof(restore);
  }
  static const unsigned char after[] = {
      0x48, 0x81, 0xc4, 0x80, 0,    0,    0,    0x41, 0x5b, 0x41,
      0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58, 0x9d, 0x48,
      0x8b, 0x8c, 0x17, 0x08, 0x01, 0,    0 /* reload borrowed pointer */
  };
  memcpy(stub + at, after, sizeof(after));
  at += sizeof(after);
  windows_absolute_jump(stub + at,
                        (void *)(native_address(DD_RESOURCE_1) + 11));
  DWORD previous;
  if (!VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &previous) ||
      !FlushInstructionCache(GetCurrentProcess(), stub, 4096))
    return 0;

  unsigned char replacement[11];
  memset(replacement, 0x90, sizeof(replacement));
  replacement[0] = 0xe9;
  int64_t delta =
      (int64_t)(uintptr_t)stub - (int64_t)(native_address(DD_RESOURCE_1) + 5);
  if (delta < INT32_MIN || delta > INT32_MAX)
    return 0;
  int32_t relative = (int32_t)delta;
  memcpy(replacement + 1, &relative, sizeof(relative));
  /* rax points to this job's actual capacity; rdx is overwritten immediately
   * by the native call setup. Never advertise more than the buffer owns.
   */
  static const unsigned char read_capacity[] = {0x8b, 0x10, 0x89, 0x54,
                                                0x24, 0x28, 0x90, 0x90};
  uint32_t capacity = dd_resource_buffer_bytes();
  return windows_plan_edit(plan, (void *)(native_address(DD_RESOURCE_0) + 1),
                           &capacity, sizeof(capacity)) &&
         windows_plan_edit(plan, (void *)native_address(DD_RESOURCE_1),
                           replacement, sizeof(replacement)) &&
         windows_plan_edit(plan, (void *)native_address(DD_RESOURCE_2),
                           read_capacity, sizeof(read_capacity));
}
#endif
