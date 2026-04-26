#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libapi.h"
#include "runtime/psyq/psyq_libcd.h"
#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/psyq/psyq_libgte.h"

#include <cstdlib>
#include <fmt/format.h>
#include <string>
#include <unordered_map>

namespace {

std::unordered_map<std::string, PsyqHleFn> &registry() {
  static std::unordered_map<std::string, PsyqHleFn> instance;
  return instance;
}

bool g_defaults_initialized = false;

} // namespace

void psyq_register(const char *name, PsyqHleFn fn) {
  registry()[name] = fn;
}

void psyq_dispatch(const char *name, recomp_context *ctx) {
  auto &reg = registry();
  auto it = reg.find(name);
  if (it == reg.end()) {
    fmt::print(stderr,
               "[PSYQ] FATAL: function '{}' marked HLE but no "
               "implementation registered (RA=0x{:08X})\n",
               name, ctx->r[31]);
    std::abort();
  }
  it->second(ctx);
}

void psyq_registry_init_defaults() {
  if (g_defaults_initialized)
    return;
  g_defaults_initialized = true;

  psyq_register("libetc_VSync",       &ps1::psyq::hle_VSync);
  psyq_register("libgpu_DrawSync",    &ps1::psyq::hle_DrawSync);
  psyq_register("libgpu_ResetGraph",  &ps1::psyq::hle_ResetGraph);
  psyq_register("libgpu_ClearOTag",   &ps1::psyq::hle_ClearOTag);
  psyq_register("libgpu_ClearOTagR",  &ps1::psyq::hle_ClearOTagR);
  psyq_register("libgpu_DrawOTag",    &ps1::psyq::hle_DrawOTag);
  psyq_register("libgpu_SetDefDispEnv", &ps1::psyq::hle_SetDefDispEnv);
  psyq_register("libgpu_PutDispEnv",  &ps1::psyq::hle_PutDispEnv);
  psyq_register("libgpu_SetDefDrawEnv", &ps1::psyq::hle_SetDefDrawEnv);
  psyq_register("libgpu_PutDrawEnv",  &ps1::psyq::hle_PutDrawEnv);
}

void psyq_register_rayman_boot() {
  ps1::psyq::psyq_register_libapi();
  ps1::psyq::psyq_register_libcd();
  ps1::psyq::psyq_register_libgte();
  ps1::psyq::psyq_register_libgpu_extras();
}
