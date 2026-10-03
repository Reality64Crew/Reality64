#pragma once

#include "core/Emulator.h"
#include "input/InputMapper.h"

namespace reality64 {

struct FrontendOptions {
    bool frameLimit = true;   // pace to the console's refresh rate
    bool fullscreen = false;
};

// Runs a booted emulator in an SDL window with video, audio and keyboard /
// gamepad input until the user quits. Returns a process exit code: 0 on a
// normal quit, 2 if the emulated CPU halted on an unsupported operation.
int runSdlFrontend(Emulator& emu, InputMapper& input, const FrontendOptions& options);

}  // namespace reality64
