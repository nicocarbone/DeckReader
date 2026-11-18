// Scripted input for headless tests and screenshots (tools/headless-run.sh):
//
//   deckreader --script "wait:500; key:right; idle; shot:/tmp/page.png; quit"
//
// Commands, separated by ";":
//   key:<action>    an action as if pressed (up, down, left, right, confirm,
//                   back, x, y, menu, view, l1, r1, l2, r2)
//   stick:<x>       hold the left stick at x (-1..1; 0 releases)
//   tap:<fx>,<fy>   a tap at a fraction of the screen (0..1, 0..1)
//   wait:<ms>       pause
//   idle            wait until background work (covers, pre-rendering) is done
//   shot:<path>     save the next frame as PNG
//   quit            leave the app (state is saved as on a normal exit)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dr {

struct ScriptCommand {
    std::string name, arg;
};

class TestScript {
public:
    explicit TestScript(const std::string& text);
    bool done() const { return pos_ >= cmds_.size(); }
    const ScriptCommand* current() const { return done() ? nullptr : &cmds_[pos_]; }
    void advance() { pos_++; }
    // When the current command may run (waits push this forward).
    uint64_t ready_at = 0;
    uint64_t idle_since = 0;

private:
    std::vector<ScriptCommand> cmds_;
    size_t pos_ = 0;
};

} // namespace dr
