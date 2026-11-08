#pragma once

// ps1Runtime -- Host input bindings
// Keyboard and SDL GameController events to PS1 pad buttons.  The SDL event
// loop in main_host.cpp only forwards events here, so the mapping is testable
// without a window.

#include <SDL2/SDL_gamecontroller.h>
#include <SDL2/SDL_keycode.h>
#include <cstdint>
#include <span>

#include <runtime/input/input.h>

namespace ps1::input {

struct KeyBinding {
  SDL_Keycode key;
  uint16_t button;
  const char *keyName;
  const char *padName;
};

/// Keyboard layout.  One table, so the startup banner and the event handler
/// cannot drift apart.
std::span<const KeyBinding> keyBindings();

/// Press or release the pad button bound to `key`; returns its binding, or
/// nullptr for an unbound key (left untouched).
const KeyBinding *applyKey(SDL_Keycode key, bool pressed,
                           InputController &input, HeldButtons &held);

/// Pad button for an SDL GameController button, by position (A = Cross);
/// 0 for buttons the PS1 pad lacks.
uint16_t controllerButton(uint8_t sdlButton);

/// The left stick drives the d-pad and the triggers act as L2/R2, since Crash
/// reads a digital pad.  Other axes are ignored.
void applyControllerAxis(uint8_t sdlAxis, int16_t value, InputController &input,
                         HeldButtons &held);

} // namespace ps1::input
