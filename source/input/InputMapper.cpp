#include "input/InputMapper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace reality64 {

namespace {

constexpr float AxisButtonThreshold = 0.5f;
constexpr int DiagonalStick = 60;  // ~85 * sin(45 deg): keeps keyboard diagonals on the stick's circle

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

}  // namespace

const std::vector<std::string>& InputMapper::actionNames() {
    static const std::vector<std::string> names = {
        "A", "B", "Z", "Start", "L", "R", "DUp", "DDown", "DLeft", "DRight",
        "CUp", "CDown", "CLeft", "CRight", "StickUp", "StickDown", "StickLeft", "StickRight"};
    return names;
}

std::string InputMapper::normalize(const std::string& name) {
    std::string s = trim(name);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool InputMapper::actionFromName(const std::string& name, Action& out) {
    const std::string wanted = normalize(name);
    const std::vector<std::string>& names = actionNames();
    for (size_t i = 0; i < names.size(); ++i) {
        if (normalize(names[i]) == wanted) {
            out = static_cast<Action>(i);
            return true;
        }
    }
    return false;
}

InputMapper::InputMapper() {
    portOwner_.fill(0);
    portUsed_.fill(false);
    resetBindings();
}

void InputMapper::resetBindings() {
    for (auto& v : keyBindings_) v.clear();
    for (auto& v : padBindings_) v.clear();

    // Keyboard (physical key names, so the layout does not matter).
    keyBindings_[A] = {"x"};
    keyBindings_[B] = {"c"};
    keyBindings_[Z] = {"z"};
    keyBindings_[Start] = {"return"};
    keyBindings_[L] = {"a"};
    keyBindings_[R] = {"s"};
    keyBindings_[DUp] = {"t"};
    keyBindings_[DDown] = {"g"};
    keyBindings_[DLeft] = {"f"};
    keyBindings_[DRight] = {"h"};
    keyBindings_[CUp] = {"i"};
    keyBindings_[CDown] = {"k"};
    keyBindings_[CLeft] = {"j"};
    keyBindings_[CRight] = {"l"};
    keyBindings_[StickUp] = {"up"};
    keyBindings_[StickDown] = {"down"};
    keyBindings_[StickLeft] = {"left"};
    keyBindings_[StickRight] = {"right"};

    // Gamepad, following the usual Xbox-style layout: "a"/"x" are the bottom
    // and left face buttons, the right stick is the C-buttons.
    padBindings_[A] = {"a"};
    padBindings_[B] = {"x"};
    padBindings_[Z] = {"+lefttrigger"};
    padBindings_[Start] = {"start"};
    padBindings_[L] = {"leftshoulder"};
    padBindings_[R] = {"rightshoulder", "+righttrigger"};
    padBindings_[DUp] = {"dpup"};
    padBindings_[DDown] = {"dpdown"};
    padBindings_[DLeft] = {"dpleft"};
    padBindings_[DRight] = {"dpright"};
    padBindings_[CUp] = {"-righty"};
    padBindings_[CDown] = {"+righty"};
    padBindings_[CLeft] = {"-rightx"};
    padBindings_[CRight] = {"+rightx"};
    padStickX_ = "leftx";
    padStickY_ = "lefty";
    deadzone_ = 0.15f;
}

bool InputMapper::loadConfig(const std::string& text, std::vector<std::string>& errors) {
    bool ok = true;
    size_t lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;

        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;

        const auto fail = [&](const std::string& msg) {
            errors.push_back("line " + std::to_string(lineNo) + ": " + msg);
            ok = false;
        };

        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            fail("expected 'key.Action = ...' or 'pad.Action = ...'");
            continue;
        }
        const std::string lhs = normalize(line.substr(0, eq));
        const std::string rhs = line.substr(eq + 1);

        const bool isKey = lhs.rfind("key.", 0) == 0;
        const bool isPad = lhs.rfind("pad.", 0) == 0;
        if (!isKey && !isPad) {
            fail("unknown setting '" + lhs + "' (expected a 'key.' or 'pad.' prefix)");
            continue;
        }
        const std::string what = lhs.substr(4);

        if (isPad && (what == "stickx" || what == "sticky")) {
            const std::string axis = normalize(rhs);
            if (axis.empty()) {
                fail("pad." + what + " needs an axis name such as leftx");
                continue;
            }
            (what == "stickx" ? padStickX_ : padStickY_) = axis;
            continue;
        }
        if (isPad && what == "deadzone") {
            char* endp = nullptr;
            const std::string v = trim(rhs);
            const float d = std::strtof(v.c_str(), &endp);
            if (v.empty() || *endp != '\0' || !(d >= 0.0f && d <= 0.9f)) {
                fail("pad.deadzone must be a number between 0 and 0.9");
                continue;
            }
            deadzone_ = d;
            continue;
        }

        Action action;
        if (!actionFromName(what, action)) {
            fail("unknown action '" + what + "'");
            continue;
        }

        std::vector<std::string> sources;
        size_t start = 0;
        while (start <= rhs.size()) {
            size_t comma = rhs.find(',', start);
            if (comma == std::string::npos) comma = rhs.size();
            const std::string item = normalize(rhs.substr(start, comma - start));
            if (!item.empty()) sources.push_back(item);
            start = comma + 1;
        }
        (isKey ? keyBindings_ : padBindings_)[action] = std::move(sources);
    }
    return ok;
}

void InputMapper::keyEvent(const std::string& keyName, bool pressed) {
    const std::string key = normalize(keyName);
    if (pressed) keysDown_.insert(key);
    else keysDown_.erase(key);
}

int InputMapper::padConnected(uint32_t padId) {
    auto it = pads_.find(padId);
    if (it != pads_.end()) return it->second.port;

    Pad pad;
    for (int port = 0; port < MaxControllerPorts; ++port) {
        if (!portUsed_[port]) {
            portUsed_[port] = true;
            portOwner_[port] = padId;
            pad.port = port;
            break;
        }
    }
    pads_[padId] = pad;
    return pad.port;
}

void InputMapper::padDisconnected(uint32_t padId) {
    auto it = pads_.find(padId);
    if (it == pads_.end()) return;
    const int freed = it->second.port;
    pads_.erase(it);
    if (freed < 0) return;

    portUsed_[freed] = false;
    // Hand the freed port to a pad that was waiting for one.
    for (auto& [id, pad] : pads_) {
        if (pad.port < 0) {
            pad.port = freed;
            portUsed_[freed] = true;
            portOwner_[freed] = id;
            break;
        }
    }
}

void InputMapper::padButton(uint32_t padId, const std::string& button, bool pressed) {
    auto it = pads_.find(padId);
    if (it == pads_.end()) return;
    const std::string name = normalize(button);
    if (pressed) it->second.buttons.insert(name);
    else it->second.buttons.erase(name);
}

void InputMapper::padAxis(uint32_t padId, const std::string& axis, float value) {
    auto it = pads_.find(padId);
    if (it == pads_.end()) return;
    it->second.axes[normalize(axis)] = std::max(-1.0f, std::min(1.0f, value));
}

void InputMapper::releaseAll() {
    keysDown_.clear();
    for (auto& [id, pad] : pads_) {
        pad.buttons.clear();
        pad.axes.clear();
    }
}

float InputMapper::padAxis(const Pad& pad, const std::string& axis) const {
    auto it = pad.axes.find(axis);
    return it == pad.axes.end() ? 0.0f : it->second;
}

bool InputMapper::padSourceActive(const Pad& pad, const std::string& source) const {
    if (!source.empty() && (source[0] == '+' || source[0] == '-')) {
        const float v = padAxis(pad, source.substr(1));
        return source[0] == '+' ? v > AxisButtonThreshold : v < -AxisButtonThreshold;
    }
    return pad.buttons.count(source) != 0;
}

bool InputMapper::padActionActive(const Pad& pad, Action action) const {
    for (const std::string& source : padBindings_[action]) {
        if (padSourceActive(pad, source)) return true;
    }
    return false;
}

ControllerState InputMapper::state(int port) const {
    ControllerState s;
    if (port < 0 || port >= MaxControllerPorts) return s;

    static const uint16_t buttonBits[] = {
        N64Button::A, N64Button::B, N64Button::Z, N64Button::Start, N64Button::L, N64Button::R,
        N64Button::DUp, N64Button::DDown, N64Button::DLeft, N64Button::DRight,
        N64Button::CUp, N64Button::CDown, N64Button::CLeft, N64Button::CRight};

    bool digital[4] = {false, false, false, false};  // up, down, left, right
    int analogX = 0, analogY = 0;

    const auto accumulate = [&](const auto& active) {
        for (int a = A; a <= CRight; ++a) {
            if (active(static_cast<Action>(a))) s.buttons |= buttonBits[a];
        }
        digital[0] |= active(StickUp);
        digital[1] |= active(StickDown);
        digital[2] |= active(StickLeft);
        digital[3] |= active(StickRight);
    };

    for (const auto& entry : pads_) {
        const Pad& pad = entry.second;  // not a structured binding: lambdas can't capture those in C++17
        if (pad.port != port) continue;
        s.connected = true;
        accumulate([&](Action a) { return padActionActive(pad, a); });

        // Radial deadzone, rescaled so the stick still reaches full deflection.
        const float vx = padAxis(pad, padStickX_);
        const float vy = -padAxis(pad, padStickY_);  // SDL reports up as negative
        const float magnitude = std::hypot(vx, vy);
        if (magnitude > deadzone_) {
            const float scale = (std::min(magnitude, 1.0f) - deadzone_) / (1.0f - deadzone_) / magnitude;
            analogX = static_cast<int>(std::lround(vx * scale * StickMax));
            analogY = static_cast<int>(std::lround(vy * scale * StickMax));
        }
    }

    if (port == 0) {
        s.connected = true;  // the keyboard is always available on port 1
        accumulate([&](Action a) {
            for (const std::string& key : keyBindings_[a]) {
                if (keysDown_.count(key)) return true;
            }
            return false;
        });
    }

    int x = analogX, y = analogY;
    if (x == 0 && y == 0) {
        const int dx = (digital[3] ? 1 : 0) - (digital[2] ? 1 : 0);
        const int dy = (digital[0] ? 1 : 0) - (digital[1] ? 1 : 0);
        const int magnitude = (dx != 0 && dy != 0) ? DiagonalStick : StickMax;
        x = dx * magnitude;
        y = dy * magnitude;
    }
    s.stickX = static_cast<int8_t>(std::max(-StickMax, std::min(StickMax, x)));
    s.stickY = static_cast<int8_t>(std::max(-StickMax, std::min(StickMax, y)));
    return s;
}

}  // namespace reality64
