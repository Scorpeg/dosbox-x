#ifndef DOSBOX_AGENT_FPU_CAPTURE_H
#define DOSBOX_AGENT_FPU_CAPTURE_H
#include "agent/capture_bytes.h"
#include "agent/agent_protocol.h"

namespace dosbox_agent {
using FpuBytes = CaptureBytes<1024>;
constexpr const char* FpuStorageContract = "fp-simd-storage-v2";

// Hash validity and denormal flags directly, without tag derivation or host FP operations.
template<class State> bool FpuRegvalidBytes(const State& state, FpuBytes& registers,
        FpuBytes& control, FpuBytes& simd, FpuBytes& simd_control) {
    for (unsigned i = 0; i < 9; ++i) {
        registers.u64(state.regs_80[i].raw.l);
        registers.u16(state.regs_80[i].raw.h);
    }
    control.u16(state.cw.reg); control.u16(state.sw.reg);
    for (unsigned i = 0; i < 9; ++i) control.u8(state.regvalid[i]);
    control.u8(state.ea_denormal);
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned lane = 0; lane < 4; ++lane) simd.u32(state.xmmreg[i].u32[lane]);
    simd_control.u32(state.mxcsr);
    return registers.valid && control.valid && simd.valid && simd_control.valid;
}

template<class State> bool FpuRegvalidFallbackBytes(const State& state, FpuBytes& fallback) {
    for (unsigned i = 0; i < 9; ++i) fallback.u64(state.regs[i].raw);
    for (unsigned i = 0; i < 9; ++i) fallback.u8(state.use80[i]);
    return fallback.valid;
}

// Hash raw x87 bits, including empty/internal slots; exclude padding and host FP operations.
template<class State> bool FpuCommonBytes(const State& state, FpuBytes& registers,
        FpuBytes& control, FpuBytes& simd, FpuBytes& simd_control) {
    for (unsigned i = 0; i < 9; ++i) {
        registers.u64(state.regs_80[i].raw.l);
        registers.u16(state.regs_80[i].raw.h);
    }
    control.u16(state.cw.reg); control.u16(state.sw.reg);
    for (unsigned i = 0; i < 9; ++i) {
        const auto tag = static_cast<unsigned>(state.tags[i]);
        if (tag > 3) return false;
        control.u8(tag);
    }
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned lane = 0; lane < 4; ++lane) simd.u32(state.xmmreg[i].u32[lane]);
    simd_control.u32(state.mxcsr);
    return registers.valid && control.valid && simd.valid && simd_control.valid;
}

template<class State> bool FpuFallbackBytes(const State& state, FpuBytes& fallback) {
    for (unsigned i = 0; i < 9; ++i) fallback.u64(static_cast<std::uint64_t>(state.regs[i].ll));
    for (unsigned i = 0; i < 9; ++i) fallback.u8(state.use80[i]);
    return fallback.valid;
}

template<class State, class Register> bool FpuAliasBytes(const State& state,
        Register* const* registers, unsigned count, FpuBytes& aliases) {
    for (unsigned i = 0; i < count; ++i) {
        unsigned slot = 0;
        while (slot < 9 && registers[i] != &state.regs_80[slot].reg_mmx) ++slot;
        if (slot == 9) return false;
        aliases.u8(slot);
    }
    return aliases.valid;
}

bool AGENT_FpuCapture(JsonValue* result, std::string* error);
}
#endif
