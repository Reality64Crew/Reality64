#pragma once

#include <string>
#include <vector>

namespace reality64 {

// User preferences, stored as a small `key = value` text file.
struct Settings {
    static constexpr unsigned MaxRecent = 6;
    static constexpr unsigned MinCyclesPerInstruction = 1;
    static constexpr unsigned MaxCyclesPerInstruction = 8;

    // Run the video at 60 Hz even for PAL games (which then play ~20% faster
    // than on real hardware). Off = the console's real 50 Hz for PAL.
    bool force60 = true;
    // CPU speed model: clock cycles charged per instruction. Higher needs less
    // host speed; games that expect a faster CPU may need a lower value.
    unsigned cyclesPerInstruction = 2;
    bool smoothScaling = true;
    int volume = 100;  // 0..100
    bool fullscreen = false;
    std::vector<std::string> recent;  // most recent first

    void addRecent(const std::string& path);
    void removeRecent(const std::string& path);

    // Lines that are invalid are reported in `errors` and skipped. Returns true
    // when every line was valid.
    bool parse(const std::string& text, std::vector<std::string>& errors);
    std::string serialize() const;
};

}  // namespace reality64
