#ifndef DD_ADDRESS_SCAN_H
#define DD_ADDRESS_SCAN_H
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
  DD_CODE = 1,
  DD_WRITE = 2,
  DD_READ = 4,
  DD_FIELD = 8,
  DD_PATTERN_LIMIT = 128
};
enum { DD_GROUP_RESOURCE = 1, DD_GROUP_RENDER = 2, DD_GROUP_RIG = 4 };
typedef struct {
  const unsigned char *bytes;
  size_t size;
  uintptr_t address;
  size_t memory_size;
  unsigned flags;
} DdRange;
typedef struct {
  const char *name, *pattern;
  unsigned symbol, group, capture, kind;
  int adjustment;
  unsigned target_flags;
} DdSignature;
static inline int dd_hex(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}
static inline size_t dd_pattern(const char *text, unsigned char *bytes,
                                unsigned char *mask) {
  size_t count = 0, fixed = 0;
  while (*text) {
    if (count == DD_PATTERN_LIMIT || !text[1])
      return 0;
    if (text[0] == '?' && text[1] == '?') {
      bytes[count] = mask[count] = 0;
    } else {
      int high = dd_hex((unsigned char)text[0]);
      int low = dd_hex((unsigned char)text[1]);
      if (high < 0 || low < 0)
        return 0;
      bytes[count] = (unsigned char)((high << 4) | low);
      mask[count] = 0xff;
      ++fixed;
    }
    ++count;
    text += 2;
    if (*text && *text++ != ' ')
      return 0;
  }
  return fixed >= 12 ? count : 0;
}
static inline int dd_contains(const DdRange *ranges, size_t count,
                              uintptr_t address, size_t length,
                              unsigned flags) {
  for (size_t i = 0; i < count; ++i) {
    const DdRange *range = ranges + i;
    if ((range->flags & flags) == flags && address >= range->address &&
        address - range->address <= range->memory_size &&
        length <= range->memory_size - (address - range->address))
      return 1;
  }
  return 0;
}
static inline int dd_add_signed(uintptr_t address, int64_t delta,
                                uintptr_t *result) {
  if (delta >= 0) {
    if ((uint64_t)delta > UINTPTR_MAX - address)
      return 0;
    *result = address + (uintptr_t)delta;
  } else {
    uint64_t magnitude = (uint64_t)(-(delta + 1)) + 1;
    if (magnitude > address)
      return 0;
    *result = address - (uintptr_t)magnitude;
  }
  return 1;
}
/* Returns 0 for no match, 1 for a unique validated target, 2 for ambiguity,
 * and -1 for a malformed profile, invalid target or unsupported field accessor.
 * The historical address is never a fallback or a preferred candidate. */
static inline int dd_scan(const DdRange *ranges, size_t count,
                          const DdSignature *signature, uintptr_t *result) {
  unsigned char bytes[DD_PATTERN_LIMIT], mask[DD_PATTERN_LIMIT];
  size_t length = dd_pattern(signature->pattern, bytes, mask), anchor = 0;
  if (!length)
    return -1;
  while (anchor < length && !mask[anchor])
    ++anchor;
  const unsigned char *matched = NULL;
  uintptr_t address = 0;
  for (size_t r = 0; r < count; ++r) {
    const DdRange *range = ranges + r;
    if (!(range->flags & DD_CODE) || range->size < length)
      continue;
    size_t position = 0;
    while (position <= range->size - length) {
      const unsigned char *hit =
          memchr(range->bytes + position + anchor, bytes[anchor],
                 range->size - length - position + 1);
      if (!hit)
        break;
      position = (size_t)(hit - range->bytes) - anchor;
      size_t i = 0;
      while (i < length && (!mask[i] || range->bytes[position + i] == bytes[i]))
        ++i;
      if (i == length) {
        if (matched)
          return 2;
        if (position > UINTPTR_MAX - range->address)
          return -1;
        matched = range->bytes + position;
        address = range->address + position;
      }
      ++position;
    }
  }
  if (!matched)
    return 0;
  uintptr_t target = address;
  size_t width = signature->kind == 3 ? 8 : 4;
  if (signature->kind) {
    if (signature->kind > 5 || signature->capture > length ||
        width > length - signature->capture)
      return -1;
    if (signature->kind == 1 || signature->kind == 5) {
      int32_t displacement;
      memcpy(&displacement, matched + signature->capture, 4);
      if (!dd_add_signed(
              address, (int64_t)signature->capture + 4 + displacement, &target))
        return -1;
    } else if (signature->kind == 2 || signature->kind == 4) {
      uint32_t value;
      memcpy(&value, matched + signature->capture, 4);
      target = value;
    } else {
      uint64_t value;
      memcpy(&value, matched + signature->capture, 8);
      if (value > UINTPTR_MAX)
        return -1;
      target = (uintptr_t)value;
    }
  }
  if (!dd_add_signed(target, signature->adjustment, &target))
    return -1;
  if (signature->kind == 5) {
    /* SysV Actor::SetSprite: follow its call, verify the native pointer-store
     * accessor, then extract disp32. No object field offset is assumed. */
    const unsigned char *setter = NULL;
    for (size_t r = 0; r < count; ++r) {
      const DdRange *range = ranges + r;
      if ((range->flags & DD_CODE) && target >= range->address &&
          target - range->address <= range->size &&
          8 <= range->size - (target - range->address))
        setter = range->bytes + (target - range->address);
    }
    if (!setter || memcmp(setter, "\x48\x89\xb7", 3) || setter[7] != 0xc3)
      return -1;
    uint32_t field;
    memcpy(&field, setter + 3, sizeof(field));
    target = field;
  }
  if (signature->target_flags == DD_FIELD) {
    if ((signature->kind != 4 && signature->kind != 5) || target < 8 ||
        target > 0x10000 || target % 8)
      return -1;
  } else if (!dd_contains(ranges, count, target,
                          signature->target_flags == DD_CODE ? 1 : 8,
                          signature->target_flags))
    return -1;
  *result = target;
  return 1;
}
static inline int dd_guard(const unsigned char *actual,
                           const unsigned char *expected, size_t length,
                           uintptr_t address, uintptr_t callee) {
  if (length == 5 && expected[0] == 0xe8) {
    int32_t displacement;
    uintptr_t target;
    memcpy(&displacement, actual + 1, 4);
    return actual[0] == 0xe8 &&
           dd_add_signed(address, (int64_t)5 + displacement, &target) &&
           target == callee;
  }
  return !memcmp(actual, expected, length);
}
#endif
