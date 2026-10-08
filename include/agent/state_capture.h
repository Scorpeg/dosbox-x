#ifndef DOSBOX_AGENT_STATE_CAPTURE_H
#define DOSBOX_AGENT_STATE_CAPTURE_H
#include "agent/agent_protocol.h"
#include <functional>
#include <vector>
namespace dosbox_agent {
// Observe before instructions: N executed instructions require N+1 observations.
class InstructionWindow {
public:
    static bool DecoderCompatible(bool normal, bool halted, bool resumes_normal) {
        // Accept the parked HLT decoder, but not stale saved decoders.
        return normal || (halted && resumes_normal);
    }
    unsigned entry_cs = 0, exit_cs = 0, limit = 0, wanted = 0, completed = 0;
    std::uint32_t entry_ip = 0, exit_ip = 0;
    bool armed = false, open = false, complete = false;
    bool timed = false, tick_seen = false;
    std::uint64_t first_tick = 0, end_tick = 0, last_tick = 0;
    unsigned access_limit = 0;
    std::vector<unsigned> pending_accesses;
    std::string failure;
    std::vector<JsonValue> cases;
    bool Arm(unsigned cs, std::uint32_t ip, unsigned end_cs, std::uint32_t end_ip,
             unsigned instructions, unsigned count, unsigned accesses = 0,
             bool interval = false, std::uint64_t first = 0, std::uint64_t end = 0) {
        if (armed || !cases.empty() || !failure.empty() || cs > 65535 || end_cs > 65535 ||
            (cs == end_cs && ip == end_ip) || !instructions || instructions > 64 || !count || count > 8 || accesses > 256 ||
            (interval ? (first >= end || end > 1000000000) : (first || end))) return false;
        entry_cs = cs; entry_ip = ip; exit_cs = end_cs; exit_ip = end_ip;
        limit = instructions; wanted = count; armed = true;
        access_limit = accesses;
        timed = interval; first_tick = first; end_tick = end;
        return true;
    }
    void Fail(const std::string& why) { if (failure.empty()) failure = why; armed = false; }
    // Inclusive start, exclusive end; check during reads and HLT to reject overdue partial cases.
    bool CheckTick(std::uint64_t tick) {
        if (!armed) return false;
        if (!timed) return true;
        if (tick_seen && tick < last_tick) { Fail("instruction window clock reversed"); return false; }
        tick_seen = true; last_tick = tick;
        if (tick >= end_tick) {
            Fail(open ? "instruction window interval ended with an open case" :
                        "instruction window interval ended before required cases completed");
            return false;
        }
        return tick >= first_tick;
    }
    unsigned BeginAccess(JsonValue event) {
        if (!armed || !open || !access_limit) return 0;
        auto& events = cases.back().object["accesses"].array;
        if (events.size() >= access_limit) { Fail("memory access limit exceeded"); return 0; }
        const unsigned id = static_cast<unsigned>(events.size());
        event.object["id"] = JsonValue::Number(std::to_string(id));
        event.object["parent"] = pending_accesses.empty() ? JsonValue::Null() :
                                JsonValue::Number(std::to_string(pending_accesses.back() - 1));
        events.push_back(event);
        pending_accesses.push_back(id + 1);
        return id + 1;
    }
    void EndAccess(unsigned item, const JsonValue& completion) {
        if (!item || !failure.empty()) return;
        if (pending_accesses.empty() || pending_accesses.back() != item) {
            Fail("memory access completion order differs"); return;
        }
        auto& event = cases.back().object["accesses"].array.at(item - 1);
        for (const auto& field : completion.object) event.object[field.first] = field.second;
        pending_accesses.pop_back();
    }
    void Observe(unsigned cs, std::uint32_t ip, const std::function<JsonValue(bool)>& snapshot, std::uint64_t tick = 0) {
        if (!CheckTick(tick) || complete || !failure.empty()) return;
        bool first = false;
        if (!open) {
            if (cs != entry_cs || ip != entry_ip) return;
            cases.push_back(JsonValue::Object());
            cases.back().object["complete"] = JsonValue::Bool(false);
            cases.back().object["frames"] = JsonValue::Array();
            if (access_limit) cases.back().object["accesses"] = JsonValue::Array();
            open = first = true;
        }
        const bool last = !first && cs == exit_cs && ip == exit_ip;
        auto& frames = cases.back().object["frames"].array;
        frames.push_back(snapshot(first || last));
        if (!failure.empty()) return;
        if (last) {
            if (!pending_accesses.empty()) { Fail("instruction window exited inside a pending memory request"); return; }
            cases.back().object["complete"] = JsonValue::Bool(true);
            open = false;
            if (++completed == wanted) { complete = true; armed = false; }
        } else if (frames.size() > limit) {
            Fail("instruction window reached its limit without the declared exit");
        }
    }
};

extern bool AGENT_InstructionWindowEnabled;
extern bool AGENT_ModuleEventsEnabled;
void AGENT_ModuleEventsObserve();
void AGENT_InstructionWindowObserve();
void AGENT_InstructionWindowReset();
void AGENT_InstructionWindowInvalidate(const char* reason);
// Read stored fields/backing only: no translation, FillFlags, device calls, pointers or padding.
JsonValue AGENT_CpuTaskStorage();
bool AGENT_MouseStorage(JsonValue* result, std::string* error);
bool AGENT_CaptureState(const JsonValue& params, JsonValue* result, std::string* error);
// Check metadata without handlers, page walks or cache fills.
bool AGENT_IsOrdinaryRam(std::uint64_t address, std::uint64_t bytes);
// Module observation admits conventional RAM, not VRAM/ROM; substitution limits stay unchanged.
bool AGENT_IsModuleRam(std::uint64_t address, std::uint64_t bytes);
}
#endif
