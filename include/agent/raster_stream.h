#ifndef DOSBOX_AGENT_RASTER_STREAM_H
#define DOSBOX_AGENT_RASTER_STREAM_H
#include "agent/agent_protocol.h"
#include <cstdint>

namespace dosbox_agent {
void AGENT_RasterReset();
void AGENT_RasterStart(bool accepted, unsigned width, unsigned height, unsigned bpp);
void AGENT_RasterLine(const void* data, unsigned width, unsigned height, unsigned bpp,
                      const std::uint8_t* palette, unsigned palette_stride,
                      std::uint32_t red_mask, std::uint32_t green_mask, std::uint32_t blue_mask);
void AGENT_RasterEnd(bool aborted);
bool AGENT_RasterCapture(bool begin, JsonValue* result, std::string* error);
}
#endif
