#ifndef DOSBOX_AGENT_AUDIO_SINK_H
#define DOSBOX_AGENT_AUDIO_SINK_H
#include <array>
#include <cstddef>
#include <cstdint>

namespace dosbox_agent {
// Host output must not drive guest synthesis or timing; caller serializes access.
template<std::size_t Capacity> struct AudioSinkQueue {
    static_assert(Capacity > 0, "audio queue must have storage");
    struct Frame { std::int16_t left, right; };
    std::array<Frame, Capacity> frames{};
    std::size_t first = 0, queued = 0;
    std::uint64_t enqueued = 0, consumed = 0, dropped = 0, underrun = 0, muted = 0;

    void Push(std::int16_t left, std::int16_t right) {
        if (queued == Capacity) {
            first = (first + 1) % Capacity;
            --queued;
            ++dropped; // Never block synthesis on the host consumer.
        }
        frames[(first + queued) % Capacity] = {left, right};
        ++queued;
        ++enqueued;
    }
    void Pull(std::int16_t* output, std::size_t count, bool silence) {
        for (std::size_t i = 0; i < count; ++i) {
            Frame sample{0, 0};
            if (queued) {
                sample = frames[first];
                first = (first + 1) % Capacity;
                --queued;
                ++consumed;
            } else {
                ++underrun; // Host silence is not emulated output.
            }
            if (silence) { sample = {0, 0}; ++muted; }
            output[i * 2] = sample.left;
            output[i * 2 + 1] = sample.right;
        }
    }
};
}
#endif
