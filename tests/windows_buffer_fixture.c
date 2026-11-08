#include "../src/native/common/skin_rig_patch.c"
#include <assert.h>
#include <malloc.h>

typedef struct {
  void *buffer;
  uint32_t capacity, sentinel;
} BufferJob;
typedef struct {
  uintptr_t buffer, r8, r9, r10, r11, flags;
  unsigned char xmm[96];
} Registers;

__attribute__((naked)) static void dispatch(void *pool __attribute__((unused)),
                                            BufferJob *job
                                            __attribute__((unused)),
                                            void *entry __attribute__((unused)),
                                            Registers *saved
                                            __attribute__((unused))) {
  __asm__ volatile(
      "push %rdi\n push %r14\n sub $40,%rsp\n mov %r9,32(%rsp)\n"
      "mov %rcx,%r14\n mov %rdx,%rdi\n sub $0x108,%rdi\n"
      "mov (%rdx),%rcx\n xor %edx,%edx\n mov %r8,%rax\n"
      "mov $18,%r8d\n mov $19,%r9d\n mov $20,%r10d\n mov $21,%r11d\n"
      "pcmpeqd %xmm0,%xmm0\n pcmpeqd %xmm1,%xmm1\n"
      "pcmpeqd %xmm2,%xmm2\n pcmpeqd %xmm3,%xmm3\n"
      "pcmpeqd %xmm4,%xmm4\n pcmpeqd %xmm5,%xmm5\n stc\n call *%rax\n"
      "pushfq\n pop %rax\n mov 32(%rsp),%rdi\n"
      "mov %rcx,0(%rdi)\n mov %r8,8(%rdi)\n mov %r9,16(%rdi)\n"
      "mov %r10,24(%rdi)\n mov %r11,32(%rdi)\n mov %rax,40(%rdi)\n"
      "movups %xmm0,48(%rdi)\n movups %xmm1,64(%rdi)\n"
      "movups %xmm2,80(%rdi)\n movups %xmm3,96(%rdi)\n"
      "movups %xmm4,112(%rdi)\n movups %xmm5,128(%rdi)\n"
      "add $40,%rsp\n pop %r14\n pop %rdi\n ret\n");
}

static void *fail_realloc(void *memory, size_t size) {
  (void)memory;
  (void)size;
  return NULL;
}

static volatile LONG worker_active = 1, worker_ticks;
static DWORD WINAPI background_worker(void *unused) {
  (void)unused;
  while (InterlockedCompareExchange(&worker_active, 1, 1)) {
    InterlockedIncrement(&worker_ticks);
    Sleep(1);
  }
  return 0;
}

static void assign_symbol(DdSymbol symbol, const void *address) {
  native_symbols.values[symbol] = (uintptr_t)address - base;
  native_symbols.present[symbol] = 1;
}

static void fragmented_image(void) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  size_t page = info.dwPageSize, regions = 192;
  size_t size = (regions * 2 + 1) * page;
  unsigned char *memory =
      VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  assert(memory);
  IMAGE_DOS_HEADER *dos = (void *)memory;
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = sizeof(*dos);
  IMAGE_NT_HEADERS64 *pe = (void *)(memory + sizeof(*dos));
  pe->Signature = IMAGE_NT_SIGNATURE;
  pe->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  pe->FileHeader.NumberOfSections = 1;
  pe->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  pe->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  pe->OptionalHeader.SizeOfImage = (DWORD)size;
  IMAGE_SECTION_HEADER *section = (void *)(pe + 1);
  section->VirtualAddress = (DWORD)page;
  section->Misc.VirtualSize = (DWORD)(size - page);
  section->Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
  DWORD previous;
  for (size_t i = 0; i < regions; ++i)
    assert(VirtualProtect(memory + (i * 2 + 2) * page, page, PAGE_NOACCESS,
                          &previous));
  DdWindowsImage image = {0};
  assert(dd_windows_loaded_image((uintptr_t)memory, &image));
  assert(image.count == regions);
  dd_windows_image_close(&image);
  assert(!image.count && !image.ranges);
  /* Readable regions with differing OS protections still form one logical
   * range, allowing signatures to span copy-on-write region boundaries.
   */
  for (size_t i = 0; i < regions; ++i)
    assert(VirtualProtect(memory + (i * 2 + 2) * page, page, PAGE_READONLY,
                          &previous));
  assert(dd_windows_loaded_image((uintptr_t)memory, &image));
  assert(image.count == 1 && image.ranges[0].size == size - page);
  dd_windows_image_close(&image);
  assert(VirtualFree(memory, 0, MEM_RELEASE));
}

int main(void) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  (void)_set_error_mode(_OUT_TO_STDERR);
  (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  fragmented_image();
  base = (uintptr_t)GetModuleHandleW(NULL);
  unsigned char *code = windows_near_page();
  assert(code);
  void *allocator =
      (void *)GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "malloc");
  assert(allocator);
  memcpy(code + 128, &allocator, sizeof(allocator));
  assign_symbol(DD_RESOURCE_ALLOCATOR, code + 128);
  memcpy(code, windows_resource_sites[0].expected, 5);
  assign_symbol(DD_RESOURCE_0, code);
  memcpy(code + 11, "\x48\x8d\x64\x24\xf8", 5);
  memcpy(code + 16, windows_resource_sites[1].expected, 11);
  memcpy(code + 27, "\x48\x8d\x64\x24\x08\xc3", 6);
  assign_symbol(DD_RESOURCE_1, code + 16);
  /* An independent native-shaped reader receives the advertised capacity.
   * Its own stack frame contains the native sixth-argument slot at rsp+0x28.
   */
  const unsigned char reader[] = {
      0x48, 0x83, 0xec, 0x38, 0x48, 0x89, 0xc8, 0xc7, 0x44, 0x24, 0x28, 0,
      0,    0,    1,    0x8b, 0x44, 0x24, 0x28, 0x48, 0x83, 0xc4, 0x38, 0xc3};
  memcpy(code + 48, reader, sizeof(reader));
  assign_symbol(DD_RESOURCE_2, code + 55);
  DWORD previous;
  assert(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &previous));
  dd_config.resource_buffer_mib = 32;
  DdWindowsPatches plan = {0};
  unsigned char *stub = NULL;
  assert(windows_resource_plan(&plan, &stub));
  HANDLE worker = CreateThread(NULL, 0, background_worker, NULL, 0, NULL);
  assert(worker);
  while (!InterlockedCompareExchange(&worker_ticks, 0, 0))
    SwitchToThread();
  assert(windows_commit(&plan));
  uint32_t (*read_capacity)(const void *) = (void *)(code + 48);

  unsigned char pool[0x80] = {0};
  void **buffers = (void **)(pool + 0x20);
  for (size_t i = 0; i < DD_WINDOWS_RESOURCE_BUFFERS; ++i) {
    /* Allocate every buffer before the simulated delayed DLL injection.
     * Some pools may already contain a mixture of old and enlarged buffers.
     */
    size_t bytes = i % 2 ? DD_WINDOWS_OLD_CAP : dd_resource_buffer_bytes();
    buffers[i] = malloc(bytes);
    assert(buffers[i]);
    memset(buffers[i], (int)i + 1, 64);
  }
  for (size_t i = 0; i < DD_WINDOWS_RESOURCE_BUFFERS; ++i) {
    BufferJob job = {buffers[i], 0, 0xfedcba98};
    Registers registers = {0};
    dispatch(pool, &job, code + 11, &registers);
    assert(job.buffer == buffers[i]);
    assert(job.capacity == dd_resource_buffer_bytes());
    assert(windows_buffer_size(job.buffer) >= job.capacity);
    assert(job.sentinel == 0xfedcba98);
    assert(read_capacity(&job.capacity) == job.capacity);
    assert(registers.buffer == (uintptr_t)job.buffer);
    assert(registers.r8 == 18 && registers.r9 == 19);
    assert(registers.r10 == 20 && registers.r11 == 21);
    assert(registers.flags & 1);
    for (size_t n = 0; n < sizeof(registers.xmm); ++n)
      assert(registers.xmm[n] == 0xff);
    for (unsigned n = 0; n < 64; ++n)
      assert(((unsigned char *)job.buffer)[n] == i + 1);
    /* A read beyond the original 16 MiB must remain inside this allocation. */
    memset(job.buffer, 0x5a, job.capacity);
    free(buffers[i]);
  }

  buffers[0] = malloc(DD_WINDOWS_OLD_CAP);
  assert(buffers[0]);
  BufferJob job = {buffers[0], 0, 0xfedcba98};
  void *(*native_realloc)(void *, size_t) = windows_buffer_realloc;
  windows_buffer_realloc = fail_realloc;
  Registers registers = {0};
  dispatch(pool, &job, code + 11, &registers);
  assert(job.buffer == buffers[0]);
  assert(job.capacity == DD_WINDOWS_OLD_CAP);
  assert(read_capacity(&job.capacity) == DD_WINDOWS_OLD_CAP);
  assert(job.sentinel == 0xfedcba98);
  windows_buffer_realloc = native_realloc;
  dispatch(pool, &job, code + 11, &registers);
  assert(job.buffer == buffers[0]);
  assert(job.capacity == dd_resource_buffer_bytes());
  assert(read_capacity(&job.capacity) == job.capacity);
  free(buffers[0]);
  InterlockedExchange(&worker_active, 0);
  assert(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0);
  CloseHandle(worker);
  assert(VirtualFree(stub, 0, MEM_RELEASE));
  assert(VirtualFree(code, 0, MEM_RELEASE));
  puts("OWN_DELAYED_BUFFER_GROWTH_OK");
  return 0;
}
