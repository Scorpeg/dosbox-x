#include "config.h"
#if defined(C_DOSBOX_AGENT) && C_DEBUG
#include "agent/native_archive.h"
#include "agent/exec_observer.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <vector>
#include <zlib.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace dosbox_agent {
namespace {
const std::string magic = "DRNATV01";
constexpr size_t header_limit = 2 * 1024 * 1024;
constexpr size_t frame_limit = 65536;
void Require(bool condition, const std::string& error) {
    if (!condition) throw std::runtime_error("native replay: " + error);
}
void Put(FILE* file, const std::string& data) {
    Require(fwrite(data.data(), 1, data.size(), file) == data.size(), "write failed (disk full?)");
}
std::string Get(FILE* file, size_t count) {
    std::string result(count, '\0');
    Require(fread(&result[0], 1, count, file) == count, "truncated archive");
    return result;
}
std::string Prefix(char kind, size_t length) {
    std::string result(5, '\0'); result[0] = kind;
    for (unsigned i = 0; i < 4; ++i) result[i+1] = char((length >> (i*8)) & 255);
    return result;
}
bool Known(char kind) { return kind == 'H' || kind == 'I' || kind == 'X' || kind == 'O' || kind == 'F'; }
}

std::string NativeDigest(const std::string& bytes) {
    return AGENT_Sha256Hex(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}
std::string NativeDigest(const JsonValue& value) { return NativeDigest(AGENT_CanonicalJson(value)); }
const JsonValue& NativeField(const JsonValue& value, const std::string& name) {
    const JsonValue* found = value.Find(name);
    Require(found != nullptr, "missing field: " + name);
    return *found;
}
std::uint64_t NativeUnsigned(const JsonValue& value) {
    Require(value.type == JsonType::Number && !value.text.empty(), "expected unsigned integer");
    std::uint64_t result = 0;
    for (char c : value.text) {
        Require(c >= '0' && c <= '9' && result <= (std::numeric_limits<std::uint64_t>::max() - (c-'0')) / 10,
                "invalid or overflowing unsigned integer");
        result = result * 10 + (c-'0');
    }
    return result;
}

NativeArchive::NativeArchive(const std::string& path, bool output) : writing(output) {
    if (writing) {
#ifdef _WIN32
        int fd = _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd >= 0) { file = _fdopen(fd, "wb"); if (!file) _close(fd); }
#else
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { file = fdopen(fd, "wb"); if (!file) close(fd); }
#endif
    } else file = fopen(path.c_str(), "rb");
    Require(file != nullptr, "cannot open " + path + ": " + strerror(errno));
    try {
        if (writing) Put(file, magic);
        else Require(Get(file, magic.size()) == magic, "unsupported archive; legacy archives require the legacy launcher");
        chain = NativeDigest(magic);
    } catch (...) { fclose(file); file = nullptr; throw; }
}
NativeArchive::~NativeArchive() { if (file) fclose(file); }
void NativeArchive::Write(char kind, const JsonValue& value) {
    Require(writing && !sealed && Known(kind), "invalid write after footer or unknown frame");
    Require(header_seen ? kind != 'H' : kind == 'H', "header must be first and unique");
    const std::string raw = AGENT_CanonicalJson(value);
    Require(!raw.empty() && raw.size() <= (kind == 'H' ? header_limit : frame_limit), "oversized JSON frame");
    uLongf length = compressBound(raw.size());
    std::string payload(length, '\0');
    Require(compress2(reinterpret_cast<Bytef*>(&payload[0]), &length,
                      reinterpret_cast<const Bytef*>(raw.data()), raw.size(), 6) == Z_OK, "compression failed");
    payload.resize(length);
    const std::string prefix = Prefix(kind, payload.size());
    chain = NativeDigest(chain + prefix + payload); // Previous digest is ASCII hex.
    Put(file, prefix); Put(file, payload); Put(file, chain);
    Require(fflush(file) == 0, "flush failed (disk full?)");
    header_seen = true; sealed = kind == 'F';
}
bool NativeArchive::Read(char* kind, JsonValue* value) {
    Require(!writing, "read on writer");
    int first = fgetc(file);
    if (first == EOF) { Require(!ferror(file) && sealed, "incomplete archive: no final footer"); return false; }
    Require(!sealed, "trailing data after footer");
    *kind = char(first);
    Require(Known(*kind) && (header_seen ? *kind != 'H' : *kind == 'H'), "invalid frame order/type");
    const std::string size = Get(file, 4);
    size_t length = 0;
    for (unsigned i = 0; i < 4; ++i) length |= size_t(static_cast<unsigned char>(size[i])) << (8*i);
    size_t limit = *kind == 'H' ? header_limit : frame_limit;
    Require(length > 0 && length <= compressBound(limit), "oversized compressed frame");
    const std::string payload = Get(file, length);
    chain = NativeDigest(chain + std::string(1, *kind) + size + payload);
    Require(Get(file, 64) == chain, "archive hash chain differs");
    std::vector<unsigned char> raw(limit + 1);
    z_stream stream = {};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(payload.data()));
    stream.avail_in = payload.size(); stream.next_out = raw.data(); stream.avail_out = raw.size();
    Require(inflateInit(&stream) == Z_OK, "inflate initialization failed");
    int result = inflate(&stream, Z_FINISH);
    bool valid = result == Z_STREAM_END && stream.avail_in == 0 && stream.total_out <= limit;
    size_t decoded = stream.total_out; inflateEnd(&stream);
    Require(valid, "oversized, truncated or concatenated zlib member");
    std::string text(raw.begin(), raw.begin() + decoded), error;
    Require(AGENT_ParseJson(text, value, &error), "invalid JSON: " + error);
    Require(AGENT_CanonicalJson(*value) == text, "noncanonical JSON");
    header_seen = true; sealed = *kind == 'F';
    return true;
}
bool NativeArchive::Find(char wanted, JsonValue* value) {
    char kind;
    while (Read(&kind, value)) if (kind == wanted) return true;
    return false;
}
void NativeArchive::Rewind() {
    Require(!writing && fseek(file, 0, SEEK_SET) == 0, "cannot rewind");
    Require(Get(file, magic.size()) == magic, "archive changed");
    chain = NativeDigest(magic); header_seen = sealed = false;
}
void NativeArchive::Validate() {
    Rewind(); char kind; JsonValue value;
    while (Read(&kind, &value)) {}
    Rewind();
}
}
#endif
