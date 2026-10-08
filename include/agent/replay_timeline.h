#ifndef DOSBOX_AGENT_REPLAY_TIMELINE_H
#define DOSBOX_AGENT_REPLAY_TIMELINE_H

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "agent/agent_protocol.h"

namespace dosbox_agent {

// Tick once per emulated millisecond at the defined PIC phase, never from host pacing.
struct ReplayKey {
    std::uint64_t tick = 0;
    std::uint32_t order = 0;
    int key = 0;
    bool pressed = false;
    std::string source;
    // Keyboard and mouse share one ordered stream.
    unsigned kind = 0;
    std::uint32_t motion[4] = {};
    std::int32_t context[4] = {}; // host cursor x/y, effective screen width/height
    unsigned mouse_flags = 0; // emulate, locked, effective driver emulation
    unsigned button = 0;
    std::int32_t wheel = 0;
    bool captured = false;
};

struct ReplayDelivery {
    ReplayKey event;
    std::uint32_t space_before = 0;
    std::uint32_t space_after = 0;
    unsigned mouse_queue_before = 0, mouse_queue_after = 0;
    unsigned buttons_before = 0, buttons_after = 0;
    bool capture_before = false, capture_after = false;
};

class ReplayTimeline {
public:
    static constexpr std::size_t Limit = 1024;
    bool active = false;
    bool replay = false;
    bool complete = false;
    // Limit retained events, not session length; eviction preserves global coordinates.
    bool streaming = false;
    bool pointer = false;
    bool capture_tracking = false;
    bool initial_mouse_captured = false, mouse_captured = false;
    bool input_closed = false;
    std::uint64_t base_order = 0;
    std::uint64_t horizon = 0;
    std::uint64_t last_event_tick = 0;
    std::uint64_t origin = 0;
    std::uint64_t tick = 0;
    std::string failure;
    std::vector<ReplayKey> events;
    std::vector<ReplayDelivery> delivered;
    std::set<int> held;
    std::set<unsigned> held_buttons;
    // Stop requests are host-only; resulting key releases are recorded inputs.
    std::string finish_request;
    std::uint64_t finish_request_tick = 0, finish_releases = 0;

    static bool ValidInput(const ReplayKey& event, bool mouse_enabled, bool capture_enabled = false) {
        if (!event.kind) return event.key > 0 && (event.source == "api" || event.source == "mapper");
        if (!mouse_enabled || event.kind > 4 || (event.source != "api" && event.source != "frontend")) return false;
        if (event.kind == 4) return capture_enabled;
        if (event.kind == 2) return event.button <= 2;
        if (event.kind == 3) return event.wheel >= -32767 && event.wheel <= 32767;
        if (event.mouse_flags > 7 || event.context[2] < 2 || event.context[3] < 2) return false;
        for (const auto bits : event.motion) {
            // Bound deltas to +/-1048576 to avoid undefined downstream float-to-int conversions.
            if ((bits & 0x7fffffffU) > 0x49800000U) return false;
        }
        return true;
    }

    bool Begin(bool playback, std::uint64_t start, const std::vector<ReplayKey>& input,
               std::string* error, bool stream = false, std::uint64_t through = 0, bool final = false, bool mouse_enabled = false,
               bool capture_enabled = false, bool initially_captured = false)
    {
        if (active) return Reject(error, "timeline is already active");
        if (input.size() > Limit) return Reject(error, "event limit exceeded");
        if (!playback && !input.empty()) return Reject(error, "record begins with an empty timeline");
        if (mouse_enabled && !stream) return Reject(error, "mouse input requires a streaming timeline");
        if (capture_enabled && !mouse_enabled) return Reject(error, "capture state requires the v2 mouse profile");
        if (initially_captured && !capture_enabled) return Reject(error, "initial capture requires the v2 mouse profile");
        std::uint64_t previous = 0;
        for (std::size_t i = 0; i < input.size(); ++i) {
            const ReplayKey& event = input[i];
            if (!event.tick || event.tick > 1000000000 || event.tick < previous ||
                event.order != i || !ValidInput(event, mouse_enabled, capture_enabled))
                return Reject(error, "invalid event coordinate/order/source");
            previous = event.tick;
        }
        if (stream && playback && (!through || through > 1000000000 || through < previous))
            return Reject(error, "playback stream requires a complete future horizon");
        if ((!stream || !playback) && (through || final))
            return Reject(error, "horizon/final only apply to playback streams");
        *this = ReplayTimeline();
        active = true;
        replay = playback;
        origin = start;
        events = input;
        streaming = stream;
        pointer = mouse_enabled;
        capture_tracking = capture_enabled;
        initial_mouse_captured = mouse_captured = initially_captured;
        horizon = through;
        input_closed = final;
        last_event_tick = previous;
        return true;
    }

    std::uint64_t EventCount() const { return base_order + events.size(); }
    std::uint64_t DeliveryCount() const { return base_order + delivered.size(); }

    bool Append(const std::vector<ReplayKey>& input, std::uint64_t expected_order,
                std::uint64_t through, bool final, std::string* error)
    {
        if (!streaming || !active || !failure.empty() || input_closed || !finish_request.empty())
            return Reject(error, "stream is not accepting input");
        if (expected_order != EventCount()) return Reject(error, "stale stream order; never refresh a retry fence");
        if (events.size() + input.size() > Limit || EventCount() + input.size() > 1000000000)
            return Reject(error, "stream window or total coordinate limit exceeded");
        std::uint64_t previous = last_event_tick;
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto& event = input[i];
            if (event.order != expected_order + i || !event.tick || event.tick <= tick ||
                event.tick > 1000000000 || event.tick < previous || (replay && event.tick <= horizon) ||
                !ValidInput(event, pointer, capture_tracking) ||
                (!replay && event.source != "api"))
                return Reject(error, "invalid stream event coordinate/order/source");
            previous = event.tick;
        }
        if (replay ? (through <= horizon || through > 1000000000 || through < previous) : (through || final))
            return Reject(error, "invalid stream horizon/final");
        // Reject invalid batches before changing the schedule.
        events.insert(events.end(), input.begin(), input.end());
        horizon = through;
        input_closed = final;
        last_event_tick = previous;
        return true;
    }

    bool Acknowledge(std::uint64_t through_order, std::string* error)
    {
        if (!streaming || through_order > DeliveryCount())
            return Reject(error, "acknowledgement is beyond delivered input");
        if (through_order <= base_order) return true; // Remain idempotent after RPC cache eviction.
        const auto count = static_cast<std::size_t>(through_order - base_order);
        events.erase(events.begin(), events.begin() + count);
        delivered.erase(delivered.begin(), delivered.begin() + count);
        base_order = through_order;
        return true;
    }

    bool Enqueue(int key, bool pressed, std::uint64_t when, const std::string& source, std::string* error)
    {
        ReplayKey event;
        event.key = key; event.pressed = pressed; event.tick = when; event.source = source;
        return EnqueueInput(event, error);
    }

    bool EnqueueInput(ReplayKey event, std::string* error)
    {
        if (!active || complete || !failure.empty() || !finish_request.empty()) return Reject(error, "recording is not active or is incomplete");
        if (replay) return Reject(error, "live input is forbidden during replay");
        if (events.size() == Limit || EventCount() == 1000000000) {
            Fail(streaming ? "stream window overflow; recorder did not drain input" : "event limit exceeded");
            return Reject(error, failure);
        }
        if (!ValidInput(event, pointer, capture_tracking) || event.tick <= tick || event.tick > 1000000000 || event.tick < last_event_tick)
            return Reject(error, "input must use a supported payload and a future nondecreasing tick");
        event.order = static_cast<std::uint32_t>(EventCount());
        events.push_back(event);
        last_event_tick = event.tick;
        return true;
    }

    bool RequestFinish(const std::string& source, std::string* error)
    {
        if (!active || replay || !streaming || !failure.empty()) return Reject(error, "no healthy live recording");
        if (source != "window-close" && source != "stop-button") return Reject(error, "unknown finish request");
        if (!finish_request.empty()) return true;
        auto keys = held;
        auto buttons = held_buttons;
        for (std::size_t i = delivered.size(); i < events.size(); ++i) {
            const auto& event = events[i];
            if (!event.kind) {
                if (event.pressed) keys.insert(event.key); else keys.erase(event.key);
            } else if (event.kind == 2) {
                if (event.pressed) buttons.insert(event.button); else buttons.erase(event.button);
            }
        }
        const auto count = keys.size() + buttons.size();
        const auto when = last_event_tick > tick ? last_event_tick : tick + 1;
        if (events.size() + count > Limit || EventCount() + count > 1000000000 || when > 1000000000)
            return Reject(error, "finish releases exceed the input window or clock limit");
        for (int key : keys)
            if (!Enqueue(key, false, when, "api", error)) return false;
        for (unsigned button : buttons) {
            ReplayKey event;
            event.kind = 2; event.button = button; event.tick = when; event.source = "api";
            if (!EnqueueInput(event, error)) return false;
        }
        finish_request = source;
        finish_request_tick = tick;
        finish_releases = count;
        return true;
    }

    void Tick(std::uint64_t absolute, const std::function<ReplayDelivery(const ReplayKey&)>& deliver)
    {
        if (!active) return;
        if (absolute <= origin || absolute - origin != tick + 1) {
            Fail("emulated clock jumped or reset");
            return;
        }
        tick = absolute - origin;
        if (!failure.empty()) return;
        if (streaming && replay && !input_closed && tick > horizon) {
            Fail("playback stream underrun; input was not supplied before its guest-time deadline");
            return;
        }
        while (delivered.size() < events.size() && events[delivered.size()].tick == tick) {
            const ReplayKey& event = events[delivered.size()];
            const bool captured_before = mouse_captured;
            if (event.kind == 4) mouse_captured = event.captured;
            ReplayDelivery observation = deliver(event);
            observation.capture_before = captured_before;
            observation.capture_after = mouse_captured;
            observation.event = event;
            delivered.push_back(observation);
            if (!event.kind) {
                if (event.pressed) held.insert(event.key);
                else held.erase(event.key);
            } else if (event.kind == 2) {
                if (event.pressed) held_buttons.insert(event.button);
                else held_buttons.erase(event.button);
            }
            if (!failure.empty()) return;
        }
    }

    bool Finish(std::string* error, bool terminal_exit = false)
    {
        if (!active) return Reject(error, "no active timeline");
        if (!failure.empty()) return Reject(error, failure);
        if (streaming && replay && !input_closed) return Reject(error, "playback input stream is not final");
        if (delivered.size() != events.size()) return Reject(error, "undelivered events remain");
        if (!terminal_exit && (!held.empty() || !held_buttons.empty())) return Reject(error, "keys or mouse buttons remain held");
        active = false;
        complete = true;
        return true;
    }

    void Fail(const std::string& reason) { if (active && failure.empty()) failure = reason; }

private:
    static bool Reject(std::string* error, const std::string& reason)
    {
        if (error) *error = reason;
        return false;
    }
};

struct ReplayCheckpoint {
    ReplayTimeline timeline;
    JsonValue origin;
    std::uint64_t discarded_live_keys = 0;
    std::uint64_t discarded_host_pointer_events = 0;
};
ReplayCheckpoint AGENT_ReplayCheckpoint();
void AGENT_ReplayRestore(const ReplayCheckpoint& checkpoint);
bool AGENT_ReplayCommand(const JsonValue& params, JsonValue* result, std::string* error);
void AGENT_ReplayTick();
bool AGENT_ReplayHostKey(int key, bool pressed);
bool AGENT_ReplayRequestFinish(const char* source);
bool AGENT_ReplayHostControl(bool pressed, bool mouse_capture = false);
void AGENT_ReplayHostCapture(bool captured);
bool AGENT_ReplayMouseCaptured(bool live);
bool AGENT_ReplayHostPointer(bool touch = false);
bool AGENT_ReplayHostMouse(ReplayKey event);
void AGENT_ReplayInputLoss(const char* reason);
void AGENT_ReplayReset();
bool AGENT_ReplayIsStreaming();
void AGENT_ReplayTickBoundary();
bool AGENT_ReplayBoundaryHeld();
bool AGENT_ReplayExitHeld();
bool AGENT_ReplaySubstitutionAllowed();
void AGENT_ReplayProgramExit(std::uint16_t psp, std::uint16_t parent, bool resident, std::uint8_t code);
bool AGENT_ReplayReleaseBoundary();

}
#endif
