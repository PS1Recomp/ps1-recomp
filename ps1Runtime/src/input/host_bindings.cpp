#include <runtime/input/host_bindings.h>

#include <fmt/format.h>

namespace ps1::input {

namespace {

const KeyBinding kPadBindings[] = {
    {SDLK_UP, BTN_UP, "Up", "D-Pad Up"},
    {SDLK_DOWN, BTN_DOWN, "Down", "D-Pad Down"},
    {SDLK_LEFT, BTN_LEFT, "Left", "D-Pad Left"},
    {SDLK_RIGHT, BTN_RIGHT, "Right", "D-Pad Right"},
    {SDLK_z, BTN_CROSS, "Z", "Cross"},
    {SDLK_x, BTN_CIRCLE, "X", "Circle"},
    {SDLK_a, BTN_SQUARE, "A", "Square"},
    {SDLK_s, BTN_TRIANGLE, "S", "Triangle"},
    {SDLK_q, BTN_L1, "Q", "L1"},
    {SDLK_w, BTN_R1, "W", "R1"},
    {SDLK_e, BTN_L2, "E", "L2"},
    {SDLK_r, BTN_R2, "R", "R2"},
    {SDLK_c, BTN_L3, "C", "L3"},
    {SDLK_v, BTN_R3, "V", "R3"},
    {SDLK_RETURN, BTN_START, "Enter", "Start"},
    {SDLK_RSHIFT, BTN_SELECT, "Right Shift", "Select"},
    {SDLK_BACKSPACE, BTN_SELECT, "Backspace", "Select"},
};

} // namespace

std::span<const KeyBinding> keyBindings() { return kPadBindings; }

const KeyBinding *applyKey(SDL_Keycode key, bool pressed,
                           InputController &input, HeldButtons &held) {
  for (const auto &b : kPadBindings) {
    if (b.key != key)
      continue;
    // Printed on every press: a button that does nothing in the game is a
    // different problem from a button that never reached the pad at all.
    if (pressed)
      fmt::print(stderr, "[pad] {} -> {}\n", b.keyName, b.padName);
    held.set(HeldButtons::Keyboard, b.button, pressed, input);
    return &b;
  }
  return nullptr;
}

uint16_t controllerButton(uint8_t sdlButton) {
  switch (sdlButton) {
  case SDL_CONTROLLER_BUTTON_A: return BTN_CROSS;
  case SDL_CONTROLLER_BUTTON_B: return BTN_CIRCLE;
  case SDL_CONTROLLER_BUTTON_X: return BTN_SQUARE;
  case SDL_CONTROLLER_BUTTON_Y: return BTN_TRIANGLE;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return BTN_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return BTN_R1;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK: return BTN_L3;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return BTN_R3;
  case SDL_CONTROLLER_BUTTON_BACK: return BTN_SELECT;
  case SDL_CONTROLLER_BUTTON_START: return BTN_START;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return BTN_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return BTN_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return BTN_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return BTN_RIGHT;
  }
  return 0;
}

void applyControllerAxis(uint8_t sdlAxis, int16_t value, InputController &input,
                         HeldButtons &held) {
  constexpr int16_t kStick = 12000, kTrigger = 8000;
  uint16_t mask = 0, want = 0;
  switch (sdlAxis) {
  case SDL_CONTROLLER_AXIS_LEFTX:
    mask = BTN_LEFT | BTN_RIGHT;
    want = value < -kStick ? BTN_LEFT : value > kStick ? BTN_RIGHT : 0;
    break;
  case SDL_CONTROLLER_AXIS_LEFTY:
    mask = BTN_UP | BTN_DOWN;
    want = value < -kStick ? BTN_UP : value > kStick ? BTN_DOWN : 0;
    break;
  case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
    mask = BTN_L2;
    want = value > kTrigger ? BTN_L2 : 0;
    break;
  case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
    mask = BTN_R2;
    want = value > kTrigger ? BTN_R2 : 0;
    break;
  default:
    return;
  }
  held.set(HeldButtons::Stick, want, true, input);
  held.set(HeldButtons::Stick, static_cast<uint16_t>(mask & ~want), false, input);
}

} // namespace ps1::input
