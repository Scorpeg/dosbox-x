#include "config.h"
#if defined(C_DOSBOX_AGENT) && C_DEBUG
#include "dosbox.h"
#include "control.h"
#include "agent/native_replay.h"
#include "agent/native_archive.h"
#include "agent/agent_bridge.h"
#include "agent/agent_server.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

namespace dosbox_agent {
namespace {
using J = JsonValue;
J S(const std::string& s) { return J::String(s); }
J N(std::uint64_t n) { return J::Number(std::to_string(n)); }
J B(bool b) { return J::Bool(b); }
J O(std::initializer_list<std::pair<const std::string, J>> fields) {
    J result = J::Object(); result.object = fields; return result;
}
void Need(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error("native replay: " + message);
}
const J& F(const J& j, const char* key) { return NativeField(j, key); }
std::uint64_t U(const J& j, const char* key) { return NativeUnsigned(F(j,key)); }
std::string Text(const J& j, const char* key) {
    const J& v = F(j,key); Need(v.type == JsonType::String, std::string("expected text: ") + key); return v.text;
}
bool Equal(const J& a, const J& b) { return AGENT_CanonicalJson(a) == AGENT_CanonicalJson(b); }

// Bump compatibility when startup, settings, timing or captured domains change.
const char* compatibility = "native-replay-experimental-v1/b60936a7/48fe7645/normal-fixed10000/silent-entry2-seed0/fpu-regvalid-v2";
bool enabled = false, playback = false, inspection = false;
std::string archive_path, game_dir;
J header, footer;
std::uint64_t tick_limit = 0;
std::thread worker;
std::atomic<int> exit_code{1};
std::atomic<bool> cancelling{false};
std::unique_ptr<NativeArchive> pending_writer;
J pending_completion;
bool completion_ready = false, finalized = false; // Read only after joining worker.

J Machine() {
    return O({
        {"sdl", O({{"output",S("surface")},{"showmenu",S("true")},{"recording controls",S("true")},
                    {"priority",S("normal,normal")},{"autolock",S("true")},{"autolock_feedback",S("none")},
                    {"mouse_wheel_key",S("0")},{"middle_unlock",S("none")},{"clip_mouse_button",S("none")}})},
        {"dosbox", O({{"machine",S("svga_s3")},{"memsize",S("16")},{"quit warning",S("false")},
                       {"startbanner",S("false")},{"fastbioslogo",S("true")},{"synchronize time",S("false")}})},
        {"cpu", O({{"core",S("normal")},{"cputype",S("pentium")},{"cycles",S("fixed 10000")}})},
        {"mixer", O({{"nosound",S("false")},{"isolated output",S("true")},{"rate",S("44100")},
                      {"blocksize",S("1024")},{"prebuffer",S("20")},{"sample accurate",S("false")},
                      {"swapstereo",S("false")},{"dc bias correction",S("false")}})},
        {"sblaster", O({{"sbtype",S("sb16")},{"sbbase",S("220")},{"irq",S("7")},{"dma",S("1")},
                         {"hdma",S("5")},{"oplmode",S("opl3")},{"oplemu",S("fast")},
                         {"oplrate",S("44100")},{"disable filtering",S("true")},
                         {"adlib force timer overflow on detect",S("false")},{"adlib pcm boost",S("false")}})},
        {"midi", O({{"mididevice",S("none")},{"mpu401",S("none")}})},
        {"gus", O({{"gus",S("false")}})},
        {"speaker", O({{"pcspeaker",S("true")},{"pcrate",S("44100")},{"tandy",S("off")},{"disney",S("false")}})},
        {"serial", O({{"serial1",S("disabled")},{"serial2",S("disabled")}})},
        {"dos", O({{"int33",S("true")},{"vmware",S("false")},{"automount",S("false")},
                    {"lfn",S("false")},{"keyboardlayout",S("none")}})},
        {"keyboard", O({{"aux",S("false")}})},
        {"joystick", O({{"joysticktype",S("none")}})},
        {"ipx", O({{"ipx",S("false")}})},
        {"ne2000", O({{"ne2000",S("false")}})}
    });
}
void SafeDosPath(const std::string& path, bool directory) {
    Need(directory || !path.empty(), "missing DOS executable name");
    Need(path.size() < 128 && path.find("..") == std::string::npos &&
         (path.empty() || (path.front() != '/' && path.back() != '/')), "unsafe DOS-relative path");
    for (char c : path) Need((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || (directory && c == '/'), "unsafe DOS path character");
    if (!directory) {
        std::string upper = path;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c){return std::toupper(c);});
        Need(upper.size() > 4 && (upper.substr(upper.size()-4) == ".EXE" || upper.substr(upper.size()-4) == ".COM"),
             "target must be an EXE or COM");
    }
}
J TargetIdentity(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    Need(bool(input), "cannot read original target: " + path);
    std::string chain = NativeDigest(std::string()), block(1024*1024, '\0');
    std::uint64_t size = 0;
    while (input) {
        input.read(&block[0], block.size()); auto count = input.gcount();
        if (count) { size += count; chain = NativeDigest(chain + block.substr(0,count)); }
    }
    Need(input.eof() && size > 0, "target read failed or empty target");
    return O({{"contract",S("file-1mib-block-chain-v1")},{"bytes",N(size)},{"sha256",S(chain)}});
}

class Rows {
    NativeArchive archive;
    char kind;
    J page;
    size_t index = 0;
    bool ended = false;
public:
    Rows(const std::string& path, char type) : archive(path,false), kind(type) {}
    const J* Peek() {
        while (!ended && index == page.array.size()) {
            ended = !archive.Find(kind, &page); index = 0;
            if (!ended) Need(page.type == JsonType::Array && !page.array.empty() && page.array.size() <= 1024, "invalid row frame");
        }
        return ended ? nullptr : &page.array[index];
    }
    J Pop() { const J* next = Peek(); Need(next != nullptr, "unexpected end of input/lifecycle stream"); J result = *next; ++index; return result; }
};

class Controller {
    AgentServer& server;
    std::string session, stream, exec_stream;
    std::uint64_t request_id = 0, inputs = 0, execs = 0, samples = 0, sent = 0;
    std::string input_chain = NativeDigest(std::string()), exec_chain = NativeDigest(std::string());
    std::unique_ptr<NativeArchive> writer, observations;
    std::unique_ptr<Rows> feed, verify, lifecycle;
    J batch = J::Array(); bool feed_final = false, closed = false;
    std::uint64_t through = 0;
    J Call(const std::string& method, J params = J::Object()) {
        Need(!cancelling, "emulator closed before completion");
        if (!session.empty()) params.object["session_id"] = S(session);
        J request = O({{"jsonrpc",S("2.0")},{"id",N(++request_id)},{"method",S(method)},{"params",params}});
        J response; std::string error;
        Need(AGENT_ParseJson(server.HandleJsonRpc(AGENT_SerializeJson(request)), &response, &error), error);
        Need(!response.Find("error"), AGENT_SerializeJson(response));
        J result = F(response,"result");
        if (result.Find("failure")) Need(Text(result,"failure").empty(), Text(result,"failure"));
        return result;
    }
    J Input(const std::string& action, J params = J::Object()) {
        params.object["action"] = S(action);
        if (!stream.empty()) params.object["stream_id"] = S(stream);
        return Call("replay.control",params);
    }
    J Exec(const std::string& action, J params = J::Object()) {
        params.object["action"] = S(action);
        if (!exec_stream.empty()) params.object["stream_id"] = S(exec_stream);
        return Call("exec.control",params);
    }
    void Batch() {
        batch = J::Array();
        while (const J* row = feed->Peek()) {
            if (batch.array.size() >= 256 && U(*row,"tick") != U(batch.array.back(),"tick")) break;
            Need(batch.array.size() < 1024, "more than 1024 tied events");
            J event = feed->Pop();
            for (const char* field : {"space_before","space_after","queue_before","queue_after",
                                      "buttons_before","buttons_after","capture_before","capture_after"}) event.object.erase(field);
            batch.array.push_back(event);
        }
        feed_final = feed->Peek() == nullptr;
        through = feed_final ? U(footer,"end_tick") + 1 : U(batch.array.back(),"tick");
    }
    void Persist(char kind, const J& rows) {
        Need(rows.type == JsonType::Array, "invalid stream rows");
        if (rows.array.empty()) return;
        if (writer) writer->Write(kind,rows);
        for (const J& row : rows.array) {
            auto& count = kind == 'I' ? inputs : execs;
            Need(U(row,"order") == count, "input/lifecycle order gap");
            if (playback) Need(Equal((kind == 'I' ? verify : lifecycle)->Pop(),row),
                                std::string(kind == 'I' ? "input delivery" : "DOS EXEC") + " mismatch at order " + std::to_string(count));
            auto& chain = kind == 'I' ? input_chain : exec_chain;
            chain = NativeDigest(chain + AGENT_CanonicalJson(row)); ++count;
        }
    }
    J Service() {
        for (;;) {
            J page = Exec("read",O({{"cursor",N(execs)},{"limit",N(4)}}));
            const J& rows = F(page,"rows");
            if (rows.array.empty()) break;
            Persist('X',rows); Exec("ack",O({{"through_order",N(execs)}}));
        }
        J state = Input("status");
        // Batch deliveries to limit archive overhead; acknowledge only after flushing.
        if (!playback && !F(state,"boundary_held").boolean && U(state,"retained_events") < 128)
            return state;
        for (;;) {
            state = Input("read",O({{"cursor",N(inputs)},{"limit",N(64)}}));
            const J& rows = F(state,"rows");
            if (rows.array.empty()) break;
            Persist('I',rows); state = Input("ack",O({{"through_order",N(inputs)}}));
        }
        if (playback && F(state,"exit").type == JsonType::Null) {
            while (!closed && U(state,"retained_events") + batch.array.size() <= 1024) {
                state = Input("append",O({{"events",batch},{"expected_order",N(sent)},
                              {"through_tick",N(through)},{"final",B(feed_final)}}));
                sent += batch.array.size(); closed = feed_final;
                if (!closed) Batch();
            }
        }
        return state;
    }
    J Capture(const std::string& contract, const J& status, const std::string& action = "capture") {
        J params = O({{"contract",S(contract)},{"action",S(action)}});
        if (action != "capture") params.object["expected_state_revision"] = F(status,"state_revision");
        J result = Call("state.capture",params);
        Need(Equal(F(result,"position"),F(status,"position")) &&
             Equal(F(result,"state_revision"),F(status,"state_revision")), "torn state capture");
        for (const char* key : {"session_id","state_revision","position","host_sink"}) result.object.erase(key);
        return result;
    }
    J Observe(const J& status, bool entry = false) {
        J domains = J::Object();
        for (const char* contract : {"integer-backing-v1","mouse-int33-v2","fp-simd-storage-v2",
                                     "raster-submitted-v1","mixer-rendered-v1","opl-dbopl-state-v1","pit-speaker-state-v1"}) {
            std::string action = "capture";
            if (entry && std::string(contract) == "raster-submitted-v1") action = "raster-begin";
            if (entry && std::string(contract) == "mixer-rendered-v1") action = "audio-begin";
            domains.object[contract] = S(NativeDigest(Capture(contract,status,action)));
        }
        domains.object["input-deliveries-v1"] = S(input_chain);
        domains.object["dos-exec-rows-v1"] = S(exec_chain);
        J after = Input("status");
        Need(Equal(F(after,"position"),F(status,"position")) &&
             Equal(F(after,"state_revision"),F(status,"state_revision")), "observation changed clock/revision");
        return O({{"tick",F(status,"tick")},{"position",F(status,"position")},{"exit",F(status,"exit")},
                   {"domains",domains},{"inputs",N(inputs)},{"exec_events",N(execs)}});
    }
public:
    explicit Controller(AgentServer& s) : server(s) {}
    void Run() {
        if (playback) {
            feed.reset(new Rows(archive_path,'I')); verify.reset(new Rows(archive_path,'I'));
            lifecycle.reset(new Rows(archive_path,'X')); observations.reset(new NativeArchive(archive_path,false));
            Batch();
        } else writer.reset(new NativeArchive(archive_path,true));
        J mount = O({{"drive",S("C")},{"host_path",S(game_dir)}}), mounts = J::Array();
        mounts.array.push_back(mount);
        J target = F(header,"target");
        J start = Call("session.start",O({{"target",target},{"mounts",mounts},{"break_at",S("entry")}}));
        session = Text(start,"session_id");
        J registers = Call("state.get_registers");
        J begin = O({{"mode",S(playback?"replay":"record")},{"stream",B(true)},
                       {"events",playback?batch:J::Array()},{"through_tick",N(playback?through:0)},
                       {"final",B(playback&&feed_final)},{"expected_state_revision",F(registers,"state_revision")},
                       {"inputs",S("keyboard-mouse-v2")},{"stop_on_exit",B(true)}});
        if (playback) begin.object["initial_mouse_captured"] = F(header,"initial_mouse_captured");
        else begin.object["finish_on_close"] = B(true);
        J status = Input("begin",begin); stream = Text(status,"stream_id");
        if (playback) { sent = batch.array.size(); closed = feed_final; if (!closed) Batch(); }
        else header.object["initial_mouse_captured"] = F(status,"initial_mouse_captured");
        J exec = Exec("status"); exec_stream = Text(exec,"stream_id");
        Exec("flow",O({{"expected_state_revision",F(exec,"state_revision")}}));
        status = Input("status");
        // Write entry CPU/RAM/clock hashes before streaming the loader rows.
        J entry = Observe(status,true);
        if (playback) Need(Equal(entry,F(header,"entry")), "executable-entry observation differs");
        else { header.object["entry"] = entry; writer->Write('H',header); }
        status = Service();
        std::cout << (playback ? "NATIVE REPLAY: full host speed; checking recorded inputs and hashes.\n" :
                                "NATIVE RECORDING: play now; close DOSBox or press Stop recording to save.\n") << std::flush;
        std::uint64_t next_tick = U(header,"sample_ticks");
        J actual;
        for (;;) {
            J expected;
            if (playback) Need(observations->Find('O',&expected), "missing next observation");
            auto tick = playback ? U(expected,"tick") : (tick_limit ? std::min(next_tick,tick_limit) : next_tick);
            auto guard = tick + (playback && F(expected,"exit").type != JsonType::Null ? 1 : 0);
            Need(guard > U(status,"tick"), "nonadvancing sample boundary");
            Input("boundary",O({{"at_tick",N(guard)},{"expected_boundary_sequence",F(status,"boundary_sequence")}}));
            J operation = Call("execution.continue");
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (;;) {
                Need(std::chrono::steady_clock::now() < deadline, "boundary watchdog expired at tick " + std::to_string(guard));
                status = Service();
                J wait = Call("execution.wait",O({{"operation_id",F(operation,"operation_id")},{"timeout_ms",N(1)}}));
                if (!wait.Find("running") || !F(wait,"running").boolean) {
                    Need(Text(wait,"state") == "stopped", "guest did not stop normally");
                    const std::string reason = Text(F(wait,"stop_reason"),"kind");
                    Need(reason == "replay_boundary" || reason == "program_exit", "unexpected guest stop: " + reason);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            status = Service();
            Need(F(status,"boundary_held").boolean, "observation is not halted");
            actual = Observe(status); ++samples;
            if (playback) Need(Equal(actual,expected), "observation mismatch at tick " + std::to_string(U(actual,"tick")) +
                "\nexpected=" + AGENT_CanonicalJson(expected) + "\nactual=" + AGENT_CanonicalJson(actual));
            else writer->Write('O',actual);
            std::cout << "tick=" << U(actual,"tick") << " inputs=" << inputs << " observations=" << samples << '\n' << std::flush;
            bool terminal = F(actual,"exit").type != JsonType::Null;
            if (playback ? samples == U(footer,"observations") : terminal ||
                F(status,"finish_request").type != JsonType::Null || (tick_limit && U(actual,"tick") == tick_limit)) break;
            next_tick = U(actual,"tick") + U(header,"sample_ticks");
        }
        J finished = Input("finish");
        Need(F(finished,"complete").boolean && inputs == U(finished,"delivery_count") &&
             inputs == U(finished,"event_count"), "incomplete input timeline");
        J completion = O({{"end_tick",F(actual,"tick")},{"inputs",N(inputs)},{"exec_events",N(execs)},
                           {"observations",N(samples)},{"last_observation_sha256",S(NativeDigest(actual))}});
        if (playback) {
            Need(Equal(completion,footer) && !verify->Peek() && !lifecycle->Peek() && !feed->Peek(), "completion/count mismatch");
            J extra; Need(!observations->Find('O',&extra), "extra observations");
        } else {
            pending_completion = completion;
            pending_writer = std::move(writer);
        }
    }
};
}

bool NativeReplayEnabled() { return enabled; }
bool NativeReplayInspectionDone() { return inspection; }
bool NativeReplayPrepare(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--replay-info") {
        NativeArchive archive(argv[2],false); archive.Validate();
        char kind; J data;
        while (archive.Read(&kind,&data))
            std::cout << AGENT_CanonicalJson(O({{"kind",S(std::string(1,kind))},{"data",data}})) << '\n';
        enabled = inspection = true;
        return true;
    }
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--record" || std::string(argv[i]) == "--replay") enabled = true;
    if (!enabled) return false;
    std::map<std::string,std::string> options;
    const std::set<std::string> allowed = {"--record","--replay","--game-dir","--target","--directory","--ticks","--sample-ticks"};
    for (int i = 1; i < argc; i += 2) {
        std::string key = argv[i];
        Need(allowed.count(key) && i+1 < argc && !options.count(key), "unknown, duplicate or valueless option: " + key);
        options[key] = argv[i+1];
    }
    Need(options.count("--record") + options.count("--replay") == 1, "choose --record OR --replay");
    playback = options.count("--replay"); archive_path = options[playback?"--replay":"--record"];
    game_dir = options["--game-dir"];
    Need(!archive_path.empty() && !game_dir.empty(), "archive and --game-dir required");
    Need(game_dir.find_first_of("\r\n\"") == std::string::npos, "unsafe host directory");
#ifdef _WIN32
    Need(_putenv_s("TZ","UTC") == 0, "cannot select recorded UTC timezone");
    _tzset();
#else
    Need(setenv("TZ","UTC",1) == 0, "cannot select recorded UTC timezone");
    tzset();
#endif
    // Resolve paths before startup changes cwd.
    auto absolute = [](const std::string& p) { return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size()>2 && p[1] == ':')); };
    Need(absolute(archive_path) && absolute(game_dir), "use absolute archive and game-directory paths");
    if (playback) {
        Need(options.size() == 2, "replay settings come only from the archive");
        NativeArchive archive(archive_path,false); archive.Validate();
        Need(archive.Find('H',&header) && archive.Find('F',&footer), "missing header/footer");
        Need(Text(header,"format") == "dosbox-x:native-replay:v1" && Text(header,"compatibility") == compatibility,
             "unsupported replay compatibility contract");
        Need(Equal(F(header,"machine"),Machine()) && U(header,"rtc") == 946684800 && U(header,"startup_random_seed") == 0,
             "unsupported machine profile; arbitrary config/autoexec is not permitted");
        Need(U(footer,"observations") > 0 && U(footer,"end_tick") <= 1000000000, "invalid replay footer");
    } else {
        if (options.count("--ticks")) tick_limit = NativeUnsigned(J::Number(options["--ticks"]));
        auto interval = options.count("--sample-ticks") ? NativeUnsigned(J::Number(options["--sample-ticks"])) : 1000;
        Need(tick_limit <= 1000000000 && interval >= 20 && interval <= 5000, "tick/sample range unsupported");
        header = O({{"format",S("dosbox-x:native-replay:v1")},{"compatibility",S(compatibility)},
                    {"machine",Machine()},{"rtc",N(946684800)},{"startup_random_seed",N(0)},{"sample_ticks",N(interval)},
                    {"storage",S("direct-existing-folder; game writes persist; target-only preflight identity")},
                    {"target",O({{"command",S(options["--target"])},{"directory",S(options["--directory"])},{"arguments",J::Array()}})},
                    {"coverage",S("sampled backing storage plus continuous input, raster, mixer and DOS EXEC histories; not whole-machine equivalence")}});
    }
    J target = F(header,"target");
    std::string command = Text(target,"command"), directory = Text(target,"directory");
    SafeDosPath(command,false); SafeDosPath(directory,true);
    Need(F(target,"arguments").type == JsonType::Array && F(target,"arguments").array.empty(), "arguments not supported by this native profile");
    J identity = TargetIdentity(game_dir + "/" + (directory.empty()?"":directory+"/") + command);
    if (playback) Need(Equal(identity,F(header,"target_identity")), "original executable identity differs");
    else header.object["target_identity"] = identity;
    return true;
}
bool NativeReplayApplyConfig(Config& config) {
    for (const auto& section : F(header,"machine").object) {
        Section* target = config.GetSection(section.first);
        Need(target != nullptr, "unknown config section " + section.first);
        for (const auto& property : section.second.object)
            Need(target->HandleInputline(property.first + "=" + property.second.text), "invalid config property " + section.first + "." + property.first);
    }
    if (playback) for (const char* value : {"turbo=true","stop turbo on key=false","stop turbo after second=0"})
        Need(config.GetSection("cpu")->HandleInputline(value), "cannot set host-only full-speed pacing");
    return true;
}
void NativeReplayStart(AgentServer& server) {
    AgentConfig config;
    config.dosbox_workdir = game_dir; config.startup_barrier = true;
    config.initial_rtc_unix_seconds = U(header,"rtc");
    config.request_timeout_ms = 10000; config.max_message_bytes = 2*1024*1024;
    config.max_memory_read_bytes = 65536; config.max_trace_events = 1024;
    std::string error; Need(server.Start(config,&error,true), error);
    worker = std::thread([&server]() {
        try { Controller(server).Run(); completion_ready = true; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n' << std::flush; }
        // Unwind held boundaries on the emulation thread.
        AGENT_EmulationQueue().Submit([](std::uint64_t) { throw 0; },true);
    });
}
void NativeReplayJoin() {
    cancelling = true;
    if (worker.joinable()) worker.join();
    if (finalized || !completion_ready) return;
    finalized = true;
    try {
        // Seal after boundary unwinding succeeds; crashes must leave an incomplete archive.
        if (pending_writer) {
            pending_writer->Write('F',pending_completion); pending_writer.reset();
            NativeArchive check(archive_path,false); check.Validate();
        }
        exit_code = 0;
        std::cout << (playback ? "NATIVE REPLAY MATCH\n" : "NATIVE RECORDING SAVED\n") << std::flush;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n' << std::flush;
    }
}
void NativeReplayCancel() { cancelling = true; }
int NativeReplayExitCode() { return exit_code.load(); }
}
#endif
