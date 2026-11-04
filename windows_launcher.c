#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "windows_arguments.h"
#include "windows_profile.h"
#include <bcrypt.h>
#include <share.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <wchar.h>

static FILE *log_file;
static void report(const char *format, ...) {
  va_list arguments, copy;
  va_start(arguments, format);
  va_copy(copy, arguments);
  vfprintf(stderr, format, arguments);
  if (log_file) {
    /* The injected DLL appends using its own shared handle. */
    (void)fseek(log_file, 0, SEEK_END);
    vfprintf(log_file, format, copy);
    fflush(log_file);
  }
  va_end(copy);
  va_end(arguments);
}
static int hash_file(HANDLE file, char output[65]) {
  BCRYPT_ALG_HANDLE algorithm = NULL;
  BCRYPT_HASH_HANDLE hash = NULL;
  unsigned char digest[32], buffer[65536];
  int valid = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                          NULL, 0) == 0;
  if (valid)
    valid = BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) == 0;
  DWORD count;
  while (valid) {
    if (!ReadFile(file, buffer, sizeof(buffer), &count, NULL)) {
      valid = 0;
      break;
    }
    if (!count)
      break;
    valid = BCryptHashData(hash, buffer, count, 0) == 0;
  }
  if (valid)
    valid = BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
  if (hash)
    BCryptDestroyHash(hash);
  if (algorithm)
    BCryptCloseAlgorithmProvider(algorithm, 0);
  if (valid) {
    for (size_t i = 0; i < sizeof(digest); ++i)
      snprintf(output + i * 2, 3, "%02x", digest[i]);
    LARGE_INTEGER zero = {0};
    valid = SetFilePointerEx(file, zero, NULL, FILE_BEGIN) != 0;
  }
  return valid;
}
static int read_at(HANDLE file, uint64_t offset, void *out, DWORD size) {
  LARGE_INTEGER position;
  position.QuadPart = (LONGLONG)offset;
  DWORD read;
  return SetFilePointerEx(file, position, NULL, FILE_BEGIN) &&
         ReadFile(file, out, size, &read, NULL) && read == size;
}
static int verify_file_sites(HANDLE file, int runtime_rig) {
  IMAGE_DOS_HEADER dos;
  IMAGE_NT_HEADERS64 pe;
  LARGE_INTEGER file_size;
  if (!GetFileSizeEx(file, &file_size) ||
      !read_at(file, 0, &dos, sizeof(dos)) ||
      dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 ||
      dos.e_lfanew > 4096 ||
      !read_at(file, (unsigned)dos.e_lfanew, &pe, sizeof(pe)) ||
      pe.Signature != IMAGE_NT_SIGNATURE ||
      pe.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      pe.OptionalHeader.SizeOfImage != DD_WINDOWS_IMAGE_SIZE ||
      pe.FileHeader.NumberOfSections > 32 ||
      pe.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64))
    return 0;
  IMAGE_SECTION_HEADER sections[32];
  if (!read_at(file, (unsigned)dos.e_lfanew + sizeof(pe), sections,
               pe.FileHeader.NumberOfSections * sizeof(sections[0])))
    return 0;
  const WindowsSite *tables[] = {windows_resource_sites, windows_hook_sites,
                                 windows_api_sites, windows_render_sites};
  size_t counts[] = {sizeof(windows_resource_sites) / sizeof(WindowsSite),
                     sizeof(windows_hook_sites) / sizeof(WindowsSite),
                     sizeof(windows_api_sites) / sizeof(WindowsSite),
                     sizeof(windows_render_sites) / sizeof(WindowsSite)};
  for (size_t t = 0; t < 4; ++t)
    for (size_t i = 0; i < counts[t]; ++i) {
      if ((t == 1 || t == 2) && !runtime_rig)
        continue;
      const WindowsSite *site = tables[t] + i;
      int found = 0;
      for (unsigned j = 0; j < pe.FileHeader.NumberOfSections; ++j) {
        const IMAGE_SECTION_HEADER *section = sections + j;
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            site->rva < section->VirtualAddress)
          continue;
        uint64_t delta = site->rva - section->VirtualAddress;
        uint64_t offset = section->PointerToRawData + delta;
        unsigned char actual[16];
        if (delta + site->length <= section->SizeOfRawData &&
            offset + site->length <= (uint64_t)file_size.QuadPart &&
            read_at(file, offset, actual, (DWORD)site->length) &&
            !memcmp(actual, site->expected, site->length))
          found = 1;
      }
      if (!found) {
        report("DD_WINDOWS_FILE_REFUSED rva=%x\n", site->rva);
        return 0;
      }
    }
  return 1;
}
static int verify_remote(HANDLE process, uintptr_t base, int runtime_rig,
                         int native_render) {
  const WindowsSite *tables[] = {windows_resource_sites, windows_hook_sites,
                                 windows_api_sites, windows_render_sites};
  size_t counts[] = {sizeof(windows_resource_sites) / sizeof(WindowsSite),
                     sizeof(windows_hook_sites) / sizeof(WindowsSite),
                     sizeof(windows_api_sites) / sizeof(WindowsSite),
                     sizeof(windows_render_sites) / sizeof(WindowsSite)};
  for (size_t t = 0; t < 4; ++t)
    for (size_t i = 0; i < counts[t]; ++i) {
      if (((t == 1 || t == 2) && !runtime_rig) || (t == 3 && !native_render))
        continue;
      const WindowsSite *site = tables[t] + i;
      unsigned char actual[16];
      SIZE_T read;
      MEMORY_BASIC_INFORMATION region;
      uintptr_t address = base + site->rva;
      if (!VirtualQueryEx(process, (void *)address, &region, sizeof(region)) ||
          region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
          (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
          !ReadProcessMemory(process, (void *)address, actual, site->length,
                             &read) ||
          read != site->length ||
          memcmp(actual, site->expected, site->length)) {
        report("DD_WINDOWS_MEMORY_REFUSED rva=%x\n", site->rva);
        return 0;
      }
    }
  return 1;
}
static int patch_resources(HANDLE process, uintptr_t base) {
  for (size_t i = 0; i < sizeof(windows_resource_sites) / sizeof(WindowsSite);
       ++i) {
    const WindowsSite *site = windows_resource_sites + i;
    void *address = (void *)(base + site->rva + site->operand);
    DWORD previous, ignored;
    SIZE_T written;
    const uint32_t cap = DD_WINDOWS_NEW_CAP;
    if (!VirtualProtectEx(process, address, sizeof(cap), PAGE_EXECUTE_READWRITE,
                          &previous))
      return 0;
    int valid =
        WriteProcessMemory(process, address, &cap, sizeof(cap), &written) &&
        written == sizeof(cap) &&
        FlushInstructionCache(process, address, sizeof(cap));
    if (!VirtualProtectEx(process, address, sizeof(cap), previous, &ignored))
      valid = 0;
    uint32_t actual;
    if (!valid ||
        !ReadProcessMemory(process, address, &actual, sizeof(actual),
                           &written) ||
        written != sizeof(actual) || actual != cap)
      return 0;
  }
  report("DD_WINDOWS_RESOURCE_APPLIED buffers=%d cap=%u sites=3\n",
         DD_WINDOWS_RESOURCE_BUFFERS, DD_WINDOWS_NEW_CAP);
  return 1;
}
static int entry_breakpoint(HANDLE process, uintptr_t image, int install) {
  const WindowsSite *site = windows_entry_sites;
  unsigned char actual[16];
  SIZE_T count;
  void *entry = (void *)(image + site->rva);
  if (!ReadProcessMemory(process, entry, actual, site->length, &count) ||
      count != site->length ||
      (install ? memcmp(actual, site->expected, site->length)
               : (actual[0] != 0xcc ||
                  memcmp(actual + 1, site->expected + 1, site->length - 1))))
    return 0;
  unsigned char value = install ? 0xcc : site->expected[0];
  DWORD previous, ignored;
  if (!VirtualProtectEx(process, entry, 1, PAGE_EXECUTE_READWRITE, &previous))
    return 0;
  int valid = WriteProcessMemory(process, entry, &value, 1, &count) &&
              count == 1 && FlushInstructionCache(process, entry, 1);
  if (!VirtualProtectEx(process, entry, 1, previous, &ignored))
    valid = 0;
  return valid;
}
static int patch_render(HANDLE process, uintptr_t image) {
  for (size_t i = 0; i < sizeof(windows_render_sites) / sizeof(WindowsSite);
       ++i) {
    const WindowsSite *site = windows_render_sites + i;
    void *address = (void *)(image + site->rva);
    DWORD previous, ignored;
    SIZE_T written;
    if (!VirtualProtectEx(process, address, site->length,
                          PAGE_EXECUTE_READWRITE, &previous))
      return 0;
    int valid =
        WriteProcessMemory(process, address, windows_render_replacements[i],
                           site->length, &written) &&
        written == site->length &&
        FlushInstructionCache(process, address, site->length);
    if (!VirtualProtectEx(process, address, site->length, previous, &ignored))
      valid = 0;
    unsigned char actual[16];
    if (!valid ||
        !ReadProcessMemory(process, address, actual, site->length, &written) ||
        written != site->length ||
        memcmp(actual, windows_render_replacements[i], site->length))
      return 0;
  }
  report("DD_WINDOWS_RENDER_APPLIED sites=7 policy=physical_canvas\n");
  return 1;
}
static uintptr_t remote_module(DWORD pid, const wchar_t *name, int full_path) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
  if (snapshot == INVALID_HANDLE_VALUE)
    return 0;
  MODULEENTRY32W entry = {0};
  entry.dwSize = sizeof(entry);
  uintptr_t result = 0;
  if (Module32FirstW(snapshot, &entry))
    do {
      if (!_wcsicmp(full_path ? entry.szExePath : entry.szModule, name)) {
        result = (uintptr_t)entry.modBaseAddr;
        break;
      }
    } while (Module32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return result;
}
static uintptr_t remote_load_library(DWORD pid) {
  FARPROC function =
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
  MEMORY_BASIC_INFORMATION memory;
  wchar_t path[32768];
  if (!function || !VirtualQuery((void *)function, &memory, sizeof(memory)) ||
      !GetModuleFileNameW(memory.AllocationBase, path,
                          (DWORD)(sizeof(path) / 2)))
    return 0;
  const wchar_t *name = wcsrchr(path, L'\\');
  uintptr_t module = remote_module(pid, name ? name + 1 : path, 0);
  return module
             ? module + (uintptr_t)function - (uintptr_t)memory.AllocationBase
             : 0;
}
static void close_debug_files(DEBUG_EVENT *event) {
  if (event->dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
    if (event->u.CreateProcessInfo.hFile)
      CloseHandle(event->u.CreateProcessInfo.hFile);
  } else if (event->dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
    if (event->u.LoadDll.hFile)
      CloseHandle(event->u.LoadDll.hFile);
  }
  /* ContinueDebugEvent owns the event's process/thread handles. */
}
static void report_exception(const PROCESS_INFORMATION *process,
                             const DEBUG_EVENT *event, uintptr_t image) {
  const EXCEPTION_DEBUG_INFO *exception = &event->u.Exception;
  uintptr_t at = (uintptr_t)exception->ExceptionRecord.ExceptionAddress;
  report("DD_WINDOWS_EXCEPTION code=%lx first_chance=%lu address=%llx "
         "image=%llx rva=%llx\n",
         exception->ExceptionRecord.ExceptionCode, exception->dwFirstChance,
         (unsigned long long)at, (unsigned long long)image,
         (unsigned long long)(at >= image && at < image + DD_WINDOWS_IMAGE_SIZE
                                  ? at - image
                                  : UINT64_MAX));
  if (exception->ExceptionRecord.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      exception->ExceptionRecord.NumberParameters >= 2)
    report(
        "DD_WINDOWS_ACCESS kind=%llu target=%llx\n",
        (unsigned long long)exception->ExceptionRecord.ExceptionInformation[0],
        (unsigned long long)exception->ExceptionRecord.ExceptionInformation[1]);
  HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                             FALSE, event->dwThreadId);
  CONTEXT context = {0};
  context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  if (thread && GetThreadContext(thread, &context)) {
    report("DD_WINDOWS_REGISTERS rip=%llx rsp=%llx rbp=%llx rax=%llx rbx=%llx "
           "rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx r12=%llx "
           "r13=%llx r14=%llx r15=%llx\n",
           context.Rip, context.Rsp, context.Rbp, context.Rax, context.Rbx,
           context.Rcx, context.Rdx, context.Rsi, context.Rdi, context.R8,
           context.R9, context.R12, context.R13, context.R14, context.R15);
    uint64_t stack[32] = {0};
    SIZE_T read = 0;
    (void)ReadProcessMemory(process->hProcess, (void *)(uintptr_t)context.Rsp,
                            stack, sizeof(stack), &read);
    for (size_t i = 0; i < read / sizeof(stack[0]); ++i)
      report("DD_WINDOWS_STACK offset=%zu value=%llx\n", i * 8,
             (unsigned long long)stack[i]);
  }
  if (thread)
    CloseHandle(thread);
}
static int read_game_memory(HANDLE process, uintptr_t address, void *out,
                            size_t length) {
  SIZE_T read;
  return ReadProcessMemory(process, (void *)address, out, length, &read) &&
         read == length;
}
static void report_target_node(HANDLE process, uintptr_t node, unsigned depth,
                               unsigned *count) {
  unsigned char data[0x30];
  if (!node || depth > 32 || *count >= 128 ||
      !read_game_memory(process, node, data, sizeof(data)) || data[0x19])
    return;
  uintptr_t left, right, target;
  uint32_t key, width, height;
  memcpy(&left, data, sizeof(left));
  memcpy(&right, data + 0x10, sizeof(right));
  memcpy(&key, data + 0x20, sizeof(key));
  memcpy(&target, data + 0x28, sizeof(target));
  ++*count;
  report_target_node(process, left, depth + 1, count);
  if (target &&
      read_game_memory(process, target + 0xc, &width, sizeof(width)) &&
      read_game_memory(process, target + 0x10, &height, sizeof(height)))
    report("DD_WINDOWS_RENDER_TARGET key=%08x width=%u height=%u\n", key, width,
           height);
  report_target_node(process, right, depth + 1, count);
}
static void report_render_targets(HANDLE process, uintptr_t image) {
  uintptr_t manager, head, root;
  if (read_game_memory(process, image + 0x1174ad0, &manager, sizeof(manager)) &&
      manager && read_game_memory(process, manager, &head, sizeof(head)) &&
      head && read_game_memory(process, head + 8, &root, sizeof(root))) {
    unsigned count = 0;
    report_target_node(process, root, 0, &count);
  }
}
static int observe_startup(const PROCESS_INFORMATION *process,
                           uintptr_t image) {
  ULONGLONG deadline = GetTickCount64() + 20000;
  DEBUG_EVENT event;
  while (GetTickCount64() < deadline) {
    if (!WaitForDebugEvent(&event, 250)) {
      if (GetLastError() == ERROR_SEM_TIMEOUT)
        continue;
      return 0;
    }
    close_debug_files(&event);
    DWORD disposition = DBG_CONTINUE;
    int exited = event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT;
    if (exited)
      report("DD_WINDOWS_GAME_EXIT code=%lu hex=%lx\n",
             event.u.ExitProcess.dwExitCode, event.u.ExitProcess.dwExitCode);
    if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
      DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
      disposition = DBG_EXCEPTION_NOT_HANDLED;
      if (!event.u.Exception.dwFirstChance ||
          code == EXCEPTION_ACCESS_VIOLATION ||
          code == EXCEPTION_ILLEGAL_INSTRUCTION || code == 0xc0000409)
        report_exception(process, &event, image);
    }
    if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition))
      return 0;
    if (exited)
      return 0;
  }
  report_render_targets(process->hProcess, image);
  report("DD_WINDOWS_STARTUP_OBSERVED seconds=20\n");
  return 1;
}
static int launch_game(const wchar_t *game, wchar_t *command,
                       const wchar_t *directory, const wchar_t *library,
                       int runtime_rig, int native_render, int diagnose) {
  uintptr_t ready_rva = 0;
  if (runtime_rig) {
    HMODULE module = LoadLibraryExW(library, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!module)
      return 0;
    FARPROC ready = GetProcAddress(module, "dd_windows_ready");
    if (ready)
      ready_rva = (uintptr_t)ready - (uintptr_t)module;
    FreeLibrary(module);
    if (!ready_rva)
      return 0;
  }
  STARTUPINFOW startup = {0};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {0};
  if (!CreateProcessW(game, command, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS,
                      NULL, directory, &startup, &process))
    return 0;
  ULONGLONG deadline = GetTickCount64() + 45000;
  uintptr_t image = 0;
  int initialized = 0, success = 0, held = 0, suspended = 0, armed = 0;
  DEBUG_EVENT event = {0};
  DWORD remote_thread_id = 0;
  HANDLE remote_thread = NULL;
  void *remote_path = NULL;
  while (GetTickCount64() < deadline) {
    if (!WaitForDebugEvent(&event, 1000)) {
      if (GetLastError() == ERROR_SEM_TIMEOUT)
        continue;
      break;
    }
    held = 1;
    close_debug_files(&event);
    DWORD disposition = DBG_CONTINUE;
    if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT)
      image = (uintptr_t)event.u.CreateProcessInfo.lpBaseOfImage;
    if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
      report("DD_WINDOWS_EARLY_EXIT code=%lu\n",
             event.u.ExitProcess.dwExitCode);
      break;
    }
    if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
      DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
      disposition = DBG_EXCEPTION_NOT_HANDLED;
      uintptr_t trap =
          (uintptr_t)event.u.Exception.ExceptionRecord.ExceptionAddress;
      if (!initialized && !armed && code == EXCEPTION_BREAKPOINT) {
        /* The loader's first breakpoint can still hold its loader lock.
         * Continue to our own EXE-entry breakpoint before loading a DLL. */
        if (!image || !entry_breakpoint(process.hProcess, image, 1))
          break;
        armed = 1;
        disposition = DBG_CONTINUE;
      } else if (!initialized && armed && code == EXCEPTION_BREAKPOINT &&
                 trap == image + DD_WINDOWS_ENTRY_RVA &&
                 event.dwThreadId == process.dwThreadId) {
        CONTEXT context = {0};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!entry_breakpoint(process.hProcess, image, 0) ||
            !GetThreadContext(process.hThread, &context))
          break;
        context.Rip = image + DD_WINDOWS_ENTRY_RVA;
        if (!SetThreadContext(process.hThread, &context) ||
            !verify_remote(process.hProcess, image, runtime_rig,
                           native_render) ||
            !patch_resources(process.hProcess, image) ||
            (native_render && !patch_render(process.hProcess, image)) ||
            SuspendThread(process.hThread) == (DWORD)-1)
          break;
        suspended = initialized = 1;
        disposition = DBG_CONTINUE;
        if (runtime_rig) {
          uintptr_t load = remote_load_library(process.dwProcessId);
          SIZE_T bytes = (wcslen(library) + 1) * sizeof(wchar_t), written;
          remote_path =
              VirtualAllocEx(process.hProcess, NULL, bytes,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
          if (!load || !remote_path ||
              !WriteProcessMemory(process.hProcess, remote_path, library, bytes,
                                  &written) ||
              written != bytes)
            break;
          remote_thread = CreateRemoteThread(process.hProcess, NULL, 0,
                                             (LPTHREAD_START_ROUTINE)load,
                                             remote_path, 0, &remote_thread_id);
          if (!remote_thread)
            break;
        } else
          success = 1;
      } else if (!event.u.Exception.dwFirstChance) {
        report_exception(&process, &event, image);
        break;
      }
    }
    if (event.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT &&
        event.dwThreadId == remote_thread_id && remote_thread_id) {
      uintptr_t module = remote_module(process.dwProcessId, library, 1);
      LONG ready = 0;
      SIZE_T read;
      success =
          module &&
          ReadProcessMemory(process.hProcess, (void *)(module + ready_rva),
                            &ready, sizeof(ready), &read) &&
          read == sizeof(ready) && ready == 1;
      if (!success) {
        report("DD_WINDOWS_DLL_REFUSED thread_exit=%lu\n",
               event.u.ExitThread.dwExitCode);
        break;
      }
      report("DD_WINDOWS_DLL_READY base=%llx ready_rva=%llx\n",
             (unsigned long long)module, (unsigned long long)ready_rva);
    }
    if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition))
      break;
    held = 0;
    if (success)
      break;
  }
  if (success && !held) {
    if (remote_path)
      VirtualFreeEx(process.hProcess, remote_path, 0, MEM_RELEASE);
    if (diagnose) {
      success = ResumeThread(process.hThread) != (DWORD)-1;
      suspended = 0;
      if (success)
        success = observe_startup(&process, image);
    }
    if (success)
      success = DebugActiveProcessStop(process.dwProcessId) != 0;
    if (success && suspended)
      success = ResumeThread(process.hThread) != (DWORD)-1;
  } else
    success = 0;
  if (!success) {
    DWORD error = GetLastError();
    TerminateProcess(process.hProcess, 125);
    if (held)
      ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
    report("DD_WINDOWS_LAUNCH_REFUSED error=%lu\n", error);
  } else
    report("DD_WINDOWS_LAUNCHED pid=%lu runtime_rig=%d disk_modified=0\n",
           process.dwProcessId, runtime_rig);
  if (remote_thread)
    CloseHandle(remote_thread);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return success;
}
static int steam_game_path(const wchar_t *original, wchar_t game[32768]) {
  DWORD count = GetFullPathNameW(original, 32768, game, NULL);
  const wchar_t *x64 = L"\\_windows\\win64\\Darkest.exe";
  const wchar_t *x86 = L"\\_windows\\Darkest.exe";
  if (!count || count >= 32768)
    return 0;
  size_t length = wcslen(game), size64 = wcslen(x64), size32 = wcslen(x86);
  if (length >= size64 && !_wcsicmp(game + length - size64, x64))
    return 1;
  if (length < size32 || _wcsicmp(game + length - size32, x86) ||
      length - size32 + size64 >= 32768)
    return 0;
  wcscpy(game + length - size32, x64);
  return 1;
}
int wmain(int argc, wchar_t **argv) {
  wchar_t game[32768] = {0}, home[32768], library[32768], command[32768] = {0};
  int check = 0, runtime_rig = 1, native_render = 1, diagnose = 0,
      remaining = argc, steam_mode = 0;
  for (int i = 1; i < argc; ++i) {
    if (!wcscmp(argv[i], L"--check"))
      check = 1;
    else if (!wcscmp(argv[i], L"--native-render"))
      native_render = 1;
    else if (!wcscmp(argv[i], L"--no-native-render"))
      native_render = 0;
    else if (!wcscmp(argv[i], L"--diagnose"))
      diagnose = 1;
    else if (!wcscmp(argv[i], L"--resources-only")) {
      runtime_rig = 0;
      native_render = 0;
    } else if (!wcscmp(argv[i], L"--steam") && i + 1 < argc) {
      if (*game || !steam_game_path(argv[i + 1], game))
        return 125;
      steam_mode = 1;
      remaining = i + 2;
      break;
    } else if (!wcscmp(argv[i], L"--game") && i + 1 < argc) {
      DWORD count = GetFullPathNameW(argv[++i], 32768, game, NULL);
      if (!count || count >= 32768)
        return 125;
    } else if (!wcscmp(argv[i], L"--")) {
      remaining = i + 1;
      break;
    } else {
      fwprintf(stderr, L"Unknown argument: %ls\n", argv[i]);
      return 125;
    }
  }
  if (!*game) {
    fputs("Use --steam <Steam game command> or --game <Windows x64 "
          "Darkest.exe> [--check] "
          "[--resources-only] [--no-native-render] [-- game arguments]\n",
          stderr);
    return 125;
  }
  if (steam_mode)
    (void)FreeConsole();
  DWORD count = GetModuleFileNameW(NULL, home, 32768);
  if (!count || count >= 32768)
    return 125;
  wchar_t *slash = wcsrchr(home, L'\\');
  if (!slash)
    return 125;
  slash[1] = 0;
  if (wcslen(home) + 40 >= 32768)
    return 125;
  wcscpy(library, home);
  wcscat(library, L"dd_skin_rig_patch.dll");
  wchar_t log_path[32768];
  wcscpy(log_path, home);
  wcscat(log_path, L"windows_launch.log");
  if (!check) {
    log_file = _wfsopen(log_path, L"w", _SH_DENYNO);
    if (!log_file)
      return 125;
    SetEnvironmentVariableW(L"DD_WINDOWS_PATCH_LOG", log_path);
  }
  HANDLE file = CreateFileW(game, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  char hash[65];
  int valid = file != INVALID_HANDLE_VALUE && hash_file(file, hash) &&
              !strcmp(hash, DD_WINDOWS_SHA256) &&
              verify_file_sites(file, runtime_rig);
  if (!valid) {
    report("DD_WINDOWS_IDENTITY_REFUSED error=%lu\n", GetLastError());
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
    return 125;
  }
  report("DD_WINDOWS_IDENTITY_OK sha256=%s resource_sites=3 rig_hooks=%d\n",
         hash, runtime_rig ? 4 : 0);
  if (check) {
    CloseHandle(file);
    return 0;
  }
  wchar_t directory[32768];
  wcscpy(directory, game);
  for (int i = 0; i < 3; ++i) {
    slash = wcsrchr(directory, L'\\');
    if (!slash) {
      CloseHandle(file);
      return 125;
    }
    *slash = 0;
  }
  valid = append_argument(command, 32768, game);
  int has_steam_default = 0;
  for (int i = remaining; i < argc; ++i)
    if (!wcscmp(argv[i], L"-skipvalidation"))
      has_steam_default = 1;
  /* App 262060's native Windows launch configuration supplies this flag.
   * Retain Steam's normal startup contract when launching the verified EXE. */
  if (valid && !has_steam_default)
    valid = append_argument(command, 32768, L"-skipvalidation");
  for (int i = remaining; valid && i < argc; ++i)
    valid = append_argument(command, 32768, argv[i]);
  /* The Steam client must already be running. This does not replace Steam's
   * ownership checks or modify its local configuration. */
  SetEnvironmentVariableW(L"SteamAppId", L"262060");
  SetEnvironmentVariableW(L"SteamGameId", L"262060");
  SetEnvironmentVariableW(L"DD_RIG_MAP", NULL);
  int result = valid && launch_game(game, command, directory, library,
                                    runtime_rig, native_render, diagnose);
  CloseHandle(file);
  if (log_file)
    fclose(log_file);
  return result ? 0 : 125;
}
#else
#include <stdio.h>
int main(void) {
  fputs("Build windows_launcher.c with a Windows x64 Clang target.\n", stderr);
  return 125;
}
#endif
