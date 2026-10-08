#ifndef DOSBOX_AGENT_AUDIO_CAPTURE_H
#define DOSBOX_AGENT_AUDIO_CAPTURE_H
#include "agent/agent_protocol.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dosbox_agent {
// Hash pre-host-output S16LE samples without retaining PCM.
struct AudioHistory {
    using Hash = std::function<std::string(const std::vector<std::uint8_t>&)>;
    static constexpr unsigned BlockFrames = 256;
    bool active = false;
    unsigned rate = 0;
    std::uint64_t origin = 0, frames = 0, blocks = 0, silent = 0, block_silent = 0;
    std::uint64_t padding_calls = 0, padding_frames = 0;
    std::string chain, padding_chain, failure;
    std::vector<std::uint8_t> pending;
    bool Begin(unsigned hz, std::uint64_t first, const Hash& hash) {
        if (active || !hz || hz > 192000) return false;
        *this = AudioHistory(); active = true; rate = hz; origin = first;
        chain = padding_chain = hash({});
        pending.reserve(BlockFrames * 4);
        return true;
    }
    void Fail(const std::string& why) { if (failure.empty()) failure = why; }
    static std::string TextHash(const std::string& text, const Hash& hash) {
        return hash(std::vector<std::uint8_t>(text.begin(), text.end()));
    }
    void Frame(unsigned hz, std::uint64_t index, std::int16_t left, std::int16_t right, const Hash& hash) {
        if (!active || !failure.empty()) return;
        if (hz != rate || index != origin + frames || frames == UINT64_C(1000000000000)) {
            Fail("audio rate, source index or frame bound changed"); return;
        }
        for (auto sample : {left, right}) {
            const auto bits = static_cast<std::uint16_t>(sample);
            pending.push_back(bits & 255); pending.push_back(bits >> 8);
        }
        ++frames;
        if (!left && !right) { ++silent; ++block_silent; }
        if (pending.size() == BlockFrames * 4) {
            chain = TextHash("mixer-rendered-v1:" + chain + ":" + std::to_string(rate) +
                    ":2:s16le:256:" + std::to_string(blocks * BlockFrames) + ":" +
                    std::to_string(block_silent) + ":" + hash(pending), hash);
            ++blocks; pending.clear(); block_silent = 0;
        }
    }
    void Padding(const char* channel, std::uint64_t index, unsigned missing, const Hash& hash) {
        if (!active || !failure.empty()) return;
        if (index < origin || padding_calls == UINT64_C(1000000000000)) {
            Fail("audio padding position or count invalid"); return;
        }
        ++padding_calls; padding_frames += missing;
        // Marks channel interpolation padding, not host underruns.
        const std::string name(channel);
        padding_chain = TextHash("mixer-padding-v1:" + padding_chain + ":" +
                std::to_string(index - origin) + ":" + std::to_string(name.size()) + ":" + name + ":" +
                std::to_string(missing), hash);
    }
};

bool AGENT_AudioCapture(bool begin, JsonValue* result, std::string* error);
void AGENT_AudioReset();
JsonValue AGENT_SbPlaybackStorage();
std::string AGENT_Sha256Hex(const std::vector<std::uint8_t>& data);
}
#endif
