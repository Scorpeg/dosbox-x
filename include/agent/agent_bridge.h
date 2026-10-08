#ifndef DOSBOX_AGENT_BRIDGE_H
#define DOSBOX_AGENT_BRIDGE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace dosbox_agent {

class EmulationThreadQueue {
public:
    typedef std::function<void(std::uint64_t)> Command;

    EmulationThreadQueue();
    ~EmulationThreadQueue();

    EmulationThreadQueue(const EmulationThreadQueue&) = delete;
    EmulationThreadQueue& operator=(const EmulationThreadQueue&) = delete;

    bool BindToCurrentThread();
    bool IsBoundToCurrentThread() const;
    std::uint64_t Submit(Command command, bool drain_safe = false);
    std::uint64_t SubmitAfter(std::uint32_t delay_ms, Command command, bool drain_safe = false);
    // During loader backpressure, service only whitelisted stream commands.
    std::size_t Pump(bool drain_only = false);
    void Shutdown();

private:
    class Impl;
    Impl* impl;
};

typedef std::function<void(std::uint16_t segment, std::uint32_t instruction_pointer)> DebuggerStopListener;

EmulationThreadQueue& AGENT_EmulationQueue();
void AGENT_BridgeAttachToCurrentThread();
std::size_t AGENT_BridgePump();
void AGENT_BridgeShutdown();
void AGENT_ArmStartupBarrier();
void AGENT_StartupBarrier();
void AGENT_ReleaseStartupBarrier();
bool AGENT_StartupStartAllowed();
void AGENT_SetInitialRtc(std::int64_t epoch);
std::int64_t AGENT_InitialRtc();
void AGENT_SetDebuggerStopListener(DebuggerStopListener listener);
void AGENT_NotifyDebuggerStopped(std::uint16_t segment, std::uint32_t instruction_pointer);
bool AGENT_RunQueueSelfTest(std::string* error);

} // namespace dosbox_agent

#endif
