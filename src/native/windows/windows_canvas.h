#ifndef DD_WINDOWS_CANVAS_H
#define DD_WINDOWS_CANVAS_H
#include <stdint.h>
#include <string.h>

/* Verified native target layout: FBO, colour texture, depth/stencil, width,
 * height. Replacing its contents preserves every native pointer to the target.
 */
typedef struct {
  uint32_t framebuffer, texture, depth;
  int32_t width, height;
} DdCanvasTarget;
_Static_assert(sizeof(DdCanvasTarget) == 20, "native render target layout");
typedef int (*DdCanvasCreate)(DdCanvasTarget *, int, int, int);
typedef void (*DdCanvasDrop)(const DdCanvasTarget *);

enum { DD_CANVAS_TARGETS = 3 };
static inline int dd_canvas_fit(int width, int height, int *out_width,
                                int *out_height) {
  if (width <= 0 || height <= 0)
    return 0;
  if ((int64_t)width * 9 <= (int64_t)height * 16) {
    *out_width = width;
    *out_height = (int)((int64_t)width * 9 / 16);
  } else {
    *out_width = (int)((int64_t)height * 16 / 9);
    *out_height = height;
  }
  return *out_width > 0 && *out_height > 0;
}
/* 0 = not ready/failed, 1 = already current, 2 = committed. Stage all GPU
 * allocations first; neither live targets nor dimensions change on failure. */
static inline int dd_canvas_rebuild(DdCanvasTarget *const targets[3], int width,
                                    int height, DdCanvasCreate create,
                                    DdCanvasDrop drop, uint32_t *texture) {
  if (width <= 0 || height <= 0)
    return 0;
  int changed = 0;
  for (unsigned i = 0; i < DD_CANVAS_TARGETS; ++i) {
    if (!targets[i] || !targets[i]->framebuffer || !targets[i]->texture ||
        !targets[i]->depth || targets[i]->width <= 0 || targets[i]->height <= 0)
      return 0;
    for (unsigned j = 0; j < i; ++j)
      if (targets[i] == targets[j] ||
          targets[i]->framebuffer == targets[j]->framebuffer ||
          targets[i]->texture == targets[j]->texture ||
          targets[i]->depth == targets[j]->depth)
        return 0;
    changed |= targets[i]->width != width || targets[i]->height != height;
  }
  if (!changed)
    return 1;
  DdCanvasTarget staged[DD_CANVAS_TARGETS] = {0};
  for (unsigned i = 0; i < DD_CANVAS_TARGETS; ++i)
    if (!create(staged + i, width, height, i != 0)) {
      for (unsigned j = 0; j <= i; ++j)
        drop(staged + j);
      return 0;
    }
  for (unsigned i = 0; i < DD_CANVAS_TARGETS; ++i) {
    DdCanvasTarget old = *targets[i];
    *targets[i] = staged[i];
    if (*texture == old.texture)
      *texture = staged[i].texture;
    drop(&old);
  }
  return 2;
}
#endif
