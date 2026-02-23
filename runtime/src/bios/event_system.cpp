#include "runtime/bios/event_system.h"
#include <fmt/core.h>

namespace ps1::bios {

EventSystem::EventSystem() {
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
      // Mode 0x1000 or 0x2000 dictates whether it's an interrupt handler call
      // vs a flag
      if (ev.mode == 0x1000 && ev.handler != 0) {
        fmt::print("[BIOS] Would dispatch event handler 0x{:08X}\n",
                   ev.handler);
        // We need to inject a jump to the handler in the cpu context next cycle
      }
    }
  }
}

void EventSystem::tick() {
  // Evaluate if any triggered events need handling
}

} // namespace ps1::bios
