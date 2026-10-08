#include "config.h"
#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "dosbox.h"
#include "agent/replay_timeline.h"
#include "agent/audio_capture.h"
#include "agent/agent_protocol.h"
#include "agent/agent_bridge.h"
#include "agent/checkpoint.h"
#include "agent/exec_observer.h"
#include "agent/state_capture.h"
#include "agent/raster_stream.h"
#include "keyboard.h"
#include "mouse.h"
#include "pic.h"
#include "cpu.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
extern bool sync_time;
extern bool en_int33, en_vmware;
extern volatile bool vmware_mouse;
extern int wheel_key;
extern uint8_t Mouse_GetButtonState();
extern bool KEYBOARD_AUX_Active();
extern bool MOUSE_IsLocked();

namespace dosbox_agent {
namespace {
ReplayTimeline timeline;
JsonValue origin_position;
std::uint64_t discarded_live_keys = 0;
std::uint64_t discarded_host_pointer_events = 0;
std::uint64_t stream_sequence = 0;
std::string stream_id;
std::atomic<bool> boundary_held{false};
std::atomic<bool> exit_held{false};
bool stop_on_exit = false;
bool finish_on_close = false;
JsonValue exit_observation;
std::uint64_t boundary_tick = 0;
std::uint64_t boundary_sequence = 0;

struct KeyName { const char* name; KBD_KEYS key; };
#define KEY(name) {#name, KBD_##name}
const KeyName keys[] = {
    KEY(a), KEY(b), KEY(c), KEY(d), KEY(e), KEY(f), KEY(g), KEY(h), KEY(i), KEY(j), KEY(k),
    KEY(l), KEY(m), KEY(n), KEY(o), KEY(p), KEY(q), KEY(r), KEY(s), KEY(t), KEY(u), KEY(v),
    KEY(w), KEY(x), KEY(y), KEY(z), KEY(0), KEY(1), KEY(2), KEY(3), KEY(4), KEY(5), KEY(6),
    KEY(7), KEY(8), KEY(9), KEY(space), KEY(enter), KEY(esc), KEY(tab), KEY(backspace),
    KEY(left), KEY(right), KEY(up), KEY(down), KEY(leftshift), KEY(rightshift),
    KEY(leftctrl), KEY(rightctrl), KEY(leftalt), KEY(rightalt),
    KEY(f1), KEY(f2), KEY(f3), KEY(f4), KEY(f5), KEY(f6), KEY(f7), KEY(f8), KEY(f9), KEY(f10), KEY(f11), KEY(f12)
};
#undef KEY
int FindKey(const std::string& name) { for (const auto& item : keys) if (item.name == name) return item.key; return 0; }
const char* KeyString(int key) { for (const auto& item : keys) if (item.key == key) return item.name; return "unsupported"; }
JsonValue Num(std::int64_t n) { return JsonValue::Number(std::to_string(n)); }
bool Unsigned(const JsonValue* value, std::uint64_t* n)
{
    if (!value || value->type != JsonType::Number || value->text.empty()) return false;
    *n = 0;
    for (char c : value->text) {
        if (c < '0' || c > '9' || *n > 1000000000) return false;
        *n = *n * 10 + static_cast<unsigned>(c - '0');
    }
    return *n <= 1000000000;
}
std::string Text(const JsonValue* value) { return value && value->type == JsonType::String ? value->text : ""; }
bool Reject(std::string* error, const char* reason) { if (error) *error = reason; return false; }
JsonValue Position()
{
    JsonValue value = JsonValue::Object();
    value.object["pic_ticks"] = Num(PIC_Ticks);
    value.object["cycle_max"] = Num(CPU_CycleMax);
    value.object["cycle_left"] = Num(CPU_CycleLeft);
    value.object["cycles"] = Num(CPU_Cycles);
    return value;
}
JsonValue Event(const ReplayKey& event)
{
    JsonValue value = JsonValue::Object();
    value.object["tick"] = Num(event.tick);
    value.object["order"] = Num(event.order);
    value.object["source"] = JsonValue::String(event.source);
    if (timeline.pointer) value.object["kind"] = JsonValue::String(event.kind == 0 ? "key" : event.kind == 1 ? "motion" : event.kind == 2 ? "button" : event.kind == 3 ? "wheel" : "capture");
    if (!event.kind) {
        value.object["key"] = JsonValue::String(KeyString(event.key));
        value.object["pressed"] = JsonValue::Bool(event.pressed);
    } else if (event.kind == 1) {
        value.object["motion_bits"] = JsonValue::Array();
        value.object["context"] = JsonValue::Array();
        for (auto bits : event.motion) value.object["motion_bits"].array.push_back(Num(bits));
        for (auto item : event.context) value.object["context"].array.push_back(Num(item));
        value.object["mouse_flags"] = Num(event.mouse_flags);
    } else if (event.kind == 2) {
        value.object["button"] = Num(event.button);
        value.object["pressed"] = JsonValue::Bool(event.pressed);
    } else if (event.kind == 3) value.object["wheel"] = Num(event.wheel);
    else value.object["captured"] = JsonValue::Bool(event.captured);
    return value;
}
JsonValue Delivery(const ReplayDelivery& delivery)
{
    JsonValue item = Event(delivery.event);
    if (!delivery.event.kind) {
        item.object["space_before"] = Num(delivery.space_before);
        item.object["space_after"] = Num(delivery.space_after);
    } else if (delivery.event.kind == 4) {
        item.object["capture_before"] = JsonValue::Bool(delivery.capture_before);
        item.object["capture_after"] = JsonValue::Bool(delivery.capture_after);
    } else {
        item.object["queue_before"] = Num(delivery.mouse_queue_before);
        item.object["queue_after"] = Num(delivery.mouse_queue_after);
        item.object["buttons_before"] = Num(delivery.buttons_before);
        item.object["buttons_after"] = Num(delivery.buttons_after);
    }
    return item;
}
bool Signed(const JsonValue* value, std::int64_t low, std::int64_t high, std::int64_t* n) {
    if (!value || value->type != JsonType::Number || value->text.empty() || value->text.size() > 11) return false;
    const auto& text = value->text;
    const bool negative = text[0] == '-';
    if (negative && text.size() == 1) return false;
    *n = 0;
    for (size_t i = negative ? 1 : 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        *n = *n * 10 + (text[i] - '0');
    }
    if (negative) *n = -*n;
    return *n >= low && *n <= high;
}
bool ReadEvents(const JsonValue* input, std::vector<ReplayKey>* events, std::string* error, bool pointer, bool capture_tracking)
{
    if (!input || input->type != JsonType::Array || input->array.size() > ReplayTimeline::Limit)
        return Reject(error, "events must be an array of at most 1024 entries per window");
    for (const JsonValue& row : input->array) {
        ReplayKey event;
        std::uint64_t order = 0;
        const JsonValue* pressed = row.Find("pressed");
        event.source = Text(row.Find("source"));
        const std::string kind = pointer ? Text(row.Find("kind")) : "key";
        event.kind = kind == "key" ? 0 : kind == "motion" ? 1 : kind == "button" ? 2 : kind == "wheel" ? 3 : kind == "capture" ? 4 : 5;
        if (row.type != JsonType::Object || !Unsigned(row.Find("tick"), &event.tick) ||
            !Unsigned(row.Find("order"), &order) || event.kind == 5)
            return Reject(error, "invalid replay input event");
        event.order = static_cast<std::uint32_t>(order);
        std::int64_t number = 0;
        const size_t fields = event.kind == 1 ? 7 : event.kind >= 3 ? 5 : pointer ? 6 : 5;
        if (row.object.size() != fields) return Reject(error, "unknown or missing input event fields");
        if (!event.kind || event.kind == 2) {
            if (!pressed || pressed->type != JsonType::Bool) return Reject(error, "missing press/release state");
            event.pressed = pressed->boolean;
        }
        if (!event.kind) event.key = FindKey(Text(row.Find("key")));
        else if (event.kind == 1) {
            const auto* motion = row.Find("motion_bits");
            const auto* context = row.Find("context");
            if (!motion || motion->type != JsonType::Array || motion->array.size() != 4 ||
                !context || context->type != JsonType::Array || context->array.size() != 4 ||
                !Signed(row.Find("mouse_flags"), 0, 7, &number)) return Reject(error, "invalid mouse motion/context");
            event.mouse_flags = static_cast<unsigned>(number);
            for (unsigned i = 0; i < 4; ++i) {
                if (!Signed(&motion->array[i], 0, UINT32_MAX, &number)) return Reject(error, "invalid binary32 bits");
                event.motion[i] = static_cast<std::uint32_t>(number);
                if (!Signed(&context->array[i], INT32_MIN, INT32_MAX, &number)) return Reject(error, "invalid mouse context coordinate");
                event.context[i] = static_cast<std::int32_t>(number);
            }
        } else if (event.kind == 2) {
            if (!Signed(row.Find("button"), 0, 2, &number)) return Reject(error, "invalid mouse button");
            event.button = static_cast<unsigned>(number);
        } else if (event.kind == 3) {
            if (!Signed(row.Find("wheel"), -32767, 32767, &number)) return Reject(error, "invalid mouse wheel");
            event.wheel = static_cast<std::int32_t>(number);
        } else {
            const auto* captured = row.Find("captured");
            if (!captured || captured->type != JsonType::Bool) return Reject(error, "invalid mouse capture state");
            event.captured = captured->boolean;
        }
        if (!ReplayTimeline::ValidInput(event, pointer, capture_tracking)) return Reject(error, "unsupported input payload or source");
        events->push_back(event);
    }
    return true;
}
JsonValue Status()
{
    JsonValue value = JsonValue::Object();
    value.object["format"] = JsonValue::String(timeline.capture_tracking ? "dosbox-proof:input-stream:v2" : timeline.pointer ? "dosbox-proof:input-stream:v1" : timeline.streaming ? "dosbox-proof:keyboard-stream:v1" : "dosbox-proof:keyboard-timeline:v1");
    value.object["phase"] = JsonValue::String("pic-tick-after-rebase-before-handlers");
    value.object["layer"] = JsonValue::String(timeline.capture_tracking ? "dosbox-input-ingress-v2" : timeline.pointer ? "dosbox-input-ingress-v1" : "KEYBOARD_AddKey-attempt");
    if (timeline.capture_tracking) {
        value.object["initial_mouse_captured"] = JsonValue::Bool(timeline.initial_mouse_captured);
        value.object["mouse_captured"] = JsonValue::Bool(timeline.mouse_captured);
    }
    value.object["active"] = JsonValue::Bool(timeline.active);
    value.object["complete"] = JsonValue::Bool(timeline.complete);
    value.object["mode"] = JsonValue::String(timeline.replay ? "replay" : "record");
    value.object["failure"] = JsonValue::String(timeline.failure);
    value.object["tick"] = Num(timeline.tick);
    value.object["origin"] = origin_position;
    value.object["position"] = Position();
    if (timeline.streaming) {
        value.object["stream_id"] = JsonValue::String(stream_id);
        value.object["base_order"] = Num(timeline.base_order);
        value.object["event_count"] = Num(timeline.EventCount());
        value.object["delivery_count"] = Num(timeline.DeliveryCount());
        value.object["retained_events"] = Num(timeline.events.size());
        value.object["window_capacity"] = Num(ReplayTimeline::Limit);
        value.object["through_tick"] = Num(timeline.horizon);
        value.object["input_final"] = JsonValue::Bool(timeline.input_closed);
        value.object["boundary_phase"] = JsonValue::String(exit_held ? "dos-terminate-after-cleanup-before-handler-return" : "pic-tick-after-handlers");
        value.object["boundary_tick"] = Num(boundary_tick);
        value.object["boundary_sequence"] = Num(boundary_sequence);
        value.object["boundary_held"] = JsonValue::Bool(boundary_held);
        value.object["finish_request"] = JsonValue::Null();
        if (!timeline.finish_request.empty()) {
            auto& request = value.object["finish_request"];
            request = JsonValue::Object();
            request.object["source"] = JsonValue::String(timeline.finish_request);
            request.object["tick"] = Num(timeline.finish_request_tick);
            request.object["release_count"] = Num(timeline.finish_releases);
        }
        if (stop_on_exit) {
            value.object["exit_policy"] = JsonValue::String("selected-dos-process-v1");
            value.object["exit"] = exit_observation;
        }
    } else {
        value.object["events"] = JsonValue::Array();
        for (const auto& event : timeline.events) value.object["events"].array.push_back(Event(event));
        value.object["delivered"] = JsonValue::Array();
        for (const auto& delivery : timeline.delivered) value.object["delivered"].array.push_back(Delivery(delivery));
    }
    value.object["held"] = JsonValue::Array();
    value.object["live_mapper_policy"] = JsonValue::String("discard-counted-during-replay");
    value.object["discarded_live_keys"] = Num(discarded_live_keys);
    value.object["host_pointer_policy"] = JsonValue::String(timeline.pointer ? "capture-effective-frontend-input-discard-on-replay" : "discard-counted-during-keyboard-timeline");
    value.object["discarded_host_pointer_events"] = Num(discarded_host_pointer_events);
    for (int key : timeline.held) value.object["held"].array.push_back(JsonValue::String(KeyString(key)));
    for (unsigned button : timeline.held_buttons) value.object["held"].array.push_back(JsonValue::String("mouse-button-" + std::to_string(button)));
    return value;
}
}

bool AGENT_ReplayCommand(const JsonValue& params, JsonValue* result, std::string* error)
{
    const std::string action = Text(params.Find("action"));
    if (action.compare(0, 5, "exec-") == 0) return AGENT_ExecCommand(params, result, error);
    if (action.compare(0, 6, "state-") == 0) return AGENT_CaptureState(params, result, error);
    if (timeline.streaming && action != "begin" && action != "status" && Text(params.Find("stream_id")) != stream_id)
        return Reject(error, "missing or stale stream identity");
    if ((action == "read" || action == "ack" || action == "append" || action == "boundary") && !timeline.streaming)
        return Reject(error, "operation requires a streaming timeline");
    if (exit_held && (action == "append" || action == "key" || action == "boundary"))
        return Reject(error, "selected DOS process has exited; input and new boundaries are closed");
    if (action == "begin") {
        if (boundary_held) return Reject(error, "cannot begin a new timeline inside a replay boundary");
        // Scheduler timing fixes cycles/tick and disables host RTC sync; observers define other limits.
        if (CPU_CycleAutoAdjust || CPU_CycleMax != 10000 || sync_time || AGENT_InitialRtc() < 0)
            return Reject(error, "timeline clock contract requires fixed-10000, pinned initial RTC and no host time sync");
        const std::string mode = Text(params.Find("mode"));
        if (mode != "record" && mode != "replay") return Reject(error, "mode must be record or replay");
        const JsonValue* stream = params.Find("stream");
        const JsonValue* exit = params.Find("stop_on_exit");
        const auto* close = params.Find("finish_on_close");
        if (close && (close->type != JsonType::Bool || (close->boolean &&
                (mode != "record" || !stream || stream->type != JsonType::Bool || !stream->boolean))))
            return Reject(error, "finish_on_close requires a live streaming recording");
        if (exit && (exit->type != JsonType::Bool || (exit->boolean && (!stream || stream->type != JsonType::Bool || !stream->boolean))))
            return Reject(error, "stop_on_exit requires a boolean and a streaming timeline");
        const auto* inputs = params.Find("inputs");
        const bool capture_tracking = Text(inputs) == "keyboard-mouse-v2";
        const bool pointer = capture_tracking || Text(inputs) == "keyboard-mouse-v1";
        if (inputs && !pointer) return Reject(error, "unknown input profile");
        const auto* initial = params.Find("initial_mouse_captured");
        if ((capture_tracking && mode == "replay") ? (!initial || initial->type != JsonType::Bool) : initial != nullptr)
            return Reject(error, "initial_mouse_captured is required only for v2 playback");
#if defined(C_SDL2)
        if (pointer && (!en_int33 || en_vmware || vmware_mouse || KEYBOARD_AUX_Active() || wheel_key != 0))
            return Reject(error, "mouse stream requires INT33, no AUX/VMware, and no wheel-to-key mapping");
#else
        if (pointer) return Reject(error, "mouse frontend recording is currently qualified only with SDL2");
#endif
        const JsonValue* final = params.Find("final");
        std::uint64_t through = 0;
        if ((stream && stream->type != JsonType::Bool) || (final && final->type != JsonType::Bool) ||
            (params.Find("through_tick") && !Unsigned(params.Find("through_tick"), &through)))
            return Reject(error, "invalid stream/horizon/final");
        std::vector<ReplayKey> events;
        if (!ReadEvents(params.Find("events"), &events, error, pointer, capture_tracking)) return false;
        if (!timeline.Begin(mode == "replay", PIC_Ticks, events, error,
                            stream && stream->boolean, through, final && final->boolean, pointer, capture_tracking,
                            capture_tracking && (initial ? initial->boolean : MOUSE_IsLocked()))) return false;
        stream_id = timeline.streaming ? "input-stream-" + std::to_string(++stream_sequence) : "";
        boundary_tick = boundary_sequence = 0;
        stop_on_exit = exit && exit->boolean;
        finish_on_close = close && close->boolean;
        exit_held = false;
        exit_observation = JsonValue::Null();
        if (timeline.streaming) AGENT_CheckpointGuestService("streaming input has external acknowledged history");
        origin_position = Position();
        discarded_live_keys = 0;
        discarded_host_pointer_events = 0;
    } else if (action == "key") {
        std::uint64_t order = 0;
        if (timeline.streaming && (!Unsigned(params.Find("expected_order"), &order) || order != timeline.EventCount()))
            return Reject(error, "missing or stale stream order; never refresh a retry fence");
        const int key = FindKey(Text(params.Find("key")));
        const JsonValue* pressed = params.Find("pressed");
        std::uint64_t tick = timeline.tick + 1;
        if (!key || !pressed || pressed->type != JsonType::Bool ||
            (params.Find("tick") && !Unsigned(params.Find("tick"), &tick)))
            return Reject(error, "invalid key, pressed or tick");
        if (!timeline.Enqueue(key, pressed->boolean, tick, "api", error)) return false;
    } else if (action == "boundary") {
        std::uint64_t expected = 0, when = timeline.tick + 1;
        if (!timeline.active || !timeline.failure.empty() || boundary_tick ||
            !Unsigned(params.Find("expected_boundary_sequence"), &expected) || expected != boundary_sequence ||
            (params.Find("at_tick") && !Unsigned(params.Find("at_tick"), &when)) ||
            when <= timeline.tick || when > 1000000000 || boundary_sequence == 1000000000)
            return Reject(error, "invalid, pending or stale replay boundary request");
        boundary_tick = when;
        ++boundary_sequence;
    } else if (action == "append") {
        std::vector<ReplayKey> events;
        std::uint64_t order = 0, through = 0;
        const JsonValue* final = params.Find("final");
        if (!Unsigned(params.Find("expected_order"), &order) || !final || final->type != JsonType::Bool ||
            !Unsigned(params.Find("through_tick"), &through)) return Reject(error, "missing stream batch fences");
        if (!ReadEvents(params.Find("events"), &events, error, timeline.pointer, timeline.capture_tracking) ||
            !timeline.Append(events, order, through, final->boolean, error)) return false;
    } else if (action == "ack") {
        std::uint64_t order = 0;
        if (!Unsigned(params.Find("through_order"), &order)) return Reject(error, "invalid acknowledgement order");
        if (!timeline.Acknowledge(order, error)) return false;
    } else if (action == "read") {
        std::uint64_t cursor = 0, limit = 0;
        if (!Unsigned(params.Find("cursor"), &cursor) || !Unsigned(params.Find("limit"), &limit) ||
            !limit || limit > ReplayTimeline::Limit || cursor < timeline.base_order || cursor > timeline.DeliveryCount())
            return Reject(error, "invalid, evicted or future stream cursor/limit");
        *result = Status();
        result->object["cursor"] = Num(cursor);
        result->object["rows"] = JsonValue::Array();
        for (std::uint64_t i = cursor; i < timeline.DeliveryCount() && i - cursor < limit; ++i)
            result->object["rows"].array.push_back(Delivery(timeline.delivered[static_cast<std::size_t>(i - timeline.base_order)]));
        result->object["next_cursor"] = Num(cursor + result->object["rows"].array.size());
        return true;
    } else if (action == "finish") {
        if (boundary_tick) return Reject(error, "a future replay boundary is still armed");
        if (!timeline.Finish(error, exit_held)) return false;
    } else if (action != "status") {
        return Reject(error, "unknown replay.control action");
    }
    *result = Status();
    return true;
}

void AGENT_ReplayTick()
{
    timeline.Tick(PIC_Ticks, [](const ReplayKey& event) {
        ReplayDelivery result;
        if (!event.kind) {
            result.space_before = static_cast<std::uint32_t>(KEYBOARD_BufferSpaceAvail());
            KEYBOARD_AddKey(static_cast<KBD_KEYS>(event.key), event.pressed);
            result.space_after = static_cast<std::uint32_t>(KEYBOARD_BufferSpaceAvail());
        } else {
            if (!en_int33 || en_vmware || vmware_mouse || KEYBOARD_AUX_Active() || wheel_key != 0) {
                timeline.Fail("mouse stream left its INT33 delivery profile");
                return result;
            }
            result.mouse_queue_before = Mouse_EventQueueSize(); result.buttons_before = Mouse_GetButtonState();
            if (event.kind == 1) {
                float motion[4];
                std::memcpy(motion, event.motion, sizeof(motion));
                const MouseMotionContext context{(event.mouse_flags & 2) != 0, (event.mouse_flags & 4) != 0,
                    event.context[0], event.context[1], event.context[2], event.context[3]};
                Mouse_CursorMovedContext(motion[0], motion[1], motion[2], motion[3], (event.mouse_flags & 1) != 0, context);
            } else if (event.kind == 2) {
                if (event.pressed) Mouse_ButtonPressed(static_cast<uint8_t>(event.button));
                else Mouse_ButtonReleased(static_cast<uint8_t>(event.button));
            } else if (event.kind == 3) Mouse_WheelMoved(event.wheel);
            // Update the emulated capture latch, never the playback user's host cursor.
            result.mouse_queue_after = Mouse_EventQueueSize(); result.buttons_after = Mouse_GetButtonState();
        }
        return result;
    });
}

static void HoldBoundary() {
    boundary_tick = 0;
    boundary_held = true;
    if (CPU_CycleAutoAdjust || CPU_CycleMax != 10000 || sync_time || AGENT_InitialRtc() < 0)
        timeline.Fail("replay boundary left the fixed-10000/pinned-RTC clock contract");
    // Hold the native stack without guest/device/UI work until Continue.
    AGENT_NotifyDebuggerStopped(SegValue(cs), reg_eip);
    while (boundary_held) {
        AGENT_BridgePump();
        if (boundary_held) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void AGENT_ReplayTickBoundary()
{
    if (!timeline.active || !boundary_tick || timeline.tick != boundary_tick) return;
    HoldBoundary();
}
void AGENT_ReplayProgramExit(std::uint16_t psp, std::uint16_t parent, bool resident, std::uint8_t code) {
    if (!stop_on_exit || !timeline.active) return;
    if (exit_held || boundary_held) { timeline.Fail("overlapping DOS exit boundary"); return; }
    exit_observation = JsonValue::Object();
    auto& e = exit_observation.object;
    e["phase"] = JsonValue::String("dos-terminate-after-cleanup-before-handler-return");
    e["psp"] = Num(psp); e["parent_psp"] = Num(parent);
    e["resident"] = JsonValue::Bool(resident); e["exit_code"] = Num(code);
    exit_held = true;
    HoldBoundary();
}
bool AGENT_ReplayBoundaryHeld() { return boundary_held; }
bool AGENT_ReplayExitHeld() { return exit_held; }
bool AGENT_ReplaySubstitutionAllowed() {
    return boundary_held && !exit_held && timeline.streaming && timeline.active && timeline.replay && timeline.failure.empty();
}
bool AGENT_ReplayReleaseBoundary() { return !exit_held && boundary_held.exchange(false); }

bool AGENT_ReplayHostKey(int key, bool pressed)
{
    if (!timeline.active) return false;
    if (timeline.replay || !timeline.finish_request.empty()) { ++discarded_live_keys; return true; }
    if (!FindKey(KeyString(key))) {
        timeline.Fail("unsupported mapper key");
        return true;
    }
    std::string error;
    if (!timeline.Enqueue(key, pressed, timeline.tick + 1, "mapper", &error)) timeline.Fail(error);
    return true;
}
void AGENT_ReplayInputLoss(const char* reason) { timeline.Fail(reason); }
bool AGENT_ReplayRequestFinish(const char* source) {
    if (!finish_on_close || timeline.replay) return false;
    // Only the controller may close the process while Finish seals the recording.
    if (timeline.active && !exit_held) {
        std::string error;
        if (!timeline.RequestFinish(source, &error)) timeline.Fail(error);
    }
    return true;
}
bool AGENT_ReplayHostControl(bool pressed, bool mouse_capture) {
    if (!timeline.active) return false;
    if (!timeline.finish_request.empty()) return true;
    if (mouse_capture && timeline.capture_tracking && !timeline.replay) return false;
    if (pressed) {
        if (timeline.replay) ++discarded_live_keys;
        else timeline.Fail("unsupported mapper control during keyboard recording");
    }
    return true; // Suppress unrecorded speed/reset/debugger/clipboard actions.
}
bool AGENT_ReplayMouseCaptured(bool live) {
    return timeline.capture_tracking && (timeline.active || timeline.complete) ? timeline.mouse_captured : live;
}
void AGENT_ReplayHostCapture(bool captured) {
    if (!timeline.active) return;
    if (!timeline.capture_tracking) {
        timeline.Fail("host mouse capture transition requires keyboard-mouse-v2");
        return;
    }
    if (timeline.replay) { ++discarded_host_pointer_events; return; }
    ReplayKey event;
    event.kind = 4; event.captured = captured;
    AGENT_ReplayHostMouse(event);
}
bool AGENT_ReplayHostPointer(bool touch) {
    // Keyboard-only mode must drop host motion before it can schedule mouse PIC events.
    if (!timeline.active) return false;
    if (timeline.pointer && !timeline.replay && timeline.finish_request.empty()) {
        if (!en_int33 || en_vmware || vmware_mouse || KEYBOARD_AUX_Active() || wheel_key != 0) {
            timeline.Fail("mouse frontend left its INT33 capture profile");
            return true;
        }
        if (!touch) return false;
        timeline.Fail("touch input is not covered by the mouse frontend contract");
    }
    ++discarded_host_pointer_events;
    return true;
}
bool AGENT_ReplayHostMouse(ReplayKey event) {
    if (!timeline.active) return false;
    if (!timeline.pointer || timeline.replay || !timeline.finish_request.empty()) { ++discarded_host_pointer_events; return true; }
    event.tick = timeline.tick + 1; event.source = "frontend";
    std::string error;
    if (!timeline.EnqueueInput(event, &error)) timeline.Fail(error);
    return true;
}
ReplayCheckpoint AGENT_ReplayCheckpoint() { return {timeline, origin_position, discarded_live_keys, discarded_host_pointer_events}; }
void AGENT_ReplayRestore(const ReplayCheckpoint& checkpoint) {
    AGENT_InstructionWindowInvalidate("checkpoint restore invalidates instruction window provenance");
    timeline = checkpoint.timeline; origin_position = checkpoint.origin; discarded_live_keys = checkpoint.discarded_live_keys;
    discarded_host_pointer_events = checkpoint.discarded_host_pointer_events;
    stream_id.clear();
}
bool AGENT_ReplayIsStreaming() { return timeline.streaming; }
void AGENT_ReplayReset() {
    AGENT_InstructionWindowReset();
    AGENT_RasterReset();
    AGENT_AudioReset();
    timeline = ReplayTimeline(); origin_position = JsonValue::Null(); discarded_live_keys = 0; stream_id.clear();
    discarded_host_pointer_events = 0;
    boundary_tick = boundary_sequence = 0; boundary_held = false;
    exit_held = false; stop_on_exit = false; exit_observation = JsonValue::Null();
    finish_on_close = false;
}
}
#endif
