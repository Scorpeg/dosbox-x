#ifndef DOSBOX_AGENT_LAUNCH_DIRECTORY_H
#define DOSBOX_AGENT_LAUNCH_DIRECTORY_H

#include <string>

namespace dosbox_agent {

// C:-relative directory; empty selects root, never a host path or shell command.
inline bool ValidLaunchDirectory(const std::string& path)
{
    if (path.size() > 240) return false;
    if (path.empty()) return true;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string part = path.substr(begin, end == std::string::npos ? end : end - begin);
        const std::size_t dot = part.find('.');
        const std::string base = part.substr(0, dot);
        if (base.empty() || base.size() > 8 ||
            (dot != std::string::npos && (part.size() - dot - 1 < 1 || part.size() - dot - 1 > 3))) return false;
        std::string upper;
        for (std::size_t i = 0; i < part.size(); ++i) {
            const char c = part[i];
            if (c == '.' && i == dot) continue;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
            if (i < base.size()) upper += c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
        }
        if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
            (upper.size() == 4 && (upper.substr(0, 3) == "COM" || upper.substr(0, 3) == "LPT") &&
             upper[3] >= '1' && upper[3] <= '9')) return false;
        if (end == std::string::npos) return true;
        begin = end + 1;
    }
    return false;
}

}
#endif
