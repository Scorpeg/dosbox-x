#include "config.h"
#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "dosbox.h"
#include "agent/video_snapshot.h"
#include "agent/agent_bridge.h"
#include "cpu.h"
#include "pic.h"
#include "vga.h"

namespace dosbox_agent {
namespace {
JsonValue N(std::int64_t n) { return JsonValue::Number(std::to_string(n)); }
bool Reject(std::string* error, const char* reason) { if (error) *error = reason; return false; }
}

bool AGENT_CaptureVideo(VideoSnapshot* result, std::string* error)
{
    if (!result || !AGENT_EmulationQueue().IsBoundToCurrentThread())
        return Reject(error, "video capture requires the emulation thread");
    // Derive layout from hardware state: games can reprogram VGA without INT10.
    const bool planar = vga.mode == M_EGA;
    if (machine != MCH_VGA || svgaCard != SVGA_S3Trio || (!planar && vga.mode != M_VGA) ||
        !vga.mem.linear || vga.mem.memsize < 262144 || cpu.pmode)
        return Reject(error, "unsupported video profile: real-mode S3 VGA 320x200 indexed graphics required");
    const unsigned vdend = vga.crtc.vertical_display_end + ((vga.crtc.overflow & 2u) << 7u) +
                           ((vga.crtc.overflow & 0x40u) << 3u);
    std::string unsupported;
    const auto check = [&unsupported](bool valid, const char* name, std::int64_t value) {
        if (!valid) unsupported += std::string(" ") + name + "=" + std::to_string(value);
    };
#define C(field, expected) check(vga.field == (expected), #field, vga.field)
    C(config.chained,!planar); C(config.compatible_chain4,true); C(config.addr_shift,planar ? 0 : 2);
    C(config.scan_len,planar ? 20 : 40); C(config.display_start,0); C(config.real_start,0);
    C(config.pel_panning,0); C(config.bytes_skip,0); C(config.hlines_skip,0);
    check(vga.config.line_compare >= 400, "config.line_compare", vga.config.line_compare);
    C(crtc.horizontal_display_end,planar ? 39 : 79); check(vdend == 399, "vertical_display_end", vdend);
    check((vga.crtc.maximum_scan_line & 0x9f) == (planar ? 0x80 : 1), "crtc.maximum_scan_line", vga.crtc.maximum_scan_line);
    C(crtc.mode_control,planar ? 0xe3 : 0xa3); C(crtc.underline_location,planar ? 0 : 0x40); C(crtc.preset_row_scan,0);
    C(seq.memory_mode,planar ? 6 : 0x0e);
    if (planar) {
        // Planar lookup uses P54S palette bits, not the text-blink flag.
        check((vga.attr.mode_control & ~0x88u) == 1, "attr.mode_control", vga.attr.mode_control);
        check((vga.gfx.mode & 0x60u) == 0, "gfx.mode", vga.gfx.mode);
        check(VGA_AC_remap == AC_4x4, "attribute_remap", VGA_AC_remap);
        C(seq.clocking_mode,9); C(seq.reset,3);
        C(dosboxig.vga_acpal_bypass,false);
    } else {
        check((vga.attr.mode_control & ~8u) == 0x41, "attr.mode_control", vga.attr.mode_control);
    }
    C(attr.horizontal_pel_panning,0);
    C(attr.disabled,0); C(attr.color_plane_enable,15); C(svga.bank_read_full,0); C(svga.bank_write_full,0);
    C(s3.hgc.curmode,0); C(dac.bits,6); C(dac.reg02,0); C(dosboxig.svga,false);
#undef C
    if (!unsupported.empty()) {
        if (error) *error = "unsupported video layout:" + unsupported;
        return false;
    }

    VideoSnapshot capture;
    capture.metadata = JsonValue::Object();
    auto& m = capture.metadata.object;
    m["format"] = JsonValue::String("dosbox-proof:video-state:v1");
    m["contract"] = JsonValue::String(planar ? "vga-mode0dh-v1" : "vga-mode13h-v1");
    m["observation"] = JsonValue::String("halted-device-state-not-scanout");
    m["complete"] = JsonValue::Bool(true);
    m["layout"] = JsonValue::Object();
    auto& layout = m["layout"].object;
    layout["width"] = N(320); layout["height"] = N(200); layout["pitch"] = N(320);
    layout["start"] = N(0); layout["address_shift"] = N(planar ? 0 : 2); layout["dac_bits"] = N(vga.dac.bits);
    layout["pel_mask"] = N(vga.dac.pel_mask);
    m["position"] = JsonValue::Object();
    auto& position = m["position"].object;
    position["pic_ticks"] = N(PIC_Ticks); position["cycle_max"] = N(CPU_CycleMax);
    position["cycle_left"] = N(CPU_CycleLeft); position["cycles"] = N(CPU_Cycles);
    m["registers"] = JsonValue::Object();
    auto& regs = m["registers"].object;
    // Capture named fields, not host layouts or a complete VGA/S3 snapshot.
#define F(group, field) regs[#group "." #field] = N(vga.group.field)
    regs["misc_output"] = N(vga.misc_output);
    F(seq,index); F(seq,reset); F(seq,clocking_mode); F(seq,map_mask);
    F(seq,character_map_select); F(seq,memory_mode);
    F(gfx,index); F(gfx,set_reset); F(gfx,enable_set_reset); F(gfx,color_compare);
    F(gfx,data_rotate); F(gfx,read_map_select); F(gfx,mode); F(gfx,miscellaneous);
    F(gfx,color_dont_care); F(gfx,bit_mask);
    F(crtc,index); F(crtc,read_only); F(crtc,horizontal_total); F(crtc,horizontal_display_end);
    F(crtc,start_horizontal_blanking); F(crtc,end_horizontal_blanking);
    F(crtc,start_horizontal_retrace); F(crtc,end_horizontal_retrace);
    F(crtc,vertical_total); F(crtc,overflow); F(crtc,preset_row_scan); F(crtc,maximum_scan_line);
    F(crtc,cursor_start); F(crtc,cursor_end); F(crtc,start_address_high); F(crtc,start_address_low);
    F(crtc,cursor_location_high); F(crtc,cursor_location_low); F(crtc,vertical_retrace_start);
    F(crtc,vertical_retrace_end); F(crtc,vertical_display_end); F(crtc,offset);
    F(crtc,underline_location); F(crtc,start_vertical_blanking); F(crtc,end_vertical_blanking);
    F(crtc,mode_control); F(crtc,line_compare);
    F(attr,index); F(attr,disabled); F(attr,mode_control); F(attr,horizontal_pel_panning);
    F(attr,overscan_color); F(attr,color_plane_enable); F(attr,color_select); F(internal,attrindex);
    for (unsigned i = 0; i < 16; ++i) regs["attr.palette." + std::to_string(i)] = N(vga.attr.palette[i]);
    F(dac,bits); F(dac,pel_mask); F(dac,pel_index); F(dac,state); F(dac,write_index); F(dac,read_index);
    F(dac,hidac_counter); F(dac,reg02);
    F(config,display_start); F(config,real_start); F(config,scan_len); F(config,line_compare);
    F(config,chained); F(config,compatible_chain4); F(config,pel_panning); F(config,bytes_skip);
    F(config,hlines_skip); F(config,addr_shift); F(config,read_mode); F(config,write_mode);
    F(config,read_map_select); F(config,color_dont_care); F(config,color_compare);
    F(config,data_rotate); F(config,raster_op); F(config,full_bit_mask); F(config,full_map_mask);
    F(config,full_not_map_mask); F(config,full_set_reset); F(config,full_not_enable_set_reset);
    F(config,full_enable_set_reset); F(config,full_enable_and_set_reset);
    F(svga,bank_read_full); F(svga,bank_write_full);
#undef F
    if (planar) {
        regs["attribute_remap"] = N(VGA_AC_remap);
        // Preserve derived palette state rather than recomputing it from registers.
        for (unsigned i = 0; i < 16; ++i) {
            regs["dac.combine." + std::to_string(i)] = N(vga.dac.combine[i]);
            const unsigned index = vga.dac.combine[i] & vga.dac.pel_mask;
            capture.effective_palette.push_back(vga.dac.rgb[index].red);
            capture.effective_palette.push_back(vga.dac.rgb[index].green);
            capture.effective_palette.push_back(vga.dac.rgb[index].blue);
        }
    }
    // VRAM stores planes 0/1/2/3 interleaved per planar address.
    for (unsigned p = 0; p < 4; ++p) {
        capture.planes[p].resize(65536);
        for (unsigned a = 0; a < 65536; ++a) capture.planes[p][a] = vga.mem.linear[a * 4 + p];
        capture.latches.push_back(vga.latch.b[p]);
    }
    capture.indexed.resize(64000);
    for (unsigned a = 0; a < 64000; ++a) {
        if (planar) {
            // Row-major 4-bit indices before attribute/DAC lookup.
            unsigned color = 0;
            for (unsigned p = 0; p < 4; ++p)
                color |= ((capture.planes[p][a >> 3u] >> (7u - (a & 7u))) & 1u) << p;
            capture.indexed[a] = static_cast<std::uint8_t>(color);
        } else {
            capture.indexed[a] = vga.mem.linear[((a & ~3u) << 2u) + (a & 3u)];
        }
    }
    for (unsigned i = 0; i < 256; ++i) {
        capture.palette.push_back(vga.dac.rgb[i].red);
        capture.palette.push_back(vga.dac.rgb[i].green);
        capture.palette.push_back(vga.dac.rgb[i].blue);
    }
    *result = std::move(capture);
    return true;
}
}
#endif
