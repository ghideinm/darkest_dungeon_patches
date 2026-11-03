#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

/* Linux 28212, SHA256 fe1a629f...1102156. Only immediate operands change. */
enum { OLD_CAP = 0x600000, NEW_CAP = 0x2000000, SITE_COUNT = 7 };
static const uintptr_t sites[SITE_COUNT] = {
    0xc3c288, 0xc3c343, 0xc3c3fa, 0xc3c4b1,
    0xc3c568, 0xc3c61f, 0xc3f77f
};

struct image {
    uintptr_t base;
    const ElfW(Phdr) *phdr;
    size_t count;
};

#ifndef DD_PATCH_SELFTEST
static int main_image(struct dl_phdr_info *info, size_t size, void *opaque) {
    (void)size;
    if (info->dlpi_name && info->dlpi_name[0]) return 0;
    struct image *image = opaque;
    image->base = info->dlpi_addr;
    image->phdr = info->dlpi_phdr;
    image->count = info->dlpi_phnum;
    return 1;
}
#endif

static int contains_code(const struct image *image, uintptr_t address, size_t length) {
    for (size_t i = 0; i < image->count; ++i) {
        const ElfW(Phdr) *p = image->phdr + i;
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        uintptr_t start = image->base + p->p_vaddr;
        if (address >= start && address + length >= address &&
            address + length <= start + p->p_memsz) return 1;
    }
    return 0;
}

static int apply_patch(const struct image *image) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 8 || (page_size & (page_size - 1))) return 1;
    uintptr_t pages[SITE_COUNT];
    size_t page_count = 0;
    /* Verify every site before making any page writable or changing a byte. */
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        uintptr_t address = image->base + sites[i];
        if (!contains_code(image, address, 5)) return 2;
        unsigned char expected[5] = {i == 6 ? 0xb9 : 0xbf, 0, 0, 0x60, 0};
        if (memcmp((void *)address, expected, sizeof(expected))) return 3;
        uintptr_t page = address & ~((uintptr_t)page_size - 1);
        size_t j;
        for (j = 0; j < page_count; ++j) if (pages[j] == page) break;
        if (j == page_count) pages[page_count++] = page;
        if (address + 4 >= page + (uintptr_t)page_size) return 4;
    }
    size_t writable = 0;
    for (; writable < page_count; ++writable) {
        if (mprotect((void *)pages[writable], (size_t)page_size,
                     PROT_READ | PROT_WRITE)) break;
    }
    if (writable != page_count) {
        for (size_t j = 0; j < writable; ++j)
            (void)mprotect((void *)pages[j], (size_t)page_size, PROT_READ | PROT_EXEC);
        return 5;
    }
    const uint32_t cap = NEW_CAP;
    for (size_t i = 0; i < SITE_COUNT; ++i)
        memcpy((void *)(image->base + sites[i] + 1), &cap, sizeof(cap));
    for (size_t j = 0; j < page_count; ++j) {
        __builtin___clear_cache((char *)pages[j], (char *)(pages[j] + page_size));
        if (mprotect((void *)pages[j], (size_t)page_size, PROT_READ | PROT_EXEC))
            return 6;
    }
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        uint32_t actual;
        memcpy(&actual, (void *)(image->base + sites[i] + 1), sizeof(actual));
        if (actual != NEW_CAP) return 7;
    }
    return 0;
}

/* Darkest::Start: follow the fitted physical canvas, retaining logical UI
 * coordinates. Existing comparisons/cmov instructions become harmless no-ops. */
static const uintptr_t render_sites[2] = {0xca13ca, 0xca13d3};
static const size_t render_lengths[2] = {5, 6};
static const unsigned char render_expected[2][8] = {
    {0xbd, 0x80, 0x07, 0, 0},
    {0x41, 0xbe, 0x38, 0x04, 0, 0}
};
static const unsigned char render_replacement[2][8] = {
    {0x44, 0x89, 0xf5, 0x90, 0x90},       /* mov r14d,ebp */
    {0x45, 0x89, 0xfe, 0x90, 0x90, 0x90} /* mov r15d,r14d */
};
static const uintptr_t dungeon_site = 0x10af426;
static const unsigned char dungeon_expected[3] = {0x0f, 0x47, 0xe8};
/* mov eax,ebp; nop: use physical main-target width, keeping the logical canvas. */
static const unsigned char dungeon_replacement[3] = {0x89, 0xc5, 0x90};
static const uintptr_t gui_sites[4] = {0xca1485, 0xca148a, 0xca14a6, 0xca14ab};
static const unsigned char gui_expected[4][5] = {
    {0xbf, 0x80, 0x07, 0, 0}, {0xbe, 0x38, 0x04, 0, 0},
    {0xbf, 0x80, 0x07, 0, 0}, {0xbe, 0x38, 0x04, 0, 0}
};
/* Use saved physical main width and r14d main height. Logical UI stays 1920x1080. */
static const unsigned char gui_replacement[4][5] = {
    {0x8b, 0x7c, 0x24, 0x1c, 0x90}, {0x44, 0x89, 0xf6, 0x90, 0x90},
    {0x8b, 0x7c, 0x24, 0x1c, 0x90}, {0x44, 0x89, 0xf6, 0x90, 0x90}
};

static int apply_render_patch(const struct image *image) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 8 || (page_size & (page_size - 1))) return 20;
    uintptr_t page = (image->base + render_sites[0]) & ~((uintptr_t)page_size - 1);
    for (size_t i = 0; i < 2; ++i) {
        uintptr_t address = image->base + render_sites[i];
        if (!contains_code(image, address, render_lengths[i]) ||
            address < page || address + render_lengths[i] > page + (uintptr_t)page_size)
            return 21;
        if (memcmp((void *)address, render_expected[i], render_lengths[i])) return 22;
    }
    uintptr_t dungeon = image->base + dungeon_site;
    uintptr_t dungeon_page = dungeon & ~((uintptr_t)page_size - 1);
    if (!contains_code(image, dungeon, 3) || dungeon + 3 > dungeon_page + (uintptr_t)page_size)
        return 21;
    if (memcmp((void *)dungeon, dungeon_expected, 3)) return 22;
    for (size_t i = 0; i < 4; ++i) {
        uintptr_t address = image->base + gui_sites[i];
        if (!contains_code(image, address, 5) || address < page || address + 5 > page + (uintptr_t)page_size)
            return 21;
        if (memcmp((void *)address, gui_expected[i], 5)) return 22;
    }
    if (mprotect((void *)page, (size_t)page_size, PROT_READ | PROT_WRITE)) return 23;
    if (mprotect((void *)dungeon_page, (size_t)page_size, PROT_READ | PROT_WRITE)) {
        (void)mprotect((void *)page, (size_t)page_size, PROT_READ | PROT_EXEC);
        return 23;
    }
    for (size_t i = 0; i < 2; ++i)
        memcpy((void *)(image->base + render_sites[i]), render_replacement[i], render_lengths[i]);
    memcpy((void *)dungeon, dungeon_replacement, 3);
    for (size_t i = 0; i < 4; ++i)
        memcpy((void *)(image->base + gui_sites[i]), gui_replacement[i], 5);
    __builtin___clear_cache((char *)page, (char *)(page + page_size));
    if (mprotect((void *)page, (size_t)page_size, PROT_READ | PROT_EXEC)) return 24;
    __builtin___clear_cache((char *)dungeon_page, (char *)(dungeon_page + page_size));
    if (mprotect((void *)dungeon_page, (size_t)page_size, PROT_READ | PROT_EXEC)) return 24;
    return 0;
}

#ifndef DD_PATCH_SELFTEST
static uintptr_t game_base;
static int render_enabled;
__attribute__((constructor)) static void initialize(void) {
    const char *enabled = getenv("DD_LOADBUFFER_PATCH");
    if (!enabled || strcmp(enabled, "1")) return;
    char executable[4096];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable) - 1) _exit(125);
    executable[length] = '\0';
    const char *name = strrchr(executable, '/');
    if (!name || strcmp(name + 1, "darkest.bin.x86_64")) return;
    struct image image = {0};
    if (!dl_iterate_phdr(main_image, &image)) _exit(125);
    game_base = image.base;
    int result = apply_patch(&image);
    if (result) {
        dprintf(STDERR_FILENO, "DD_LOADBUFFER_PATCH_REFUSED code=%d\n", result);
        _exit(125);
    }
    dprintf(STDERR_FILENO,
            "DD_LOADBUFFER_PATCH_APPLIED cap=%u buffers=6 sites=7\n", NEW_CAP);
    const char *render = getenv("DD_NATIVE_RENDER");
    if (render && !strcmp(render, "1")) {
        result = apply_render_patch(&image);
        if (result) {
            dprintf(STDERR_FILENO, "DD_NATIVE_RENDER_PATCH_REFUSED code=%d\n", result);
            _exit(125);
        }
        render_enabled = 1;
        dprintf(STDERR_FILENO, "DD_NATIVE_RENDER_PATCH_APPLIED cap=game_canvas sites=7 dungeon=physical_width gui=main_size\n");
    }
}

/* Read-only diagnostics at the native swap boundary. No game state, transforms,
 * filtering, lighting or save data are changed here. */
static uint64_t previous_targets;
static unsigned swaps;
static void report_targets(uintptr_t node, unsigned depth, uint64_t *signature) {
    if (!node || depth > 16) return;
    const uintptr_t *links = (const uintptr_t *)node;
    report_targets(links[2], depth + 1, signature);
    uintptr_t target;
    memcpy(&target, (void *)(node + 40), sizeof(target));
    if (target) {
        uint32_t fields[5];
        memcpy(fields, (void *)target, sizeof(fields));
        for (size_t i = 0; i < 5; ++i) *signature = (*signature ^ fields[i]) * UINT64_C(1099511628211);
    }
    report_targets(links[3], depth + 1, signature);
}

static void print_targets(uintptr_t node, unsigned depth) {
    if (!node || depth > 16) return;
    const uintptr_t *links = (const uintptr_t *)node;
    print_targets(links[2], depth + 1);
    uintptr_t target;
    memcpy(&target, (void *)(node + 40), sizeof(target));
    if (target) {
        uint32_t fields[5];
        memcpy(fields, (void *)target, sizeof(fields));
        dprintf(STDERR_FILENO, "DD_RENDER_TARGET fbo=%u texture=%u size=%ux%u\n",
                fields[0], fields[1], fields[3], fields[4]);
    }
    print_targets(links[3], depth + 1);
}

static void capture_backbuffer(void *window) {
    const char *directory = getenv("DD_RENDER_CAPTURE_DIR");
    if (!directory) return;
    char request[4096], output[4096];
    if (snprintf(request, sizeof(request), "%s/capture.request", directory) >= (int)sizeof(request) ||
        snprintf(output, sizeof(output), "%s/capture.ppm", directory) >= (int)sizeof(output)) return;
    if (access(request, F_OK)) return;
    void (*size)(void *, int *, int *) = dlsym(RTLD_NEXT, "SDL_GL_GetDrawableSize");
    void (*get)(unsigned, int *) = dlsym(RTLD_NEXT, "glGetIntegerv");
    void (*read)(int, int, int, int, unsigned, unsigned, void *) = dlsym(RTLD_NEXT, "glReadPixels");
    void (*pack)(unsigned, int) = dlsym(RTLD_NEXT, "glPixelStorei");
    void (*bind)(unsigned, unsigned) = dlsym(RTLD_NEXT, "glBindFramebuffer");
    void (*read_buffer)(unsigned) = dlsym(RTLD_NEXT, "glReadBuffer");
    if (!size || !get || !read || !pack || !bind || !read_buffer) return;
    int width = 0, height = 0, framebuffer = -1, alignment = 4;
    size(window, &width, &height);
    get(0x8ca6, &framebuffer);
    if (framebuffer != 0 || width < 1 || height < 1 || width > 8192 || height > 8192) return;
    unsigned char *pixels = malloc((size_t)width * height * 3);
    if (!pixels) return;
    int previous_read = 0, previous_buffer = 0;
    get(0x8caa, &previous_read);
    bind(0x8ca8, 0);
    get(0x0c02, &previous_buffer);
    read_buffer(0x0405);
    get(0x0d05, &alignment);
    pack(0x0d05, 1);
    read(0, 0, width, height, 0x1907, 0x1401, pixels);
    pack(0x0d05, alignment);
    read_buffer((unsigned)previous_buffer);
    bind(0x8ca8, (unsigned)previous_read);
    int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        FILE *file = fdopen(fd, "wb");
        if (file) {
            fprintf(file, "P6\n%d %d\n255\n", width, height);
            for (int y = height - 1; y >= 0; --y)
                fwrite(pixels + (size_t)y * width * 3, 3, (size_t)width, file);
            fclose(file);
            unlink(request);
            dprintf(STDERR_FILENO, "DD_RENDER_CAPTURE size=%dx%d file=%s\n", width, height, output);
        } else close(fd);
    }
    free(pixels);
}

/* Verify actual private dungeon FBOs too; they are not registered in the
 * RenderTargetManager map. Run once after FBO 5 exists, restoring all bindings. */
static void report_private_targets(void) {
    static int done;
    if (done) return;
    unsigned char (*is_fbo)(unsigned) = dlsym(RTLD_NEXT, "glIsFramebuffer");
    void (*bind_fbo)(unsigned, unsigned) = dlsym(RTLD_NEXT, "glBindFramebuffer");
    void (*attachment)(unsigned, unsigned, unsigned, int *) = dlsym(RTLD_NEXT, "glGetFramebufferAttachmentParameteriv");
    void (*get)(unsigned, int *) = dlsym(RTLD_NEXT, "glGetIntegerv");
    void (*bind_tex)(unsigned, unsigned) = dlsym(RTLD_NEXT, "glBindTexture");
    void (*tex_level)(unsigned, int, unsigned, int *) = dlsym(RTLD_NEXT, "glGetTexLevelParameteriv");
    if (!is_fbo || !bind_fbo || !attachment || !get || !bind_tex || !tex_level || !is_fbo(5)) return;
    int previous_fbo = 0, previous_texture = 0;
    get(0x8caa, &previous_fbo);
    get(0x8069, &previous_texture);
    for (unsigned id = 1; id <= 5; ++id) {
        if (!is_fbo(id)) continue;
        bind_fbo(0x8ca8, id);
        int texture = 0, width = 0, height = 0;
        attachment(0x8ca8, 0x8ce0, 0x8cd1, &texture);
        if (!texture) continue;
        bind_tex(0x0de1, (unsigned)texture);
        tex_level(0x0de1, 0, 0x1000, &width);
        tex_level(0x0de1, 0, 0x1001, &height);
        dprintf(STDERR_FILENO, "DD_RENDER_GPU_TARGET fbo=%u texture=%d size=%dx%d\n", id, texture, width, height);
    }
    bind_tex(0x0de1, (unsigned)previous_texture);
    bind_fbo(0x8ca8, (unsigned)previous_fbo);
    done = 1;
}

void SDL_GL_SwapWindow(void *window) {
    static void (*real_swap)(void *);
    if (!real_swap) real_swap = dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    if (!real_swap) _exit(125);
    if (render_enabled && ++swaps % 60 == 0) {
        uintptr_t manager;
        memcpy(&manager, (void *)(game_base + 0x1d746d8), sizeof(manager));
        if (manager) {
            uintptr_t root;
            memcpy(&root, (void *)(manager + 16), sizeof(root));
            uint64_t signature = UINT64_C(14695981039346656037);
            report_targets(root, 0, &signature);
            if (signature != previous_targets) {
                print_targets(root, 0);
                previous_targets = signature;
            }
        }
        capture_backbuffer(window);
        report_private_targets();
    }
    real_swap(window);
}
#else
int main(void) {
    const size_t length = 0x10b0000;
    unsigned char *memory = mmap(NULL, length, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 10;
    ElfW(Phdr) phdr = {.p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_memsz = length};
    struct image image = {(uintptr_t)memory, &phdr, 1};
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        unsigned char bytes[5] = {i == 6 ? 0xb9 : 0xbf, 0, 0, 0x60, 0};
        memcpy(memory + sites[i], bytes, 5);
    }
    memory[sites[6]] ^= 1;
    if (apply_patch(&image) != 3) return 11;
    for (size_t i = 0; i < SITE_COUNT; ++i) {
        uint32_t value;
        memcpy(&value, memory + sites[i] + 1, 4);
        if (value != OLD_CAP) return 12;
    }
    memory[sites[6]] ^= 1;
    if (apply_patch(&image)) return 13;
    if (apply_patch(&image) != 3) return 14;
    for (size_t i = 0; i < 2; ++i)
        memcpy(memory + render_sites[i], render_expected[i], render_lengths[i]);
    memcpy(memory + dungeon_site, dungeon_expected, 3);
    for (size_t i = 0; i < 4; ++i)
        memcpy(memory + gui_sites[i], gui_expected[i], 5);
    memory[render_sites[1]] ^= 1;
    if (apply_render_patch(&image) != 22) return 16;
    if (memcmp(memory + render_sites[0], render_expected[0], render_lengths[0])) return 17;
    memory[render_sites[1]] ^= 1;
    memory[dungeon_site] ^= 1;
    if (apply_render_patch(&image) != 22) return 21;
    if (memcmp(memory + render_sites[0], render_expected[0], render_lengths[0])) return 22;
    memory[dungeon_site] ^= 1;
    memory[gui_sites[3]] ^= 1;
    if (apply_render_patch(&image) != 22) return 24;
    if (memcmp(memory + render_sites[0], render_expected[0], render_lengths[0])) return 25;
    memory[gui_sites[3]] ^= 1;
    if (apply_render_patch(&image)) return 18;
    if (memcmp(memory + dungeon_site, dungeon_replacement, 3)) return 23;
    for (size_t i = 0; i < 4; ++i)
        if (memcmp(memory + gui_sites[i], gui_replacement[i], 5)) return 26;
    for (size_t i = 0; i < 2; ++i)
        if (memcmp(memory + render_sites[i], render_replacement[i], render_lengths[i])) return 19;
    if (apply_render_patch(&image) != 22) return 20;
    if (munmap(memory, length)) return 15;
    puts("PATCH_SELFTEST_PASS: loader and render operands; mismatch changes nothing; repeat refused");
    return 0;
}
#endif
