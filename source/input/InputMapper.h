#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "input/Controller.h"

namespace reality64 {

// Turns raw keyboard and gamepad events into N64 controller state. It knows
// nothing about the windowing library: keys are identified by name ("Z",
// "Return", "Left Shift") and gamepad inputs by the SDL gamepad names
// ("a", "leftshoulder", "dpup", "leftx", "lefttrigger", ...), so any frontend
// (SDL, a mobile wrapper, a test) can feed it.
//
// Gamepads are handed out to controller ports 1-4 in the order they connect.
// The keyboard drives port 1 and is merged with a pad on that port.
class InputMapper {
public:
    InputMapper();

    // Restores the built-in bindings.
    void resetBindings();

    // Parses lines of the form `key.A = X, Space` / `pad.Z = +lefttrigger`.
    // Blank lines and '#' comments are ignored. Bad lines are reported in
    // `errors` (with line numbers) and skipped; the rest still apply.
    // Returns true when every line was valid.
    bool loadConfig(const std::string& text, std::vector<std::string>& errors);

    void keyEvent(const std::string& keyName, bool pressed);

    // Returns the port assigned to the pad, or -1 if all ports are taken.
    int padConnected(uint32_t padId);
    void padDisconnected(uint32_t padId);
    void padButton(uint32_t padId, const std::string& button, bool pressed);
    // value is normalised: -1..1 for sticks, 0..1 for triggers.
    void padAxis(uint32_t padId, const std::string& axis, float value);

    ControllerState state(int port) const;

    // Clears all held inputs (e.g. when the window loses focus).
    void releaseAll();

    // Names accepted on the left-hand side of config lines after `key.`/`pad.`.
    static const std::vector<std::string>& actionNames();

private:
    enum Action {
        A, B, Z, Start, L, R, DUp, DDown, DLeft, DRight, CUp, CDown, CLeft, CRight,
        StickUp, StickDown, StickLeft, StickRight, ActionCount
    };

    struct Pad {
        int port = -1;
        std::set<std::string> buttons;
        std::map<std::string, float> axes;
    };

    static std::string normalize(const std::string& name);
    static bool actionFromName(const std::string& name, Action& out);
    bool padSourceActive(const Pad& pad, const std::string& source) const;
    float padAxis(const Pad& pad, const std::string& axis) const;
    bool padActionActive(const Pad& pad, Action action) const;

    std::array<std::vector<std::string>, ActionCount> keyBindings_;
    std::array<std::vector<std::string>, ActionCount> padBindings_;
    std::string padStickX_;
    std::string padStickY_;
    float deadzone_ = 0.15f;

    std::set<std::string> keysDown_;
    std::map<uint32_t, Pad> pads_;
    std::array<uint32_t, MaxControllerPorts> portOwner_{};
    std::array<bool, MaxControllerPorts> portUsed_{};
};

}  // namespace reality64
