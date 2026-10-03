#include "core/Settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace reality64 {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parseBool(const std::string& text, bool& out) {
    const std::string v = lower(text);
    if (v == "1" || v == "true" || v == "on" || v == "yes") { out = true; return true; }
    if (v == "0" || v == "false" || v == "off" || v == "no") { out = false; return true; }
    return false;
}

bool parseInt(const std::string& text, long& out) {
    char* end = nullptr;
    out = std::strtol(text.c_str(), &end, 10);
    return !text.empty() && end && *end == '\0';
}

}  // namespace

void Settings::addRecent(const std::string& path) {
    removeRecent(path);
    recent.insert(recent.begin(), path);
    if (recent.size() > MaxRecent) recent.resize(MaxRecent);
}

void Settings::removeRecent(const std::string& path) {
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
}

bool Settings::parse(const std::string& text, std::vector<std::string>& errors) {
    bool ok = true;
    std::vector<std::string> recentFromFile;
    size_t lineNo = 0, pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;

        line = trim(line);
        if (line.empty() || line[0] == '#') continue;  // no inline comments: paths may contain '#'

        const auto fail = [&](const std::string& msg) {
            errors.push_back("line " + std::to_string(lineNo) + ": " + msg);
            ok = false;
        };

        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            fail("expected 'name = value'");
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));

        bool b;
        long n;
        if (key == "force60") {
            if (parseBool(value, b)) force60 = b; else fail("force60 must be on or off");
        } else if (key == "smoothscaling") {
            if (parseBool(value, b)) smoothScaling = b; else fail("smoothscaling must be on or off");
        } else if (key == "fullscreen") {
            if (parseBool(value, b)) fullscreen = b; else fail("fullscreen must be on or off");
        } else if (key == "cyclesperinstruction") {
            if (parseInt(value, n) && n >= static_cast<long>(MinCyclesPerInstruction) &&
                n <= static_cast<long>(MaxCyclesPerInstruction)) {
                cyclesPerInstruction = static_cast<unsigned>(n);
            } else {
                fail("cyclesperinstruction must be a number from 1 to 8");
            }
        } else if (key == "volume") {
            if (parseInt(value, n) && n >= 0 && n <= 100) volume = static_cast<int>(n);
            else fail("volume must be a number from 0 to 100");
        } else if (key == "recent") {
            if (!value.empty()) recentFromFile.push_back(value);
        } else {
            fail("unknown setting '" + key + "'");
        }
    }

    if (!recentFromFile.empty()) {
        recent = recentFromFile;
        if (recent.size() > MaxRecent) recent.resize(MaxRecent);
    }
    return ok;
}

std::string Settings::serialize() const {
    const auto onOff = [](bool v) { return v ? "on" : "off"; };
    std::string out = "# Reality64 settings (edited from the Settings screen)\n";
    out += std::string("force60 = ") + onOff(force60) + "\n";
    out += "cyclesPerInstruction = " + std::to_string(cyclesPerInstruction) + "\n";
    out += std::string("smoothScaling = ") + onOff(smoothScaling) + "\n";
    out += "volume = " + std::to_string(volume) + "\n";
    out += std::string("fullscreen = ") + onOff(fullscreen) + "\n";
    for (const std::string& path : recent) out += "recent = " + path + "\n";
    return out;
}

}  // namespace reality64
