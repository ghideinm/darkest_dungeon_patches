#include "../src/native/common/skin_rig_patch.c"
#include <assert.h>

typedef struct {
  unsigned char bytes[0x30];
} Node;
static Node head, nodes[3];
static void *registry_head = &head, *registry = &registry_head, *bound_target;
static DdCanvasTarget targets[3];
static unsigned next_id, created, deleted, fail_at, swaps;
static unsigned textures[4], active, renderbuffer, draw_buffer, read_buffer,
    unpack;
static int context_ready, missing_proc, drawable_width = 3840,
                                        drawable_height = 2160;
static DWORD render_thread;
static void *game_window = (void *)1;
static unsigned char alive[4096];
static void store(void *p, size_t offset, const void *value) {
  memcpy((char *)p + offset, &value, sizeof(value));
}
static void APIENTRY active_texture(unsigned value) {
  active = value - 0x84c0;
  assert(active < 4);
}
static void APIENTRY get_integer(unsigned name, int *out) {
  assert(GetCurrentThreadId() == render_thread);
  switch (name) {
  case 0x8ca6:
    *out = (int)draw_buffer;
    break;
  case 0x8caa:
    *out = (int)read_buffer;
    break;
  case 0x88ef:
    *out = (int)unpack;
    break;
  case 0x8ca7:
    *out = (int)renderbuffer;
    break;
  case 0x8069:
    *out = (int)textures[active];
    break;
  case 0x84e0:
    *out = (int)(0x84c0 + active);
    break;
  case 0x8b4d:
    *out = 4;
    break;
  case 0x0d33:
  case 0x84e8:
    *out = 8192;
    break;
  default:
    assert(0);
  }
}
static void APIENTRY gen(int count, unsigned *out) {
  assert(swaps && count == 1 && next_id < sizeof(alive));
  *out = next_id++;
  alive[*out] = 1;
}
static void APIENTRY gen_framebuffer(int count, unsigned *out) {
  ++created;
  gen(count, out);
}
static void APIENTRY drop(int count, const unsigned *id) {
  assert(count == 1 && *id < sizeof(alive) && alive[*id]);
  alive[*id] = 0;
  ++deleted;
  for (unsigned i = 0; i < 4; ++i)
    if (textures[i] == *id)
      textures[i] = 0;
  if (renderbuffer == *id)
    renderbuffer = 0;
}
static void APIENTRY bind_framebuffer(unsigned kind, unsigned id) {
  assert(kind == 0x8d40);
  draw_buffer = read_buffer = id;
}
static void APIENTRY bind_texture(unsigned kind, unsigned id) {
  assert(kind == 0xde1 && (!id || alive[id]));
  textures[active] = id;
}
static void APIENTRY bind_renderbuffer(unsigned kind, unsigned id) {
  assert(kind == 0x8d41 && (!id || alive[id]));
  renderbuffer = id;
}
static void APIENTRY image(unsigned kind, int level, int format, int width,
                           int height, int border, unsigned external,
                           unsigned type, const void *pixels) {
  assert(kind == 0xde1 && level == 0 && !border && !pixels && !unpack);
  assert(format == (created == 1 ? 0x1907 : 0x1908) &&
         external == (unsigned)format && type == 0x1401);
  assert(width == drawable_width && height == drawable_height);
}
static void APIENTRY parameter(unsigned kind, unsigned name, int value) {
  (void)name;
  (void)value;
  assert(kind == 0xde1);
}
static void APIENTRY storage(unsigned kind, unsigned format, int width,
                             int height) {
  assert(kind == 0x8d41 && format == 0x88f0 && width == drawable_width &&
         height == drawable_height);
}
static void APIENTRY attach_texture(unsigned kind, unsigned attachment,
                                    unsigned type, unsigned id, int level) {
  assert(kind == 0x8ca9 && attachment == 0x8ce0 && type == 0xde1 && id &&
         level == 0);
}
static void APIENTRY attach_depth(unsigned kind, unsigned attachment,
                                  unsigned type, unsigned id) {
  assert(kind == 0x8ca9 && attachment == 0x821a && type == 0x8d41 && id);
}
static unsigned APIENTRY complete(unsigned kind) {
  assert(kind == 0x8ca9);
  return created == fail_at ? 0x8cd6 : 0x8cd5;
}
static void *proc(const char *name) {
  if (missing_proc)
    return missing_proc == 1 ? NULL : (void *)1;
#define PROC(n, f)                                                             \
  if (!strcmp(name, n))                                                        \
  return (void *)f
  PROC("glActiveTexture", active_texture);
  PROC("glGetIntegerv", get_integer);
  PROC("glGenFramebuffers", gen_framebuffer);
  PROC("glBindFramebuffer", bind_framebuffer);
  PROC("glDeleteFramebuffers", drop);
  PROC("glGenTextures", gen);
  PROC("glBindTexture", bind_texture);
  PROC("glDeleteTextures", drop);
  PROC("glTexImage2D", image);
  PROC("glTexParameteri", parameter);
  PROC("glGenRenderbuffers", gen);
  PROC("glBindRenderbuffer", bind_renderbuffer);
  PROC("glDeleteRenderbuffers", drop);
  PROC("glRenderbufferStorage", storage);
  PROC("glFramebufferTexture2D", attach_texture);
  PROC("glFramebufferRenderbuffer", attach_depth);
  PROC("glCheckFramebufferStatus", complete);
#undef PROC
  assert(0);
  return NULL;
}
static void swap(void *window) {
  assert(window == (void *)1);
  ++swaps;
}
static void size(void *window, int *width, int *height) {
  assert(window == (void *)1);
  *width = drawable_width;
  *height = drawable_height;
}
static void *context(void) { return context_ready ? (void *)1 : NULL; }
static void reset(void) {
  memset(alive, 0, sizeof(alive));
  for (unsigned i = 0; i < 10; ++i)
    alive[i] = 1;
  alive[77] = 1;
  for (unsigned i = 0; i < 3; ++i)
    targets[i] = (DdCanvasTarget){i * 3 + 1, i * 3 + 2, i * 3 + 3, 1920, 1080};
  textures[0] = 2;
  textures[1] = 5;
  textures[2] = 8;
  textures[3] = 77;
  active = 2;
  renderbuffer = 9;
  draw_buffer = read_buffer = unpack = 0;
  next_id = 100;
  created = deleted = swaps = 0;
  canvas_done = canvas_failed_width = canvas_failed_height = canvas_gl_ready =
      0;
  bound_target = NULL;
  context_ready = 1;
  missing_proc = 0;
}
int main(void) {
  render_thread = GetCurrentThreadId();
  const char *names[] = {"main", "gui", "gui_utility"};
  for (unsigned i = 0; i < 3; ++i) {
    uint32_t hash = 0;
    for (const char *p = names[i]; *p; ++p)
      hash = hash * 53U + (unsigned char)*p;
    memcpy(nodes[i].bytes + 0x20, &hash, sizeof(hash));
    store(nodes + i, 0x28, targets + i);
  }
  /* Sort node pointers, leaving their registered target identities unchanged.
   */
  Node *sorted[3] = {nodes, nodes + 1, nodes + 2};
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = i + 1; j < 3; ++j)
      if ((unsigned)int_at(sorted[i], 0x20) >
          (unsigned)int_at(sorted[j], 0x20)) {
        Node *n = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = n;
      }
  head.bytes[0x19] = 1;
  store(&head, 8, sorted[1]);
  store(sorted[1], 0, sorted[0]);
  store(sorted[1], 0x10, sorted[2]);
  store(sorted[0], 0, &head);
  store(sorted[0], 0x10, &head);
  store(sorted[2], 0, &head);
  store(sorted[2], 0x10, &head);
  base = 0;
  native_symbols.values[DD_RENDER_REGISTRY] = (uintptr_t)&registry;
  native_symbols.values[DD_RENDER_WINDOW] = (uintptr_t)&game_window;
  native_symbols.values[DD_RENDER_BOUND_TARGET] = (uintptr_t)&bound_target;
  windows_original_swap = swap;
  windows_drawable_size = size;
  windows_gl_context = context;
  windows_gl_proc = proc;
  dd_windows_ready = 1;
  for (fail_at = 0; fail_at <= 3; ++fail_at) {
    reset();
    windows_canvas_frame((void *)2);
    assert(!created);
    context_ready = 0;
    windows_canvas_swap((void *)1);
    assert(!created);
    context_ready = 1;
    bound_target = targets;
    windows_canvas_swap((void *)1);
    assert(!created);
    bound_target = NULL;
    unpack = 42;
    windows_canvas_swap((void *)1);
    assert(!created);
    unpack = 0;
    missing_proc = 1;
    canvas_gl_ready = 0;
    windows_canvas_swap((void *)1);
    assert(!created);
    missing_proc = 2;
    windows_canvas_swap((void *)1);
    assert(!created);
    missing_proc = 0;
    /* Existing canvas precedes the late module's first eligible frame. */
    windows_canvas_swap((void *)1);
    assert(!draw_buffer && !read_buffer && active == 2 && textures[3] == 77);
    if (fail_at) {
      assert(created == fail_at && deleted == 3 * fail_at && !canvas_done);
      assert(targets[0].width == 1920 && textures[0] == 2 && textures[1] == 5 &&
             textures[2] == 8 && renderbuffer == 9);
    } else {
      assert(created == 3 && deleted == 9 && canvas_done);
      for (unsigned i = 0; i < 3; ++i) {
        assert(targets[i].width == 3840 && targets[i].height == 2160);
        assert(windows_canvas_target(names[i]) == targets + i);
        assert(textures[i] == targets[i].texture);
      }
      assert(renderbuffer == targets[2].depth);
    }
    unsigned count = created;
    windows_canvas_swap((void *)1);
    assert(created == count);
  }
  fail_at = 0;
  reset();
  for (unsigned i = 0; i < 3; ++i) {
    targets[i].width = 3840;
    targets[i].height = 2160;
  }
  windows_canvas_swap((void *)1);
  assert(canvas_done && !created && !deleted);
  puts("DELAYED_CANVAS_OK");
  return 0;
}
