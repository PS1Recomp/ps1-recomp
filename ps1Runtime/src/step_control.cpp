#include "runtime/step_control.h"

#include <sstream>

namespace ps1 {

StepControl::StepControl(std::istream &in, std::ostream &out, Hooks hooks,
                         uint32_t spinPolls)
    : in_(in), out_(out), hooks_(std::move(hooks)),
      spinPolls_(spinPolls ? spinPolls : 1) {}

bool StepControl::poll(uint32_t vsync, bool waiting) {
  if (ended_)
    return false;
  if (!waiting && ++busyPolls_ < spinPolls_)
    return false;
  busyPolls_ = 0;
  if (remaining_ > 0) {
    --remaining_;
    return true;
  }
  out_ << "frame " << vsync << '\n' << std::flush;

  std::string line;
  while (std::getline(in_, line)) {
    std::istringstream cmd(line);
    std::string op;
    cmd >> op >> std::hex;
    uint32_t a = 0, b = 0;
    if (op == "step" && cmd >> a >> b && a > 0) {
      hooks_.setButtons(static_cast<uint16_t>(b));
      remaining_ = a - 1; // this VBlank is the first of them
      return true;
    }
    if (op == "peek" && cmd >> a) {
      out_ << std::hex << hooks_.peek(a) << std::dec << '\n' << std::flush;
    } else if (op == "hash" && cmd >> a >> b) {
      out_ << std::hex << hooks_.hash(a, b) << std::dec << '\n' << std::flush;
    } else if (op == "hostpad") {
      out_ << std::hex << hooks_.hostPad() << std::dec << '\n' << std::flush;
    } else if (op == "poke" && cmd >> a >> b) {
      hooks_.poke(a, b);
      out_ << "ok\n" << std::flush;
    } else if (op == "shot" && cmd >> std::ws && std::getline(cmd, line)) {
      out_ << (hooks_.shot(line) ? "ok\n" : "err\n") << std::flush;
    } else if (op == "quit") {
      break;
    } else {
      out_ << "err " << line << '\n' << std::flush;
    }
  }
  // Quit or the client went away: let go of the pad and stop the game.
  hooks_.setButtons(0);
  ended_ = true;
  return false;
}

} // namespace ps1
