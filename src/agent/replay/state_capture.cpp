#include "config.h"
#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "dosbox.h"
#include "agent/state_capture.h"
#include "agent/function_boundary.h"
#include "agent/function_effects.h"
#include "agent/callback_observer.h"
#include "agent/io_observer.h"
#include "agent/audio_capture.h"
#include "agent/opl_capture.h"
#include "agent/speaker_capture.h"
#include "agent/fpu_capture.h"
#include "agent/agent_bridge.h"
#include "agent/exec_observer.h"
#include "agent/raster_stream.h"
#include "agent/replay_timeline.h"
#include "cpu.h"
#include "paging.h"
#include "pic.h"
#include "../../cpu/lazyflags.h"
#include <algorithm>
#include <limits>

extern size_t MemSize;
extern void* memory_file_base;
extern size_t memory_file_size;
extern bool CPU_IsHLTed();
extern bool MOUSE_IsLocked();

namespace dosbox_agent {
namespace {
JsonValue N(std::int64_t n) { return JsonValue::Number(std::to_string(n)); }
bool Reject(std::string* error, const char* reason) { if (error) *error = reason; return false; }
std::string Text(const JsonValue* value) { return value && value->type == JsonType::String ? value->text : ""; }
bool U64(const JsonValue* value, std::uint64_t* n) {
    if (!value || value->type != JsonType::Number || value->text.empty()) return false;
    *n = 0;
    for (char c : value->text) {
        if (c < '0' || c > '9' || *n > (std::numeric_limits<std::uint64_t>::max() - (c - '0')) / 10) return false;
        *n = *n * 10 + (c - '0');
    }
    return true;
}
std::string Hash(const unsigned char* ptr, size_t size) {
    return AGENT_Sha256Hex(std::vector<std::uint8_t>(ptr, ptr + size));
}
JsonValue Extent(const unsigned char* ptr, std::uint64_t base, std::uint64_t size) {
    // Hash bounded blocks as ASCII previous:physical-address:byte-count:sha256(block), without newline.
    std::string chain = AGENT_Sha256Hex({});
    for (std::uint64_t offset = 0; offset < size;) {
        const size_t count = static_cast<size_t>(std::min<std::uint64_t>(1048576, size - offset));
        const std::string row = chain + ":" + std::to_string(base + offset) + ":" + std::to_string(count) + ":" + Hash(ptr + offset, count);
        chain = AGENT_Sha256Hex(std::vector<std::uint8_t>(row.begin(), row.end()));
        offset += count;
    }
    JsonValue result = JsonValue::Object();
    result.object["base"] = N(base); result.object["bytes"] = N(size);
    result.object["chain_sha256"] = JsonValue::String(chain);
    return result;
}
JsonValue Cpu() {
    JsonValue result = JsonValue::Object();
    auto& c = result.object;
    c["mode"] = JsonValue::String(!cpu.pmode ? "real" : (reg_flags & FLAG_VM) ? "v86" : "protected");
#define R(field) c[#field] = N(field)
    R(reg_eax); R(reg_ebx); R(reg_ecx); R(reg_edx); R(reg_esi); R(reg_edi); R(reg_ebp); R(reg_esp);
    R(reg_eip); R(reg_flags); R(cpu.cpl); R(cpu.mpl); R(cpu.cr0); R(cpu.cr4); R(cpu.pmode);
    R(cpu.masks.eflags); R(cpu.stack.mask); R(cpu.stack.notmask); R(cpu.stack.big); R(cpu.code.big);
    R(cpu.hlt.cs); R(cpu.hlt.eip); R(cpu.exception.which); R(cpu.exception.error);
    R(cpu.direction); R(cpu.trap_skip);
    R(lf_var1d); R(lf_var2d); R(lf_resd); R(lflags.type); R(lflags.prev_type); R(lflags.oldcf);
    R(paging.cr2); R(paging.cr3); R(paging.base.page); R(paging.base.addr); R(paging.enabled); R(paging.wp);
    R(CPU_ArchitectureType);
#undef R
    c["gdt.base"] = N(cpu.gdt.GetBase()); c["gdt.limit"] = N(cpu.gdt.GetLimit());
    c["ldt.base"] = N(cpu.gdt.LdtBase()); c["ldt.limit"] = N(cpu.gdt.LdtLimit()); c["ldt.selector"] = N(cpu.gdt.SLDT());
    c["idt.base"] = N(cpu.idt.GetBase()); c["idt.limit"] = N(cpu.idt.GetLimit());
    for (unsigned i = 0; i < 8; ++i) {
        c["dr." + std::to_string(i)] = N(cpu.drx[i]); c["tr." + std::to_string(i)] = N(cpu.trx[i]);
    }
    for (unsigned i = 0; i < 6; ++i) {
        const std::string name = "segment." + std::to_string(i);
        c[name + ".selector"] = N(Segs.val[i]); c[name + ".base"] = N(Segs.phys[i]);
        c[name + ".limit"] = N(Segs.limit[i]); c[name + ".expanddown"] = N(Segs.expanddown[i]);
    }
    c["task"] = AGENT_CpuTaskStorage();
    return result;
}

InstructionWindow window;
std::uint64_t memory_epoch = 1;
std::uint64_t watch_address = 0, watch_bytes = 0, guard_address = 0, guard_bytes = 0;
std::uint64_t code_address = 0, code_bytes = 0;
JsonValue window_configuration;
std::vector<std::string> window_devices;
FunctionBoundary function;
struct FunctionRange { std::string name, base; std::uint32_t offset = 0, bytes = 0; };
struct FunctionCase {
    JsonValue entry, exit;
    std::vector<std::uint64_t> addresses;
    std::vector<std::vector<std::uint8_t>> before, after;
    std::uint32_t return_ip = 0, return_esp = 0;
    unsigned return_cs = 0;
    unsigned return_ss = 0;
    std::uint32_t stack_base = 0;
    FunctionEffects effects;
};
std::vector<FunctionRange> function_ranges;
std::vector<FunctionCase> function_cases;
JsonValue function_configuration;
std::uint64_t function_code = 0, function_code_bytes = 0, function_guard = 0, function_guard_bytes = 0;
unsigned function_cs = 0;
std::uint32_t function_ip = 0;
std::string function_code_hash;
bool function_effects = false;
bool function_bindings = false;
bool function_io = false;
bool function_context = false;
unsigned function_effect_limit = 0;
struct EffectRange { std::uint32_t address = 0, bytes = 0; };
std::vector<EffectRange> function_effect_writes;
std::vector<EffectRange> function_effect_ports;
const char* FunctionContract() {
    return function_context ? "function-boundary-effects-flat32-v4" :
           function_io ? "function-boundary-effects-flat32-v3" :
           function_bindings ? "function-boundary-effects-flat32-v2" :
           function_effects ? "function-boundary-effects-flat32-v1" : "function-boundary-flat32-v1";
}
JsonValue Position() {
    JsonValue value = JsonValue::Object();
    value.object["pic_ticks"] = N(PIC_Ticks); value.object["cycle_max"] = N(CPU_CycleMax);
    value.object["cycle_left"] = N(CPU_CycleLeft); value.object["cycles"] = N(CPU_Cycles);
    return value;
}
std::string Hex(const unsigned char* bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string result; result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15];
    }
    return result;
}
bool Backing(std::uint64_t address, std::uint64_t size) {
    return MemBase && size && address < MemSize && size <= MemSize - address;
}
#include "module_events.inc"
bool EffectReady() {
    return function_effects && function.armed && function.open && function.CheckTick(PIC_Ticks) && !function_cases.empty();
}
JsonValue EffectCpu() {
    // Capture raw and lazy flags without calling FillFlags.
    const auto all = Cpu();
    JsonValue result = JsonValue::Object();
    for (const char* key : {"mode", "reg_eax", "reg_ebx", "reg_ecx", "reg_edx", "reg_esi", "reg_edi", "reg_ebp", "reg_esp", "reg_eip",
                           "reg_flags", "lf_var1d", "lf_var2d", "lf_resd", "lflags.type", "lflags.prev_type", "lflags.oldcf",
                           "cpu.code.big", "cpu.stack.big", "cpu.stack.mask", "cpu.stack.notmask", "cpu.cpl", "cpu.pmode", "paging.enabled",
                           "segment.1.selector", "segment.1.base", "segment.2.selector", "segment.2.base"})
        result.object[key] = *all.Find(key);
    if (function_bindings) {
        result.object["idt.base"] = *all.Find("idt.base");
        result.object["idt.limit"] = *all.Find("idt.limit");
    }
    return result;
}
JsonValue EffectSite() {
    if (!AGENT_MemoryInstructionSite.active) return JsonValue::Null();
    JsonValue result = JsonValue::Object();
    result.object["cs"] = N(AGENT_MemoryInstructionSite.cs);
    result.object["ip"] = N(AGENT_MemoryInstructionSite.ip);
    result.object["mode"] = N(AGENT_MemoryInstructionSite.mode);
    return result;
}
void EffectFailure() {
    if (!function_cases.empty() && !function_cases.back().effects.failure.empty())
        function.Fail(function_cases.back().effects.failure);
    if (!function.failure.empty()) {
        AGENT_MemoryAccessEnabled = false;
        AGENT_InstructionWindowEnabled = false;
    }
}
MemoryAccessToken EffectBegin(JsonValue event) {
    MemoryAccessToken token;
    if (!EffectReady()) { EffectFailure(); return token; }
    event.object["begin"] = Position(); event.object["end"] = JsonValue::Null();
    event.object["instruction"] = EffectSite();
    event.object["outcome"] = JsonValue::String("pending");
    token.epoch = memory_epoch; token.collector = 1;
    token.item = function_cases.back().effects.Begin(std::move(event));
    EffectFailure();
    return token;
}
bool FunctionMapping() {
    // Check this capture profile without descriptor fetches, page walks or handler reads.
    return cpu.pmode && !(reg_flags & FLAG_VM) && cpu.code.big && cpu.stack.big &&
           !paging.enabled && MEM_A20_Enabled() && Segs.phys[cs] == 0 && Segs.phys[ds] == 0 && Segs.phys[es] == 0;
}
std::uint64_t FunctionBase(const std::string& name) {
    if (name == "eax") return reg_eax;
    if (name == "ebx") return reg_ebx;
    if (name == "ecx") return reg_ecx;
    if (name == "edx") return reg_edx;
    if (name == "esi") return reg_esi;
    if (name == "edi") return reg_edi;
    if (name == "ebp") return reg_ebp;
    if (name == "esp") return reg_esp;
    return 0;
}
JsonValue FunctionSnapshot(FunctionCase& selected, bool entry) {
    JsonValue result = JsonValue::Object();
    result.object["cpu"] = Cpu(); result.object["position"] = Position();
    result.object["code_sha256"] = JsonValue::String(Hash(MemBase + function_code, function_code_bytes));
    auto& stored = entry ? selected.before : selected.after;
    result.object["ranges"] = JsonValue::Array();
    for (size_t i = 0; i < function_ranges.size(); ++i) {
        const auto& range = function_ranges[i];
        const auto address = selected.addresses[i];
        stored.emplace_back(MemBase + address, MemBase + address + range.bytes);
        JsonValue descriptor = JsonValue::Object();
        descriptor.object["name"] = JsonValue::String(range.name);
        descriptor.object["address"] = N(address); descriptor.object["bytes"] = N(range.bytes);
        descriptor.object["sha256"] = JsonValue::String(AGENT_Sha256Hex(stored.back()));
        result.object["ranges"].array.push_back(descriptor);
    }
    // Hash the guard minus separately captured ranges, using one 1 MiB scratch block.
    std::string chain = AGENT_Sha256Hex({});
    for (std::uint64_t at = function_guard; at < function_guard + function_guard_bytes;) {
        const auto count = std::min<std::uint64_t>(1048576, function_guard + function_guard_bytes - at);
        std::vector<std::uint8_t> bytes(MemBase + at, MemBase + at + count);
        for (size_t i = 0; i < function_ranges.size(); ++i) {
            const auto first = std::max(at, selected.addresses[i]);
            const auto last = std::min(at + count, selected.addresses[i] + function_ranges[i].bytes);
            if (first < last) std::fill(bytes.begin() + (first - at), bytes.begin() + (last - at), 0);
        }
        const auto row = chain + ":" + std::to_string(at) + ":" + std::to_string(count) + ":" + AGENT_Sha256Hex(bytes);
        chain = AGENT_Sha256Hex(std::vector<std::uint8_t>(row.begin(), row.end()));
        at += count;
    }
    result.object["outside_ranges_sha256"] = JsonValue::String(chain);
    return result;
}
void FunctionObserve() {
    if (!function.armed) return;
    const bool entry = SegValue(cs) == function_cs && reg_eip == function_ip;
    const bool returned = function.open && SegValue(cs) == function_cases.back().return_cs &&
                          reg_eip == function_cases.back().return_ip && reg_esp == function_cases.back().return_esp &&
                          SegValue(ss) == function_cases.back().return_ss && Segs.phys[ss] == function_cases.back().stack_base;
    const auto event = function.Observe(entry, returned, PIC_Ticks);
    if (event == FunctionBoundary::None) return;
    if (!FunctionMapping() || !Backing(function_code, function_code_bytes) ||
        !Backing(function_guard, function_guard_bytes) || Hash(MemBase + function_code, function_code_bytes) != function_code_hash) {
        function.Fail("function boundary mapping/backing/code identity differs"); return;
    }
    if (event == FunctionBoundary::Entry) {
        const auto stack = static_cast<std::uint64_t>(Segs.phys[ss]) + reg_esp;
        if (reg_esp > UINT32_MAX - 4 || !Backing(stack, 4) || reg_esp > Segs.limit[ss] || Segs.limit[ss] - reg_esp < 3) {
            function.Fail("function return stack is not bounded 32-bit backing"); return;
        }
        FunctionCase selected;
        selected.effects.limit = function_effect_limit;
        selected.return_cs = SegValue(cs); selected.return_esp = reg_esp + 4;
        selected.return_ss = SegValue(ss); selected.stack_base = Segs.phys[ss];
        for (unsigned i = 0; i < 4; ++i) selected.return_ip |= static_cast<std::uint32_t>(MemBase[stack + i]) << (8 * i);
        if (selected.return_ip == function_ip || selected.return_ip > Segs.limit[cs]) {
            function.Fail("function return address is invalid/recursive"); return;
        }
        for (const auto& range : function_ranges) {
            const auto address = FunctionBase(range.base) + range.offset;
            if (address > UINT32_MAX || !Backing(address, range.bytes) || address < function_guard ||
                address - function_guard > function_guard_bytes || range.bytes > function_guard_bytes - (address - function_guard)) {
                function.Fail("resolved function range exceeds backing/guard"); return;
            }
            selected.addresses.push_back(address);
        }
        selected.entry = FunctionSnapshot(selected, true);
        function_cases.push_back(std::move(selected));
    } else {
        auto& selected = function_cases.back();
        if (function_effects && !selected.effects.Finish()) { function.Fail(selected.effects.failure); return; }
        // Freeze ranges at entry; output registers must not rebind them.
        selected.exit = FunctionSnapshot(selected, false);
    }
}

bool FunctionCommand(const JsonValue& params, JsonValue* result, std::string* error) {
    const auto action = Text(params.Find("action"));
    const bool with_context = Text(params.Find("contract")) == "function-boundary-effects-flat32-v4";
    const bool with_io = with_context || Text(params.Find("contract")) == "function-boundary-effects-flat32-v3";
    const bool with_bindings = with_io || Text(params.Find("contract")) == "function-boundary-effects-flat32-v2";
    const bool with_effects = with_bindings || Text(params.Find("contract")) == "function-boundary-effects-flat32-v1";
    if (action == "state-function-arm") {
        std::uint64_t cs = 0, ip = 0, count = 0, limit = 0, first = 0, end = 0, code = 0, bytes = 0, guard = 0, guard_bytes = 0;
        if (function.used || window.armed || !window.cases.empty() || modules.used) return Reject(error, "capture already armed/used");
        const auto* ranges = params.Find("ranges");
        const auto identity = Text(params.Find("code_sha256"));
        if (cpudecoder != CPU_Core_Normal_Run || !U64(params.Find("entry_cs"), &cs) || cs > 65535 ||
            !U64(params.Find("entry_ip"), &ip) || ip > UINT32_MAX ||
            !U64(params.Find("case_limit"), &count) || !count || count > 8 ||
            !U64(params.Find("instruction_limit"), &limit) || !limit || limit > 10000000 ||
            !U64(params.Find("first_pic_tick"), &first) || !U64(params.Find("end_pic_tick"), &end) || first < PIC_Ticks || first >= end || end > 1000000000 ||
            !U64(params.Find("code_address"), &code) || !U64(params.Find("code_bytes"), &bytes) || bytes > 4096 || !Backing(code, bytes) ||
            code != ip || identity.size() != 64 || identity.find_first_not_of("0123456789abcdef") != std::string::npos ||
            !U64(params.Find("guard_address"), &guard) || !U64(params.Find("guard_bytes"), &guard_bytes) ||
            guard_bytes > 67108864 || !Backing(guard, guard_bytes) ||
            !ranges || ranges->type != JsonType::Array || ranges->array.empty() || ranges->array.size() > 8)
            return Reject(error, "invalid flat32 function capture configuration");
        std::vector<FunctionRange> configured;
        std::uint64_t total = 0;
        for (const auto& item : ranges->array) {
            const auto name = Text(item.Find("name")), base = Text(item.Find("base"));
            std::uint64_t offset = 0, size = 0;
            if (item.type != JsonType::Object || item.object.size() != 4 || name.empty() || name.size() > 32 ||
                name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos ||
                (base != "absolute" && base != "eax" && base != "ebx" && base != "ecx" && base != "edx" && base != "esi" && base != "edi" && base != "ebp" && base != "esp") ||
                !U64(item.Find("offset"), &offset) || offset > UINT32_MAX || !U64(item.Find("bytes"), &size) || !size || size > 1048576)
                return Reject(error, "invalid function range descriptor");
            for (const auto& previous : configured) if (previous.name == name) return Reject(error, "duplicate function range name");
            FunctionRange range; range.name = name; range.base = base; range.offset = static_cast<std::uint32_t>(offset); range.bytes = static_cast<std::uint32_t>(size);
            configured.push_back(range); total += size;
        }
        if (total * count * 2 > 16777216) return Reject(error, "function capture exceeds 16 MiB retained byte budget");
        std::vector<EffectRange> effect_ranges;
        std::vector<EffectRange> port_ranges;
        std::uint64_t effect_limit = 0;
        if (with_effects) {
            const auto* effects = params.Find("effects");
            const auto* writes = effects ? effects->Find("writes") : nullptr;
            const auto* bindings = effects ? effects->Find("bindings") : nullptr;
            const auto* ports = effects ? effects->Find("io") : nullptr;
            const auto* context = effects ? effects->Find("callback_context") : nullptr;
            if (!effects || effects->type != JsonType::Object || effects->object.size() != (with_context ? 5u : with_io ? 4u : with_bindings ? 3u : 2u) ||
                (with_context ? (!context || context->type != JsonType::Bool || !context->boolean) : context != nullptr) ||
                (with_io ? (!ports || ports->type != JsonType::Array || ports->array.empty() || ports->array.size() > 8) : ports != nullptr) ||
                (with_bindings ? (!bindings || bindings->type != JsonType::Bool || !bindings->boolean) : bindings != nullptr) ||
                !U64(effects->Find("event_limit"), &effect_limit) || !effect_limit || effect_limit > 512 || effect_limit * count > 1024 ||
                !writes || writes->type != JsonType::Array || writes->array.empty() || writes->array.size() > 8)
                return Reject(error, "invalid bounded function effects configuration");
            for (const auto& item : writes->array) {
                std::uint64_t address = 0, length = 0;
                if (item.type != JsonType::Object || item.object.size() != 2 || !U64(item.Find("address"), &address) ||
                    !U64(item.Find("bytes"), &length) || !length || length > 1048576 || address > UINT32_MAX ||
                    !Backing(address, length) || address < guard || address + length > guard + guard_bytes)
                    return Reject(error, "invalid function effect write range");
                EffectRange selected; selected.address = static_cast<std::uint32_t>(address); selected.bytes = static_cast<std::uint32_t>(length);
                effect_ranges.push_back(selected);
            }
            if (with_io) for (const auto& item : ports->array) {
                std::uint64_t port = 0, length = 0;
                if (item.type != JsonType::Object || item.object.size() != 2 || !U64(item.Find("port"), &port) || port > 65535 ||
                    !U64(item.Find("bytes"), &length) || !length || length > 65536 || port + length > 65536)
                    return Reject(error, "invalid function effect port range");
                EffectRange selected; selected.address = static_cast<std::uint32_t>(port); selected.bytes = static_cast<std::uint32_t>(length);
                port_ranges.push_back(selected);
            }
        } else if (params.Find("effects")) return Reject(error, "effects require the versioned effect contract");
        if (!function.Arm(static_cast<unsigned>(count), limit, first, end)) return Reject(error, "invalid function interval/limits");
        function_effects = with_effects; function_bindings = with_bindings; function_io = with_io; function_context = with_context;
        function_effect_limit = static_cast<unsigned>(effect_limit);
        function_effect_writes = std::move(effect_ranges);
        function_effect_ports = std::move(port_ranges);
        function_ranges = std::move(configured); function_cs = static_cast<unsigned>(cs); function_ip = static_cast<std::uint32_t>(ip);
        function_code = code; function_code_bytes = bytes; function_code_hash = identity;
        function_guard = guard; function_guard_bytes = guard_bytes;
        function_configuration = JsonValue::Object();
        for (const char* name : {"entry_cs", "entry_ip", "case_limit", "instruction_limit", "first_pic_tick", "end_pic_tick",
                                "code_address", "code_bytes", "code_sha256", "guard_address", "guard_bytes", "ranges"})
            function_configuration.object[name] = *params.Find(name);
        if (with_effects) function_configuration.object["effects"] = *params.Find("effects");
        AGENT_InstructionWindowEnabled = true;
    } else if (action != "state-function-read" && action != "state-function-bytes") return Reject(error, "unknown function capture action");
    if (!function.used) return Reject(error, "function capture was not armed");
    if (with_effects != function_effects || with_bindings != function_bindings || with_io != function_io || with_context != function_context)
        return Reject(error, "function effect contract changed after arm");
    function.CheckTick(PIC_Ticks);
    if (function.armed && !InstructionWindow::DecoderCompatible(cpudecoder == CPU_Core_Normal_Run, CPU_IsHLTed(), cpu.hlt.old_decoder == CPU_Core_Normal_Run))
        function.Fail("CPU decoder changed during function capture");
    std::uint64_t index = 0;
    if (action != "state-function-arm" && (!U64(params.Find("index"), &index) || index > function_cases.size()))
        return Reject(error, "invalid function case index");
    JsonValue value = JsonValue::Object();
    value.object["contract"] = JsonValue::String(FunctionContract());
    if (action == "state-function-bytes") {
        std::uint64_t range = 0, offset = 0, bytes = 0;
        const auto side = Text(params.Find("side"));
        if (!function.failure.empty() || index >= function.completed || index >= function_cases.size() ||
            !U64(params.Find("range"), &range) || range >= function_ranges.size() ||
            (side != "entry" && side != "exit") || !U64(params.Find("offset"), &offset) ||
            !U64(params.Find("bytes"), &bytes) || !bytes || bytes > 65536)
            return Reject(error, "invalid/incomplete function byte request");
        const auto& selected = function_cases[index];
        const auto& data = (side == "entry" ? selected.before : selected.after)[range];
        if (offset >= data.size() || bytes > data.size() - offset) return Reject(error, "function bytes outside captured range");
        value.object["index"] = N(index); value.object["range"] = N(range); value.object["side"] = JsonValue::String(side);
        value.object["offset"] = N(offset); value.object["bytes"] = N(bytes);
        value.object["sha256"] = JsonValue::String(Hash(data.data() + offset, bytes));
        value.object["data_hex"] = JsonValue::String(Hex(data.data() + offset, bytes));
    } else {
        value.object["configuration"] = function_configuration;
        value.object["phase"] = JsonValue::String("normal-core-before-decode-after-cycle-decrement");
        value.object["armed"] = JsonValue::Bool(function.armed); value.object["open"] = JsonValue::Bool(function.open);
        value.object["complete"] = JsonValue::Bool(function.complete); value.object["completed"] = N(function.completed);
        value.object["failure"] = JsonValue::String(function.failure); value.object["index"] = N(index);
        if (index < function.completed && index < function_cases.size()) {
            const auto& selected = function_cases[index];
            JsonValue selected_case = JsonValue::Object();
            selected_case.object["entry"] = selected.entry; selected_case.object["exit"] = selected.exit;
            selected_case.object["return_cs"] = N(selected.return_cs); selected_case.object["return_ip"] = N(selected.return_ip);
            selected_case.object["return_esp"] = N(selected.return_esp);
            if (function_effects) {
                selected_case.object["effects"] = JsonValue::Array();
                selected_case.object["effects"].array = selected.effects.events;
            }
            value.object["case"] = selected_case;
        }
    }
    AGENT_InstructionWindowEnabled = window.armed || function.armed;
    *result = value;
    return true;
}
bool WindowDevice(const std::string& name, JsonValue* result, std::string* error) {
    if (name == "fp-simd-storage-v2") {
#if C_FPU
        return AGENT_FpuCapture(result, error);
#else
        return Reject(error, "FPU/SIMD instruction windows require an FPU-enabled build");
#endif
    }
    if (name == "pit-speaker-state-v1") return AGENT_SpeakerCapture(result, error);
    if (name == "opl-dbopl-state-v1") return AGENT_OplCapture(result, error);
    return Reject(error, "unsupported instruction-window device contract");
}
JsonValue WindowFrame(bool boundary) {
    JsonValue frame = JsonValue::Object(), position = JsonValue::Object();
    position.object["pic_ticks"] = N(PIC_Ticks); position.object["cycle_max"] = N(CPU_CycleMax);
    position.object["cycle_left"] = N(CPU_CycleLeft); position.object["cycles"] = N(CPU_Cycles);
    frame.object["position"] = position;
    frame.object["cpu"] = Cpu();
    frame.object["memory_hex"] = JsonValue::String(Hex(MemBase + watch_address, watch_bytes));
    if (!window_devices.empty()) {
        auto devices = JsonValue::Object();
        for (const auto& name : window_devices) {
            JsonValue value; std::string error;
            // Capture storage only; do not run devices, callbacks or save-state handlers.
            if (!WindowDevice(name, &value, &error)) {
                window.Fail("instruction-window device capture failed: " + name + ": " + error);
                return frame;
            }
            devices.object[name] = std::move(value);
        }
        frame.object["devices"] = std::move(devices);
    }
    if (boundary) {
        frame.object["code_sha256"] = JsonValue::String(Hash(MemBase + code_address, code_bytes));
        // Hash the guard outside the window; consumers must also check captured window bytes.
        std::vector<std::uint8_t> guard(MemBase + guard_address, MemBase + guard_address + guard_bytes);
        std::fill(guard.begin() + (watch_address - guard_address),
                  guard.begin() + (watch_address - guard_address) + watch_bytes, 0);
        frame.object["outside_window_sha256"] = JsonValue::String(AGENT_Sha256Hex(guard));
    }
    return frame;
}

bool WindowCommand(const JsonValue& params, JsonValue* result, std::string* error) {
    const std::string action = Text(params.Find("action"));
    const std::string contract = Text(params.Find("contract"));
    const bool fpu = contract == "instruction-window-v4" || contract == "instruction-window-access-v4";
    const bool devices = fpu || contract == "instruction-window-v3" || contract == "instruction-window-access-v3";
    const bool accesses = contract == "instruction-window-access-v1" || contract == "instruction-window-access-v2" ||
                          contract == "instruction-window-access-v3" || contract == "instruction-window-access-v4";
    const bool timed = devices || contract == "instruction-window-v2" || contract == "instruction-window-access-v2";
    if (action == "state-window-arm") {
        if (function.used || modules.used) return Reject(error, "instruction collector already armed/used");
        std::uint64_t cs = 0, ip = 0, end_cs = 0, end_ip = 0, limit = 0, count = 0;
        std::uint64_t address = 0, bytes = 0, guard = 0, guard_size = 0, code = 0, code_size = 0;
        std::uint64_t access_limit = 0;
        std::uint64_t first_tick = 0, end_tick = 0;
        if (timed) {
            if (!U64(params.Find("first_pic_tick"), &first_tick) || !U64(params.Find("end_pic_tick"), &end_tick) ||
                first_tick < PIC_Ticks || first_tick >= end_tick || end_tick > 1000000000)
                return Reject(error, "invalid instruction window interval");
        } else if (params.Find("first_pic_tick") || params.Find("end_pic_tick"))
            return Reject(error, "instruction window intervals require the v2 contract");
        if (accesses && (!U64(params.Find("access_limit"), &access_limit) || !access_limit || access_limit > 256))
            return Reject(error, "access_limit must be within 1..256");
        if (!accesses && params.Find("access_limit")) return Reject(error, "v1 windows do not record memory accesses");
        if (cpudecoder != CPU_Core_Normal_Run) return Reject(error, "instruction windows currently require the normal CPU core");
        std::vector<std::string> selected_devices;
        if (devices) {
            const auto* names = params.Find("devices");
            if (!names || names->type != JsonType::Array || names->array.empty() || names->array.size() > (fpu ? 3 : 2))
                return Reject(error, "devices exceeds the versioned passive-contract bounds");
            for (const auto& item : names->array) {
                const auto name = Text(&item);
                if (!fpu && name == "fp-simd-storage-v2") return Reject(error, "FPU instruction windows require the v4 contract");
                if (std::find(selected_devices.begin(), selected_devices.end(), name) != selected_devices.end())
                    return Reject(error, "duplicate instruction-window device contract");
                JsonValue ignored;
                if (!WindowDevice(name, &ignored, error)) return false;
                selected_devices.push_back(name);
            }
            if (fpu && std::find(selected_devices.begin(), selected_devices.end(), "fp-simd-storage-v2") == selected_devices.end())
                return Reject(error, "v4 instruction windows require FPU storage coverage");
        } else if (params.Find("devices")) return Reject(error, "device windows require the v3 contract");
        if (!U64(params.Find("entry_cs"), &cs) || cs > 65535 || !U64(params.Find("entry_ip"), &ip) || ip > UINT32_MAX ||
            !U64(params.Find("exit_cs"), &end_cs) || end_cs > 65535 || !U64(params.Find("exit_ip"), &end_ip) || end_ip > UINT32_MAX ||
            !U64(params.Find("instruction_limit"), &limit) || !limit || limit > 64 ||
            !U64(params.Find("case_limit"), &count) || !count || count > 8 ||
            !U64(params.Find("watch_address"), &address) || !U64(params.Find("watch_bytes"), &bytes) || bytes > 256 ||
            !U64(params.Find("guard_address"), &guard) || !U64(params.Find("guard_bytes"), &guard_size) || guard_size > 1048576 ||
            !U64(params.Find("code_address"), &code) || !U64(params.Find("code_bytes"), &code_size) || code_size > 4096 ||
            !Backing(address, bytes) || !Backing(guard, guard_size) || !Backing(code, code_size) ||
            address < guard || address - guard > guard_size || bytes > guard_size - (address - guard))
            return Reject(error, "invalid instruction window bounds or physical backing range");
        if (!window.Arm(cs, ip, end_cs, end_ip, limit, count, access_limit, timed, first_tick, end_tick)) return Reject(error, "invalid/already-used instruction window");
        watch_address = address; watch_bytes = bytes; guard_address = guard; guard_bytes = guard_size;
        code_address = code; code_bytes = code_size;
        window_devices = std::move(selected_devices);
        window_configuration = JsonValue::Object();
        for (const char* key : {"entry_cs", "entry_ip", "exit_cs", "exit_ip", "instruction_limit", "case_limit",
                               "watch_address", "watch_bytes", "guard_address", "guard_bytes", "code_address", "code_bytes"})
            window_configuration.object[key] = *params.Find(key);
        if (accesses) window_configuration.object["access_limit"] = N(access_limit);
        if (devices) window_configuration.object["devices"] = *params.Find("devices");
        if (timed) {
            window_configuration.object["first_pic_tick"] = N(first_tick);
            window_configuration.object["end_pic_tick"] = N(end_tick);
        }
        AGENT_InstructionWindowEnabled = true;
    } else if (action != "state-window-read") return Reject(error, "unknown instruction window action");
    const bool captured_fpu = std::find(window_devices.begin(), window_devices.end(), "fp-simd-storage-v2") != window_devices.end();
    if (accesses != (window.access_limit != 0) || timed != window.timed || devices != !window_devices.empty() || fpu != captured_fpu)
        return Reject(error, "window reader contract differs from the armed collector");
    std::uint64_t index = 0;
    if (action == "state-window-read" && (!U64(params.Find("index"), &index) || index > window.cases.size()))
        return Reject(error, "invalid instruction window index");
    if (window.armed && !InstructionWindow::DecoderCompatible(
            cpudecoder == CPU_Core_Normal_Run, CPU_IsHLTed(), cpu.hlt.old_decoder == CPU_Core_Normal_Run))
        AGENT_InstructionWindowInvalidate("CPU decoder changed during instruction window collection");
    window.CheckTick(PIC_Ticks);
    if (!window.failure.empty()) AGENT_InstructionWindowInvalidate(window.failure.c_str());
    JsonValue value = JsonValue::Object();
    value.object["contract"] = JsonValue::String(contract);
    value.object["phase"] = JsonValue::String("normal-core-before-decode-after-cycle-decrement");
    value.object["configuration"] = window_configuration;
    value.object["armed"] = JsonValue::Bool(window.armed);
    value.object["open"] = JsonValue::Bool(window.open);
    value.object["complete"] = JsonValue::Bool(window.complete);
    value.object["completed"] = N(window.completed);
    value.object["index"] = N(index);
    value.object["failure"] = JsonValue::String(window.failure);
    if (action == "state-window-read" && index < window.cases.size()) value.object["case"] = window.cases[index];
    *result = value;
    return true;
}
}

bool AGENT_InstructionWindowEnabled = false;
bool AGENT_ModuleEventsEnabled = false;
void AGENT_ModuleEventsObserve() { ModuleObserve(); }
bool AGENT_MemoryAccessEnabled = false;
unsigned AGENT_MemoryFetchDepth = 0;
int AGENT_MemoryInstructionFrame = -1;
MemoryInstructionSite AGENT_MemoryInstructionSite;
int AGENT_MemoryCurrentFrame() {
    return window.open && window.armed ? static_cast<int>(window.cases.back().object["frames"].array.size()) - 1 : -1;
}
MemoryAccessToken AGENT_MemoryAccessBegin(std::uint32_t address, unsigned bytes, bool write,
        bool checked, const char* path, bool mapped, std::uint64_t physical, std::uint32_t value) {
    MemoryAccessToken token;
    if (!AGENT_MemoryAccessEnabled || AGENT_MemoryFetchDepth) return token;
    if (function_effects) {
        if (!write || !EffectReady()) { EffectFailure(); return token; }
        bool selected = false;
        for (const auto& range : function_effect_writes)
            selected = selected || FunctionEffects::Intersects(address, bytes, range.address, range.bytes);
        if (!selected) return token;
        JsonValue event = JsonValue::Object();
        event.object["kind"] = JsonValue::String("write");
        event.object["linear"] = N(address); event.object["bytes"] = N(bytes);
        event.object["checked"] = JsonValue::Bool(checked); event.object["path"] = JsonValue::String(path);
        event.object["physical"] = mapped ? N(physical) : JsonValue::Null();
        event.object["value"] = N(value);
        return EffectBegin(std::move(event));
    }
    if (!window.CheckTick(PIC_Ticks)) {
        if (!window.failure.empty()) AGENT_InstructionWindowInvalidate(window.failure.c_str());
        return token;
    }
    JsonValue event = JsonValue::Object();
    event.object["frame"] = AGENT_MemoryInstructionFrame >= 0 ? N(AGENT_MemoryInstructionFrame) : JsonValue::Null();
    event.object["context"] = JsonValue::String(AGENT_MemoryInstructionFrame >= 0 ? "normal-core-path" : "between-instructions");
    event.object["kind"] = JsonValue::String(write ? "write" : "read");
    event.object["linear"] = N(address); event.object["bytes"] = N(bytes);
    event.object["checked"] = JsonValue::Bool(checked);
    event.object["path"] = JsonValue::String(path);
    event.object["physical"] = mapped ? N(physical) : JsonValue::Null();
    // Retain requested writes, including faults; obtain read values only from successful returns.
    event.object["value"] = write ? N(value) : JsonValue::Null();
    event.object["outcome"] = JsonValue::String("pending");
    event.object["begin"] = Position(); event.object["end"] = JsonValue::Null();
    token.epoch = memory_epoch;
    token.item = window.BeginAccess(event);
    if (!window.failure.empty()) AGENT_InstructionWindowInvalidate(window.failure.c_str());
    return token;
}
void AGENT_MemoryAccessEnd(MemoryAccessToken token, const char* outcome, bool has_value, std::uint32_t value) {
    if (token.collector == 1) {
        if (!token.item || token.epoch != memory_epoch || !EffectReady()) { EffectFailure(); return; }
        JsonValue completion = JsonValue::Object();
        completion.object["outcome"] = JsonValue::String(outcome); completion.object["end"] = Position();
        if (has_value) completion.object["value"] = N(value);
        function_cases.back().effects.End(token.item, completion); EffectFailure();
        return;
    }
    if (!token.item || token.epoch != memory_epoch || !window.failure.empty()) return;
    if (!window.CheckTick(PIC_Ticks)) {
        if (!window.failure.empty()) AGENT_InstructionWindowInvalidate(window.failure.c_str());
        return;
    }
    JsonValue completion = JsonValue::Object();
    completion.object["outcome"] = JsonValue::String(outcome);
    completion.object["end"] = Position();
    if (has_value) completion.object["value"] = N(value);
    window.EndAccess(token.item, completion);
    if (!window.failure.empty()) AGENT_InstructionWindowInvalidate(window.failure.c_str());
}
void AGENT_InstructionWindowObserve() {
    if (function.armed) FunctionObserve();
    if (!window.armed) {
        AGENT_InstructionWindowEnabled = function.armed;
        AGENT_MemoryAccessEnabled = function_effects && function.armed && function.open;
        return;
    }
    if (cpudecoder != CPU_Core_Normal_Run || !Backing(watch_address, watch_bytes) ||
        !Backing(guard_address, guard_bytes) || !Backing(code_address, code_bytes)) {
        AGENT_InstructionWindowInvalidate("CPU decoder or backing ranges changed");
        return;
    }
    window.Observe(SegValue(cs), reg_eip, WindowFrame, PIC_Ticks);
    AGENT_InstructionWindowEnabled = window.armed;
    AGENT_MemoryAccessEnabled = window.armed && window.open && window.access_limit;
}
void AGENT_InstructionWindowReset() {
    modules = ModuleJournal(); AGENT_ModuleEventsEnabled = false;
    AGENT_InstructionWindowEnabled = false;
    AGENT_MemoryAccessEnabled = false;
    ++memory_epoch; // Reject scopes left over from previous sessions.
    window = InstructionWindow(); window_configuration = JsonValue::Null();
    window_devices.clear();
    function = FunctionBoundary(); function_cases.clear(); function_ranges.clear(); function_configuration = JsonValue::Null();
    function_effects = false; function_bindings = false; function_io = false; function_context = false;
    function_effect_limit = 0; function_effect_writes.clear(); function_effect_ports.clear();
}
MemoryAccessToken AGENT_EffectTransitionBegin(const char* kind, std::uint64_t number,
        std::uint64_t type, std::uint32_t old_ip, bool use32) {
    if (!EffectReady()) { EffectFailure(); return MemoryAccessToken(); }
    JsonValue event = JsonValue::Object();
    event.object["kind"] = JsonValue::String(kind);
    event.object["number"] = N(number); event.object["type"] = N(type);
    event.object["old_ip"] = N(old_ip); event.object["use32"] = JsonValue::Bool(use32);
    event.object["before"] = EffectCpu(); event.object["after"] = JsonValue::Null();
    return EffectBegin(std::move(event));
}
void AGENT_EffectTransitionEnd(MemoryAccessToken token, bool unwinding) {
    if (!token.item || token.epoch != memory_epoch || !EffectReady()) { EffectFailure(); return; }
    JsonValue completion = JsonValue::Object();
    completion.object["outcome"] = JsonValue::String(unwinding ? "exception" : "returned");
    completion.object["end"] = Position(); completion.object["after"] = EffectCpu();
    function_cases.back().effects.End(token.item, completion); EffectFailure();
}
MemoryAccessToken AGENT_EffectCallbackBegin(unsigned index, CallBack_Handler handler) {
    if (!function_bindings || !EffectReady()) { EffectFailure(); return MemoryAccessToken(); }
    JsonValue event = JsonValue::Object();
    event.object["kind"] = JsonValue::String("host-callback");
    event.object["index"] = N(index);
    const auto* identity = AGENT_BiosCallbackIdentity(handler, function_context);
    event.object["identity"] = identity ? JsonValue::String(identity) : JsonValue::Null();
    event.object["before"] = EffectCpu(); event.object["after"] = JsonValue::Null();
    event.object["return_value"] = JsonValue::Null();
    if (function_context) {
        event.object["context_before"] = AGENT_BiosCallbackContext(handler);
        event.object["context_after"] = JsonValue::Null();
    }
    return EffectBegin(std::move(event));
}
void AGENT_EffectCallbackEnd(MemoryAccessToken token, bool unwinding, std::uint64_t returned, CallBack_Handler handler) {
    if (!token.item || token.epoch != memory_epoch || !function_bindings || !EffectReady()) { EffectFailure(); return; }
    JsonValue completion = JsonValue::Object();
    completion.object["outcome"] = JsonValue::String(unwinding ? "exception" : "returned");
    completion.object["end"] = Position(); completion.object["after"] = EffectCpu();
    completion.object["return_value"] = unwinding ? JsonValue::Null() : JsonValue::Number(std::to_string(returned));
    if (function_context) completion.object["context_after"] = AGENT_BiosCallbackContext(handler);
    function_cases.back().effects.End(token.item, completion); EffectFailure();
}
JsonValue AGENT_CallbackBacking(std::uint64_t address, unsigned bytes) {
    if (!EffectReady() || bytes > 256 || !Backing(address, bytes)) {
        function.Fail("callback context backing unavailable"); EffectFailure();
        return JsonValue::Null();
    }
    JsonValue result = JsonValue::Object();
    result.object["address"] = N(address); result.object["bytes"] = N(bytes);
    result.object["data_hex"] = JsonValue::String(Hex(MemBase + address, bytes));
    result.object["sha256"] = JsonValue::String(Hash(MemBase + address, bytes));
    return result;
}
MemoryAccessToken AGENT_EffectIoBegin(std::uint64_t port, unsigned bytes, bool write, std::uint32_t value) {
    if (!function_io || !EffectReady()) { EffectFailure(); return MemoryAccessToken(); }
    if (port > 65535 || (bytes != 1 && bytes != 2 && bytes != 4)) {
        function.Fail("port API request outside supported geometry"); EffectFailure(); return MemoryAccessToken();
    }
    bool selected = false;
    for (const auto& range : function_effect_ports)
        if (FunctionEffects::Intersects(static_cast<std::uint32_t>(port), bytes, range.address, range.bytes)) selected = true;
    if (!selected) return MemoryAccessToken();
    JsonValue event = JsonValue::Object();
    event.object["kind"] = JsonValue::String(write ? "io-write" : "io-read");
    event.object["port"] = N(port); event.object["bytes"] = N(bytes);
    event.object["value"] = write ? N(value) : JsonValue::Null();
    return EffectBegin(std::move(event));
}
void AGENT_EffectIoEnd(MemoryAccessToken token, bool unwinding, bool has_value, std::uint32_t value) {
    if (!token.item || token.epoch != memory_epoch || !function_io || !EffectReady()) { EffectFailure(); return; }
    JsonValue completion = JsonValue::Object();
    completion.object["outcome"] = JsonValue::String(unwinding ? "exception" : "returned");
    completion.object["end"] = Position();
    if (has_value && !unwinding) completion.object["value"] = N(value);
    function_cases.back().effects.End(token.item, completion); EffectFailure();
}
void AGENT_InstructionWindowInvalidate(const char* reason) {
    ModuleFail(reason);
    if (window.armed || !window.cases.empty()) window.Fail(reason);
    function.Fail(reason);
    AGENT_InstructionWindowEnabled = false;
    AGENT_MemoryAccessEnabled = false;
}

// Validate and allocate before writing; remain on the held emulation thread.
bool SubstituteRam(const JsonValue& params, JsonValue* result, std::string* error) {
    if (params.type != JsonType::Object || params.object.size() != 9)
        return Reject(error, "substitution requires the exact v1 request fields");
    for (const char* field : {"session_id", "action", "contract", "expected_state_revision", "expected_position",
                              "address", "bytes", "before_sha256", "spans"})
        if (!params.Find(field)) return Reject(error, "missing substitution request field");
    if (Text(params.Find("contract")) != "ram-substitution-v1" ||
        !AGENT_ReplaySubstitutionAllowed() || cpudecoder != CPU_Core_Normal_Run || paging.enabled ||
        window.armed || function.armed || modules.armed)
        return Reject(error, "substitution requires a held healthy playback, normal core, no paging or active trace");
    const auto position = Position();
    if (AGENT_CanonicalJson(*params.Find("expected_position")) != AGENT_CanonicalJson(position))
        return Reject(error, "substitution clock position changed");
    std::uint64_t address = 0, size = 0;
    if (!U64(params.Find("address"), &address) || !U64(params.Find("bytes"), &size) || !size || size > 65536 ||
        !AGENT_IsOrdinaryRam(address, size)) return Reject(error, "substitution requires bounded ordinary extended RAM");
    const auto* spans = params.Find("spans");
    if (spans->type != JsonType::Array || spans->array.empty() || spans->array.size() > 64)
        return Reject(error, "substitution requires 1..64 nonoverlapping ordered spans");
    const auto before = Hash(MemBase + address, size);
    if (Text(params.Find("before_sha256")) != before) return Reject(error, "substitution before-image hash differs");
    std::vector<unsigned char> staged(MemBase + address, MemBase + address + size);
    JsonValue receipts = JsonValue::Array();
    unsigned total = 0, changed = 0;
    std::uint64_t last = 0;
    const std::string digits = "0123456789abcdef";
    for (const auto& span : spans->array) {
        std::uint64_t offset = 0;
        const auto text = Text(span.Find("data_hex"));
        if (span.type != JsonType::Object || span.object.size() != 2 || !U64(span.Find("offset"), &offset) ||
            text.empty() || text.size() % 2 || text.size() > 512 || offset < last || offset >= size ||
            text.size() / 2 > size - offset || total + text.size() / 2 > 256)
            return Reject(error, "invalid or oversized substitution span");
        const unsigned count = static_cast<unsigned>(text.size() / 2);
        for (unsigned i = 0; i < count; ++i) {
            const auto hi = digits.find(text[2 * i]), lo = digits.find(text[2 * i + 1]);
            if (hi == std::string::npos || lo == std::string::npos) return Reject(error, "substitution hex must be lowercase");
            staged[offset + i] = static_cast<unsigned char>((hi << 4) | lo);
            if (staged[offset + i] != MemBase[address + offset + i]) ++changed;
        }
        JsonValue row = JsonValue::Object();
        row.object["offset"] = N(offset); row.object["bytes"] = N(count);
        row.object["before_sha256"] = JsonValue::String(Hash(MemBase + address + offset, count));
        row.object["after_sha256"] = JsonValue::String(Hash(staged.data() + offset, count));
        receipts.array.push_back(std::move(row));
        last = offset + count; total += count;
    }
    JsonValue receipt = JsonValue::Object();
    receipt.object["contract"] = JsonValue::String("ram-substitution-v1");
    receipt.object["position"] = position;
    receipt.object["address"] = N(address); receipt.object["bytes"] = N(size);
    receipt.object["before_sha256"] = JsonValue::String(before);
    receipt.object["after_sha256"] = JsonValue::String(Hash(staged.data(), size));
    receipt.object["written_bytes"] = N(total); receipt.object["changed_bytes"] = N(changed);
    receipt.object["spans"] = std::move(receipts);
    // Write only validated spans, without allocation or guest/device work.
    for (const auto& span : spans->array) {
        std::uint64_t offset = 0; U64(span.Find("offset"), &offset);
        const auto count = span.Find("data_hex")->text.size() / 2;
        std::copy_n(staged.data() + offset, count, MemBase + address + offset);
    }
    *result = std::move(receipt);
    return true;
}

bool AGENT_CaptureState(const JsonValue& params, JsonValue* result, std::string* error) {
    if (!result || !AGENT_EmulationQueue().IsBoundToCurrentThread()) return Reject(error, "state capture requires the emulation thread");
    if (Text(params.Find("contract")) == "module-events-unpaged-v1" ||
        Text(params.Find("contract")) == "module-events-unpaged-v2") return ModuleCommand(params, result, error);
    if (Text(params.Find("action")) == "state-substitute") return SubstituteRam(params, result, error);
    if (Text(params.Find("contract")) == "fp-simd-storage-v2") {
#if C_FPU
        if (Text(params.Find("action")) != "state-capture") return Reject(error, "FPU/SIMD storage is a passive capture only");
        if (!InstructionWindow::DecoderCompatible(cpudecoder == CPU_Core_Normal_Run,
                CPU_IsHLTed(), cpu.hlt.old_decoder == CPU_Core_Normal_Run))
            return Reject(error, "FPU/SIMD storage currently requires the normal CPU core");
        if (!AGENT_FpuCapture(result, error)) return false;
        result->object["position"] = Position();
        return true;
#else
        return Reject(error, "FPU/SIMD storage requires an FPU-enabled build");
#endif
    }
    if (Text(params.Find("contract")) == "opl-dbopl-state-v1") {
        if (Text(params.Find("action")) != "state-capture") return Reject(error, "OPL storage is a passive capture only");
        if (!AGENT_OplCapture(result, error)) return false;
        result->object["position"] = Position();
        return true;
    }
    if (Text(params.Find("contract")) == "pit-speaker-state-v1") {
        if (Text(params.Find("action")) != "state-capture") return Reject(error, "PIT/speaker storage is a passive capture only");
        if (!AGENT_SpeakerCapture(result, error)) return false;
        result->object["position"] = Position();
        return true;
    }
    if (Text(params.Find("contract")) == "function-boundary-flat32-v1" ||
        Text(params.Find("contract")) == "function-boundary-effects-flat32-v1" ||
        Text(params.Find("contract")) == "function-boundary-effects-flat32-v2" ||
        Text(params.Find("contract")) == "function-boundary-effects-flat32-v3" ||
        Text(params.Find("contract")) == "function-boundary-effects-flat32-v4") return FunctionCommand(params, result, error);
    if (Text(params.Find("contract")) == "mixer-rendered-v1") {
        const auto action = Text(params.Find("action"));
        if (action != "state-capture" && action != "state-audio-begin") return Reject(error, "unknown audio action");
        return AGENT_AudioCapture(action == "state-audio-begin", result, error);
    }
    if (Text(params.Find("contract")) == "instruction-window-v1" ||
        Text(params.Find("contract")) == "instruction-window-access-v1" ||
        Text(params.Find("contract")) == "instruction-window-v2" ||
        Text(params.Find("contract")) == "instruction-window-access-v2" ||
        Text(params.Find("contract")) == "instruction-window-v3" ||
        Text(params.Find("contract")) == "instruction-window-access-v3" ||
        Text(params.Find("contract")) == "instruction-window-v4" ||
        Text(params.Find("contract")) == "instruction-window-access-v4") return WindowCommand(params, result, error);
    if (Text(params.Find("contract")) == "raster-submitted-v1") {
        const auto action = Text(params.Find("action"));
        if (action != "state-capture" && action != "state-raster-begin") return Reject(error, "unknown raster action");
        return AGENT_RasterCapture(action == "state-raster-begin", result, error);
    }
    const auto mouse_contract = Text(params.Find("contract"));
    if ((mouse_contract == "mouse-int33-v1" || mouse_contract == "mouse-int33-v2") && Text(params.Find("action")) == "state-capture") {
        JsonValue capture = JsonValue::Object(), storage;
        if (!AGENT_MouseStorage(&storage, error)) return false;
        capture.object["contract"] = JsonValue::String(mouse_contract);
        if (mouse_contract == "mouse-int33-v2")
            storage.object["captured"] = JsonValue::Bool(AGENT_ReplayMouseCaptured(MOUSE_IsLocked()));
        capture.object["storage"] = storage;
        capture.object["position"] = JsonValue::Object();
        auto& position = capture.object["position"].object;
        position["pic_ticks"] = N(PIC_Ticks); position["cycle_max"] = N(CPU_CycleMax);
        position["cycle_left"] = N(CPU_CycleLeft); position["cycles"] = N(CPU_Cycles);
        *result = capture;
        return true;
    }
    if (Text(params.Find("contract")) != "integer-backing-v1") return Reject(error, "unknown state capture contract");
    const std::uint64_t high_base = UINT64_C(0x100000000);
    const std::uint64_t high_bytes = static_cast<std::uint64_t>(MEM_TotalPagesAt4GB()) * 4096;
    if (!MemBase || !MemSize || MemSize > high_base ||
        (high_bytes && (!memory_file_base || high_base > memory_file_size || high_bytes > memory_file_size - high_base)))
        return Reject(error, "physical backing bounds are unavailable or inconsistent");
    JsonValue capture = JsonValue::Object();
    auto& out = capture.object;
    out["contract"] = JsonValue::String("integer-backing-v1");
    out["position"] = JsonValue::Object();
    auto& p = out["position"].object;
    p["pic_ticks"] = N(PIC_Ticks); p["cycle_max"] = N(CPU_CycleMax); p["cycle_left"] = N(CPU_CycleLeft); p["cycles"] = N(CPU_Cycles);
    const bool raw_bytes = Text(params.Find("action")) == "state-ram-bytes";
    if (Text(params.Find("action")) == "state-ram-range" || raw_bytes) {
        std::uint64_t address = 0, size = 0;
        const std::uint64_t limit = raw_bytes ? 65536 : 1048576;
        if (!U64(params.Find("address"), &address) || !U64(params.Find("bytes"), &size) || !size || size > limit)
            return Reject(error, "invalid backing range or request size");
        const unsigned char* ptr = nullptr;
        if (address < MemSize && size <= MemSize - address) ptr = MemBase + address;
        else if (address >= high_base && address - high_base < high_bytes && size <= high_bytes - (address - high_base))
            ptr = static_cast<const unsigned char*>(memory_file_base) + address;
        if (!ptr) return Reject(error, "range lies outside physical backing");
        out["address"] = N(address); out["bytes"] = N(size); out["sha256"] = JsonValue::String(Hash(ptr, size));
        // Optional physical backing capture bypasses translation/MMIO; ordinary observations stay hash-only.
        if (raw_bytes) out["data_hex"] = JsonValue::String(Hex(ptr, size));
    } else if (Text(params.Find("action")) == "state-capture") {
        out["cpu"] = Cpu();
        out["ram"] = JsonValue::Object();
        auto& ram = out["ram"].object;
        ram["chunk_bytes"] = N(1048576); ram["reported_low_pages"] = N(MEM_TotalPages());
        ram["reported_high_pages"] = N(MEM_TotalPagesAt4GB()); ram["address_bits"] = N(MEM_get_address_bits());
        ram["a20_enabled"] = JsonValue::Bool(MEM_A20_Enabled());
        ram["extents"] = JsonValue::Array();
        ram["extents"].array.push_back(Extent(MemBase, 0, MemSize));
        if (high_bytes) ram["extents"].array.push_back(Extent(static_cast<const unsigned char*>(memory_file_base) + high_base, high_base, high_bytes));
        out["whole_machine_qualified"] = JsonValue::Bool(false);
        out["omitted"] = JsonValue::Array();
        for (const char* name : {"fpu-simd", "tlb-and-page-handlers", "decoder-internals", "devices-and-vram", "dos-host-services"})
            out["omitted"].array.push_back(JsonValue::String(name));
    } else return Reject(error, "unknown state capture action");
    *result = capture;
    return true;
}
}
#endif
