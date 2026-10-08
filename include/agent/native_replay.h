#ifndef DOSBOX_NATIVE_REPLAY_H
#define DOSBOX_NATIVE_REPLAY_H
class Config;
namespace dosbox_agent {
class AgentServer;
bool NativeReplayPrepare(int argc, char** argv);
bool NativeReplayEnabled();
bool NativeReplayInspectionDone();
bool NativeReplayApplyConfig(Config& config);
void NativeReplayStart(AgentServer& server);
void NativeReplayJoin();
void NativeReplayCancel();
int NativeReplayExitCode();
}
#endif
