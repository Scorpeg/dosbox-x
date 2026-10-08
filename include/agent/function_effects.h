#ifndef DOSBOX_AGENT_FUNCTION_EFFECTS_H
#define DOSBOX_AGENT_FUNCTION_EFFECTS_H
#include "agent/agent_protocol.h"
#include "agent/memory_observer.h"
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace dosbox_agent {
// Parent is the nearest retained API scope; faulting writes retain their requested values.
class FunctionEffects {
public:
    unsigned limit = 0;
    unsigned sequence = 0;
    std::string failure;
    std::vector<JsonValue> events;
    std::vector<unsigned> pending;
    explicit FunctionEffects(unsigned maximum = 0) : limit(maximum) {}
    unsigned Begin(JsonValue event) {
        if (!failure.empty()) return 0;
        if (!limit || limit > 512 || events.size() >= limit) { failure = "function effect limit exceeded"; return 0; }
        const unsigned token = static_cast<unsigned>(events.size()) + 1;
        event.object["id"] = JsonValue::Number(std::to_string(token - 1));
        event.object["begin_order"] = JsonValue::Number(std::to_string(sequence++));
        event.object["end_order"] = JsonValue::Null();
        event.object["parent"] = pending.empty() ? JsonValue::Null() : JsonValue::Number(std::to_string(pending.back() - 1));
        events.push_back(std::move(event)); pending.push_back(token);
        return token;
    }
    void End(unsigned token, const JsonValue& completion) {
        if (!token || !failure.empty()) return;
        if (pending.empty() || pending.back() != token) { failure = "function effect completion order differs"; return; }
        for (const auto& field : completion.object) events.at(token - 1).object[field.first] = field.second;
        events.at(token - 1).object["end_order"] = JsonValue::Number(std::to_string(sequence++));
        pending.pop_back();
    }
    bool Finish() {
        if (!pending.empty() && failure.empty()) failure = "function returned with pending effect scopes";
        return failure.empty();
    }
    static bool Intersects(std::uint32_t address, unsigned bytes, std::uint32_t watch, std::uint32_t count) {
        return bytes && count && std::uint64_t(address) < std::uint64_t(watch) + count &&
               std::uint64_t(watch) < std::uint64_t(address) + bytes;
    }
};

MemoryAccessToken AGENT_EffectTransitionBegin(const char* kind, std::uint64_t number,
        std::uint64_t type, std::uint32_t old_ip, bool use32);
void AGENT_EffectTransitionEnd(MemoryAccessToken token, bool unwinding);
// Native API return does not imply successful interrupt/IRET delivery.
class EffectTransitionScope {
    MemoryAccessToken token;
public:
    EffectTransitionScope(const char* kind, std::uint64_t number, std::uint64_t type,
                          std::uint32_t old_ip, bool use32 = false) :
        token(AGENT_EffectTransitionBegin(kind, number, type, old_ip, use32)) {}
    ~EffectTransitionScope() { if (token.item) AGENT_EffectTransitionEnd(token, std::uncaught_exception()); }
    EffectTransitionScope(const EffectTransitionScope&) = delete;
    EffectTransitionScope& operator=(const EffectTransitionScope&) = delete;
};
}
#endif
