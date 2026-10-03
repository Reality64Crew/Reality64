#pragma once

#include <string>

#include "input/InputMapper.h"

namespace reality64 {

struct FrontendOptions {
    bool frameLimit = true;   // pace to the console's refresh rate
    bool fullscreen = false;  // start in fullscreen (also settable in the Settings screen)
    // Command-line overrides of the saved settings; negative / zero = use the setting.
    int force60 = -1;
    unsigned cyclesPerInstruction = 0;
};

// Runs the Reality64 application: a window with a menu (open a ROM, recent
// games, settings, controls) that plays games with video, audio and keyboard /
// gamepad input. `romPath` may be empty; if it is given the game starts at once.
// Returns a process exit code (0 on a normal quit, 1 if the window could not be
// created).
int runSdlFrontend(InputMapper& input, const FrontendOptions& options, const std::string& romPath);

}  // namespace reality64
