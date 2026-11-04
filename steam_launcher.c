#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { MAX_SITES = 64, MAX_BYTES = 32 };
typedef struct {
  uint64_t address;
  size_t length;
  unsigned char bytes[MAX_BYTES];
} Site;
typedef struct {
  char game[PATH_MAX], log[PATH_MAX], sha256[66];
  Site sites[MAX_SITES];
  size_t count;
} Config;

static int line(FILE *file, char *out, size_t capacity) {
  if (!fgets(out, (int)capacity, file))
    return 0;
  size_t length = strlen(out);
  if (!length || out[length - 1] != '\n')
    return 0;
  out[length - 1] = 0;
  return 1;
}

static int read_config(const char *directory, Config *config) {
  char path[PATH_MAX], text[256];
  int n = snprintf(path, sizeof(path), "%s/launcher.conf", directory);
  if (n < 0 || (size_t)n >= sizeof(path))
    return 0;
  FILE *file = fopen(path, "r");
  if (!file)
    return 0;
  int valid =
      line(file, text, sizeof(text)) && !strcmp(text, "dd_steam_launcher_v1") &&
      line(file, config->game, sizeof(config->game)) &&
      config->game[0] == '/' && line(file, config->log, sizeof(config->log)) &&
      config->log[0] == '/' &&
      line(file, config->sha256, sizeof(config->sha256)) &&
      strlen(config->sha256) == 64 &&
      strspn(config->sha256, "0123456789abcdef") == 64;
  while (valid && fgets(text, sizeof(text), file)) {
    char hex[MAX_BYTES * 2 + 1], extra;
    unsigned long long address;
    if (config->count == MAX_SITES ||
        sscanf(text, "%llx %64s %c", &address, hex, &extra) != 2 ||
        !strlen(hex) || strlen(hex) % 2 ||
        strspn(hex, "0123456789abcdef") != strlen(hex)) {
      valid = 0;
      break;
    }
    Site *site = config->sites + config->count++;
    site->address = (uint64_t)address;
    site->length = strlen(hex) / 2;
    for (size_t i = 0; i < site->length; ++i) {
      char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
      site->bytes[i] = (unsigned char)strtoul(pair, NULL, 16);
    }
  }
  valid = valid && !ferror(file) && config->count > 0;
  fclose(file);
  return valid;
}

/* Use the system's native coreutils implementation, without invoking a shell.
 * Hash the already-open descriptor also used for ELF/instruction validation. */
static int check_sha256(int game, const char *expected) {
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC))
    return 0;
  pid_t child = fork();
  if (!child) {
    close(pipefd[0]);
    if (dup2(game, STDIN_FILENO) < 0 || dup2(pipefd[1], STDOUT_FILENO) < 0)
      _exit(125);
    execlp("sha256sum", "sha256sum", (char *)NULL);
    _exit(125);
  }
  close(pipefd[1]);
  if (child < 0) {
    close(pipefd[0]);
    return 0;
  }
  char actual[66] = {0};
  size_t used = 0;
  while (used < sizeof(actual) - 1) {
    ssize_t n = read(pipefd[0], actual + used, sizeof(actual) - 1 - used);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    used += (size_t)n;
  }
  close(pipefd[0]);
  int status;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  return waited == child && WIFEXITED(status) && !WEXITSTATUS(status) &&
         used == 65 && actual[64] == ' ' && !memcmp(actual, expected, 64);
}

static int check_sites(const unsigned char *data, size_t size,
                       const Config *config) {
  Elf64_Ehdr header;
  if (size < sizeof(header))
    return 0;
  memcpy(&header, data, sizeof(header));
  if (memcmp(header.e_ident, ELFMAG, SELFMAG) ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64 ||
      (header.e_type != ET_EXEC && header.e_type != ET_DYN) ||
      header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum ||
      header.e_phnum > 128 || header.e_phoff > size ||
      header.e_phnum > (size - header.e_phoff) / sizeof(Elf64_Phdr))
    return 0;
  for (size_t i = 0; i < config->count; ++i) {
    const Site *site = config->sites + i;
    int found = 0;
    for (size_t j = 0; j < header.e_phnum; ++j) {
      Elf64_Phdr segment;
      memcpy(&segment, data + header.e_phoff + j * sizeof(segment),
             sizeof(segment));
      if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X) ||
          site->address < segment.p_vaddr)
        continue;
      uint64_t delta = site->address - segment.p_vaddr;
      if (delta > segment.p_filesz || site->length > segment.p_filesz - delta ||
          segment.p_offset > size || delta > size - segment.p_offset ||
          site->length > size - segment.p_offset - delta)
        continue;
      if (!memcmp(data + segment.p_offset + delta, site->bytes, site->length))
        found = 1;
      break;
    }
    if (!found) {
      fprintf(stderr, "DD_STEAM_SITE_REFUSED address=%llx\n",
              (unsigned long long)site->address);
      return 0;
    }
  }
  return 1;
}

static int validate(const Config *config) {
  int fd = open(config->game, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  struct stat info;
  int valid = !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size > 0 &&
              (uintmax_t)info.st_size <= SIZE_MAX &&
              check_sha256(fd, config->sha256);
  if (valid) {
    size_t size = (size_t)info.st_size;
    void *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED)
      valid = 0;
    else {
      valid = check_sites(data, size, config);
      munmap(data, size);
    }
  }
  close(fd);
  return valid;
}

static int configure_environment(const char *directory) {
  const char *names[] = {"libdd_resource_loading_patch.so",
                         "libdd_skin_rig_patch.so"};
  char libraries[2][PATH_MAX];
  for (size_t i = 0; i < 2; ++i) {
    int n = snprintf(libraries[i], sizeof(libraries[i]), "%s/%s", directory,
                     names[i]);
    struct stat info;
    if (n < 0 || (size_t)n >= sizeof(libraries[i]) ||
        stat(libraries[i], &info) || !S_ISREG(info.st_mode))
      return 0;
  }
  const char *previous = getenv("LD_PRELOAD");
  if (previous && (strstr(previous, names[0]) || strstr(previous, names[1]))) {
    fprintf(stderr, "DD_STEAM_PRELOAD_REFUSED duplicate_patch\n");
    return 0;
  }
  char *preload = NULL;
  if (asprintf(&preload, "%s:%s%s%s", libraries[0], libraries[1],
               previous && *previous ? ":" : "", previous ? previous : "") < 0)
    return 0;
  int valid = !setenv("LD_PRELOAD", preload, 1) &&
              !setenv("DD_LOADBUFFER_PATCH", "1", 1) &&
              !setenv("DD_NATIVE_RENDER", "1", 1) &&
              !setenv("DD_RIG_RUNTIME", "1", 1) && !unsetenv("DD_RIG_MAP");
  free(preload);
  return valid;
}

/* Steam may wrap the executable in reaper/runtime commands. Preserve that
 * complete argument vector, choosing only the verified 64-bit game entry. */
static int select_game(char **command, const Config *config) {
  char sibling[PATH_MAX];
  size_t length = strlen(config->game);
  if (length < 3 || strcmp(config->game + length - 3, "_64"))
    return 0;
  memcpy(sibling, config->game, length - 3);
  sibling[length - 3] = 0;
  int found = 0;
  for (char **arg = command; *arg; ++arg) {
    char resolved[PATH_MAX];
    if (!realpath(*arg, resolved))
      continue;
    if (!strcmp(resolved, config->game) || !strcmp(resolved, sibling)) {
      if (found)
        return 0;
      *arg = (char *)config->game;
      found = 1;
    }
  }
  return found;
}

int main(int argc, char **argv) {
  int check = argc == 2 && !strcmp(argv[1], "--check");
  if (!check && (argc < 3 || strcmp(argv[1], "--"))) {
    fprintf(stderr, "Usage: steam_launcher --check | -- <Steam command...>\n");
    return 125;
  }
  char directory[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", directory, sizeof(directory) - 1);
  if (n <= 0 || (size_t)n >= sizeof(directory) - 1)
    return 125;
  directory[n] = 0;
  char *slash = strrchr(directory, '/');
  if (!slash)
    return 125;
  *slash = 0;
  if (strpbrk(directory, ": \t\r\n"))
    return 125;
  Config config = {0};
  if (!read_config(directory, &config)) {
    fprintf(stderr, "DD_STEAM_CONFIG_REFUSED\n");
    return 125;
  }
  if (!check) {
    int log = open(config.log, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (log < 0 || dup2(log, STDOUT_FILENO) < 0 ||
        dup2(log, STDERR_FILENO) < 0) {
      if (log >= 0)
        close(log);
      return 125;
    }
    close(log);
    fprintf(stderr, "DD_STEAM_LAUNCH time=%lld pid=%ld\n",
            (long long)time(NULL), (long)getpid());
  }
  if (!validate(&config)) {
    fprintf(stderr, "DD_STEAM_GAME_REFUSED game=%s\n", config.game);
    return 125;
  }
  if (check) {
    printf("DD_STEAM_LAUNCHER_CHECK_OK game=%s sites=%zu\n", config.game,
           config.count);
    return 0;
  }
  char **command = argv + 2;
  if (!select_game(command, &config) || !configure_environment(directory)) {
    fprintf(stderr, "DD_STEAM_COMMAND_REFUSED\n");
    return 125;
  }
  fprintf(stderr, "DD_STEAM_LAUNCHER_READY game=%s sites=%zu\n", config.game,
          config.count);
  fflush(NULL);
  /* Steam/user launch options intentionally select this wrapper chain.
   * select_game checked its game payload; forward argv without a shell. */
  // NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint)
  execvp(command[0], command);
  perror("DD_STEAM_EXEC_FAILED");
  return 125;
}
