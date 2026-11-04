#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "windows_arguments.h"
#include "windows_setup_payload.h"
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>

enum { PATH_CAP = 32768 };
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
  DWORD written = 0;
  int valid =
      WriteFile(file, bytes, length, &written, NULL) && written == length;
  CloseHandle(file);
  return valid;
}
static void remove_payload(const wchar_t *root) {
  const wchar_t *names[] = {L"windows_steam.ps1", L"dd_windows_launcher.exe",
                            L"dd_skin_rig_patch.dll"};
  wchar_t path[PATH_CAP];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (join_path(path, root, names[i]))
      (void)DeleteFileW(path);
  (void)RemoveDirectoryW(root);
}
static int setup(int argc, wchar_t **argv, wchar_t log_path[PATH_CAP],
                 int *quiet) {
  const wchar_t *action = L"Install";
  int selected = 0;
  for (int i = 1; i < argc; ++i) {
    const wchar_t *next = NULL;
    if (!wcscmp(argv[i], L"--quiet"))
      *quiet = 1;
    else if (!wcscmp(argv[i], L"--install"))
      next = L"Install";
    else if (!wcscmp(argv[i], L"--uninstall"))
      next = L"Uninstall";
    else if (!wcscmp(argv[i], L"--status"))
      next = L"Status";
    else if (!wcscmp(argv[i], L"--preview"))
      next = L"Preview";
    else if ((!wcscmp(argv[i], L"--steam-root") ||
              !wcscmp(argv[i], L"--steam-user") ||
              !wcscmp(argv[i], L"--game-root") ||
              !wcscmp(argv[i], L"--install-root")) &&
             i + 1 < argc)
      ++i;
    else
      return 125;
    if (next) {
      if (selected++)
        return 125;
      action = next;
    }
  }
  wchar_t root[PATH_CAP], system[PATH_CAP], powershell[PATH_CAP];
  DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", root, PATH_CAP);
  if (!count || count >= PATH_CAP ||
      !join_path(system, root, L"DarkestDungeonPatches"))
    return 125;
  int result = SHCreateDirectoryExW(NULL, system, NULL);
  if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS &&
      result != ERROR_FILE_EXISTS)
    return 125;
  if (!join_path(log_path, system, L"setup.log"))
    return 125;
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE log =
      CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  HANDLE input =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
    if (log != INVALID_HANDLE_VALUE)
      CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE)
      CloseHandle(input);
    return 125;
  }
  wchar_t temporary[PATH_CAP], script[PATH_CAP], command[PATH_CAP] = {0};
  int owned = 0, exit_code = 125;
  count = GetTempPathW(PATH_CAP, root);
  if (!count || count >= PATH_CAP ||
      !GetTempFileNameW(root, L"ddp", 0, temporary))
    goto done;
  if (!DeleteFileW(temporary) || !CreateDirectoryW(temporary, NULL))
    goto done;
  owned = 1;
  if (!write_payload(temporary, L"windows_steam.ps1", setup_script,
                     (DWORD)sizeof(setup_script)) ||
      !write_payload(temporary, L"dd_windows_launcher.exe", setup_launcher,
                     (DWORD)sizeof(setup_launcher)) ||
      !write_payload(temporary, L"dd_skin_rig_patch.dll", setup_library,
                     (DWORD)sizeof(setup_library)) ||
      !join_path(script, temporary, L"windows_steam.ps1"))
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
    else if (!wcscmp(argv[i], L"--steam-user"))
      parameter = L"-SteamUser";
    else if (!wcscmp(argv[i], L"--game-root"))
      parameter = L"-GameRoot";
    else if (!wcscmp(argv[i], L"--install-root"))
      parameter = L"-InstallRoot";
    if (parameter && (!append_argument(command, PATH_CAP, parameter) ||
                      !append_argument(command, PATH_CAP, argv[++i])))
      goto done;
  }
  STARTUPINFOW startup = {0};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input;
  startup.hStdOutput = log;
  startup.hStdError = log;
  PROCESS_INFORMATION process = {0};
  if (CreateProcessW(powershell, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                     NULL, temporary, &startup, &process)) {
    DWORD code;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &code))
      exit_code = code == 0 ? 0 : 1;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
done:
  if (exit_code == 125) {
    char message[128];
    int length =
        snprintf(message, sizeof(message),
                 "Native setup failed, Windows error: %lu\r\n", GetLastError());
    DWORD written;
    if (length > 0 && (size_t)length < sizeof(message))
      (void)WriteFile(log, message, (DWORD)length, &written, NULL);
  }
  CloseHandle(input);
  CloseHandle(log);
  if (owned)
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
  wchar_t log_path[PATH_CAP] = {0};
  int result = argv ? setup(argc, argv, log_path, &quiet) : 125;
  if (argv)
    LocalFree(argv);
  if (!quiet) {
    wchar_t message[PATH_CAP + 512];
    swprintf(message, sizeof(message) / sizeof(message[0]),
             L"%ls\n\nLog: %ls\n\nClose the game and Steam normally before "
             L"installing or removing the patch. After installing, reopen "
             L"Steam and use Play.\n\nRemove: dd_patch_setup.exe --uninstall",
             result ? L"Patch setup did not complete. See the log for details."
                    : L"Patch setup operation completed.",
             *log_path ? log_path : L"not available");
    MessageBoxW(NULL, message, L"Darkest Dungeon Patches",
                MB_OK | (result ? MB_ICONERROR : MB_ICONINFORMATION));
  }
  return result;
}
#else
#include <stdio.h>
int main(void) {
  fputs("Build windows_setup.c with a Windows x64 Clang target.\n", stderr);
  return 125;
}
#endif
