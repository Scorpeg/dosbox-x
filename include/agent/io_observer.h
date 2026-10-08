#ifndef DOSBOX_AGENT_IO_OBSERVER_H
#define DOSBOX_AGENT_IO_OBSERVER_H
#include "agent/memory_observer.h"

namespace dosbox_agent {
// Observe API results once, including V86; never reread a device or infer bus transactions.
MemoryAccessToken AGENT_EffectIoBegin(std::uint64_t port, unsigned bytes, bool write, std::uint32_t value);
void AGENT_EffectIoEnd(MemoryAccessToken token, bool unwinding, bool has_value, std::uint32_t value);
class EffectIoScope {
    MemoryAccessToken token;
public:
    EffectIoScope(std::uint64_t port, unsigned bytes, bool write, std::uint32_t value = 0) :
        token(AGENT_EffectIoBegin(port, bytes, write, value)) {}
    void Returned() {
        if (token.item) AGENT_EffectIoEnd(token, false, false, 0);
        token.item = 0;
    }
    template<class T> T Returned(T value) {
        if (token.item) AGENT_EffectIoEnd(token, false, true, value);
        token.item = 0;
        return value;
    }
    ~EffectIoScope() { if (token.item) AGENT_EffectIoEnd(token, true, false, 0); }
    EffectIoScope(const EffectIoScope&) = delete;
    EffectIoScope& operator=(const EffectIoScope&) = delete;
};
}
#endif
