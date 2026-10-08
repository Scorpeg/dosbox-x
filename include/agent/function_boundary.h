#ifndef DOSBOX_AGENT_FUNCTION_BOUNDARY_H
#define DOSBOX_AGENT_FUNCTION_BOUNDARY_H
#include <cstdint>
#include <string>

namespace dosbox_agent {
// Capture at entry and matched return, not every instruction.
class FunctionBoundary {
public:
    enum Event { None, Entry, Return };
    bool used = false, armed = false, open = false, complete = false, tick_seen = false;
    unsigned wanted = 0, completed = 0;
    std::uint64_t limit = 0, instructions = 0, first_tick = 0, end_tick = 0, last_tick = 0;
    std::string failure;
    bool Arm(unsigned count, std::uint64_t maximum, std::uint64_t first, std::uint64_t end) {
        if (used || !count || count > 8 || !maximum || maximum > 10000000 || first >= end || end > 1000000000)
            return false;
        used = armed = true; wanted = count; limit = maximum; first_tick = first; end_tick = end;
        return true;
    }
    void Fail(const std::string& reason) {
        if (!used) return;
        if (failure.empty()) failure = reason;
        armed = complete = false;
    }
    bool CheckTick(std::uint64_t tick) {
        if (!armed) return false;
        if (tick_seen && tick < last_tick) { Fail("function capture clock reversed"); return false; }
        tick_seen = true; last_tick = tick;
        if (tick >= end_tick) {
            Fail(open ? "function interval ended before return" : "function interval ended before required calls");
            return false;
        }
        return tick >= first_tick;
    }
    Event Observe(bool entry, bool returned, std::uint64_t tick) {
        if (!CheckTick(tick)) return None;
        if (!open) {
            if (!entry) return None;
            open = true; instructions = 0;
            return Entry;
        }
        if (++instructions > limit) { Fail("function instruction limit exceeded"); return None; }
        if (!returned) return None;
        open = false;
        if (++completed == wanted) { armed = false; complete = true; }
        return Return;
    }
};
}
#endif
