// Stubs for symbols defined in recompiled_out.cpp that the runtime library
// references but are not available during unit testing.

#include <cstdint>
#include <runtime/cpu_context.h>

// Stub recomp_dispatch -- called by EventSystem and Bios drainPendingCallbacks.
// In tests we simply do nothing.
void recomp_dispatch(uint8_t * /*rdram*/, recomp_context * /*ctx*/,
                     uint32_t /*addr*/) {
  // no-op in test builds
}
