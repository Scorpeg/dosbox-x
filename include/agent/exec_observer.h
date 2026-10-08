#ifndef DOSBOX_AGENT_EXEC_OBSERVER_H
#define DOSBOX_AGENT_EXEC_OBSERVER_H
#include "agent/agent_protocol.h"
#include <deque>
#include <utility>

namespace dosbox_agent {

// Loader-safe actions must not execute guest code, touch devices/memory or replace the timeline.
inline bool ExecDrainSafeAction(const std::string& method, const std::string& action) {
    if (method == "exec.control") return action == "status" || action == "read" || action == "ack";
    return method == "replay.control" && (action == "status" || action == "read" || action == "ack" ||
           action == "append" || action == "key" || action == "boundary");
}

// Eviction must preserve global sequence numbers.
class ExecEventWindow {
public:
    static constexpr std::size_t Limit = 256;
    std::uint64_t base = 0, count = 0;
    std::string failure;
    std::deque<JsonValue> rows;
    void Fail(const std::string& reason) { if (failure.empty()) failure = reason; }
    bool Push(JsonValue value) {
        if (!failure.empty()) return false;
        if (rows.size() == Limit || count == 1000000000) {
            Fail("DOS EXEC observation window overflow");
            return false;
        }
        value.object["order"] = JsonValue::Number(std::to_string(count++));
        rows.push_back(std::move(value));
        return true;
    }
    bool Ack(std::uint64_t through) {
        if (through > count) return false;
        while (base < through) { rows.pop_front(); ++base; }
        return true;
    }
    bool Read(std::uint64_t cursor, std::uint64_t limit, JsonValue* output) const {
        if (cursor < base || cursor > count || !limit || limit > 32) return false;
        *output = JsonValue::Array();
        for (auto order = cursor; order < count && order - cursor < limit; ++order)
            output->array.push_back(rows[static_cast<std::size_t>(order - base)]);
        return true;
    }
};

std::string AGENT_Sha256Hex(const std::vector<std::uint8_t>& data);
void AGENT_ExecReset();
void AGENT_ExecEntry(std::uint16_t segment, std::uint32_t offset);
void AGENT_ExecExit(std::uint16_t psp, std::uint16_t parent, bool resident, std::uint8_t code);
bool AGENT_ExecCommand(const JsonValue& params, JsonValue* result, std::string* error);

class ExecObservation {
public:
    ExecObservation(const char* name, std::uint16_t mode, std::uint32_t parameter);
    ~ExecObservation();
    void Mode(std::uint16_t effective);
    void Opened(std::uint16_t handle);
    void Header(const std::uint8_t* bytes, std::uint16_t length, bool com);
    void Image(std::uint16_t load, std::uint16_t psp, std::uint16_t environment,
               std::uint32_t header_bytes, std::uint16_t relocations, std::uint16_t relocation_segment);
    void Payload(const std::uint8_t* bytes, std::uint16_t length, bool success);
    void Relocation(const std::uint8_t* bytes, std::uint16_t length, bool success);
    void Success(std::uint32_t entry, bool has_entry);
private:
    bool active = false, success = false, has_entry = false;
    std::uint64_t id = 0;
    std::uint16_t mode = 0;
    std::uint16_t parent_psp = 0, child_psp = 0;
    std::uint32_t entry = 0;
    JsonValue fields;
    std::vector<std::uint8_t> payload, relocations;
};

// Journal child exits; stop only after the selected process finishes DOS_Terminate cleanup.
class ExecExitObservation {
public:
    ExecExitObservation(std::uint16_t psp, std::uint16_t parent, bool resident, std::uint8_t code);
    ~ExecExitObservation();
private:
    std::uint16_t psp, parent;
    bool resident, selected;
    std::uint8_t code;
};
}
#endif
