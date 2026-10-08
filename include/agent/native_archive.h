#ifndef DOSBOX_NATIVE_ARCHIVE_H
#define DOSBOX_NATIVE_ARCHIVE_H

#include "agent/agent_protocol.h"
#include <cstdio>
#include <string>

namespace dosbox_agent {

// Bounded native format; reject unsealed recordings and legacy DRINPUT.
class NativeArchive {
public:
    NativeArchive(const std::string& path, bool writing);
    ~NativeArchive();
    NativeArchive(const NativeArchive&) = delete;
    NativeArchive& operator=(const NativeArchive&) = delete;
    void Write(char kind, const JsonValue& value);
    bool Read(char* kind, JsonValue* value);
    bool Find(char wanted, JsonValue* value);
    void Rewind();
    void Validate();
    const std::string& Root() const { return chain; }
private:
    FILE* file = nullptr;
    bool writing = false, header_seen = false, sealed = false;
    std::string chain;
};

std::string NativeDigest(const std::string& bytes);
std::string NativeDigest(const JsonValue& value);
const JsonValue& NativeField(const JsonValue& value, const std::string& name);
std::uint64_t NativeUnsigned(const JsonValue& value);

}
#endif
