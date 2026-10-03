#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/Emulator.h"
#include "input/InputMapper.h"

#ifdef REALITY64_SDL
#include "frontend/SdlFrontend.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#endif

#ifndef REALITY64_VERSION
#define REALITY64_VERSION "dev"
#endif

namespace {

constexpr uint64_t DefaultMaxSteps = 10000000;

struct Options {
    std::string romPath;
    std::string inputConfig;
    std::string screenshot;
    bool infoOnly = false;
    bool trace = false;
    bool headless = false;
    bool noLimit = false;
    bool fullscreen = false;
    bool stepsGiven = false;
    uint64_t maxSteps = DefaultMaxSteps;
    uint64_t frames = 0;
};

void printUsage(const char* argv0) {
    std::printf(
        "Reality64 %s - Nintendo 64 emulator\n"
        "\n"
#ifdef REALITY64_SDL
        "Usage: %s [options] [rom.z64|rom.v64|rom.n64]\n"
        "\n"
        "With no ROM, a menu opens where you can pick one.\n"
#else
        "Usage: %s [options] <rom.z64|rom.v64|rom.n64>\n"
#endif
        "\n"
        "Options:\n"
#ifdef REALITY64_SDL
        "  --fullscreen         start in fullscreen\n"
        "  --no-limit           do not pace to the console's refresh rate\n"
        "  --headless           run without a window (for testing)\n"
#endif
        "  --input-config <f>   keyboard/gamepad bindings (default: ./input.cfg if present)\n"
        "  --info               print the ROM header and exit\n"
        "  --steps <n>          headless: stop after n CPU instructions (default %" PRIu64 ")\n"
        "  --frames <n>         headless: stop after n video frames\n"
        "  --screenshot <f.ppm> headless: save the last frame when finished\n"
        "  --trace              headless: print every executed instruction\n"
        "  --version            print the version and exit\n"
        "  -h, --help           show this help\n"
#ifdef REALITY64_SDL
        "\n"
        "Window keys: Esc back to the menu (quit from the menu), P pause, F11 fullscreen.\n"
#endif
        ,
        REALITY64_VERSION, argv0, DefaultMaxSteps);
}

void printHeader(const reality64::Rom& rom) {
    const reality64::RomHeader& h = rom.header();
    std::printf("Format:       %s\n", reality64::romFormatName(rom.originalFormat()));
    std::printf("Title:        %s\n", h.name.c_str());
    std::printf("Game code:    %s\n", h.gameCode.c_str());
    std::printf("Version:      %u\n", static_cast<unsigned>(h.version));
    std::printf("Boot address: 0x%08" PRIX32 "\n", h.bootAddress);
    std::printf("CRC:          %08" PRIX32 " %08" PRIX32 "\n", h.crc1, h.crc2);
    std::printf("CIC:          %s\n", reality64::cicName(reality64::detectCic(rom.data())));
    std::printf("Size:         %zu bytes\n", rom.data().size());
}

void printRegisters(const reality64::VR4300& cpu) {
    static const char* const names[32] = {
        "r0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    std::printf("pc = %016" PRIx64 "\n", cpu.pc());
    for (unsigned i = 0; i < 32; ++i) {
        std::printf("%-2s = %016" PRIx64 "%s", names[i], cpu.gpr(i), (i % 2) ? "\n" : "   ");
    }
    std::printf("hi = %016" PRIx64 "   lo = %016" PRIx64 "\n", cpu.hi(), cpu.lo());
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writePpm(const std::string& path, const reality64::VideoFrame& frame) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
    for (size_t i = 0; i < frame.rgba.size(); i += 4) {
        f.write(reinterpret_cast<const char*>(&frame.rgba[i]), 3);
    }
    return static_cast<bool>(f);
}

bool parseUnsigned(const char* text, uint64_t& out) {
    char* end = nullptr;
    out = std::strtoull(text, &end, 0);
    return end && *end == '\0' && *text != '\0';
}

// Returns -1 to continue, otherwise the exit code.
int parseArgs(int argc, char* argv[], Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--version") {
            std::printf("Reality64 %s\n", REALITY64_VERSION);
            return 0;
        } else if (arg == "--info") {
            opt.infoOnly = true;
        } else if (arg == "--trace") {
            opt.trace = true;
            opt.headless = true;
        } else if (arg == "--headless") {
            opt.headless = true;
        } else if (arg == "--no-limit") {
            opt.noLimit = true;
        } else if (arg == "--fullscreen") {
            opt.fullscreen = true;
        } else if (arg == "--steps" || arg == "--frames") {
            const char* v = needValue(arg.c_str());
            if (!v) return 1;
            uint64_t n;
            if (!parseUnsigned(v, n)) {
                std::fprintf(stderr, "error: invalid value for %s: %s\n", arg.c_str(), v);
                return 1;
            }
            if (arg == "--steps") {
                opt.maxSteps = n;
                opt.stepsGiven = true;
            } else {
                opt.frames = n;
            }
            opt.headless = true;
        } else if (arg == "--screenshot" || arg == "--input-config") {
            const char* v = needValue(arg.c_str());
            if (!v) return 1;
            (arg == "--screenshot" ? opt.screenshot : opt.inputConfig) = v;
            if (arg == "--screenshot") opt.headless = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", arg.c_str());
            return 1;
        } else if (opt.romPath.empty()) {
            opt.romPath = arg;
        } else {
            std::fprintf(stderr, "error: more than one ROM given\n");
            return 1;
        }
    }
    return -1;
}

#if defined(REALITY64_SDL) && defined(_WIN32)
// When the program is started by double-clicking it, Windows gives it a console
// of its own that would sit behind the window. If we are the only process
// attached to the console, nobody is reading it, so drop it. Started from a
// terminal, the console is shared and stays.
void detachOwnConsole() {
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) <= 1) FreeConsole();
}
#endif

void loadInputConfig(reality64::InputMapper& input, const std::string& explicitPath) {
    const std::string path = explicitPath.empty() ? "input.cfg" : explicitPath;
    std::string text;
    if (!readTextFile(path, text)) {
        if (!explicitPath.empty()) std::fprintf(stderr, "warning: cannot read input config '%s'\n", path.c_str());
        return;
    }
    std::vector<std::string> errors;
    input.loadConfig(text, errors);
    for (const std::string& e : errors) std::fprintf(stderr, "%s: %s\n", path.c_str(), e.c_str());
}

}  // namespace

int main(int argc, char* argv[]) {
    Options opt;
    const int parsed = parseArgs(argc, argv, opt);
    if (parsed >= 0) return parsed;

#ifndef REALITY64_SDL
    if (!opt.headless && !opt.infoOnly) {
        std::fprintf(stderr, "note: built without a window frontend; running headless\n");
        opt.headless = true;
    }
#endif

    reality64::InputMapper input;
    loadInputConfig(input, opt.inputConfig);

#ifdef REALITY64_SDL
    // The windowed app: with no ROM it opens on a menu instead of exiting.
    if (!opt.headless && !opt.infoOnly) {
#ifdef _WIN32
        detachOwnConsole();
#endif
        reality64::FrontendOptions fo;
        fo.frameLimit = !opt.noLimit;
        fo.fullscreen = opt.fullscreen;
        return reality64::runSdlFrontend(input, fo, opt.romPath);
    }
#endif

    if (opt.romPath.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    reality64::Emulator emu;
    std::string error;
    if (!emu.loadRom(opt.romPath, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    printHeader(*emu.rom());
    if (opt.infoOnly) return 0;

    emu.bus().setControllerProvider([&input](int port) { return input.state(port); });

    if (!emu.boot(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    if (opt.trace) {
        emu.cpu().setTraceHook([](uint64_t pc, uint32_t instr) {
            std::printf("%016" PRIx64 ": %08" PRIx32 "\n", pc, instr);
        });
    }

    uint64_t executed = 0;
    if (opt.frames > 0 && !opt.stepsGiven) {
        for (uint64_t f = 0; f < opt.frames && !emu.cpu().halted(); ++f) executed += emu.runFrame();
    } else {
        executed = emu.run(opt.maxSteps);
    }

    std::printf("\nExecuted %" PRIu64 " instructions", executed);
    if (emu.cpu().halted()) {
        std::printf(", CPU halted: %s\n", emu.cpu().haltReason().c_str());
    } else {
        std::printf(" (limit reached)\n");
    }
    std::printf("Unmapped bus accesses: %" PRIu64 ", RSP start requests: %" PRIu64 "\n",
                emu.bus().unmappedAccesses(), emu.bus().rspStartRequests());
    printRegisters(emu.cpu());

    if (!opt.screenshot.empty()) {
        reality64::VideoFrame frame;
        if (!emu.bus().captureFrame(frame)) {
            std::fprintf(stderr, "screenshot: video output is off, nothing to save\n");
        } else if (!writePpm(opt.screenshot, frame)) {
            std::fprintf(stderr, "error: cannot write '%s'\n", opt.screenshot.c_str());
            return 1;
        } else {
            std::printf("Saved %ux%u frame to %s\n", frame.width, frame.height, opt.screenshot.c_str());
        }
    }
    return emu.cpu().halted() ? 2 : 0;
}
