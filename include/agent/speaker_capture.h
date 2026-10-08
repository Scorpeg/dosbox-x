#ifndef DOSBOX_AGENT_SPEAKER_CAPTURE_H
#define DOSBOX_AGENT_SPEAKER_CAPTURE_H
#include "agent/agent_protocol.h"
#include "agent/capture_bytes.h"

namespace dosbox_agent {
using SpeakerBytes = CaptureBytes<262144>;

template<class Timer> bool SpeakerPitBytes(const Timer& t, SpeakerBytes& b) {
    if (t.mode > 5 || t.read_state > 3 || static_cast<unsigned>(t.write_state) > 3) return false;
    b.u8(t.latched_timerstatus); b.u8(t.latched_timerstatus_locked);
    b.u64(t.cntr); b.u64(t.cntr_cur);
    b.normalized(t.delay); b.normalized(t.start); b.normalized(t.now);
    b.u16(t.read_latch); b.u16(t.write_latch);
    b.u8(t.mode); b.u8(t.read_state); b.u8(static_cast<unsigned>(t.write_state)); b.u8(t.cycle_base);
    b.u8(t.bcd); b.u8(t.go_read_latch); b.u8(t.new_mode); b.u8(t.counterstatus_set);
    b.u8(t.counting); b.u8(t.update_count); b.u8(t.gate); b.u8(t.output);
    b.u16(t.last_counter.counter); b.u16(t.last_counter.cycle);
    return b.valid;
}

template<class Speaker, class Real>
bool SpeakerStorageBytes(const Speaker& s, bool raw_gate, bool previous_output, std::uint8_t port61,
                         Real speed, SpeakerBytes& state, SpeakerBytes& queue) {
    if (s.pit_mode > 5 || s.used > 8192) return false;
    state.u64(s.pit_mode); state.u64(s.rate);
    state.u8(s.pit_output_enabled); state.u8(s.pit_clock_gate_enabled); state.u8(s.pit_output_level);
    state.normalized(s.pit_new_max); state.normalized(s.pit_new_half);
    state.normalized(s.pit_max); state.normalized(s.pit_half); state.normalized(s.pit_index);
    state.u8(s.pit_mode1_waiting_for_counter); state.u8(s.pit_mode1_waiting_for_trigger);
    state.normalized(s.pit_mode1_pending_max); state.u8(s.pit_mode3_counting);
    state.normalized(s.volwant); state.normalized(s.volcur); state.u64(s.last_ticks);
    state.normalized(s.last_index); state.u64(s.minimum_counter); state.u64(s.used);
    state.u8(raw_gate); state.u8(previous_output); state.normalized(speed);
    state.u32(10000); state.u32(8192); state.u8(port61);
    // Exclude uninitialized queue entries.
    for (unsigned i = 0; i < s.used; ++i) {
        queue.normalized(s.entries[i].index); queue.u8(s.entries[i].output_level);
    }
    return state.valid && queue.valid;
}

bool AGENT_PitStorage(SpeakerBytes& bytes);
std::uint8_t AGENT_SpeakerPort61();
bool AGENT_SpeakerCapture(JsonValue* result, std::string* error);
}
#endif
