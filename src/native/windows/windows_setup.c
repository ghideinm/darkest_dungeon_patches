#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "windows_arguments.h"
#include "windows_setup_payload.h"
#include <shellapi.h>
#include <stdio.h>

enum { PATH_CAP = 32768, OUTPUT_CAP = 16384 };
static wchar_t setup_message[OUTPUT_CAP];
static int join_path(wchar_t out[PATH_CAP], const wchar_t *root,
                     const wchar_t *name) {
  int n = swprintf(out, PATH_CAP, L"%ls\\%ls", root, name);
  return n > 0 && n < PATH_CAP;
}
static int write_payload(const wchar_t *root, const wchar_t *name,
                         const unsigned char *bytes, DWORD length) {
  wchar_t path[PATH_CAP];
  if (!join_path(path, root, name))
    return 0;
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE)
    return 0;
  DWORD written;
  int valid =
      WriteFile(file, bytes, length, &written, NULL) && written == length;
  CloseHandle(file);
  return valid;
}
static void remove_payload(const wchar_t *root) {
  const wchar_t *names[] = {L"windows_install.ps1", L"winmm.dll",
                            L"dd_engine_patch.asi", L"winmm.ini",
                            L"dd_engine_patch.ini"};
  wchar_t path[PATH_CAP];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (join_path(path, root, names[i]))
      (void)DeleteFileW(path);
  (void)RemoveDirectoryW(root);
}
static int setup(int argc, wchar_t **argv, int *quiet) {
  const wchar_t *action = L"Install";
  int selected = 0, has_root = 0;
  for (int i = 1; i < argc; ++i) {
    const wchar_t *next = NULL;
    if (!wcscmp(argv[i], L"--quiet"))
      *quiet = 1;
    else if (!wcscmp(argv[i], L"--license")) {
      (void)MultiByteToWideChar(CP_UTF8, 0, (const char *)setup_license,
                                (int)sizeof(setup_license), setup_message,
                                OUTPUT_CAP - 1);
      return 0;
    } else if (!wcscmp(argv[i], L"--install"))
      next = L"Install";
    else if (!wcscmp(argv[i], L"--uninstall"))
      next = L"Uninstall";
    else if (!wcscmp(argv[i], L"--status"))
      next = L"Status";
    else if (!wcscmp(argv[i], L"--preview"))
      next = L"Preview";
    else if ((!wcscmp(argv[i], L"--steam-root") ||
              !wcscmp(argv[i], L"--game-root")) &&
             i + 1 < argc) {
      if (!wcscmp(argv[i], L"--game-root"))
        has_root = 1;
      ++i;
    } else
      return 125;
    if (next) {
      if (selected++)
        return 125;
      action = next;
    }
  }
  wchar_t temporary[PATH_CAP], root[PATH_CAP], system[PATH_CAP],
      powershell[PATH_CAP], script[PATH_CAP], command[PATH_CAP] = {0};
  DWORD count = GetTempPathW(PATH_CAP, root);
  if (!count || count >= PATH_CAP ||
      !GetTempFileNameW(root, L"ddp", 0, temporary))
    return 125;
  if (!DeleteFileW(temporary) || !CreateDirectoryW(temporary, NULL))
    return 125;
  int exit_code = 125;
  HANDLE input = INVALID_HANDLE_VALUE, reader = NULL, writer = NULL;
  if (!write_payload(temporary, L"windows_install.ps1", setup_script,
                     (DWORD)sizeof(setup_script)) ||
      !write_payload(temporary, L"winmm.dll", setup_proxy,
                     (DWORD)sizeof(setup_proxy)) ||
      !write_payload(temporary, L"winmm.ini", setup_config,
                     (DWORD)sizeof(setup_config)) ||
      !write_payload(temporary, L"dd_engine_patch.ini", setup_patch_config,
                     (DWORD)sizeof(setup_patch_config)) ||
      !write_payload(temporary, L"dd_engine_patch.asi", setup_library,
                     (DWORD)sizeof(setup_library)) ||
      !join_path(script, temporary, L"windows_install.ps1"))
    goto done;
  count = GetSystemDirectoryW(system, PATH_CAP);
  if (!count || count >= PATH_CAP ||
      !join_path(powershell, system,
                 L"WindowsPowerShell\\v1.0\\powershell.exe"))
    goto done;
  const wchar_t *arguments[] = {
      powershell,          L"-NoLogo", L"-NoProfile", L"-NonInteractive",
      L"-ExecutionPolicy", L"Bypass",  L"-File",      script,
      L"-Action",          action};
  for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
    if (!append_argument(command, PATH_CAP, arguments[i]))
      goto done;
  for (int i = 1; i < argc; ++i) {
    const wchar_t *parameter = NULL;
    if (!wcscmp(argv[i], L"--steam-root"))
      parameter = L"-SteamRoot";
    else if (!wcscmp(argv[i], L"--game-root"))
      parameter = L"-GameRoot";
    if (parameter && (!append_argument(command, PATH_CAP, parameter) ||
                      !append_argument(command, PATH_CAP, argv[++i])))
      goto done;
  }
  /* Dropping the installer into the game root also works without Steam. */
  if (!has_root) {
    count = GetModuleFileNameW(NULL, root, PATH_CAP);
    wchar_t *slash = count && count < PATH_CAP ? wcsrchr(root, L'\\') : NULL;
    if (slash) {
      *slash = 0;
      wchar_t candidate[PATH_CAP];
      const wchar_t *folders[] = {L"_windows\\win64\\Darkest.exe",
                                  L"_windowsnosteam\\win64\\Darkest.exe"};
      for (size_t i = 0; i < 2; ++i)
        if (join_path(candidate, root, folders[i]) &&
            GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
          if (!append_argument(command, PATH_CAP, L"-GameRoot") ||
              !append_argument(command, PATH_CAP, root))
            goto done;
          break;
        }
    }
  }
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  if (!CreatePipe(&reader, &writer, &security, 0) ||
      !SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0))
    goto done;
  input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (input == INVALID_HANDLE_VALUE)
    goto done;
  STARTUPINFOW startup = {0};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input;
  startup.hStdOutput = writer;
  startup.hStdError = writer;
  PROCESS_INFORMATION process = {0};
  if (CreateProcessW(powershell, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                     NULL, temporary, &startup, &process)) {
    CloseHandle(writer);
    writer = NULL;
    char output[OUTPUT_CAP];
    size_t used = 0;
    DWORD got;
    char chunk[1024];
    while (ReadFile(reader, chunk, sizeof(chunk), &got, NULL) && got) {
      size_t keep =
          got < sizeof(output) - 1 - used ? got : sizeof(output) - 1 - used;
      memcpy(output + used, chunk, keep);
      used += keep;
    }
    output[used] = 0;
    (void)MultiByteToWideChar(CP_UTF8, 0, output, -1, setup_message,
                              OUTPUT_CAP);
    DWORD code;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &code))
      exit_code = code == 0 ? 0 : 1;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    HANDLE standard = GetStdHandle(STD_OUTPUT_HANDLE);
    if (standard && standard != INVALID_HANDLE_VALUE) {
      DWORD written;
      (void)WriteFile(standard, output, (DWORD)used, &written, NULL);
    }
  }
done:
  if (input != INVALID_HANDLE_VALUE)
    CloseHandle(input);
  if (reader)
    CloseHandle(reader);
  if (writer)
    CloseHandle(writer);
  remove_payload(temporary);
  return exit_code;
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, wchar_t *line,
                    int show) {
  (void)instance;
  (void)previous;
  (void)line;
  (void)show;
  int argc = 0, quiet = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  int result = argv ? setup(argc, argv, &quiet) : 125;
  if (argv)
    LocalFree(argv);
  if (!quiet)
    MessageBoxW(NULL,
                *setup_message ? setup_message
                               : L"Setup could not complete. Close the game "
                                 L"and specify --game-root if needed.",
                L"Darkest Dungeon Patches",
                MB_OK | (result ? MB_ICONERROR : MB_ICONINFORMATION));
  return result;
}
#else
#include <stdio.h>
int main(void) {
  fputs("Build windows_setup.c with a Windows x64 Clang target.\n", stderr);
  return 125;
}
#endif
