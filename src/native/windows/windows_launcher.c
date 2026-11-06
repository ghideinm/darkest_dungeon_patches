#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "windows_arguments.h"
#include "windows_image.h"
#include "windows_profile.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <wchar.h>

static HANDLE log_file = INVALID_HANDLE_VALUE;
static int report_stderr = 1;
static void report(const char *format, ...) {
  va_list arguments, copy;
  va_start(arguments, format);
  if (report_stderr) {
    va_copy(copy, arguments);
    vfprintf(stderr, format, copy);
    va_end(copy);
  }
  if (log_file != INVALID_HANDLE_VALUE) {
    char line[4096];
    int length = vsnprintf(line, sizeof(line), format, arguments);
    if (length > 0) {
      DWORD count = (DWORD)((size_t)length < sizeof(line) ? (size_t)length
                                                          : sizeof(line) - 1);
      DWORD written;
      /* Kernel append semantics also preserve writes from the injected DLL. */
      (void)WriteFile(log_file, line, count, &written, NULL);
    }
  }
  va_end(arguments);
}
static int read_at(HANDLE file, uint64_t offset, void *out, DWORD size) {
  LARGE_INTEGER position;
  position.QuadPart = (LONGLONG)offset;
  DWORD read;
  return SetFilePointerEx(file, position, NULL, FILE_BEGIN) &&
         ReadFile(file, out, size, &read, NULL) && read == size;
}
static int read_pe(HANDLE file, IMAGE_DOS_HEADER *dos, IMAGE_NT_HEADERS64 *pe) {
  return read_at(file, 0, dos, sizeof(*dos)) &&
         dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew >= 64 &&
         read_at(file, (unsigned)dos->e_lfanew, pe, sizeof(*pe)) &&
         pe->Signature == IMAGE_NT_SIGNATURE &&
         pe->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
         pe->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
         pe->FileHeader.NumberOfSections > 0 &&
         pe->FileHeader.NumberOfSections <= 32 &&
         pe->FileHeader.SizeOfOptionalHeader == sizeof(IMAGE_OPTIONAL_HEADER64);
}
static int resolve_image(const DdWindowsImage *image, int runtime_rig,
                         int native_render, DdResolution *symbols) {
  const DdSignature *failed = NULL;
  int matches = 0;
  unsigned groups = DD_GROUP_RESOURCE | (runtime_rig ? DD_GROUP_RIG : 0) |
                    (native_render ? DD_GROUP_RENDER : 0);
  if (!dd_resolve_profile(image->ranges, image->count, groups, symbols, &failed,
                          &matches)) {
    report("DD_WINDOWS_SIGNATURE_MISSING feature=%s matches=%d\n", failed->name,
           matches);
    return 0;
  }
  const WindowsSite *tables[] = {windows_resource_sites, windows_hook_sites,
                                 windows_api_sites, windows_render_sites};
  const size_t counts[] = {sizeof(windows_resource_sites) / sizeof(WindowsSite),
                           sizeof(windows_hook_sites) / sizeof(WindowsSite),
                           sizeof(windows_api_sites) / sizeof(WindowsSite),
                           sizeof(windows_render_sites) / sizeof(WindowsSite)};
  for (size_t t = 0; t < 4; ++t)
    for (size_t i = 0; i < counts[t]; ++i) {
      if (((t == 1 || t == 2) && !runtime_rig) || (t == 3 && !native_render))
        continue;
      const WindowsSite *site = tables[t] + i;
      uintptr_t at = symbols->values[site->symbol];
      const unsigned char *actual = NULL;
      for (size_t r = 0; r < image->count; ++r) {
        const DdRange *range = image->ranges + r;
        if (at >= range->address && at - range->address <= range->size &&
            site->length <= range->size - (at - range->address))
          actual = range->bytes + (at - range->address);
      }
      if (!actual || !dd_guard(actual, site->expected, site->length, at,
                               symbols->values[DD_NATIVE_NEW])) {
        report("DD_WINDOWS_INSTRUCTION_MISMATCH symbol=%u rva=%llx\n",
               (unsigned)site->symbol, (unsigned long long)at);
        return 0;
      }
    }
  return 1;
}
static int verify_file_sites(HANDLE file, int runtime_rig, int native_render,
                             DdResolution *symbols) {
  DdWindowsImage image = {0};
  int result = dd_windows_file_image(file, &image) &&
               resolve_image(&image, runtime_rig, native_render, symbols);
  dd_windows_image_close(&image);
  return result;
}
static int verify_remote(HANDLE process, uintptr_t base, int runtime_rig,
                         int native_render, DdResolution *symbols) {
  DdWindowsImage image = {0};
  int result = dd_windows_remote_image(process, base, &image) &&
               resolve_image(&image, runtime_rig, native_render, symbols);
  dd_windows_image_close(&image);
  return result;
}
static int patch_resources(HANDLE process, uintptr_t base,
                           const DdResolution *symbols) {
  for (size_t i = 0; i < sizeof(windows_resource_sites) / sizeof(WindowsSite);
       ++i) {
    const WindowsSite *site = windows_resource_sites + i;
    void *address =
        (void *)(base + symbols->values[site->symbol] + site->operand);
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
typedef struct {
  uintptr_t address;
  unsigned char original;
} EntryBreakpoint;
static int find_entry(HANDLE process, uintptr_t image, EntryBreakpoint *entry) {
  IMAGE_DOS_HEADER dos;
  IMAGE_NT_HEADERS64 pe;
  SIZE_T count;
  if (!ReadProcessMemory(process, (void *)image, &dos, sizeof(dos), &count) ||
      count != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < 64 ||
      !ReadProcessMemory(process, (void *)(image + (unsigned)dos.e_lfanew), &pe,
                         sizeof(pe), &count) ||
      count != sizeof(pe) || pe.Signature != IMAGE_NT_SIGNATURE ||
      !pe.OptionalHeader.AddressOfEntryPoint ||
      pe.OptionalHeader.AddressOfEntryPoint >= pe.OptionalHeader.SizeOfImage)
    return 0;
  entry->address = image + pe.OptionalHeader.AddressOfEntryPoint;
  return ReadProcessMemory(process, (void *)entry->address, &entry->original, 1,
                           &count) &&
         count == 1;
}
static int entry_breakpoint(HANDLE process, const EntryBreakpoint *entry,
                            int install) {
  unsigned char actual;
  SIZE_T count;
  void *address = (void *)entry->address;
  if (!ReadProcessMemory(process, address, &actual, 1, &count) || count != 1 ||
      actual != (install ? entry->original : 0xcc))
    return 0;
  unsigned char value = install ? 0xcc : entry->original;
  DWORD previous, ignored;
  if (!VirtualProtectEx(process, address, 1, PAGE_EXECUTE_READWRITE, &previous))
    return 0;
  int valid = WriteProcessMemory(process, address, &value, 1, &count) &&
              count == 1 && FlushInstructionCache(process, address, 1);
  if (!VirtualProtectEx(process, address, 1, previous, &ignored))
    valid = 0;
  return valid;
}
static int patch_render(HANDLE process, uintptr_t image,
                        const DdResolution *symbols) {
  for (size_t i = 0; i < sizeof(windows_render_sites) / sizeof(WindowsSite);
       ++i) {
    const WindowsSite *site = windows_render_sites + i;
    void *address = (void *)(image + symbols->values[site->symbol]);
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
static void report_exception(const DEBUG_EVENT *event, uintptr_t image) {
  const EXCEPTION_DEBUG_INFO *exception = &event->u.Exception;
  uintptr_t at = (uintptr_t)exception->ExceptionRecord.ExceptionAddress;
  report("DD_WINDOWS_EXCEPTION code=%lx first_chance=%lu address=%llx "
         "image=%llx rva=%llx\n",
         exception->ExceptionRecord.ExceptionCode, exception->dwFirstChance,
         (unsigned long long)at, (unsigned long long)image,
         (unsigned long long)(at >= image ? at - image : UINT64_MAX));
  if (exception->ExceptionRecord.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      exception->ExceptionRecord.NumberParameters >= 2)
    report(
        "DD_WINDOWS_ACCESS kind=%llu target=%llx\n",
        (unsigned long long)exception->ExceptionRecord.ExceptionInformation[0],
        (unsigned long long)exception->ExceptionRecord.ExceptionInformation[1]);
}
static int launch_game(const wchar_t *game, wchar_t *command,
                       const wchar_t *directory, const wchar_t *library,
                       int runtime_rig, int native_render) {
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
  wchar_t *mutable_command = _wcsdup(command);
  if (!mutable_command)
    return 0;
  int created = CreateProcessW(game, mutable_command, NULL, NULL, FALSE,
                               DEBUG_ONLY_THIS_PROCESS, NULL, directory,
                               &startup, &process) != 0;
  free(mutable_command);
  if (!created)
    return 0;
  ULONGLONG deadline = GetTickCount64() + 45000;
  uintptr_t image = 0;
  EntryBreakpoint entry = {0};
  DdResolution symbols = {0};
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
        if (!image || !find_entry(process.hProcess, image, &entry) ||
            !entry_breakpoint(process.hProcess, &entry, 1))
          break;
        armed = 1;
        disposition = DBG_CONTINUE;
      } else if (!initialized && armed && code == EXCEPTION_BREAKPOINT &&
                 trap == entry.address &&
                 event.dwThreadId == process.dwThreadId) {
        CONTEXT context = {0};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!entry_breakpoint(process.hProcess, &entry, 0) ||
            !GetThreadContext(process.hThread, &context))
          break;
        context.Rip = entry.address;
        if (!SetThreadContext(process.hThread, &context) ||
            !verify_remote(process.hProcess, image, runtime_rig, native_render,
                           &symbols) ||
            !patch_resources(process.hProcess, image, &symbols) ||
            (native_render &&
             !patch_render(process.hProcess, image, &symbols)) ||
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
        report_exception(&event, image);
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
    (void)DebugActiveProcessStop(process.dwProcessId);
    (void)WaitForSingleObject(process.hProcess, 5000);
    report("DD_WINDOWS_PATCH_FAILED error=%lu\n", error);
  } else
    report("DD_WINDOWS_LAUNCHED pid=%lu runtime_rig=%d disk_modified=0\n",
           process.dwProcessId, runtime_rig);
  if (remote_thread)
    CloseHandle(remote_thread);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return success;
}
static int launch_unmodified(const wchar_t *game, const wchar_t *command,
                             const wchar_t *directory) {
  STARTUPINFOW startup = {0};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {0};
  wchar_t *mutable_command = _wcsdup(command);
  if (!mutable_command)
    return 0;
  int result = CreateProcessW(game, mutable_command, NULL, NULL, FALSE, 0, NULL,
                              directory, &startup, &process) != 0;
  free(mutable_command);
  if (result) {
    report("DD_WINDOWS_UNMODIFIED_LAUNCHED pid=%lu\n", process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  } else
    report("DD_WINDOWS_GAME_START_FAILED error=%lu\n", GetLastError());
  return result;
}
static int steam_game_path(const wchar_t *original, wchar_t game[32768]) {
  DWORD count = GetFullPathNameW(original, 32768, game, NULL);
  const wchar_t *x64 = L"\\_windows\\win64\\Darkest.exe";
  const wchar_t *x86 = L"\\_windows\\Darkest.exe";
  if (!count || count >= 32768)
    return 0;
  size_t length = wcslen(game), size64 = wcslen(x64), size32 = wcslen(x86);
  if (length >= size32 && !_wcsicmp(game + length - size32, x86) &&
      length - size32 + size64 < 32768) {
    wchar_t candidate[32768];
    wcscpy(candidate, game);
    wcscpy(candidate + length - size32, x64);
    if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES)
      wcscpy(game, candidate);
  }
  return 1;
}
int wmain(int argc, wchar_t **argv) {
  wchar_t game[32768] = {0}, home[32768], library[32768], command[32768] = {0};
  int check = 0, runtime_rig = 1, native_render = 1, remaining = argc,
      steam_mode = 0;
  for (int i = 1; i < argc; ++i) {
    if (!wcscmp(argv[i], L"--check"))
      check = 1;
    else if (!wcscmp(argv[i], L"--native-render"))
      native_render = 1;
    else if (!wcscmp(argv[i], L"--no-native-render"))
      native_render = 0;
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
  if (steam_mode) {
    /* FreeConsole closes OS handles without updating the CRT's stderr. A
     * later file can reuse that handle, causing duplicate or corrupt logs. */
    report_stderr = 0;
    (void)FreeConsole();
  }
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
    log_file = CreateFileW(log_path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE)
      report("DD_WINDOWS_LOG_UNAVAILABLE error=%lu\n", GetLastError());
    SetEnvironmentVariableW(L"DD_WINDOWS_PATCH_LOG", log_path);
  }
  HANDLE file = CreateFileW(game, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  IMAGE_DOS_HEADER dos;
  IMAGE_NT_HEADERS64 pe;
  int readable = file != INVALID_HANDLE_VALUE && read_pe(file, &dos, &pe);
  DdResolution symbols = {0};
  int compatible =
      readable && verify_file_sites(file, runtime_rig, native_render, &symbols);
  if (file != INVALID_HANDLE_VALUE)
    CloseHandle(file);
  if (!compatible)
    report("DD_WINDOWS_PATCH_SKIPPED reason=%s\n",
           readable ? "instruction_mismatch" : "executable_format");
  if (check)
    return readable ? 0 : 125;
  wchar_t directory[32768];
  wcscpy(directory, game);
  const wchar_t *suffix = L"\\_windows\\win64\\Darkest.exe";
  const wchar_t *x86_suffix = L"\\_windows\\Darkest.exe";
  size_t length = wcslen(directory), suffix_length = wcslen(suffix);
  int parents = 1;
  if (length >= suffix_length &&
      !_wcsicmp(directory + length - suffix_length, suffix))
    parents = 3;
  else if (length >= wcslen(x86_suffix) &&
           !_wcsicmp(directory + length - wcslen(x86_suffix), x86_suffix))
    parents = 2;
  for (int i = 0; i < parents; ++i) {
    slash = wcsrchr(directory, L'\\');
    if (!slash)
      return 125;
    if (slash == directory + 2 && directory[1] == L':')
      slash[1] = 0;
    else
      *slash = 0;
  }
  int valid = append_argument(command, 32768, game);
  int has_steam_default = 0;
  for (int i = remaining; i < argc; ++i)
    if (!wcscmp(argv[i], L"-skipvalidation"))
      has_steam_default = 1;
  /* App 262060's native Windows launch configuration supplies this flag.
   * Retain Steam's normal startup contract when launching the game EXE. */
  if (valid && !has_steam_default)
    valid = append_argument(command, 32768, L"-skipvalidation");
  for (int i = remaining; valid && i < argc; ++i)
    valid = append_argument(command, 32768, argv[i]);
  /* The Steam client must already be running. This does not replace Steam's
   * ownership checks or modify its local configuration. */
  SetEnvironmentVariableW(L"SteamAppId", L"262060");
  SetEnvironmentVariableW(L"SteamGameId", L"262060");
  SetEnvironmentVariableW(L"DD_RIG_MAP", NULL);
  int result = valid && compatible &&
               launch_game(game, command, directory, library, runtime_rig,
                           native_render);
  if (valid && !result) {
    if (compatible)
      report("DD_WINDOWS_PATCH_SKIPPED reason=initialization_failed\n");
    result = launch_unmodified(game, command, directory);
  }
  if (log_file != INVALID_HANDLE_VALUE)
    CloseHandle(log_file);
  return result ? 0 : 125;
}
#else
#include <stdio.h>
int main(void) {
  fputs("Build windows_launcher.c with a Windows x64 Clang target.\n", stderr);
  return 125;
}
#endif
