#ifndef DD_ADDRESS_PROFILE_H
#define DD_ADDRESS_PROFILE_H
#ifdef DD_ADDRESS_TEST_PROFILE
#include DD_ADDRESS_TEST_PROFILE
#elif defined(_WIN32)
#include "../windows/windows_signatures.h"
#else
#include "../linux/linux_signatures.h"
#endif
/* Resolve a complete feature group before permitting any of its writes. */
static inline int dd_resolve_profile(const DdRange *ranges, size_t count,
                                     unsigned groups, DdResolution *out,
                                     const DdSignature **failed, int *matches) {
  DdResolution staged = *out;
  for (size_t i = 0; i < sizeof(dd_signatures) / sizeof(*dd_signatures); ++i) {
    const DdSignature *signature = dd_signatures + i;
    if (!(signature->group & groups))
      continue;
    uintptr_t address;
    int result = dd_scan(ranges, count, signature, &address);
    if (result != 1) {
      *failed = signature;
      *matches = result;
      return 0;
    }
    staged.values[signature->symbol] = address;
    staged.present[signature->symbol] = 1;
  }
  *out = staged;
  return 1;
}
#endif
