#ifndef DOSBOX_AGENT_CAPTURE_BYTES_H
#define DOSBOX_AGENT_CAPTURE_BYTES_H
#include <cmath>
#include <cfenv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace dosbox_agent {
// Preserve host FP state so normalization cannot affect guest FPU instructions.
class CaptureFpEnvironment {
    std::fenv_t saved;
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    // fesetenv may omit x87 condition bits; preserve the full 28-byte environment.
    unsigned char x87[28];
#endif
    bool active;
    bool& valid;
public:
    explicit CaptureFpEnvironment(bool& result)
        : active(false), valid(result) {
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
        __asm__ volatile("fnstenv %0; fldenv %0" : "=m"(x87) : : "memory");
#endif
        active = std::feholdexcept(&saved) == 0;
        if (!active) valid = false;
    }
    ~CaptureFpEnvironment() {
        if (active) {
            if (std::fesetenv(&saved) != 0) valid = false;
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
            __asm__ volatile("fldenv %0" : : "m"(x87) : "st", "st(1)", "st(2)", "st(3)",
                             "st(4)", "st(5)", "st(6)", "st(7)", "memory");
#endif
        }
    }
    explicit operator bool() const { return active; }
    CaptureFpEnvironment(const CaptureFpEnvironment&) = delete;
    CaptureFpEnvironment& operator=(const CaptureFpEnvironment&) = delete;
};

// Serialize bounded fields, excluding host pointers and padding.
template<std::size_t Limit = 32768> struct CaptureBytes {
    std::vector<std::uint8_t> bytes;
    bool valid = true;
    void integer(std::uint64_t value, unsigned width) {
        if (width > 8 || bytes.size() + width > Limit) { valid = false; return; }
        for (unsigned i = 0; i < width; ++i) { bytes.push_back(value & 255); value >>= 8; }
    }
    void u8(std::uint8_t v) { integer(v, 1); }
    void u16(std::uint16_t v) { integer(v, 2); }
    void u32(std::uint32_t v) { integer(v, 4); }
    void u64(std::uint64_t v) { integer(v, 8); }
    void real32(float value) {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "requires IEEE binary32");
        std::uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); u32(bits);
        if ((bits & 0x7f800000u) == 0x7f800000u) valid = false;
    }
    void real64(double value) {
        static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "requires IEEE binary64");
        std::uint64_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); u64(bits);
        if ((bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000)) valid = false;
    }
    template<class Real> void normalized(const Real& value) {
        const CaptureFpEnvironment environment(valid);
        if (!environment) return;
        // 22 bytes: sign, precision, LE exponent, four LE u32 fraction limbs (highest first); retain signed zero.
        constexpr int digits = std::numeric_limits<Real>::digits;
        if (std::numeric_limits<Real>::radix != 2 || digits > 128 || digits <= 0 || !std::isfinite(value)) {
            valid = false; return;
        }
        int exponent = 0;
        Real fraction = std::frexp(std::fabs(value), &exponent);
        u8(std::signbit(value)); u8(digits); u32(static_cast<std::uint32_t>(exponent));
        for (int i = 0; i < 4; ++i) {
            fraction = std::ldexp(fraction, 32);
            const auto limb = static_cast<std::uint32_t>(fraction);
            u32(limb); fraction -= limb;
        }
        if (fraction != 0) valid = false;
    }
};
}
#endif
