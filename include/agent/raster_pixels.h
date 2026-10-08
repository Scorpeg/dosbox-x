#ifndef DOSBOX_AGENT_RASTER_PIXELS_H
#define DOSBOX_AGENT_RASTER_PIXELS_H
#include <cstdint>
namespace dosbox_agent {
inline bool RasterRgb32MasksValid(std::uint32_t red, std::uint32_t green, std::uint32_t blue) {
    return red && green && blue && !(red & green) && !(red & blue) && !(green & blue);
}
inline std::uint32_t RasterRgb32(std::uint32_t pixel, std::uint32_t red,
                                 std::uint32_t green, std::uint32_t blue) {
    // Ignore host-added alpha; preserve only channels selected by guest RGB masks.
    return pixel & (red | green | blue);
}
}
#endif
