#pragma once

#include <string>

#include "input/InputMapper.h"

namespace reality64 {

struct FrontendOptions {
    bool frameLimit = true;   // pace to the console's refresh rate
    bool fullscreen = false;
};

// Runs the Reality64 application: a window with a menu (open a ROM, controls,
// quit) that plays games with video, audio and keyboard / gamepad input.
// `romPath` may be empty; if it is given the game starts straight away.
// Returns a process exit code (0 on a normal quit, 1 if the window could not be
// created).
int runSdlFrontend(InputMapper& input, const FrontendOptions& options, const std::string& romPath);

}  // namespace reality64
