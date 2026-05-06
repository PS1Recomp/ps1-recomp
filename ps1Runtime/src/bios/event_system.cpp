#include "runtime/bios/event_system.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <cstdlib>
#include <fmt/core.h>

// Set PS1_BIOS_DEBUG=1 to enable verbose event system logging.
static const bool s_evtVerbose = (std::getenv("PS1_BIOS_DEBUG") != nullptr);
#define EVT_LOG(...) do { if (s_evtVerbose) fmt::print(__VA_ARGS__); } while(0)

// Forward declare recomp_dispatch (defined in recompiled_out.cpp, global
// namespace)
extern void recomp_dispatch(uint8_t *rdram, recomp_context *ctx,
                            uint32_t target_pc);

namespace ps1::bios {

EventSystem::EventSystem(recomp_context &ctx) : ctx_(ctx) {
  // Usually games assume event IDs start from a specific range or we can just
  // use vector index
  events_.reserve(MAX_EVENTS);
}

uint32_t EventSystem::openEvent(uint32_t classId, uint32_t specId,
                                uint32_t mode, uint32_t handler) {
  std::lock_guard<std::mutex> lk(eventsMtx_);
  if (events_.size() >= MAX_EVENTS) {
    EVT_LOG("[BIOS] OpenEvent failed: Max events reached\n");
    return INVALID_EVENT_ID;
  }

  uint32_t eventId = events_.size();

  Event ev;
  ev.classId = classId;
  ev.specId = specId;
  ev.mode = mode;
  ev.handler = handler;
  // Events start disabled per PS1 hardware spec.
  // Triggers that arrive before EnableEvent are stored as pendingTrigger and
  // applied when EnableEvent is called, so late CDROM interrupts aren't lost.
  ev.enabled = false;
  ev.triggered = false;
  ev.pendingTrigger = false;

  events_.push_back(ev);

  EVT_LOG("[BIOS] OpenEvent(class: 0x{:X}, spec: 0x{:X}, mode: 0x{:X}, "
             "handler: 0x{:08X}) -> ID {}\n",
             classId, specId, mode, handler, eventId);

  return eventId;
}

uint32_t EventSystem::closeEvent(uint32_t eventId) {
  std::lock_guard<std::mutex> lk(eventsMtx_);
  if (eventId >= events_.size())
    return 0;

  events_[eventId].enabled = false;
  events_[eventId].pendingTrigger = false;
  events_[eventId].classId = 0xFFFFFFFF; // Mark inactive
  triggeredBits_.fetch_and(~(1u << eventId), std::memory_order_release);

  EVT_LOG("[BIOS] CloseEvent({})\n", eventId);
  return 1;
}

uint32_t EventSystem::waitEvent(uint32_t eventId) {
  if (eventId >= events_.size())
    return 0;

  // Check if the event has been triggered via the atomic bitmask
  uint32_t bit = 1u << eventId;
  uint32_t bits = triggeredBits_.load(std::memory_order_acquire);
  if (bits & bit) {
    // Clear this bit atomically (acknowledge) — same pattern as testEvent
    triggeredBits_.fetch_and(~bit, std::memory_order_release);
    return 1;
  }

  // Not triggered yet — on a real PS1, this would spin-wait.
  // In our HLE, the caller (recompiled code) is in a polling loop
  // that will call drainPendingCallbacks() before re-checking.
  return 0;
}

uint32_t EventSystem::testEvent(uint32_t eventId) {
  if (eventId >= events_.size())
    return 0;

  // Use atomic bitmask for thread-safe check (main thread writes, game thread
  // reads)
  uint32_t bit = 1u << eventId;
  uint32_t bits = triggeredBits_.load(std::memory_order_acquire);
  if (bits & bit) {
    // Clear this bit atomically (acknowledge)
    triggeredBits_.fetch_and(~bit, std::memory_order_release);
    return 1;
  }

  return 0; // Not triggered yet
}

uint32_t EventSystem::enableEvent(uint32_t eventId) {
  std::lock_guard<std::mutex> lk(eventsMtx_);
  if (eventId >= events_.size())
    return 0;
  auto &ev = events_[eventId];
  ev.enabled = true;
  // Apply any trigger that arrived before EnableEvent was called.
  if (ev.pendingTrigger) {
    ev.pendingTrigger = false;
    triggeredBits_.fetch_or(1u << eventId, std::memory_order_release);
    EVT_LOG("[BIOS] EnableEvent({}): applied pending trigger\n", eventId);
  }
  EVT_LOG("[BIOS] EnableEvent({})\n", eventId);
  return 1;
}

uint32_t EventSystem::disableEvent(uint32_t eventId) {
  std::lock_guard<std::mutex> lk(eventsMtx_);
  if (eventId >= events_.size())
    return 0;
  events_[eventId].enabled = false;
  EVT_LOG("[BIOS] DisableEvent({})\n", eventId);
  return 1;
}

void EventSystem::triggerEvent(uint32_t classId, uint32_t specId) {
  std::lock_guard<std::mutex> lk(eventsMtx_);
  bool found = false;
  for (size_t i = 0; i < events_.size(); i++) {
    auto &ev = events_[i];
    if (ev.classId == classId && ev.specId == specId) {
      found = true;
      if (!ev.enabled) {
        // Buffer the trigger; will be applied when EnableEvent is called.
        ev.pendingTrigger = true;
        continue;
      }
      // Set bit in atomic bitmask (thread-safe for main→game thread visibility)
      uint32_t oldBits =
          triggeredBits_.fetch_or(1u << i, std::memory_order_release);
      if (ev.mode == 0x1000 && ev.handler != 0) {
        // Queue for game-thread dispatch instead of calling recomp_dispatch
        // directly (which would be thread-unsafe when called from main thread).
        std::lock_guard<std::mutex> lk2(cbMtx_);
        pendingCallbacks_.push_back({ev.handler, 0, 0, false, false});
      }
    }
  }
  // Debug: log card event triggers
  if (classId == 0xF4000001) {
    static int cardTriggerCount = 0;
    cardTriggerCount++;
    if (cardTriggerCount <= 30) {
      EVT_LOG("[EVTDBG] triggerEvent(0x{:08X}, 0x{:04X}) found={} "
                 "evCount={} bits=0x{:08X}\n",
                 classId, specId, found, events_.size(), triggeredBits_.load());
    }
  }
  // Debug: log VSync callback queueing.  Sample 1-of-30 after first
  // burst so we see post-OpenEvent state (ID 9 registered late in boot).
  if (classId == 0xF2000002 && specId == 0x0002) {
    static int vsyncTriggerCount = 0;
    int n = ++vsyncTriggerCount;
    bool log = (n <= 10) || (n % 30 == 0);
    if (log) {
      std::lock_guard<std::mutex> lk2(cbMtx_);
      EVT_LOG("[VSYNC-DBG] #{} triggerEvent(0x{:08X}, 0x{:04X}) found={} "
                 "pendingCBs={} evCount={}\n",
                 n, classId, specId, found, pendingCallbacks_.size(),
                 events_.size());
    }
  }
}

void EventSystem::tick() {
  // Evaluate if any triggered events need handling
}

void EventSystem::drainPendingCallbacks() {
  // Move callbacks to a local copy under the lock, then release the lock
  // before dispatching (dispatched callbacks may re-enter triggerEvent).
  std::vector<PendingCallback> local;
  {
    std::lock_guard<std::mutex> lk(cbMtx_);
    if (pendingCallbacks_.empty())
      return;
    local = std::move(pendingCallbacks_);
    pendingCallbacks_.clear();
  }

  for (const auto &cb : local) {
    if (cb.handlerPc == 0)
      continue; // Guard: skip null callbacks

    // Save and restore registers around EACH callback individually.
    auto saved = static_cast<ps1::CPUContext>(ctx_);

    if (cb.hasArg) {
      ctx_.r4 = cb.a0;
    }
    if (cb.hasA1) {
      ctx_.r5 = cb.a1;
    }
    static int dispCbCount = 0;
    if (dispCbCount++ < 5)
      fmt::print(stderr, "[EVT] drainPendingCallbacks: dispatching 0x{:08X} a0=0x{:X}\n",
                 cb.handlerPc, cb.hasArg ? cb.a0 : 0);
    recomp_dispatch(ctx_.mem->ramPtr(), &ctx_, cb.handlerPc);

    // Restore all registers that the game was using
    static_cast<ps1::CPUContext &>(ctx_) = saved;
  }
}

} // namespace ps1::bios
