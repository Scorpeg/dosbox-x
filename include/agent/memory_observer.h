#ifndef DOSBOX_AGENT_MEMORY_OBSERVER_H
#define DOSBOX_AGENT_MEMORY_OBSERVER_H
#include <cstdint>

namespace dosbox_agent {
// Observe existing memory API requests; never reread devices.
extern bool AGENT_MemoryAccessEnabled;
extern unsigned AGENT_MemoryFetchDepth;
extern int AGENT_MemoryInstructionFrame;
struct MemoryInstructionSite {
    bool active = false;
    unsigned cs = 0, mode = 0; // 0 real, 1 protected, 2 v86
    std::uint32_t ip = 0;
};
extern MemoryInstructionSite AGENT_MemoryInstructionSite;

struct MemoryAccessToken {
    std::uint64_t epoch = 0;
    unsigned item = 0;
    unsigned collector = 0; // 0 instruction window, 1 function effects
};
MemoryAccessToken AGENT_MemoryAccessBegin(std::uint32_t address, unsigned bytes,
        bool write, bool checked, const char* path, bool mapped,
        std::uint64_t physical, std::uint32_t value);
void AGENT_MemoryAccessEnd(MemoryAccessToken token, const char* outcome,
                          bool has_value, std::uint32_t value);

class MemoryAccessScope {
    MemoryAccessToken token;
public:
    MemoryAccessScope(std::uint32_t address, unsigned bytes, bool write,
            bool checked, const char* path, bool mapped, std::uint64_t physical,
            std::uint32_t value = 0) :
        token(AGENT_MemoryAccessBegin(address, bytes, write, checked, path, mapped, physical, value)) {}
    ~MemoryAccessScope() {
        if (token.item) AGENT_MemoryAccessEnd(token, "exception", false, 0);
    }
    void Complete(bool fault, bool has_value, std::uint32_t value) {
        AGENT_MemoryAccessEnd(token, fault ? "checked-fault" : "completed", has_value, value);
        token.item = 0;
    }
    MemoryAccessScope(const MemoryAccessScope&) = delete;
    MemoryAccessScope& operator=(const MemoryAccessScope&) = delete;
};

class MemoryFetchScope {
    bool active;
public:
    MemoryFetchScope() : active(AGENT_MemoryAccessEnabled) { if (active) ++AGENT_MemoryFetchDepth; }
    ~MemoryFetchScope() { if (active) --AGENT_MemoryFetchDepth; }
};

// Exclude instruction fetches and between-instruction service work from attribution.
class MemoryInstructionScope {
    int previous;
    MemoryInstructionSite previous_site;
public:
    explicit MemoryInstructionScope(int frame, unsigned cs = 0, std::uint32_t ip = 0, unsigned mode = 0) :
        previous(AGENT_MemoryInstructionFrame), previous_site(AGENT_MemoryInstructionSite) {
        AGENT_MemoryInstructionFrame = frame;
        AGENT_MemoryInstructionSite.active = true;
        AGENT_MemoryInstructionSite.cs = cs; AGENT_MemoryInstructionSite.ip = ip; AGENT_MemoryInstructionSite.mode = mode;
    }
    ~MemoryInstructionScope() { AGENT_MemoryInstructionFrame = previous; AGENT_MemoryInstructionSite = previous_site; }
};
int AGENT_MemoryCurrentFrame();
}
#endif
