#pragma once
// Lockstep control (`PS1_STEP=<dir>`): the game advances only when a client
// asks, so a slow player (a script, an LLM, a learning agent) sees every frame
// it acts on.  Line protocol, one command per line, hex numbers:
//
//   step <frames> <buttons>   hold the PS1 pad mask for <frames> VBlanks;
//                             answered with "frame <vsync>" once they ran
//   peek <addr>               -> "<value>"
//   poke <addr> <value>       -> "ok"
//   shot <path>               VRAM dump -> "ok" / "err"
//   hash <addr> <len>         fingerprint of guest RAM -> "<hash>"
//   hostpad                   buttons the player holds on the host -> "<mask>"
//   quit                      end the session
//
// "frame <vsync>" is also sent before the first VBlank.  Anything else is
// answered with "err <line>".
//
// Without a clock, a VBlank happens when the game waits for one (VSync) or
// after `spinPolls` polls from code that never waits (a loader spinning on the
// CD-ROM).  Hardware ticks VBlank by CPU time; this keeps a frame's own work
// from costing VBlanks, as if the CPU were infinitely fast.

#include <cstdint>
#include <functional>
#include <istream>
#include <ostream>
#include <string>

namespace ps1 {

class StepControl {
public:
  struct Hooks {
    std::function<uint32_t(uint32_t)> peek;
    std::function<void(uint32_t, uint32_t)> poke;
    std::function<bool(const std::string &)> shot;
    std::function<void(uint16_t)> setButtons;
    std::function<uint64_t(uint32_t, uint32_t)> hash;
    std::function<uint16_t()> hostPad;
  };

  StepControl(std::istream &in, std::ostream &out, Hooks hooks,
              uint32_t spinPolls = 1);

  /// Called at every VBlank opportunity; `waiting` when the game is idle in
  /// VSync.  True when a VBlank happens now.  Blocks on the client when the
  /// current step is used up.
  bool poll(uint32_t vsync, bool waiting);
  /// The session is over (quit or end of input).
  bool ended() const { return ended_; }

  /// A VBlank opportunity while waiting; false once the session has ended.
  bool beforeVBlank(uint32_t vsync) { return poll(vsync, true); }

private:
  std::istream &in_;
  std::ostream &out_;
  Hooks hooks_;
  uint32_t spinPolls_;
  uint32_t busyPolls_ = 0;
  uint32_t remaining_ = 0;
  bool ended_ = false;
};

} // namespace ps1
