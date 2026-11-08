#ifndef DD_PATCH_CONFIG_H
#define DD_PATCH_CONFIG_H
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define DD_ORIGINAL_BUFFER_MIB 16U
#else
#include <dlfcn.h>
#include <unistd.h>
#define DD_ORIGINAL_BUFFER_MIB 6U
#endif

#define DD_MIB UINT32_C(1048576)
#define DD_CONFIG_NAME "dd_engine_patch.ini"
typedef struct {
  uint32_t resource_buffer_mib;
  size_t cache_initial_capacity;
  int log_to_file;
  char log_file[4096];
} DdPatchConfig;
static DdPatchConfig dd_config = {32, 16, 0, "dd_engine_patch.log"};
static int dd_config_initialized;
static FILE *dd_log_file;

static inline int dd_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
         c == '\f';
}
static inline int dd_copy(char *out, size_t capacity, const char *text) {
  size_t length = strlen(text);
  if (length >= capacity)
    return 0;
  memcpy(out, text, length + 1);
  return 1;
}
static inline char *dd_trim(char *text) {
  while (dd_space(*text))
    ++text;
  size_t length = strlen(text);
  while (length && dd_space(text[length - 1]))
    text[--length] = 0;
  return text;
}
static inline int dd_config_number(const char *text, uint64_t minimum,
                                   uint64_t maximum, uint64_t *out) {
  if (!*text)
    return 0;
  uint64_t number = 0;
  for (const char *p = text; *p; ++p) {
    if (*p < '0' || *p > '9' || number > maximum / 10 ||
        (number == maximum / 10 && (unsigned)(*p - '0') > maximum % 10))
      return 0;
    number = number * 10 + (unsigned)(*p - '0');
  }
  if (number < minimum)
    return 0;
  *out = number;
  return 1;
}
/* Parse into defaults, retaining a valid field when another field is invalid.
 */
static inline unsigned dd_config_read(FILE *file, DdPatchConfig *config,
                                      int *log_explicit) {
  char line[8192], section[32] = {0};
  unsigned invalid = 0;
  int first = 1;
  while (!feof(file) && !ferror(file) && fgets(line, sizeof(line), file)) {
    if (!strchr(line, '\n') && !feof(file)) {
      int c;
      while ((c = fgetc(file)) != '\n' && c != EOF) {
      }
      ++invalid;
      if (c == EOF)
        break;
      continue;
    }
    char *text = line;
    if (first && !strncmp(text, "\xef\xbb\xbf", 3))
      text += 3;
    first = 0;
    text = dd_trim(text);
    if (!*text || *text == ';' || *text == '#')
      continue;
    if (*text == '[') {
      char *end = strchr(text, ']');
      if (!end || *dd_trim(end + 1) ||
          (size_t)(end - text - 1) >= sizeof(section)) {
        *section = 0;
        ++invalid;
        continue;
      }
      *end = 0;
      snprintf(section, sizeof(section), "%s", text + 1);
      continue;
    }
    char *equal = strchr(text, '=');
    if (!equal) {
      ++invalid;
      continue;
    }
    *equal = 0;
    char *key = dd_trim(text), *value = dd_trim(equal + 1);
    uint64_t number;
    if (!strcmp(section, "memory") && !strcmp(key, "resource_buffer_mib")) {
      if (dd_config_number(value, DD_ORIGINAL_BUFFER_MIB, INT32_MAX / DD_MIB,
                           &number))
        config->resource_buffer_mib = (uint32_t)number;
      else
        ++invalid;
    } else if (!strcmp(section, "memory") &&
               !strcmp(key, "cache_initial_capacity")) {
      if (dd_config_number(value, 1, PTRDIFF_MAX / sizeof(void *), &number))
        config->cache_initial_capacity = (size_t)number;
      else
        ++invalid;
    } else if (!strcmp(section, "logging") && !strcmp(key, "log_to_file")) {
      *log_explicit = 1;
      if (dd_config_number(value, 0, 1, &number))
        config->log_to_file = (int)number;
      else
        ++invalid;
    } else if (!strcmp(section, "logging") && !strcmp(key, "log_file")) {
      if (!*value ||
          !dd_copy(config->log_file, sizeof(config->log_file), value))
        ++invalid;
    }
  }
  return invalid + (unsigned)(ferror(file) != 0);
}
static inline int dd_absolute_path(const char *path) {
#ifdef _WIN32
  return *path == '/' || *path == '\\' ||
         (((path[0] >= 'A' && path[0] <= 'Z') ||
           (path[0] >= 'a' && path[0] <= 'z')) &&
          path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
#else
  return *path == '/';
#endif
}
static inline int dd_relative_path(char out[4096], const char *anchor,
                                   const char *path) {
  if (dd_absolute_path(path)) {
    return dd_copy(out, 4096, path);
  }
  const char *slash = strrchr(anchor, '/');
#ifdef _WIN32
  const char *back = strrchr(anchor, '\\');
  if (back && (!slash || back > slash))
    slash = back;
#endif
  size_t prefix = slash ? (size_t)(slash - anchor + 1) : 0;
  if (prefix >= 4096 || strlen(path) >= 4096 - prefix)
    return 0;
  memcpy(out, anchor, prefix);
  memcpy(out + prefix, path, strlen(path) + 1);
  return 1;
}
static inline FILE *dd_open_utf8(const char *path, int append) {
#ifdef _WIN32
  wchar_t wide[4096];
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 4096))
    return NULL;
  return _wfopen(wide, append ? L"ab" : L"rb");
#else
  return fopen(path, append ? "ab" : "rb");
#endif
}
static inline int dd_module_path(char path[4096], const void *anchor) {
#ifdef _WIN32
  HMODULE module = NULL;
  wchar_t wide[4096];
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (const wchar_t *)anchor, &module))
    return 0;
  DWORD length = GetModuleFileNameW(module, wide, 4096);
  return length && length < 4096 &&
         WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, 4096, NULL, NULL) > 0;
#else
  Dl_info info;
  if (!dladdr(anchor, &info) || !info.dli_fname)
    return 0;
  if (*info.dli_fname == '/') {
    return dd_copy(path, 4096, info.dli_fname);
  }
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd)))
    return 0;
  int length = snprintf(path, 4096, "%s/%s", cwd, info.dli_fname);
  return length > 0 && length < 4096;
#endif
}
static inline const char *dd_path_environment(const char *name,
                                              char utf8[4096]) {
#ifdef _WIN32
  wchar_t key[64], value[4096];
  if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, key, 64))
    return NULL;
  DWORD length = GetEnvironmentVariableW(key, value, 4096);
  if (!length || length >= 4096 ||
      !WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, 4096, NULL, NULL))
    return NULL;
  return utf8;
#else
  (void)utf8;
  return getenv(name);
#endif
}
static inline void dd_debug_line(int descriptor, const char *text,
                                 size_t length) {
#ifdef _WIN32
  (void)descriptor;
  (void)length;
  OutputDebugStringA(text);
#else
  (void)write(descriptor, text, length);
#endif
}
static inline int dd_patch_log(int descriptor, const char *format, ...) {
  int saved_errno = errno;
#ifdef _WIN32
  DWORD saved_error = GetLastError();
#endif
  char line[4096];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (length >= 0) {
    size_t size =
        (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1;
    dd_debug_line(descriptor, line, size);
    if (dd_log_file) {
      (void)fwrite(line, 1, size, dd_log_file);
      (void)fflush(dd_log_file);
    }
  }
  errno = saved_errno;
#ifdef _WIN32
  SetLastError(saved_error);
#endif
  return length;
}
static inline void dd_runtime_init(const void *anchor) {
  if (dd_config_initialized)
    return;
  dd_config_initialized = 1;
  char module[4096] = {0}, config_path[4096] = {0};
  int module_found = dd_module_path(module, anchor);
  char explicit_utf8[4096], legacy_utf8[4096];
  const char *explicit_path =
      dd_path_environment("DD_PATCH_CONFIG", explicit_utf8);
  int located = explicit_path && *explicit_path
                    ? dd_copy(config_path, sizeof(config_path), explicit_path)
                    : module_found &&
                          dd_relative_path(config_path, module, DD_CONFIG_NAME);
  int log_explicit = 0;
  unsigned invalid = 0;
  FILE *file = located ? dd_open_utf8(config_path, 0) : NULL;
  int config_found = file != NULL;
  if (file) {
    invalid = dd_config_read(file, &dd_config, &log_explicit);
    fclose(file);
  }
  const char *legacy_log = dd_path_environment("DD_PATCH_LOG", legacy_utf8);
  if (!config_found && !log_explicit && legacy_log && *legacy_log &&
      dd_copy(dd_config.log_file, sizeof(dd_config.log_file), legacy_log))
    dd_config.log_to_file = 1;
  if (dd_config.log_to_file) {
    char path[4096];
    const char *base = located ? config_path : module;
    if (dd_relative_path(path, base, dd_config.log_file))
      dd_log_file = dd_open_utf8(path, 1);
    if (!dd_log_file)
      dd_patch_log(2, "DD_PATCH_LOG_UNAVAILABLE\n");
  }
  if (invalid)
    dd_patch_log(2, "DD_PATCH_CONFIG_INVALID fields=%u path=%s\n", invalid,
                 config_path);
  dd_patch_log(2,
               "DD_PATCH_CONFIG buffer_mib=%u cache_initial=%zu log_to_file=%d "
               "path=%s\n",
               dd_config.resource_buffer_mib, dd_config.cache_initial_capacity,
               dd_config.log_to_file, config_found ? config_path : "defaults");
}
static inline uint32_t dd_resource_buffer_bytes(void) {
  return dd_config.resource_buffer_mib * DD_MIB;
}
static inline uint32_t dd_rig_file_limit(void) {
  const char *enabled = getenv("DD_LOADBUFFER_PATCH");
#ifdef _WIN32
  int patched = !enabled || strcmp(enabled, "0");
#else
  int patched = enabled && !strcmp(enabled, "1");
#endif
  return patched ? dd_resource_buffer_bytes() : DD_ORIGINAL_BUFFER_MIB * DD_MIB;
}
#endif
