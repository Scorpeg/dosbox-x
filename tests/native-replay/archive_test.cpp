#include "agent/native_archive.h"
#include <iostream>
#include <stdexcept>
using namespace dosbox_agent;
int main(int argc, char** argv) {
    try {
        if (argc != 3) return 2;
        std::string action = argv[1];
        if (action == "write" || action == "partial") {
            NativeArchive archive(argv[2], true);
            JsonValue value = JsonValue::Object();
            value.object["config"] = JsonValue::String("[cpu]\ncore=normal\n");
            archive.Write('H', value);
            value.object.clear(); value.object["tick"] = JsonValue::Number("1000");
            archive.Write('O', value);
            if (action == "write") archive.Write('F', value);
        } else if (action == "check") {
            NativeArchive archive(argv[2], false);
            archive.Validate();
            char kind; JsonValue value;
            while (archive.Read(&kind, &value)) std::cout << kind << AGENT_CanonicalJson(value) << '\n';
            std::cout << archive.Root() << '\n';
        } else if (action == "hash") {
            std::cout << NativeDigest(std::string(argv[2])) << '\n';
        } else return 2;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
