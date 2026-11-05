#ifndef DD_LINUX_RUNTIME_H
#define DD_LINUX_RUNTIME_H
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static inline void dd_linux_log(void) {
  const char *log = getenv("DD_PATCH_LOG");
  if (!log || !*log)
    return;
  int file = open(log, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
  if (file < 0) {
    dprintf(STDERR_FILENO, "DD_PATCH_LOG_UNAVAILABLE\n");
    return;
  }
  if (file != STDERR_FILENO) {
    if (dup2(file, STDERR_FILENO) < 0)
      dprintf(STDERR_FILENO, "DD_PATCH_LOG_UNAVAILABLE\n");
    close(file);
  }
}
#endif
