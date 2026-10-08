// Passive dispatch-boundary journal: stored CPU/RAM only, not a write history.
struct ModuleRange {
    std::string name, base;
    unsigned segment = 6;
    std::uint64_t offset = 0, subtract = 0, bytes = 0;
    bool retain = false;
};
struct ModuleSite {
    std::string name, mode, action, hash;
    std::vector<std::string> inactive_hashes;
    std::uint64_t inactive_hits = 0;
    unsigned selector = 0;
    std::uint64_t ip = 0, cs_base = 0, code_offset = 0, code_bytes = 0;
    bool big = false;
    std::vector<ModuleRange> ranges;
};
struct ModuleWatch {
    std::string name;
    std::uint64_t address = 0, bytes = 0;
    bool dirty = false;
    std::vector<unsigned char> previous;
};
struct ModuleJournal {
    bool used = false, armed = false, started = false, complete = false;
    bool qualified_sites = false;
    std::uint64_t first = 0, end = 0, last = 0, limit = 0, budget = 0, retained = 0;
    std::string failure;
    JsonValue configuration;
    std::vector<ModuleSite> sites;
    std::vector<ModuleWatch> watches;
    std::vector<JsonValue> events;
} modules;

void ModuleFail(const std::string& why) {
    if (!modules.used) return;
    if (modules.failure.empty()) modules.failure = why;
    modules.armed = modules.complete = AGENT_ModuleEventsEnabled = false;
}
bool ModuleToken(const std::string& name) {
    return !name.empty() && name.size() <= 64 &&
           name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == std::string::npos;
}
JsonValue ModuleEvent(const char* kind, const std::string& name) {
    auto row = JsonValue::Object();
    row.object["order"] = N(modules.events.size());
    row.object["kind"] = JsonValue::String(kind);
    row.object["name"] = JsonValue::String(name);
    row.object["position"] = Position(); row.object["cpu"] = Cpu();
    row.object["watches"] = JsonValue::Array();
    for (const auto& watch : modules.watches) {
        auto value = JsonValue::Object();
        value.object["name"] = JsonValue::String(watch.name);
        value.object["address"] = N(watch.address); value.object["bytes"] = N(watch.bytes);
        value.object["dirty"] = JsonValue::Bool(watch.dirty);
        value.object["sha256"] = JsonValue::String(Hash(MemBase + watch.address, watch.bytes));
        row.object["watches"].array.push_back(std::move(value));
    }
    row.object["ranges"] = JsonValue::Array();
    return row;
}
bool ModuleAppend(JsonValue row) {
    const auto bytes = AGENT_CanonicalJson(row).size() + 1;
    if (modules.events.size() >= modules.limit || bytes > modules.budget - modules.retained) {
        ModuleFail("module event/metadata budget exhausted; evidence is incomplete"); return false;
    }
    modules.retained += bytes; modules.events.push_back(std::move(row)); return true;
}
bool ModuleBacking() {
    if (paging.enabled) { ModuleFail("module events do not yet resolve paging"); return false; }
    for (const auto& watch : modules.watches)
        if (!AGENT_IsModuleRam(watch.address, watch.bytes)) {
            ModuleFail("module watch mapping is no longer ordinary identity-mapped RAM"); return false;
        }
    return true;
}
void ModuleObserve() {
    if (!modules.armed) return;
    if (PIC_Ticks < modules.last) { ModuleFail("module event clock reversed"); return; }
    modules.last = PIC_Ticks;
    if (PIC_Ticks < modules.first) return;
    if (cpudecoder != CPU_Core_Normal_Run || !ModuleBacking()) {
        ModuleFail("module event decoder/mapping changed"); return;
    }
    if (PIC_Ticks >= modules.end) {
        if (!modules.started) { ModuleFail("module interval ended without a resident baseline"); return; }
        if (ModuleAppend(ModuleEvent("end", "interval-end"))) {
            modules.complete = true; modules.armed = AGENT_ModuleEventsEnabled = false;
        }
        return;
    }
    if (!modules.started) {
        for (auto& watch : modules.watches)
            watch.previous.assign(MemBase + watch.address, MemBase + watch.address + watch.bytes);
        modules.started = true;
        if (!ModuleAppend(ModuleEvent("baseline", "resident-baseline"))) return;
    }
    // Emit one invalidation per dirty period; verified completion resets the baseline.
    for (auto& watch : modules.watches) {
        if (!watch.dirty && !std::equal(watch.previous.begin(), watch.previous.end(), MemBase + watch.address)) {
            watch.dirty = true;
            if (!ModuleAppend(ModuleEvent("invalidate", watch.name))) return;
        }
    }
    const std::string mode = !cpu.pmode ? "real" : (reg_flags & FLAG_VM) ? "v86" : "protected";
    for (auto& site : modules.sites) {
        if (SegValue(cs) != site.selector || reg_eip != site.ip) continue;
        const auto address = site.cs_base + site.code_offset;
        if (mode != site.mode || Segs.phys[cs] != site.cs_base || bool(cpu.code.big) != site.big ||
            Segs.expanddown[cs] || site.code_offset + site.code_bytes - 1 > Segs.limit[cs] ||
            !AGENT_IsModuleRam(address, site.code_bytes)) {
            ModuleFail("module site execution context or loaded-code identity differs"); return;
        }
        const auto identity = Hash(MemBase + address, site.code_bytes);
        if (identity != site.hash) {
            if (std::find(site.inactive_hashes.begin(), site.inactive_hashes.end(), identity) == site.inactive_hashes.end()) {
                ModuleFail("module site loaded code is neither the target nor a declared inactive image"); return;
            }
            if (++site.inactive_hits > 1000000000) { ModuleFail("module inactive-site counter overflow"); return; }
            continue;
        }
        if (site.action == "invalidate") for (auto& watch : modules.watches) watch.dirty = true;
        if (site.action == "validate") for (auto& watch : modules.watches) {
            watch.previous.assign(MemBase + watch.address, MemBase + watch.address + watch.bytes);
            watch.dirty = false;
        }
        auto row = ModuleEvent("site", site.name);
        row.object["action"] = JsonValue::String(site.action);
        row.object["code_sha256"] = JsonValue::String(site.hash);
        for (const auto& range : site.ranges) {
            std::uint64_t offset = range.offset;
            if (range.base == "bp16") offset += reg_ebp & 65535;
            if (range.base == "sp16") offset += reg_esp & 65535;
            if (range.base == "ebp") offset += reg_ebp;
            if (range.base == "esp") offset += reg_esp;
            if (offset < range.subtract) { ModuleFail("module range offset underflows"); return; }
            offset -= range.subtract;
            auto physical = offset;
            if (range.segment < 6) {
                if (Segs.expanddown[range.segment] || offset > Segs.limit[range.segment] ||
                    range.bytes - 1 > Segs.limit[range.segment] - offset) {
                    ModuleFail("module range exceeds stored segment bounds"); return;
                }
                physical += Segs.phys[range.segment];
            }
            if (!AGENT_IsModuleRam(physical, range.bytes)) { ModuleFail("module range mapping is unsupported"); return; }
            auto value = JsonValue::Object();
            value.object["name"] = JsonValue::String(range.name);
            value.object["address"] = N(physical); value.object["bytes"] = N(range.bytes);
            value.object["sha256"] = JsonValue::String(Hash(MemBase + physical, range.bytes));
            if (range.retain) value.object["data_hex"] = JsonValue::String(Hex(MemBase + physical, range.bytes));
            row.object["ranges"].array.push_back(std::move(value));
        }
        if (!ModuleAppend(std::move(row))) return;
    }
}
bool ModuleCommand(const JsonValue& params, JsonValue* result, std::string* error) {
    const auto action = Text(params.Find("action"));
    const auto contract = Text(params.Find("contract"));
    const bool qualified_sites = contract == "module-events-unpaged-v2";
    if (!qualified_sites && contract != "module-events-unpaged-v1") return Reject(error, "unknown module event contract");
    if (action == "state-module-arm") {
        if (modules.used || window.armed || !window.cases.empty() || function.used)
            return Reject(error, "another instruction collector is already armed/used");
        ModuleJournal selected;
        selected.qualified_sites = qualified_sites;
        const auto* config = params.Find("configuration");
        if (!config || config->type != JsonType::Object || config->object.size() != 6 ||
            cpudecoder != CPU_Core_Normal_Run || paging.enabled ||
            !U64(config->Find("first_pic_tick"), &selected.first) || selected.first < PIC_Ticks ||
            !U64(config->Find("end_pic_tick"), &selected.end) || selected.end <= selected.first || selected.end > 1000000000 ||
            !U64(config->Find("event_limit"), &selected.limit) || !selected.limit || selected.limit > 256 ||
            !U64(config->Find("byte_limit"), &selected.budget) || selected.budget < 4096 || selected.budget > 1048576)
            return Reject(error, "invalid module interval/budget/core configuration");
        const auto* watches = config->Find("watches");
        const auto* sites = config->Find("sites");
        if (!watches || watches->type != JsonType::Array || watches->array.empty() || watches->array.size() > 8 ||
            !sites || sites->type != JsonType::Array || sites->array.empty() || sites->array.size() > 16)
            return Reject(error, "module events require 1..8 watches and 1..16 sites");
        std::uint64_t total = 0;
        for (const auto& item : watches->array) {
            ModuleWatch watch; watch.name = Text(item.Find("name"));
            if (item.type != JsonType::Object || item.object.size() != 3 || !ModuleToken(watch.name) ||
                !U64(item.Find("address"), &watch.address) || !U64(item.Find("bytes"), &watch.bytes) ||
                !watch.bytes || watch.bytes > 8192 || !AGENT_IsModuleRam(watch.address, watch.bytes))
                return Reject(error, "invalid module watch");
            for (const auto& prior : selected.watches)
                if (watch.name == prior.name || (watch.address < prior.address + prior.bytes && prior.address < watch.address + watch.bytes))
                    return Reject(error, "duplicate/overlapping module watch");
            total += watch.bytes; selected.watches.push_back(std::move(watch));
        }
        if (total > 8192) return Reject(error, "module watches exceed 8192-byte comparison budget");
        for (const auto& item : sites->array) {
            ModuleSite site; std::uint64_t selector = 0;
            site.name = Text(item.Find("name")); site.mode = Text(item.Find("mode"));
            site.action = Text(item.Find("action")); site.hash = Text(item.Find("code_sha256"));
            const auto* big = item.Find("code_big"); const auto* ranges = item.Find("ranges");
            if (item.type != JsonType::Object || item.object.size() != (qualified_sites ? 12u : 11u) || !ModuleToken(site.name) ||
                (site.mode != "real" && site.mode != "protected" && site.mode != "v86") ||
                (site.action != "observe" && site.action != "validate" && site.action != "invalidate") ||
                !big || big->type != JsonType::Bool ||
                !U64(item.Find("cs"), &selector) || selector > 65535 ||
                !U64(item.Find("ip"), &site.ip) || site.ip > UINT32_MAX ||
                !U64(item.Find("cs_base"), &site.cs_base) || site.cs_base > UINT32_MAX ||
                !U64(item.Find("code_offset"), &site.code_offset) || site.code_offset > UINT32_MAX ||
                !U64(item.Find("code_bytes"), &site.code_bytes) || !site.code_bytes || site.code_bytes > 4096 ||
                site.ip < site.code_offset || site.ip - site.code_offset >= site.code_bytes ||
                (!big->boolean && (site.ip > 65535 || site.code_offset + site.code_bytes > 65536)) ||
                !AGENT_IsModuleRam(site.cs_base + site.code_offset, site.code_bytes) ||
                site.hash.size() != 64 || site.hash.find_first_not_of("0123456789abcdef") != std::string::npos ||
                !ranges || ranges->type != JsonType::Array || ranges->array.size() > 8)
                return Reject(error, "invalid module site");
            const auto* inactive = item.Find("inactive_code_sha256");
            if (qualified_sites) {
                if (!inactive || inactive->type != JsonType::Array || inactive->array.size() > 8)
                    return Reject(error, "invalid bounded inactive module identities");
                for (const auto& hash : inactive->array) {
                    const auto identity = Text(&hash);
                    if (identity.size() != 64 || identity.find_first_not_of("0123456789abcdef") != std::string::npos || identity == site.hash ||
                        std::find(site.inactive_hashes.begin(), site.inactive_hashes.end(), identity) != site.inactive_hashes.end())
                        return Reject(error, "invalid/duplicate inactive module identity");
                    site.inactive_hashes.push_back(identity);
                }
            } else if (inactive) return Reject(error, "inactive module identities require v2");
            site.selector = static_cast<unsigned>(selector); site.big = big->boolean;
            for (const auto& prior : selected.sites)
                if (site.name == prior.name || (site.selector == prior.selector && site.ip == prior.ip))
                    return Reject(error, "duplicate module site");
            std::uint64_t capture_bytes = 0, retained_bytes = 0;
            for (const auto& entry : ranges->array) {
                ModuleRange range; std::uint64_t segment = 0;
                range.name = Text(entry.Find("name")); range.base = Text(entry.Find("base"));
                const auto* retain = entry.Find("retain");
                if (entry.type != JsonType::Object || entry.object.size() != 7 || !ModuleToken(range.name) ||
                    (range.base != "absolute" && range.base != "bp16" && range.base != "sp16" && range.base != "ebp" && range.base != "esp") ||
                    !U64(entry.Find("segment"), &segment) || segment > 6 ||
                    !U64(entry.Find("offset"), &range.offset) || range.offset > UINT32_MAX ||
                    !U64(entry.Find("subtract"), &range.subtract) || range.subtract > 65535 ||
                    !U64(entry.Find("bytes"), &range.bytes) || !range.bytes || range.bytes > 65536 ||
                    !retain || retain->type != JsonType::Bool || (retain->boolean && range.bytes > 64))
                    return Reject(error, "invalid module capture range");
                for (const auto& prior : site.ranges)
                    if (prior.name == range.name) return Reject(error, "duplicate module capture range");
                range.segment = static_cast<unsigned>(segment); range.retain = retain->boolean;
                capture_bytes += range.bytes; if (range.retain) retained_bytes += range.bytes;
                site.ranges.push_back(std::move(range));
            }
            if (capture_bytes > 65536 || retained_bytes > 256) return Reject(error, "module site capture budget exceeded");
            selected.sites.push_back(std::move(site));
        }
        selected.configuration = *config;
        // Reserve fixed space for v2 counters, independent of hit count.
        selected.retained = AGENT_CanonicalJson(*config).size() + (qualified_sites ? 2048 : 0);
        if (selected.retained >= selected.budget) return Reject(error, "module configuration exceeds metadata budget");
        selected.used = selected.armed = true; selected.last = PIC_Ticks;
        modules = std::move(selected); AGENT_ModuleEventsEnabled = true;
    } else if (action != "state-module-read") return Reject(error, "unknown module event action");
    if (!modules.used) return Reject(error, "module events were not armed");
    if (qualified_sites != modules.qualified_sites) return Reject(error, "module event contract changed after arm");
    std::uint64_t index = 0;
    if (action == "state-module-read" && (!U64(params.Find("index"), &index) || index > modules.events.size()))
        return Reject(error, "invalid module event cursor");
    if (modules.armed && (!InstructionWindow::DecoderCompatible(cpudecoder == CPU_Core_Normal_Run,
            CPU_IsHLTed(), cpu.hlt.old_decoder == CPU_Core_Normal_Run) || paging.enabled))
        ModuleFail("module event decoder/mapping changed");
    // An end_tick hold precedes execution; reads must not invent an end observation.
    if (modules.armed && PIC_Ticks < modules.last) ModuleFail("module event clock reversed");
    auto value = JsonValue::Object();
    value.object["contract"] = JsonValue::String(contract);
    if (qualified_sites) {
        auto counts = JsonValue::Object();
        for (const auto& site : modules.sites) counts.object[site.name] = N(site.inactive_hits);
        value.object["inactive_site_hits"] = std::move(counts);
    }
    value.object["phase"] = JsonValue::String("normal-core-before-decode-after-cycle-decrement");
    value.object["configuration"] = modules.configuration;
    value.object["armed"] = JsonValue::Bool(modules.armed); value.object["complete"] = JsonValue::Bool(modules.complete);
    value.object["failure"] = JsonValue::String(modules.failure);
    value.object["event_count"] = N(modules.events.size()); value.object["retained_bytes"] = N(modules.retained);
    value.object["index"] = N(index);
    if (index < modules.events.size()) value.object["event"] = modules.events[index];
    *result = std::move(value); return true;
}
