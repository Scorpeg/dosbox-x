#include "fpu_types.h"
#include "agent/fpu_capture.h"
#include <cassert>
#include <cstring>

struct State {
    FPU_Reg_80 regs_80[9] = {};
    FPU_Reg_64 regs[9] = {};
    bool regvalid[9] = {}, use80[9] = {}, ea_denormal = false;
    struct Control { uint16_t reg = 0; } cw, sw;
    XMM_Reg xmmreg[8] = {};
    uint32_t mxcsr = 0;
};
using dosbox_agent::FpuBytes;

static FpuBytes control(const State& state) {
    FpuBytes r, c, s, sc;
    assert(dosbox_agent::FpuRegvalidBytes(state, r, c, s, sc));
    assert(r.bytes.size() == 90 && c.bytes.size() == 14);
    assert(s.bytes.size() == 128 && sc.bytes.size() == 4);
    return c;
}

int main() {
    State state;
    state.cw.reg = 0x1234;
    state.sw.reg = 0x5678;
    const auto initial = control(state).bytes;
    assert(initial[0] == 0x34 && initial[1] == 0x12);
    assert(initial[2] == 0x78 && initial[3] == 0x56);
    for (unsigned i = 0; i < 9; ++i) {
        state.regvalid[i] = true;
        auto expected = initial; expected[4+i] = 1;
        assert(control(state).bytes == expected);
        state.regvalid[i] = false;
    }
    state.ea_denormal = true;
    auto expected = initial; expected[13] = 1;
    assert(control(state).bytes == expected);
    // Include nonfinite/internal-slot bits; exclude padding.
    state.regs_80[8].raw.l = UINT64_C(0x8000000000001234);
    state.regs_80[8].raw.h = 0x7fff;
    state.xmmreg[7].u32[3] = 0x12345678;
    state.mxcsr = 0x87654321;
    FpuBytes r, c, s, sc;
    assert(dosbox_agent::FpuRegvalidBytes(state, r, c, s, sc));
    assert(r.bytes[80] == 0x34 && r.bytes[81] == 0x12 && r.bytes[87] == 0x80);
    assert(r.bytes[88] == 0xff && r.bytes[89] == 0x7f);
    assert(s.bytes[124] == 0x78 && s.bytes[127] == 0x12);
    assert(sc.bytes[0] == 0x21 && sc.bytes[3] == 0x87);
    std::memset(reinterpret_cast<unsigned char*>(&state.regs_80[8]) + 10, 0xa5, 6);
    FpuBytes r2, c2, s2, sc2;
    assert(dosbox_agent::FpuRegvalidBytes(state, r2, c2, s2, sc2));
    assert(r.bytes == r2.bytes && c.bytes == c2.bytes && s.bytes == s2.bytes && sc.bytes == sc2.bytes);
    state.regs[8].raw = UINT64_C(0xfff0000000004321);
    state.use80[8] = true;
    FpuBytes fallback;
    assert(dosbox_agent::FpuRegvalidFallbackBytes(state, fallback));
    assert(fallback.bytes.size() == 81 && fallback.bytes[64] == 0x21 && fallback.bytes[80] == 1);
    MMX_reg* aliases[9];
    for (unsigned i = 0; i < 9; ++i) aliases[i] = &state.regs_80[i].reg_mmx;
    FpuBytes bindings;
    assert(dosbox_agent::FpuAliasBytes(state, aliases, 9, bindings));
    assert(bindings.bytes[8] == 8);
    aliases[0] = nullptr;
    FpuBytes invalid;
    assert(!dosbox_agent::FpuAliasBytes(state, aliases, 9, invalid));
}
