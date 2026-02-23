#pragma once

#include <cstdint>
#include <vector>

namespace ps1::bios {

struct Event {
  uint32_t classId;
  uint32_t specId;
  uint32_t mode;
  uint32_t handler;
  bool enabled;
  bool triggered;
};

class EventSystem {
public:
  EventSystem();
  ~EventSystem() = default;

  // BIOS Event Functions
  uint32_t openEvent(uint32_t classId, uint32_t specId, uint32_t mode,
                     uint32_t handler);
  uint32_t closeEvent(uint32_t eventId);
  uint32_t waitEvent(uint32_t eventId);
  uint32_t testEvent(uint32_t eventId);
  uint32_t enableEvent(uint32_t eventId);
  uint32_t disableEvent(uint32_t eventId);

  // Internal Emulator Functions (e.g. called by simulated hardware)
  void triggerEvent(uint32_t classId, uint32_t specId);
  void tick(); // Evaluate events

private:
  std::vector<Event> events_;

  static constexpr uint32_t MAX_EVENTS = 32;
  static constexpr uint32_t INVALID_EVENT_ID = 0xFFFFFFFF;
};

} // namespace ps1::bios
