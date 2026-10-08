#ifndef DOSBOX_AGENT_CALLBACK_OBSERVER_H
#define DOSBOX_AGENT_CALLBACK_OBSERVER_H
#include "callback.h"
#include "agent/function_effects.h"

namespace dosbox_agent {
// Identify callbacks by implementation, not mutable slots or host addresses.
const char* AGENT_BiosCallbackIdentity(CallBack_Handler handler, bool context = false);
JsonValue AGENT_BiosCallbackContext(CallBack_Handler handler);
// Read backing memory without guest/device calls.
JsonValue AGENT_CallbackBacking(std::uint64_t address, unsigned bytes);
MemoryAccessToken AGENT_EffectCallbackBegin(unsigned index, CallBack_Handler handler);
void AGENT_EffectCallbackEnd(MemoryAccessToken token, bool unwinding, std::uint64_t returned, CallBack_Handler handler);
class EffectCallbackScope {
    MemoryAccessToken token;
    CallBack_Handler handler;
public:
    EffectCallbackScope(unsigned index, CallBack_Handler target) : token(AGENT_EffectCallbackBegin(index, target)), handler(target) {}
    void Returned(std::uint64_t value) {
        if (token.item) AGENT_EffectCallbackEnd(token, false, value, handler);
        token.item = 0;
    }
    ~EffectCallbackScope() { if (token.item) AGENT_EffectCallbackEnd(token, true, 0, handler); }
    EffectCallbackScope(const EffectCallbackScope&) = delete;
    EffectCallbackScope& operator=(const EffectCallbackScope&) = delete;
};
}
#endif
