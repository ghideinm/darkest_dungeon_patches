#ifndef DD_WINDOWS_RENDER_H
#define DD_WINDOWS_RENDER_H
#include "windows_canvas.h"

/* No graphics work occurs during LoadLibrary. The swap hook runs after native
 * presentation, on the thread which owns the current OpenGL context. */
static void (*windows_original_swap)(void *);
static void (*windows_drawable_size)(void *, int *, int *);
static void *(*windows_gl_context)(void);
static void *(*windows_gl_proc)(const char *);
static void(APIENTRY *canvas_active_texture)(unsigned);
static void(APIENTRY *canvas_get_integer)(unsigned, int *);
static void(APIENTRY *canvas_gen_framebuffers)(int, unsigned *);
static void(APIENTRY *canvas_bind_framebuffer)(unsigned, unsigned);
static void(APIENTRY *canvas_delete_framebuffers)(int, const unsigned *);
static void(APIENTRY *canvas_gen_textures)(int, unsigned *);
static void(APIENTRY *canvas_bind_texture)(unsigned, unsigned);
static void(APIENTRY *canvas_delete_textures)(int, const unsigned *);
static void(APIENTRY *canvas_tex_image)(unsigned, int, int, int, int, int,
                                        unsigned, unsigned, const void *);
static void(APIENTRY *canvas_tex_parameter)(unsigned, unsigned, int);
static void(APIENTRY *canvas_gen_renderbuffers)(int, unsigned *);
static void(APIENTRY *canvas_bind_renderbuffer)(unsigned, unsigned);
static void(APIENTRY *canvas_delete_renderbuffers)(int, const unsigned *);
static void(APIENTRY *canvas_renderbuffer_storage)(unsigned, unsigned, int,
                                                   int);
static void(APIENTRY *canvas_framebuffer_texture)(unsigned, unsigned, unsigned,
                                                  unsigned, int);
static void(APIENTRY *canvas_framebuffer_renderbuffer)(unsigned, unsigned,
                                                       unsigned, unsigned);
static unsigned(APIENTRY *canvas_check_framebuffer)(unsigned);
static int canvas_gl_ready, canvas_done, canvas_failed_width,
    canvas_failed_height;

__declspec(dllexport) volatile LONG dd_windows_canvas_ready;
static unsigned canvas_wait_logged;
static void windows_canvas_wait(unsigned reason, const char *name) {
  if (!(canvas_wait_logged & reason)) {
    canvas_wait_logged |= reason;
    dprintf(2, "DD_NATIVE_CANVAS_PENDING reason=%s\n", name);
  }
}
static int windows_canvas_gl(void) {
  if (canvas_gl_ready)
    return 1;
#define GL_LOAD(target, name)                                                  \
  do {                                                                         \
    target = (void *)windows_gl_proc(name);                                    \
    if ((uintptr_t)target <= 3 || (uintptr_t)target == UINTPTR_MAX)            \
      return 0;                                                                \
  } while (0)
  GL_LOAD(canvas_active_texture, "glActiveTexture");
  GL_LOAD(canvas_get_integer, "glGetIntegerv");
  GL_LOAD(canvas_gen_framebuffers, "glGenFramebuffers");
  GL_LOAD(canvas_bind_framebuffer, "glBindFramebuffer");
  GL_LOAD(canvas_delete_framebuffers, "glDeleteFramebuffers");
  GL_LOAD(canvas_gen_textures, "glGenTextures");
  GL_LOAD(canvas_bind_texture, "glBindTexture");
  GL_LOAD(canvas_delete_textures, "glDeleteTextures");
  GL_LOAD(canvas_tex_image, "glTexImage2D");
  GL_LOAD(canvas_tex_parameter, "glTexParameteri");
  GL_LOAD(canvas_gen_renderbuffers, "glGenRenderbuffers");
  GL_LOAD(canvas_bind_renderbuffer, "glBindRenderbuffer");
  GL_LOAD(canvas_delete_renderbuffers, "glDeleteRenderbuffers");
  GL_LOAD(canvas_renderbuffer_storage, "glRenderbufferStorage");
  GL_LOAD(canvas_framebuffer_texture, "glFramebufferTexture2D");
  GL_LOAD(canvas_framebuffer_renderbuffer, "glFramebufferRenderbuffer");
  GL_LOAD(canvas_check_framebuffer, "glCheckFramebufferStatus");
#undef GL_LOAD
  canvas_gl_ready = 1;
  return 1;
}
static void windows_canvas_drop(const DdCanvasTarget *target) {
  if (target->depth)
    canvas_delete_renderbuffers(1, &target->depth);
  if (target->texture)
    canvas_delete_textures(1, &target->texture);
  if (target->framebuffer)
    canvas_delete_framebuffers(1, &target->framebuffer);
}
static int windows_canvas_create(DdCanvasTarget *out, int width, int height,
                                 int alpha) {
  out->width = width;
  out->height = height;
  canvas_gen_framebuffers(1, &out->framebuffer);
  canvas_gen_textures(1, &out->texture);
  canvas_gen_renderbuffers(1, &out->depth);
  if (!out->framebuffer || !out->texture || !out->depth)
    return 0;
  canvas_bind_framebuffer(0x8d40, out->framebuffer);
  canvas_bind_texture(0x0de1, out->texture);
  unsigned format = alpha ? 0x1908U : 0x1907U;
  canvas_tex_image(0x0de1, 0, (int)format, width, height, 0, format, 0x1401,
                   NULL);
  canvas_tex_parameter(0x0de1, 0x2802, 0x812f);
  canvas_tex_parameter(0x0de1, 0x2803, 0x812f);
  canvas_tex_parameter(0x0de1, 0x2801, 0x2601);
  canvas_tex_parameter(0x0de1, 0x2800, 0x2601);
  canvas_framebuffer_texture(0x8ca9, 0x8ce0, 0x0de1, out->texture, 0);
  canvas_bind_renderbuffer(0x8d41, out->depth);
  canvas_renderbuffer_storage(0x8d41, 0x88f0, width, height);
  canvas_framebuffer_renderbuffer(0x8ca9, 0x821a, 0x8d41, out->depth);
  return canvas_check_framebuffer(0x8ca9) == 0x8cd5;
}
static DdCanvasTarget *windows_canvas_target(const char *name) {
  uint32_t hash = 0;
  for (const unsigned char *p = (const void *)name; *p; ++p)
    hash = hash * 53U + *p;
  void *registry = *(void **)native_address(DD_RENDER_REGISTRY);
  void *head = registry ? pointer_at(registry, 0) : NULL;
  void *node = head ? pointer_at(head, 8) : NULL;
  for (unsigned depth = 0; node && depth < 64; ++depth) {
    if (*(unsigned char *)((char *)node + 0x19))
      break;
    uint32_t key = (uint32_t)int_at(node, 0x20);
    if (key == hash)
      return pointer_at(node, 0x28);
    node = pointer_at(node, hash < key ? 0 : 0x10);
  }
  return NULL;
}
static void windows_canvas_frame(void *window) {
  if (canvas_done || !dd_windows_ready)
    return;
  if (*(void **)native_address(DD_RENDER_WINDOW) != window)
    return;
  if (!windows_gl_context()) {
    windows_canvas_wait(1, "context");
    return;
  }
  int width = 0, height = 0;
  windows_drawable_size(window, &width, &height);
  if (!dd_canvas_fit(width, height, &width, &height) ||
      (width == canvas_failed_width && height == canvas_failed_height))
    return;
  /* Startup's existing target publication must have finished, and no native
   * target may still be bound. Never touch GPU state with an unpack PBO active.
   */
  if (*(void **)native_address(DD_RENDER_BOUND_TARGET)) {
    windows_canvas_wait(2, "native_target_bound");
    return;
  }
  if (!windows_canvas_gl()) {
    windows_canvas_wait(4, "gl_api");
    return;
  }
  int draw = 0, read = 0, unpack = 0, renderbuffer = 0;
  canvas_get_integer(0x8ca6, &draw);
  canvas_get_integer(0x8caa, &read);
  canvas_get_integer(0x88ef, &unpack);
  if (draw || read || unpack) {
    windows_canvas_wait(8, "frame_boundary");
    return;
  }
  DdCanvasTarget *targets[3] = {windows_canvas_target("main"),
                                windows_canvas_target("gui"),
                                windows_canvas_target("gui_utility")};
  for (unsigned i = 0; i < DD_CANVAS_TARGETS; ++i)
    if (!targets[i]) {
      windows_canvas_wait(16, "target_registry");
      return;
    }
  canvas_get_integer(0x8ca7, &renderbuffer);
  int active = 0, units = 0;
  canvas_get_integer(0x84e0, &active);
  canvas_get_integer(0x8b4d, &units);
  if (units <= 0 || units > 256 || active < 0x84c0 || active >= 0x84c0 + units)
    return;
  int limit = 0;
  canvas_get_integer(0x0d33, &limit); /* GL_MAX_TEXTURE_SIZE */
  if (width > limit || height > limit)
    return;
  canvas_get_integer(0x84e8, &limit); /* GL_MAX_RENDERBUFFER_SIZE */
  if (width > limit || height > limit)
    return;
  unsigned textures[256];
  for (int i = 0; i < units; ++i) {
    int texture = 0;
    canvas_active_texture(0x84c0U + (unsigned)i);
    canvas_get_integer(0x8069, &texture);
    textures[i] = (unsigned)texture;
  }
  canvas_active_texture((unsigned)active);
  DdCanvasTarget old[3];
  for (unsigned i = 0; i < DD_CANVAS_TARGETS; ++i)
    old[i] = *targets[i];
  uint32_t restored_texture = textures[active - 0x84c0];
  int result = dd_canvas_rebuild(targets, width, height, windows_canvas_create,
                                 windows_canvas_drop, &restored_texture);
  canvas_bind_framebuffer(0x8d40, 0);
  if (result == 2)
    for (unsigned i = 0; i < DD_CANVAS_TARGETS; ++i) {
      if ((unsigned)renderbuffer == old[i].depth)
        renderbuffer = (int)targets[i]->depth;
      for (int unit = 0; unit < units; ++unit)
        if (textures[unit] == old[i].texture)
          textures[unit] = targets[i]->texture;
    }
  canvas_bind_renderbuffer(0x8d41, (unsigned)renderbuffer);
  for (int i = 0; i < units; ++i) {
    canvas_active_texture(0x84c0U + (unsigned)i);
    canvas_bind_texture(0x0de1, textures[i]);
  }
  canvas_active_texture((unsigned)active);
  if (result) {
    canvas_done = 1;
    InterlockedExchange(&dd_windows_canvas_ready, 1);
    dprintf(2, "DD_NATIVE_CANVAS_READY width=%d height=%d rebuilt=%d\n", width,
            height, result == 2);
  } else {
    InterlockedExchange(&dd_windows_canvas_ready, -1);
    canvas_failed_width = width;
    canvas_failed_height = height;
    dprintf(2, "DD_NATIVE_CANVAS_REBUILD_FAILED width=%d height=%d\n", width,
            height);
  }
}
static void windows_canvas_swap(void *window) {
  windows_original_swap(window);
  windows_canvas_frame(window);
}
static int windows_render_plan(DdWindowsPatches *plan) {
  void **slot = (void **)native_address(DD_RENDER_SWAP);
  HMODULE module = NULL;
  if (!*slot ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (const wchar_t *)*slot, &module))
    return 0;
  windows_original_swap = (void *)GetProcAddress(module, "SDL_GL_SwapWindow");
  windows_drawable_size =
      (void *)GetProcAddress(module, "SDL_GL_GetDrawableSize");
  windows_gl_context =
      (void *)GetProcAddress(module, "SDL_GL_GetCurrentContext");
  windows_gl_proc = (void *)GetProcAddress(module, "SDL_GL_GetProcAddress");
  if ((void *)windows_original_swap != *slot || !windows_drawable_size ||
      !windows_gl_context || !windows_gl_proc)
    return 0;
  void *hook = (void *)windows_canvas_swap;
  return windows_plan_edit(plan, slot, &hook, sizeof(hook));
}
#endif
