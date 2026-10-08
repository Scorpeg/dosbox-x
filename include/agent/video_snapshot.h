#ifndef DOSBOX_AGENT_VIDEO_SNAPSHOT_H
#define DOSBOX_AGENT_VIDEO_SNAPSHOT_H
#include "agent/agent_protocol.h"
#include <vector>
#include <cstdint>

namespace dosbox_agent {
struct VideoSnapshot {
    JsonValue metadata;
    std::vector<std::uint8_t> indexed;
    std::vector<std::uint8_t> palette;
    std::vector<std::uint8_t> effective_palette;
    std::vector<std::uint8_t> planes[4];
    std::vector<std::uint8_t> latches;
};
bool AGENT_CaptureVideo(VideoSnapshot* result, std::string* error);
}
#endif
