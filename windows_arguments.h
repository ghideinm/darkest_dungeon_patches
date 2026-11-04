#ifndef DD_WINDOWS_ARGUMENTS_H
#define DD_WINDOWS_ARGUMENTS_H
#include <stddef.h>
#include <wchar.h>

static int append_argument(wchar_t *command, size_t capacity,
                           const wchar_t *argument) {
  size_t used = wcslen(command);
#define PUT(character)                                                         \
  do {                                                                         \
    if (used + 1 >= capacity)                                                  \
      return 0;                                                                \
    command[used++] = (character);                                             \
  } while (0)
  if (used)
    PUT(L' ');
  PUT(L'"');
  for (const wchar_t *p = argument; *p;) {
    size_t slashes = 0;
    while (*p == L'\\') {
      ++slashes;
      ++p;
    }
    size_t escaped = (*p == L'"' || !*p) ? slashes * 2 : slashes;
    for (size_t i = 0; i < escaped; ++i)
      PUT(L'\\');
    if (!*p)
      break;
    if (*p == L'"')
      PUT(L'\\');
    PUT(*p++);
  }
  PUT(L'"');
  command[used] = 0;
#undef PUT
  return 1;
}
#endif
