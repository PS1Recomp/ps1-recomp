#include "runtime/bios/event_system.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include <fmt/core.h>

// Forward declare recomp_dispatch (defined in recompiled_out.cpp, global namespace)
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
  if (events_.size() >= MAX_EVENTS) {
    fmt::print("[BIOS] OpenEvent failed: Max events reached\n");
    return INVALID_EVENT_ID;
  }

  uint32_t eventId = events_.size();

  Event ev;
  ev.classId = classId;
  ev.specId = specId;
  ev.mode = mode;
  ev.handler = handler;
  ev.enabled = false;
  ev.triggered = false;

  events_.push_back(ev);

  fmt::print("[BIOS] OpenEvent(class: 0x{:X}, spec: 0x{:X}, mode: 0x{:X}, "
             "handler: 0x{:08X}) -> ID {}\n",
             classId, specId, mode, handler, eventId);

  return eventId;
}

uint32_t EventSystem::closeEvent(uint32_t eventId) {
  if (eventId >= events_.size())
    return 0;

  // In PS1 BIOS, closing does not necessarily deallocate the slot dynamically
  // like modern vectors, it usually just marks it free. For our stub, marking
  // it disabled/triggered=false is a start.
  events_[eventId].enabled = false;
  events_[eventId].classId = 0xFFFFFFFF; // Mark inactive

  fmt::print("[BIOS] CloseEvent({})\n", eventId);
  return 1;
}

uint32_t EventSystem::waitEvent(uint32_t eventId) {
  if (eventId >= events_.size())
    return 0;

  fmt::print("[BIOS] WaitEvent({}) [STUB]\n", eventId);

  // Proper emulation requires yielding the recompiled thread and waiting for an
  // interrupt to trigger this event. In a pure recompiler without threading,
  // this requires returning to a scheduling loop or fast-forwarding time.
  events_[eventId].triggered = false; // Acknowledge wait

  return 1;
}

uint32_t EventSystem::testEvent(uint32_t eventId) {
  if (eventId >= events_.size())
    return 0;

  bool isTriggered = events_[eventId].triggered;
  if (isTriggered) {
    events_[eventId].triggered = false; // Acknowledge
    fmt::print(
        "[BIOS] testEvent({}) -> TRIGGERED (class=0x{:X}, spec=0x{:X})\n",
        eventId, events_[eventId].classId, events_[eventId].specId);
    return 1;
  }

  return 0; // Not triggered yet
}

uint32_t EventSystem::enableEvent(uint32_t eventId) {
  if (eventId >= events_.size())
    return 0;
  events_[eventId].enabled = true;
  fmt::print("[BIOS] EnableEvent({})\n", eventId);
  return 1;
}

uint32_t EventSystem::disableEvent(uint32_t eventId) {
  if (eventId >= events_.size())
    return 0;
  events_[eventId].enabled = false;
  fmt::print("[BIOS] DisableEvent({})\n", eventId);
  return 1;
}

void EventSystem::triggerEvent(uint32_t classId, uint32_t specId) {
  for (auto &ev : events_) {
    if (ev.classId == classId && ev.specId == specId && ev.enabled) {
      ev.triggered = true;
      if (ev.mode == 0x1000 && ev.handler != 0) {
        // Queue for game-thread dispatch instead of calling recomp_dispatch
        // directly (which would be thread-unsafe when called from main thread).
        std::lock_guard<std::mutex> lk(cbMtx_);
        pendingCallbacks_.push_back({ev.handler, 0, 0, false, false});
      }
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

  // Save volatile registers around callback dispatch so we don't corrupt
  // the game thread's register state.
  auto saved = static_cast<ps1::CPUContext>(ctx_);

  for (const auto &cb : local) {
    // Set up register arguments before dispatch if needed
    if (cb.hasArg) {
      ctx_.r4 = cb.a0;
    }
    if (cb.hasA1) {
      ctx_.r5 = cb.a1;
    }
    // Debug: log CD data callback with remaining value
    if (cb.handlerPc == 0x80045C04) {
      static int cdDispatchCount = 0;
      cdDispatchCount++;
      int32_t remaining = (int32_t)ctx_.mem->read32(0x80055898);
      if (cdDispatchCount <= 20) {
        fmt::print(stderr, "[CD-DISPATCH] #{} a0={} remaining={} sectorRdy=?\n",
                   cdDispatchCount, cb.a0, remaining);
      }
    }
    fmt::print("[BIOS] Dispatching event callback 0x{:08X}\n", cb.handlerPc);
    recomp_dispatch(ctx_.mem->ramPtr(), &ctx_, cb.handlerPc);
  }

  // Restore all registers that the game was using
  static_cast<ps1::CPUContext &>(ctx_) = saved;
}

} // namespace ps1::bios
