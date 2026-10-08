#include "config.h"
#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "dosbox.h"
#include "agent/exec_observer.h"
#include "agent/agent_bridge.h"
#include "agent/replay_timeline.h"
#include "dos_inc.h"
#include "dos_system.h"
#include "pic.h"
#include "cpu.h"
#include <chrono>
#include <exception>
#include <thread>

namespace dosbox_agent {
namespace {
ExecEventWindow window;
bool armed = false, entered = false;
bool backpressure = false, drain_held = false;
std::uint64_t drain_holds = 0;
std::uint64_t epoch = 0, call_count = 0, selected = 0;
std::uint32_t selected_entry = 0;
std::uint16_t selected_psp = 0, selected_parent = 0;
JsonValue Num(std::int64_t n) { return JsonValue::Number(std::to_string(n)); }
bool Uint(const JsonValue* value, std::uint64_t* n) {
    if (!value || value->type != JsonType::Number || value->text.empty()) return false;
    *n = 0;
    for (char c : value->text) {
        if (c < '0' || c > '9' || *n > 1000000000) return false;
        *n = *n * 10 + static_cast<unsigned>(c - '0');
    }
    return *n <= 1000000000;
}
std::string Text(const JsonValue* value) { return value && value->type == JsonType::String ? value->text : ""; }
std::string StreamId() { return "exec-stream-" + std::to_string(epoch); }
std::string HexName(const char* value) {
    std::string result;
    const char hex[] = "0123456789abcdef";
    if (!value) return result;
    for (unsigned i = 0; value[i]; ++i) {
        if (i == 260) { window.Fail("DOS EXEC name exceeds observation bound"); break; }
        const auto byte = static_cast<unsigned char>(value[i]);
        result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]);
    }
    return result;
}
JsonValue Position() {
    JsonValue value = JsonValue::Object();
    value.object["pic_ticks"] = Num(PIC_Ticks);
    value.object["cycle_max"] = Num(CPU_CycleMax);
    value.object["cycle_left"] = Num(CPU_CycleLeft);
    value.object["cycles"] = Num(CPU_Cycles);
    return value;
}
void Emit(JsonValue value, const char* kind, const char* phase) {
    if (!armed) return;
    value.object["kind"] = JsonValue::String(kind);
    value.object["phase"] = JsonValue::String(phase);
    value.object["position"] = Position();
    if (backpressure && window.failure.empty() && window.rows.size() == ExecEventWindow::Limit) {
        // While DOS is mid-operation, retain the event without dispatching guest/device/UI work.
        drain_held = true;
        ++drain_holds;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (window.failure.empty() && window.rows.size() == ExecEventWindow::Limit) {
            AGENT_EmulationQueue().Pump(true);
            if (window.rows.size() < ExecEventWindow::Limit) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                window.Fail("DOS EXEC drain watchdog exceeded; incomplete history");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        drain_held = false;
    }
    if (!window.Push(std::move(value))) AGENT_ReplayInputLoss(window.failure.c_str());
}
JsonValue Status() {
    JsonValue value = JsonValue::Object();
    value.object["format"] = JsonValue::String("dosbox-proof:dos-exec-stream:v1");
    value.object["stream_id"] = JsonValue::String(StreamId());
    value.object["armed"] = JsonValue::Bool(armed);
    value.object["failure"] = JsonValue::String(window.failure);
    value.object["base_order"] = Num(window.base);
    value.object["event_count"] = Num(window.count);
    value.object["retained_events"] = Num(window.rows.size());
    value.object["window_capacity"] = Num(ExecEventWindow::Limit);
    value.object["backpressure_enabled"] = JsonValue::Bool(backpressure);
    value.object["drain_held"] = JsonValue::Bool(drain_held);
    value.object["drain_holds"] = Num(drain_holds);
    value.object["selected_call_id"] = Num(selected);
    value.object["entry_observed"] = JsonValue::Bool(entered);
    value.object["position"] = Position();
    return value;
}
}

void AGENT_ExecReset() {
    window = ExecEventWindow(); armed = true; entered = false;
    backpressure = drain_held = false; drain_holds = 0;
    call_count = selected = 0; selected_entry = 0; selected_psp = selected_parent = 0; ++epoch;
}

ExecObservation::ExecObservation(const char* name, std::uint16_t mode_, std::uint32_t parameter)
    : active(armed), mode(mode_), fields(JsonValue::Object()) {
    if (!active) return;
    id = ++call_count;
    fields.object["call_id"] = Num(id);
    fields.object["requested_mode"] = Num(mode);
    fields.object["requested_name_hex"] = JsonValue::String(HexName(name));
    parent_psp = dos.psp();
    fields.object["parent_psp"] = Num(parent_psp);
    fields.object["parameter_physical"] = Num(parameter);
    Emit(fields, "request", "dos-execute-before-parameter-read");
}
void ExecObservation::Opened(std::uint16_t handle) {
    if (!active) return;
    const auto real = RealHandle(handle);
    if (real >= DOS_FILES || !Files[real]) { window.Fail("DOS EXEC opened handle is unavailable"); return; }
    fields.object["opened_name_hex"] = JsonValue::String(HexName(Files[real]->GetName()));
    fields.object["opened_drive"] = Num(Files[real]->GetDrive());
}
void ExecObservation::Mode(std::uint16_t effective) {
    mode = effective;
    if (active) fields.object["effective_mode"] = Num(mode);
}
void ExecObservation::Header(const std::uint8_t* bytes, std::uint16_t length, bool com) {
    if (!active) return;
    fields.object["image_kind"] = JsonValue::String(com ? "com" : "mz");
    fields.object["header_read_bytes"] = Num(length);
    fields.object["header_read_sha256"] = JsonValue::String(AGENT_Sha256Hex(std::vector<std::uint8_t>(bytes, bytes + length)));
}
void ExecObservation::Image(std::uint16_t load, std::uint16_t psp, std::uint16_t environment,
                            std::uint32_t header_bytes, std::uint16_t count, std::uint16_t relocation_segment) {
    if (!active) return;
    child_psp = psp;
    fields.object["load_segment"] = Num(load);
    fields.object["child_psp"] = (mode & 0x7f) == 3 ? JsonValue::Null() : Num(psp);
    fields.object["environment_segment"] = (mode & 0x7f) == 3 ? JsonValue::Null() : Num(environment);
    fields.object["declared_header_bytes"] = Num(header_bytes);
    fields.object["declared_relocations"] = Num(count);
    fields.object["relocation_segment"] = Num(relocation_segment);
}
void ExecObservation::Payload(const std::uint8_t* bytes, std::uint16_t length, bool ok) {
    if (!active) return;
    if (!ok || payload.size() + length > 1024 * 1024) { window.Fail("DOS EXEC payload read failed or exceeds bound"); return; }
    payload.insert(payload.end(), bytes, bytes + length);
}
void ExecObservation::Relocation(const std::uint8_t* bytes, std::uint16_t length, bool ok) {
    if (!active) return;
    if (!ok || length != 4 || relocations.size() + length > 4 * 65535) {
        window.Fail("DOS EXEC relocation read failed/short or exceeds bound"); return;
    }
    relocations.insert(relocations.end(), bytes, bytes + length);
}
void ExecObservation::Success(std::uint32_t entry_, bool has_entry_) {
    success = true; entry = entry_; has_entry = has_entry_;
}
ExecObservation::~ExecObservation() noexcept(false) {
    if (std::uncaught_exception()) { window.Fail("DOS EXEC unwound before completion"); return; }
    if (!active) return;
    fields.object["success"] = JsonValue::Bool(success);
    fields.object["dos_error"] = success ? JsonValue::Null() : Num(dos.errorcode);
    fields.object["result_psp"] = Num(dos.psp());
    fields.object["payload_read_bytes"] = Num(payload.size());
    fields.object["payload_read_sha256"] = JsonValue::String(AGENT_Sha256Hex(payload));
    fields.object["relocation_read_bytes"] = Num(relocations.size());
    fields.object["relocation_read_sha256"] = JsonValue::String(AGENT_Sha256Hex(relocations));
    fields.object["entry_segment"] = has_entry ? Num(entry >> 16) : JsonValue::Null();
    fields.object["entry_offset"] = has_entry ? Num(entry & 0xffff) : JsonValue::Null();
    const bool first = success && (mode & 0x7f) == 0 && !selected;
    if (first) { selected = id; selected_entry = entry; selected_psp = child_psp; selected_parent = parent_psp; }
    fields.object["selected_initial_exec"] = JsonValue::Bool(first);
    Emit(fields, "result", "dos-execute-return-before-guest-resume");
}
void AGENT_ExecEntry(std::uint16_t segment, std::uint32_t offset) {
    if (!armed || entered) return;
    if (!selected || offset > 0xffff || (static_cast<std::uint32_t>(segment) << 16 | offset) != selected_entry) {
        window.Fail("startup stop does not match selected successful DOS EXEC entry"); return;
    }
    entered = true;
    JsonValue value = JsonValue::Object();
    value.object["call_id"] = Num(selected);
    value.object["segment"] = Num(segment); value.object["offset"] = Num(offset);
    Emit(value, "entry", "selected-executable-before-first-instruction");
}
void AGENT_ExecExit(std::uint16_t psp, std::uint16_t parent, bool resident, std::uint8_t code) {
    if (!armed) return;
    JsonValue value = JsonValue::Object();
    value.object["psp"] = Num(psp); value.object["parent_psp"] = Num(parent);
    value.object["resident"] = JsonValue::Bool(resident); value.object["exit_code"] = Num(code);
    Emit(value, "exit", "dos-terminate-before-cleanup");
}
ExecExitObservation::ExecExitObservation(std::uint16_t psp_, std::uint16_t parent_, bool resident_, std::uint8_t code_)
    : psp(psp_), parent(parent_), resident(resident_),
      selected(armed && entered && psp_ == selected_psp && parent_ == selected_parent), code(code_) {
    AGENT_ExecExit(psp, parent, resident, code);
}
ExecExitObservation::~ExecExitObservation() noexcept(false) {
    // Allow DOSBox's integer shutdown exception through this scope guard.
    if (std::uncaught_exception()) { window.Fail("DOS termination unwound before completion"); return; }
    if (selected) AGENT_ReplayProgramExit(psp, parent, resident, code);
}
bool AGENT_ExecCommand(const JsonValue& params, JsonValue* result, std::string* error) {
    const auto reject = [error](const char* reason) { if (error) *error = reason; return false; };
    const auto action = Text(params.Find("action"));
    if (!armed) return reject("DOS EXEC observer is not armed");
    if (action != "exec-status" && Text(params.Find("stream_id")) != StreamId())
        return reject("missing or stale DOS EXEC stream identity");
    if (action == "exec-flow") {
        if (!entered || window.count != 3 || window.base || backpressure)
            return reject("DOS EXEC flow requires pristine selected entry history");
        backpressure = true;
    } else if (action == "exec-ack") {
        std::uint64_t through = 0;
        if (!Uint(params.Find("through_order"), &through) || !window.Ack(through)) return reject("invalid DOS EXEC acknowledgement");
    } else if (action != "exec-read" && action != "exec-status") return reject("unknown DOS EXEC action");
    *result = Status();
    if (action == "exec-read") {
        std::uint64_t cursor = 0, limit = 0;
        JsonValue rows;
        if (!Uint(params.Find("cursor"), &cursor) || !Uint(params.Find("limit"), &limit) || !window.Read(cursor, limit, &rows))
            return reject("invalid, evicted or future DOS EXEC cursor/limit");
        result->object["cursor"] = Num(cursor);
        result->object["next_cursor"] = Num(cursor + rows.array.size());
        result->object["rows"] = std::move(rows);
    }
    return true;
}
}
#endif
