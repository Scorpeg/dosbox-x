#ifndef DOSBOX_AGENT_OPL_CAPTURE_H
#define DOSBOX_AGENT_OPL_CAPTURE_H
#include "agent/agent_protocol.h"
#include "agent/capture_bytes.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace dosbox_agent {
using OplBytes = CaptureBytes<32768>;

struct OplCaptureAccess {
    template<class Timer> static void timer(const Timer& t, OplBytes& b) {
        b.real64(t.start); b.real64(t.trigger); b.real64(t.clockInterval); b.real64(t.counterInterval);
        b.u8(t.counter); b.u8(t.masked); b.u8(t.enabled); b.u8(t.overflow);
    }
    template<class Module> static int mode(const Module& m) { return static_cast<int>(m.mode); }
    template<class Module> static void frontend(const Module& m, OplBytes& registers, OplBytes& timers) {
        registers.u8(m.mode); registers.u32(m.reg.normal);
        registers.u8(m.ctrl.active); registers.u8(m.ctrl.index); registers.u8(m.ctrl.lvol);
        registers.u8(m.ctrl.rvol); registers.u8(m.ctrl.mixer); registers.u32(m.lastUsed);
        registers.u8(m.esfm_nativemode);
        for (auto v : m.cache) registers.u8(v);
        for (const auto& c : m.chip) {
            timer(c.timer0, timers); timer(c.timer1, timers);
            timers.real64(c.last_poll); timers.u32(c.poll_counter);
        }
    }
};

template<class Chip, class VolumeId, class SynthId, class WaveId>
bool OplDboplBytes(const Chip& c, VolumeId volume_id, SynthId synth_id, WaveId wave_id, OplBytes& b) {
    b.u32(c.lfoCounter); b.u32(c.lfoAdd); b.u32(c.noiseCounter); b.u32(c.noiseAdd); b.u32(c.noiseValue);
    for (auto v : c.freqMul) b.u32(v);
    for (auto v : c.linearRates) b.u32(v);
    for (auto v : c.attackRates) b.u32(v);
    b.u8(c.reg104); b.u8(c.reg08); b.u8(c.reg04); b.u8(c.regBD);
    b.u8(c.vibratoIndex); b.u8(c.tremoloIndex); b.u8(c.vibratoSign); b.u8(c.vibratoShift);
    b.u8(c.tremoloValue); b.u8(c.vibratoStrength); b.u8(c.tremoloStrength); b.u8(c.waveFormMask);
    b.u8(c.opl3Active); b.u8(c.opl3Mode);
    for (const auto& channel : c.chan) {
        const int synth = synth_id(channel.synthHandler);
        if (synth < 0 || synth > 9) return false;
        b.u8(synth); b.u32(channel.chanData);
        for (auto v : channel.old) b.u32(v);
        b.u8(channel.feedback); b.u8(channel.regB0); b.u8(channel.regC0); b.u8(channel.fourMask);
        b.u8(channel.maskLeft); b.u8(channel.maskRight);
        for (const auto& o : channel.op) {
            const int volume = volume_id(o.volHandler), wave = wave_id(o.waveBase);
            if (volume < 0 || volume > 4 || wave < 0 || (wave >= 4096 && wave != 65535)) return false;
            b.u8(volume); b.u16(wave); b.u32(o.waveMask); b.u32(o.waveStart);
            b.u32(o.waveIndex); b.u32(o.waveAdd); b.u32(o.waveCurrent); b.u32(o.chanData); b.u32(o.freqMul);
            b.u32(o.vibrato); b.u32(o.sustainLevel); b.u32(o.totalLevel); b.u32(o.currentLevel); b.u32(o.volume);
            b.u32(o.attackAdd); b.u32(o.decayAdd); b.u32(o.releaseAdd); b.u32(o.rateIndex);
            b.u8(o.rateZero); b.u8(o.keyOn); b.u8(o.reg20); b.u8(o.reg40); b.u8(o.reg60);
            b.u8(o.reg80); b.u8(o.regE0); b.u8(o.state); b.u8(o.tremoloMask); b.u8(o.vibStrength); b.u8(o.ksr);
        }
    }
    return b.valid;
}

template<class Channel> bool OplMixerBytes(const Channel& c, OplBytes& b) {
    if (c.msbuffer_o > 2048 || c.msbuffer_i > c.msbuffer_o || c.lowpass_order > 8) return false;
    for (auto v : c.volmain) b.real32(v);
    for (auto v : c.scale) b.real32(v);
    for (auto v : c.volmul) b.u32(v);
    for (const auto& row : c.lowpass) for (auto v : row) b.u32(v);
    b.u32(c.lowpass_alpha); b.u64(c.lowpass_freq); b.u32(c.lowpass_order);
    b.u8(c.lowpass_on_load); b.u8(c.lowpass_on_out);
    b.u32(c.freq_f); b.u32(c.freq_fslew); b.u32(c.freq_nslew); b.u32(c.freq_nslew_want);
    b.u32(c.rendering_to_n); b.u32(c.rendering_to_d); b.u32(c.rend_n); b.u32(c.rend_d);
    b.u32(c.freq_n); b.u32(c.freq_d); b.u32(c.freq_d_orig); b.u8(c.current_loaded);
    for (auto v : c.current) b.u32(v);
    for (auto v : c.last) b.u32(v);
    for (auto v : c.delta) b.u32(v);
    b.u32(c.max_change); b.u64(c.last_sample_write); b.u64(c.msbuffer_o); b.u64(c.msbuffer_i); b.u8(c.enabled);
    // Read only initialized slots.
    for (unsigned i = 0; i < c.msbuffer_o; ++i) for (auto v : c.msbuffer[i]) b.u32(v);
    return b.valid;
}

bool AGENT_OplCapture(JsonValue* result, std::string* error);
}
#endif
