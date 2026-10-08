#include "config.h"
#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "dosbox.h"
#include "agent/raster_stream.h"
#include "agent/raster_pixels.h"
#include "agent/exec_observer.h"
#include "agent/agent_bridge.h"
#include "cpu.h"
#include "pic.h"
#include <cstring>
#include <limits>

namespace dosbox_agent {
namespace {
struct Raster {
    bool active = false, frame_open = false;
    std::uint64_t events = 0, lines = 0, starts = 0, skipped = 0, completed = 0, partial = 0, orphan = 0;
    unsigned width = 0, height = 0, bpp = 0, frame_lines = 0;
    std::string chain, frame_chain, last_frame, failure;
} raster;
JsonValue N(std::uint64_t n) { return JsonValue::Number(std::to_string(n)); }
std::string Hash(const std::string& text) {
    return AGENT_Sha256Hex(std::vector<std::uint8_t>(text.begin(), text.end()));
}
std::string Clock() {
    return std::to_string(PIC_Ticks) + ":" + std::to_string(CPU_CycleMax) + ":" +
           std::to_string(CPU_CycleLeft) + ":" + std::to_string(CPU_Cycles);
}
bool Live() { return raster.active && raster.failure.empty(); }
bool Shape(unsigned w, unsigned h, unsigned b) {
    return w > 0 && w <= 8192 && h > 0 && h <= 8192 && (b == 8 || b == 15 || b == 16 || b == 32);
}
void Event(const std::string& payload) {
    if (raster.events == std::numeric_limits<std::uint64_t>::max()) {
        raster.failure = "raster event counter exhausted";
        return;
    }
    raster.chain = Hash(raster.chain + ":" + std::to_string(raster.events++) + ":" + Clock() + ":" + payload);
}
}

void AGENT_RasterReset() { raster = Raster(); }

void AGENT_RasterStart(bool accepted, unsigned width, unsigned height, unsigned bpp) {
    if (!Live()) return;
    if (!Shape(width, height, bpp)) {
        raster.failure = "unsupported raster source dimensions or pixel format";
        return;
    }
    Event(std::string(accepted ? "B:" : "S:") + std::to_string(width) + ":" +
          std::to_string(height) + ":" + std::to_string(bpp));
    if (!accepted) { ++raster.skipped; return; }
    if (raster.frame_open) { raster.failure = "overlapping accepted raster frames"; return; }
    ++raster.starts;
    raster.frame_open = true;
    raster.width = width; raster.height = height; raster.bpp = bpp;
    raster.frame_lines = 0;
    raster.frame_chain = Hash("");
}

void AGENT_RasterLine(const void* data, unsigned width, unsigned height, unsigned bpp,
                      const std::uint8_t* palette, unsigned palette_stride,
                      std::uint32_t red_mask, std::uint32_t green_mask, std::uint32_t blue_mask) {
    if (!Live()) return;
    if (!data || !Shape(width, height, bpp) || (bpp == 8 && (!palette || palette_stride < 3))) {
        raster.failure = "invalid raster row";
        return;
    }
    if (bpp == 32 && !RasterRgb32MasksValid(red_mask, green_mask, blue_mask)) {
        raster.failure = "invalid RGB masks for a 32-bit raster source";
        return;
    }
    // Hash pre-scaling pixels in LE order, excluding padding; scratch space is at most 32768 bytes.
    const unsigned bytes = (bpp + 7) / 8;
    std::vector<std::uint8_t> row(width * bytes);
    const auto* source = static_cast<const std::uint8_t*>(data);
    for (unsigned x = 0; x < width; ++x) {
        std::uint32_t pixel = 0;
        if (bytes == 1) pixel = source[x];
        else if (bytes == 2) { std::uint16_t p; std::memcpy(&p, source + x * 2, 2); pixel = p; }
        else {
            std::memcpy(&pixel, source + x * 4, 4);
            pixel = RasterRgb32(pixel, red_mask, green_mask, blue_mask);
        }
        for (unsigned byte = 0; byte < bytes; ++byte) row[x * bytes + byte] = (pixel >> (byte * 8)) & 255;
    }
    std::string colors = "-";
    if (bpp == 8) {
        std::vector<std::uint8_t> rgb;
        rgb.reserve(768);
        for (unsigned i = 0; i < 256; ++i)
            for (unsigned component = 0; component < 3; ++component) rgb.push_back(palette[i * palette_stride + component]);
        colors = AGENT_Sha256Hex(rgb);
    }
    // Bind each row to its palette; 15/16-bit sources use fixed RGB masks.
    if (bpp == 15) { red_mask = 0x7c00; green_mask = 0x3e0; blue_mask = 0x1f; }
    if (bpp == 16) { red_mask = 0xf800; green_mask = 0x7e0; blue_mask = 0x1f; }
    if (bpp == 8) red_mask = green_mask = blue_mask = 0;
    const std::string content = std::to_string(width) + ":" + std::to_string(height) + ":" +
        std::to_string(bpp) + ":" + std::to_string(red_mask) + ":" + std::to_string(green_mask) + ":" +
        std::to_string(blue_mask) + ":" + colors + ":" + AGENT_Sha256Hex(row);
    Event("L:" + content);
    ++raster.lines;
    if (raster.frame_open) {
        if (width != raster.width || height != raster.height || bpp != raster.bpp || raster.frame_lines >= height) {
            raster.failure = "raster shape changed inside a frame or excess rows";
            return;
        }
        raster.frame_chain = Hash(raster.frame_chain + ":" + std::to_string(raster.frame_lines++) + ":" + content);
    } else ++raster.orphan; // Preserve the initial partial frame without marking it complete.
}

void AGENT_RasterEnd(bool aborted) {
    if (!Live()) return;
    Event(std::string("E:") + (aborted ? "1:" : "0:") + (raster.frame_open ? "1:" : "0:") +
          std::to_string(raster.frame_lines));
    if (raster.frame_open && !aborted && raster.frame_lines == raster.height) {
        ++raster.completed;
        raster.last_frame = raster.frame_chain;
    } else ++raster.partial;
    raster.frame_open = false;
}

bool AGENT_RasterCapture(bool begin, JsonValue* result, std::string* error) {
    if (!result || !AGENT_EmulationQueue().IsBoundToCurrentThread()) {
        if (error) *error = "raster capture requires the emulation thread";
        return false;
    }
    if (begin) {
        if (raster.active) { if (error) *error = "raster epoch already active"; return false; }
        raster = Raster(); raster.active = true;
        raster.chain = raster.frame_chain = raster.last_frame = Hash("");
    }
    if (!raster.active || !raster.failure.empty()) {
        if (error) *error = raster.active ? raster.failure : "raster epoch has not begun";
        return false;
    }
    JsonValue capture = JsonValue::Object();
    auto& c = capture.object;
    c["contract"] = JsonValue::String("raster-submitted-v1");
    c["observation"] = JsonValue::String("render-submissions-not-physical-scanout");
    c["whole_video_qualified"] = JsonValue::Bool(false);
    c["storage"] = JsonValue::Object();
    auto& s = c["storage"].object;
#define F(field) s[#field] = N(raster.field)
    F(events); F(lines); F(starts); F(skipped); F(completed); F(partial); F(orphan);
    F(width); F(height); F(bpp); F(frame_lines);
#undef F
    s["frame_open"] = JsonValue::Bool(raster.frame_open);
    s["chain_sha256"] = JsonValue::String(raster.chain);
    s["frame_chain_sha256"] = JsonValue::String(raster.frame_chain);
    s["last_frame_sha256"] = JsonValue::String(raster.last_frame);
    c["position"] = JsonValue::Object();
    auto& p = c["position"].object;
    p["pic_ticks"] = N(PIC_Ticks); p["cycle_max"] = JsonValue::Number(std::to_string(CPU_CycleMax));
    p["cycle_left"] = JsonValue::Number(std::to_string(CPU_CycleLeft)); p["cycles"] = JsonValue::Number(std::to_string(CPU_Cycles));
    *result = std::move(capture);
    return true;
}
}
#endif
