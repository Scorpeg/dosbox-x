#include "config.h"
#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "dosbox.h"
#include "agent/checkpoint.h"
#include "agent/agent_bridge.h"
#include "agent/replay_timeline.h"
#include "cpu.h"
#include "debug.h"
#include "mem.h"
#include "dos_inc.h"
#include "dos_system.h"
#include "setup.h"
#include "control.h"
#include "pic.h"
#include "vga.h"
#include <sstream>
#include <map>
#include <utility>
#include <exception>
#include <algorithm>

extern bool sync_time;
namespace dosbox_agent {
namespace {
struct Checkpoint {
    SaveState::AgentImage components;
    std::map<std::string, std::string> supplements;
    ReplayCheckpoint replay;
    std::uint16_t psp = 0;
    JsonValue position;
    std::string invalidated;
    const void* ram_base = nullptr;
    const void* vram_base = nullptr;
    const void* vram_allocation = nullptr;
    std::uint32_t vram_size = 0;
};
std::map<std::string, Checkpoint> checkpoints;
std::uint64_t sequence = 0;
bool poisoned = false;
bool Reject(std::string* error, const std::string& reason) { if (error) *error = reason; return false; }
JsonValue N(std::uint64_t n) { return JsonValue::Number(std::to_string(n)); }
typedef void (*Save)(std::ostream&);
typedef void (*Load)(std::istream&);
struct Supplement { const char* name; Save save; Load load; };
const Supplement supplements[] = {{"keyboard-exact", AGENT_SaveKeyboard, AGENT_LoadKeyboard},
    {"cmos-exact", AGENT_SaveCmos, AGENT_LoadCmos}, {"vga-exact", AGENT_SaveVgaDrawExact, AGENT_LoadVgaDrawExact},
    {"pic-exact", AGENT_SavePicExact, AGENT_LoadPicExact}};

bool Profile(std::string* error) {
    if (!AGENT_EmulationQueue().IsBoundToCurrentThread()) return Reject(error, "checkpoint requires the emulation thread");
    if (AGENT_ReplayIsStreaming()) return Reject(error, "streaming input checkpoint/archival cursor rollback is not qualified");
    if (cpu.pmode || machine != MCH_VGA || svgaCard != SVGA_S3Trio || CPU_CycleAutoAdjust || CPU_CycleMax != 10000 ||
        CPU_ArchitectureType != CPU_ARCHTYPE_386 || cpudecoder != CPU_Core_Normal_Run || sync_time ||
        AGENT_InitialRtc() < 0 || MEM_TotalPages() != 4096 || !AGENT_PicCheckpointSafe())
        return Reject(error, "unsupported checkpoint profile or unsafe event-service boundary");
    const auto section = [](const char* name) { return static_cast<Section_prop*>(control->GetSection(name)); };
    if (!section("mixer")->Get_bool("nosound") || std::string(section("midi")->Get_string("mididevice")) != "none" ||
        std::string(section("sblaster")->Get_string("sbtype")) != "none" || section("gus")->Get_bool("gus") ||
        section("speaker")->Get_bool("pcspeaker"))
        return Reject(error, "checkpoint audio profile is unsupported; devices must be disabled");
    if (section("ne2000")->Get_bool("ne2000") || section("ipx")->Get_bool("ipx"))
        return Reject(error, "checkpoint networking must be disabled");
    // Only guest-clock-driven vsync state is audited.
    if (vsync.manual || vsync.persistent || vsync.faithful || uservsyncjolt != 0.0f)
        return Reject(error, "checkpoint requires unmodified guest-clock vsync mode");
    for (int index = 1; index <= 9; ++index) {
        const std::string name = "serial" + std::to_string(index);
        const auto prop = section("serial")->Get_multival(name);
        if (!prop || std::string(prop->GetSection()->Get_string("type")) != "disabled")
            return Reject(error, "checkpoint serial devices must be disabled");
    }
    // Reject open non-device DOS files: restoring components cannot undo host file writes.
    for (unsigned i = 0; i < DOS_FILES; ++i)
        if (Files[i] && Files[i]->IsOpen() &&
            (!dynamic_cast<DOS_Device*>(Files[i]) ||
             !(Files[i]->IsName("CON") || Files[i]->IsName("NUL") || Files[i]->IsName("PRN") ||
               Files[i]->IsName("LPT1") || Files[i]->IsName("AUX"))))
            return Reject(error, "checkpoint requires only standard DOS device handles");
    // GetInformation on CON can execute guest code via INT16; do not call it here.
    return true;
}
JsonValue Describe(const std::string& id, const Checkpoint& checkpoint) {
    JsonValue value = JsonValue::Object();
    value.object["checkpoint_id"] = JsonValue::String(id);
    value.object["format"] = JsonValue::String("dosbox-proof:process-checkpoint:v1");
    value.object["scope"] = JsonValue::String("same-process-session-only");
    value.object["whole_machine_qualified"] = JsonValue::Bool(false);
    value.object["position"] = checkpoint.position;
    value.object["components"] = JsonValue::Object();
    std::uint64_t total = 0;
    for (const auto& item : checkpoint.components) {
        value.object["components"].object[item.first] = N(item.second.size()); total += item.second.size();
    }
    value.object["supplements"] = JsonValue::Object();
    for (const auto& item : checkpoint.supplements) {
        value.object["supplements"].object[item.first] = N(item.second.size()); total += item.second.size();
    }
    value.object["byte_count"] = N(total);
    value.object["timeline_tick"] = N(checkpoint.replay.timeline.tick);
    value.object["delivered_events"] = N(checkpoint.replay.timeline.delivered.size());
    value.object["scheduled_events"] = N(checkpoint.replay.timeline.events.size());
    return value;
}
}

bool AGENT_CheckpointHealthy(std::string* error) {
    return !poisoned || Reject(error, "checkpoint restore failed; this emulator process must be discarded");
}
void AGENT_CheckpointReset() { checkpoints.clear(); }
void AGENT_CheckpointGuestService(const char* name) {
    // Invalidate checkpoints after non-rollbackable DOS/media effects.
    for (auto& item : checkpoints) item.second.invalidated = name;
}

static bool CheckpointCommand(const JsonValue& params, JsonValue* result, std::string* error) {
    if (!AGENT_CheckpointHealthy(error) || !Profile(error)) return false;
    const JsonValue* action = params.Find("action");
    if (!action || action->type != JsonType::String) return Reject(error, "checkpoint action is required");
    if (action->text == "save") {
        if (checkpoints.size() >= 2) return Reject(error, "process checkpoint limit reached; release a checkpoint first");
        Checkpoint checkpoint;
        checkpoint.replay = AGENT_ReplayCheckpoint();
        if (!checkpoint.replay.timeline.active || !checkpoint.replay.timeline.failure.empty())
            return Reject(error, "checkpoint save requires a healthy active timeline");
        JsonValue status;
        JsonValue request = JsonValue::Object(); request.object["action"] = JsonValue::String("status");
        if (!AGENT_ReplayCommand(request, &status, error)) return false;
        checkpoint.position = status.object["position"];
        checkpoint.psp = dos.psp();
        checkpoint.ram_base = MemBase;
        checkpoint.vram_base = vga.mem.linear;
        checkpoint.vram_allocation = vga.mem.linear_orgptr;
        checkpoint.vram_size = vga.mem.memsize;
        for (const auto& supplement : supplements) {
            std::ostringstream stream(std::ios::out | std::ios::binary);
            supplement.save(stream); checkpoint.supplements[supplement.name] = stream.str();
            if (!stream) return Reject(error, "checkpoint supplement capture failed");
        }
        if (!SaveState::instance().captureAgentImage(&checkpoint.components, error)) return false;
        const std::string id = "checkpoint-" + std::to_string(++sequence);
        checkpoints[id] = std::move(checkpoint);
        *result = Describe(id, checkpoints.at(id));
        return true;
    }
    const JsonValue* id = params.Find("checkpoint_id");
    if (!id || id->type != JsonType::String || !checkpoints.count(id->text)) return Reject(error, "unknown process checkpoint");
    if (action->text == "release") {
        *result = Describe(id->text, checkpoints.at(id->text)); checkpoints.erase(id->text); return true;
    }
    if (action->text != "restore") return Reject(error, "unknown checkpoint action");
    const Checkpoint& checkpoint = checkpoints.at(id->text);
    if (dos.psp() != checkpoint.psp) return Reject(error, "checkpoint target process changed");
    if (!checkpoint.invalidated.empty())
        return Reject(error, "checkpoint interval contains unsupported guest service: " + checkpoint.invalidated);
    if (MemBase != checkpoint.ram_base || vga.mem.linear != checkpoint.vram_base ||
        vga.mem.linear_orgptr != checkpoint.vram_allocation || vga.mem.memsize != checkpoint.vram_size)
        return Reject(error, "checkpoint memory allocations changed");
    // A partial restore is fatal; the machine is already mutated.
    poisoned = true;
    if (!SaveState::instance().restoreAgentImage(checkpoint.components, error)) return false;
    for (const auto& supplement : supplements) {
        std::istringstream stream(checkpoint.supplements.at(supplement.name), std::ios::in | std::ios::binary);
        supplement.load(stream);
        if (!stream || stream.peek() != std::char_traits<char>::eof()) return Reject(error, "checkpoint supplement restore failed");
    }
    AGENT_ReplayRestore(checkpoint.replay);
    DEBUG_AgentClearLastBreakpoint();
    // Host-byte equality cannot prove restoration of fields omitted by serializers.
    SaveState::AgentImage roundtrip;
    if (!SaveState::instance().captureAgentImage(&roundtrip, error)) return false;
    JsonValue differences = JsonValue::Array();
    for (const auto& item : checkpoint.components) {
        const auto& actual = roundtrip.at(item.first);
        if (actual == item.second) continue;
        std::size_t first = 0;
        while (first < std::min(actual.size(), item.second.size()) && actual[first] == item.second[first]) ++first;
        JsonValue difference = JsonValue::Object();
        difference.object["component"] = JsonValue::String(item.first);
        difference.object["first_byte"] = N(first);
        difference.object["saved_size"] = N(item.second.size());
        difference.object["current_size"] = N(actual.size());
        differences.array.push_back(difference);
    }
    poisoned = false;
    *result = Describe(id->text, checkpoint);
    result->object["diagnostic_native_host_byte_differences"] = differences;
    return true;
}

bool AGENT_CheckpointCommand(const JsonValue& params, JsonValue* result, std::string* error) {
    try { return CheckpointCommand(params, result, error); }
    catch (const std::exception& exception) {
        poisoned = true;
        return Reject(error, std::string("checkpoint exception; discard emulator process: ") + exception.what());
    }
    catch (...) {
        poisoned = true;
        return Reject(error, "checkpoint exception; discard emulator process");
    }
}
}
#endif
