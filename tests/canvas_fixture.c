#include "windows_canvas.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static unsigned made, dropped, fail;
static int create(DdCanvasTarget *out, int width, int height, int alpha) {
  ++made;
  *out = (DdCanvasTarget){100 + made, 200 + made, 300 + made, width, height};
  assert(alpha == (made % 3 != 1));
  return made != fail;
}
static void drop(const DdCanvasTarget *target) {
  assert(target->framebuffer && target->texture && target->depth);
  ++dropped;
}
int main(void) {
  int width, height;
  assert(dd_canvas_fit(3840, 2160, &width, &height));
  assert(width == 3840 && height == 2160);
  assert(dd_canvas_fit(3440, 1440, &width, &height));
  assert(width == 2560 && height == 1440);
  assert(dd_canvas_fit(1920, 1200, &width, &height));
  assert(width == 1920 && height == 1080);
  assert(!dd_canvas_fit(0, 1080, &width, &height));
  assert(dd_canvas_fit(INT_MAX, INT_MAX, &width, &height));
  assert(width == INT_MAX && height > 0);
  for (fail = 1; fail <= 4; ++fail) {
    DdCanvasTarget old[3] = {
        {1, 2, 3, 1920, 1080}, {4, 5, 6, 1920, 1080}, {7, 8, 9, 1920, 1080}};
    DdCanvasTarget original[3];
    memcpy(original, old, sizeof(old));
    DdCanvasTarget *targets[3] = {old, old + 1, old + 2};
    uint32_t texture = 5;
    made = dropped = 0;
    assert(dd_canvas_rebuild(targets, 3840, 2160, create, drop, &texture) ==
           (fail <= 3 ? 0 : 2));
    if (fail <= 3) {
      assert(!memcmp(old, original, sizeof(old)));
      assert(texture == 5 && made == fail && dropped == fail);
    } else {
      assert(made == 3 && dropped == 3 && texture == old[1].texture);
      for (unsigned i = 0; i < 3; ++i)
        assert(targets[i] == old + i && old[i].width == 3840 &&
               old[i].height == 2160);
      assert(dd_canvas_rebuild(targets, 3840, 2160, create, drop, &texture) ==
             1);
      assert(made == 3 && dropped == 3);
    }
    targets[2] = NULL;
    assert(!dd_canvas_rebuild(targets, 3840, 2160, create, drop, &texture));
    targets[2] = old + 2;
    targets[2]->texture = targets[0]->texture;
    assert(!dd_canvas_rebuild(targets, 3840, 2160, create, drop, &texture));
    targets[2] = targets[0];
    assert(!dd_canvas_rebuild(targets, 3840, 2160, create, drop, &texture));
  }
  puts("CANVAS_TRANSACTION_OK");
  return 0;
}
