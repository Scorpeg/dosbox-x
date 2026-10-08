#ifndef DOSBOX_AGENT_CHECKPOINT_H
#define DOSBOX_AGENT_CHECKPOINT_H
#include "agent/agent_protocol.h"
#include <iosfwd>

namespace dosbox_agent {
bool AGENT_CheckpointCommand(const JsonValue& params, JsonValue* result, std::string* error);
bool AGENT_CheckpointHealthy(std::string* error);
void AGENT_CheckpointReset();
void AGENT_CheckpointGuestService(const char* name);
}

// Same-process state only; never deserialize client-supplied streams.
void AGENT_SaveKeyboard(std::ostream& stream);
void AGENT_LoadKeyboard(std::istream& stream);
void AGENT_SaveCmos(std::ostream& stream);
void AGENT_LoadCmos(std::istream& stream);
bool AGENT_PicCheckpointSafe();
void AGENT_SavePicExact(std::ostream& stream);
void AGENT_LoadPicExact(std::istream& stream);
void AGENT_SaveVgaDrawExact(std::ostream& stream);
void AGENT_LoadVgaDrawExact(std::istream& stream);
#endif
