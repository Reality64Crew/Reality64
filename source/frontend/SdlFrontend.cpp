#include "frontend/SdlFrontend.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Emulator.h"
#include "core/Settings.h"
#include "frontend/AppIcon.h"

namespace reality64 {

namespace {

constexpr int LogicalWidth = 640;
constexpr int LogicalHeight = 480;
constexpr Uint64 MenuInputGraceMs = 600;
constexpr unsigned MaxRecentShown = 4;
constexpr int FastForwardFrames = 4;

enum class Screen { Menu, Settings, Controls, Playing };

struct Color {
    Uint8 r, g, b;
};
constexpr Color White{235, 235, 240};
constexpr Color Dim{140, 140, 155};
constexpr Color Accent{255, 214, 64};
constexpr Color Error{255, 96, 96};
constexpr Color Notice{120, 200, 255};
constexpr Color Good{120, 230, 140};

struct MenuEntry {
    enum Kind { Open, Recent, Settings, Controls, Quit } kind;
    int recentIndex;
    std::string label;
};

enum SettingsRow { RowForce60, RowCpu, RowSmooth, RowVolume, RowFullscreen, RowBack, SettingsRowCount };

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.erase(dot);
    return name;
}

std::string safeFileName(const std::string& s) {
    std::string out;
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return out.empty() ? "game" : out;
}

float normalizeAxis(Sint16 value) {
    return value < 0 ? static_cast<float>(value) / 32768.0f : static_cast<float>(value) / 32767.0f;
}

// Splits `s` into lines of at most `width` characters, breaking at spaces.
std::vector<std::string> wrap(const std::string& s, size_t width) {
    std::vector<std::string> lines;
    std::string line;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(' ', pos);
        if (end == std::string::npos) end = s.size();
        std::string word = s.substr(pos, end - pos);
        pos = end + 1;
        while (word.size() > width) {  // a single very long word (a path)
            if (!line.empty()) { lines.push_back(line); line.clear(); }
            lines.push_back(word.substr(0, width));
            word.erase(0, width);
        }
        if (!line.empty() && line.size() + 1 + word.size() > width) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += ' ';
        line += word;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

std::string truncate(const std::string& s, size_t width) {
    return s.size() <= width ? s : s.substr(0, width - 3) + "...";
}

class App {
public:
    App(InputMapper& input, const FrontendOptions& options) : input_(input), options_(options) {}
    ~App() { shutdown(); }

    bool init();
    int run(const std::string& romPath);

private:
    // setup / teardown
    void shutdown();
    void loadControllerDatabase();
    void loadSettings();
    void saveSettings();
    void applyAudioVolume();

    // events
    void pumpEvents();
    void handleKey(const SDL_KeyboardEvent& key);
    void handleGamepadButton(const SDL_GamepadButtonEvent& button);
    void menuKey(SDL_Scancode scancode);
    void settingsKey(SDL_Scancode scancode);
    void activateMenuEntry(int index);
    void adjustSetting(int row, int direction);
    void openGamepad(SDL_JoystickID id);
    void closeGamepad(SDL_JoystickID id);
    void addGenericMapping(SDL_JoystickID id);
    void pollFileDialog();
    void showFileDialog();
    static void SDLCALL onFileChosen(void* userdata, const char* const* files, int filter);
    void blockMenuInput() { menuBlockedUntil_ = SDL_GetTicks() + MenuInputGraceMs; }
    bool menuInputBlocked() const { return SDL_GetTicks() < menuBlockedUntil_; }

    // game lifecycle and features
    bool startGame(const std::string& path);
    void stopGame();
    void onAudio(const uint8_t* samples, uint32_t bytes, uint32_t rate);
    std::string statePath() const;
    void saveState();
    void loadState();
    void takeScreenshot();
    void showToast(const std::string& text, Color color = Good);

    // drawing
    void buildMenu();
    void text(float x, float y, float scale, Color color, const std::string& s);
    void centeredText(float y, float scale, Color color, const std::string& s);
    void drawMenu();
    void drawSettings();
    void drawControls();
    void drawGame();
    void drawMessage(float y);
    void updateTitle(double fps);

    InputMapper& input_;
    FrontendOptions options_;
    Settings settings_;
    std::string prefPath_;

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;      // game frame
    SDL_Texture* iconTexture_ = nullptr;  // logo on the menu
    int textureWidth_ = 0;
    int textureHeight_ = 0;
    SDL_AudioStream* audio_ = nullptr;
    uint32_t audioRate_ = 0;
    std::map<SDL_JoystickID, SDL_Gamepad*> gamepads_;

    std::unique_ptr<Emulator> game_;
    std::string gamePath_;
    VideoFrame frame_;
    Screen screen_ = Screen::Menu;
    std::vector<MenuEntry> menu_;
    int selected_ = 0;
    int settingsSelected_ = 0;
    Uint64 menuBlockedUntil_ = 0;
    bool running_ = true;
    bool paused_ = false;
    bool fastForward_ = false;
    bool haltReported_ = false;
    bool fullscreen_ = false;

    std::string message_;
    bool messageIsError_ = false;
    std::string toast_;
    Color toastColor_ = Good;
    Uint64 toastUntil_ = 0;

    // The file dialog may call back from another thread.
    std::mutex dialogMutex_;
    bool dialogOpen_ = false;
    std::string dialogResult_;
    std::string dialogError_;
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool App::init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "error: cannot initialise SDL: %s\n", SDL_GetError());
        return false;
    }
    loadSettings();

    window_ = SDL_CreateWindow("Reality64", LogicalWidth, LogicalHeight, SDL_WINDOW_RESIZABLE);
    if (!window_) {
        std::fprintf(stderr, "error: cannot create window: %s\n", SDL_GetError());
        return false;
    }
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        std::fprintf(stderr, "error: cannot create renderer: %s\n", SDL_GetError());
        return false;
    }
    // Always present a 4:3 picture, letterboxed in whatever size the window is.
    SDL_SetRenderLogicalPresentation(renderer_, LogicalWidth, LogicalHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    if (SDL_IOStream* io = SDL_IOFromConstMem(kAppIconPng, static_cast<size_t>(kAppIconPngSize))) {
        if (SDL_Surface* icon = SDL_LoadPNG_IO(io, true)) {
            SDL_SetWindowIcon(window_, icon);
            iconTexture_ = SDL_CreateTextureFromSurface(renderer_, icon);
            if (iconTexture_) SDL_SetTextureScaleMode(iconTexture_, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(icon);
        } else {
            std::fprintf(stderr, "warning: cannot load the window icon: %s\n", SDL_GetError());
        }
    }

    if (options_.fullscreen) settings_.fullscreen = true;
    fullscreen_ = settings_.fullscreen;
    if (fullscreen_) SDL_SetWindowFullscreen(window_, true);

    // Audio is optional: carry on silently if there is no output device.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_S16BE;
        spec.channels = 2;
        spec.freq = 44100;
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_) {
            applyAudioVolume();
            SDL_ResumeAudioStreamDevice(audio_);
            audioRate_ = 44100;
        } else {
            std::fprintf(stderr, "warning: no audio output: %s\n", SDL_GetError());
        }
    }

    loadControllerDatabase();
    buildMenu();
    return true;
}

void App::shutdown() {
    game_.reset();
    for (auto& entry : gamepads_) SDL_CloseGamepad(entry.second);
    gamepads_.clear();
    if (audio_) SDL_DestroyAudioStream(audio_);
    audio_ = nullptr;
    if (iconTexture_) SDL_DestroyTexture(iconTexture_);
    iconTexture_ = nullptr;
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = nullptr;
    if (renderer_) SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
    if (window_) SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_Quit();
}

// Extra controller mappings can be dropped next to the executable, in the SDL
// GameControllerDB format (https://github.com/mdqinc/SDL_GameControllerDB).
void App::loadControllerDatabase() {
    std::string path = "gamecontrollerdb.txt";
    if (const char* base = SDL_GetBasePath()) path = std::string(base) + path;
    const int added = SDL_AddGamepadMappingsFromFile(path.c_str());
    if (added > 0) std::fprintf(stderr, "Loaded %d gamepad mappings from %s\n", added, path.c_str());
}

void App::loadSettings() {
    if (char* pref = SDL_GetPrefPath("Reality64Crew", "Reality64")) {
        prefPath_ = pref;
        SDL_free(pref);
    }
    if (prefPath_.empty()) return;

    std::ifstream f(prefPath_ + "settings.cfg", std::ios::binary);
    if (!f) return;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::string> errors;
    settings_.parse(text, errors);
    for (const std::string& e : errors) std::fprintf(stderr, "settings.cfg: %s\n", e.c_str());
}

void App::saveSettings() {
    if (prefPath_.empty()) return;
    std::ofstream f(prefPath_ + "settings.cfg", std::ios::binary | std::ios::trunc);
    if (f) f << settings_.serialize();
}

void App::applyAudioVolume() {
    if (audio_) SDL_SetAudioStreamGain(audio_, static_cast<float>(settings_.volume) / 100.0f);
}

// ---------------------------------------------------------------------------
// Gamepads
// ---------------------------------------------------------------------------

void App::openGamepad(SDL_JoystickID id) {
    if (gamepads_.count(id)) return;
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        std::fprintf(stderr, "warning: cannot open gamepad %u: %s\n", static_cast<unsigned>(id), SDL_GetError());
        return;
    }
    gamepads_[id] = pad;
    const int port = input_.padConnected(static_cast<uint32_t>(id));
    const char* name = SDL_GetGamepadName(pad);
    if (port >= 0) std::fprintf(stderr, "Gamepad connected: %s -> controller %d\n", name ? name : "?", port + 1);
    else std::fprintf(stderr, "Gamepad connected: %s (all four controller ports are in use)\n", name ? name : "?");
}

void App::closeGamepad(SDL_JoystickID id) {
    auto it = gamepads_.find(id);
    if (it == gamepads_.end()) return;
    SDL_CloseGamepad(it->second);
    gamepads_.erase(it);
    input_.padDisconnected(static_cast<uint32_t>(id));
    std::fprintf(stderr, "Gamepad disconnected\n");
}

// SDL only exposes devices it has a mapping for as gamepads. For any other
// joystick, install a conventional layout so it works out of the box; it can
// be corrected with a gamecontrollerdb.txt entry.
void App::addGenericMapping(SDL_JoystickID id) {
    char guid[64];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
    const char* name = SDL_GetJoystickNameForID(id);
    const std::string mapping = std::string(guid) + "," + (name ? name : "Generic joystick") +
        ",a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,"
        "leftstick:b8,rightstick:b9,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";
    if (SDL_AddGamepadMapping(mapping.c_str()) >= 0) {
        std::fprintf(stderr, "Using a generic layout for unrecognised controller '%s'\n", name ? name : "?");
    }
}

// ---------------------------------------------------------------------------
// Game lifecycle and features
// ---------------------------------------------------------------------------

bool App::startGame(const std::string& path) {
    auto emu = std::make_unique<Emulator>();
    std::string error;
    if (!emu->loadRom(path, error)) {
        message_ = "Could not open that ROM: " + error;
        messageIsError_ = true;
        settings_.removeRecent(path);
        saveSettings();
        buildMenu();
        return false;
    }

    // Command-line options win over the saved settings.
    const unsigned cpi = options_.cyclesPerInstruction ? options_.cyclesPerInstruction : settings_.cyclesPerInstruction;
    const bool force60 = options_.force60 >= 0 ? options_.force60 != 0 : settings_.force60;
    emu->setCyclesPerInstruction(cpi);
    emu->setForcedRefresh(force60 ? 60 : 0);

    if (!emu->boot(error)) {
        message_ = "Could not start that ROM: " + error;
        messageIsError_ = true;
        return false;
    }
    emu->bus().setControllerProvider([this](int port) { return input_.state(port); });
    if (audio_) {
        emu->bus().setAudioSink([this](const uint8_t* samples, uint32_t bytes, uint32_t rate) {
            onAudio(samples, bytes, rate);
        });
    }

    game_ = std::move(emu);
    gamePath_ = path;
    input_.releaseAll();
    screen_ = Screen::Playing;
    paused_ = false;
    fastForward_ = false;
    haltReported_ = false;
    message_.clear();

    settings_.addRecent(path);
    saveSettings();
    buildMenu();
    return true;
}

void App::stopGame() {
    game_.reset();
    if (audio_) SDL_ClearAudioStream(audio_);
    input_.releaseAll();
    screen_ = Screen::Menu;
    SDL_SetWindowTitle(window_, "Reality64");
    blockMenuInput();
}

void App::onAudio(const uint8_t* samples, uint32_t bytes, uint32_t rate) {
    if (fastForward_) return;  // sped-up sound is just noise
    if (rate != audioRate_) {
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_S16BE;
        spec.channels = 2;
        spec.freq = static_cast<int>(rate);
        SDL_SetAudioStreamFormat(audio_, &spec, nullptr);
        audioRate_ = rate;
    }
    // Keep latency bounded: if more than half a second is already queued, drop.
    if (SDL_GetAudioStreamQueued(audio_) > static_cast<int>(rate * 2)) return;
    SDL_PutAudioStreamData(audio_, samples, static_cast<int>(bytes));
}

void App::showToast(const std::string& text, Color color) {
    toast_ = text;
    toastColor_ = color;
    toastUntil_ = SDL_GetTicks() + 2500;
}

// One save state slot per game, kept in the per-user data folder.
std::string App::statePath() const {
    if (prefPath_.empty() || !game_ || !game_->rom()) return {};
    const RomHeader& h = game_->rom()->header();
    char crc[24];
    std::snprintf(crc, sizeof crc, "_%08X%08X", h.crc1, h.crc2);
    return prefPath_ + "states/" + safeFileName(h.name) + crc + ".state";
}

void App::saveState() {
    const std::string path = statePath();
    if (path.empty()) {
        showToast("No place to save states", Error);
        return;
    }
    SDL_CreateDirectory((prefPath_ + "states").c_str());
    const std::vector<uint8_t> data = game_->saveState();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (f) showToast("State saved");
    else showToast("Could not write the state file", Error);
}

void App::loadState() {
    const std::string path = statePath();
    std::ifstream f(path, std::ios::binary);
    if (path.empty() || !f) {
        showToast("No saved state for this game yet", Error);
        return;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string error;
    if (game_->loadState(data, error)) {
        if (audio_) SDL_ClearAudioStream(audio_);
        haltReported_ = false;
        showToast("State loaded");
    } else {
        showToast("Cannot load state: " + error, Error);
    }
}

void App::takeScreenshot() {
    if (!game_ || !game_->bus().captureFrame(frame_)) {
        showToast("Nothing to capture yet", Error);
        return;
    }
    if (prefPath_.empty()) {
        showToast("No place to save screenshots", Error);
        return;
    }
    SDL_CreateDirectory((prefPath_ + "screenshots").c_str());

    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &local);
    const std::string name = game_->rom() ? safeFileName(game_->rom()->header().name) : "game";
    const std::string path = prefPath_ + "screenshots/" + name + "_" + stamp + ".png";

    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(frame_.width), static_cast<int>(frame_.height),
                                                 SDL_PIXELFORMAT_RGBA32, frame_.rgba.data(),
                                                 static_cast<int>(frame_.width) * 4);
    const bool saved = surface && SDL_SavePNG(surface, path.c_str());
    if (surface) SDL_DestroySurface(surface);
    showToast(saved ? "Screenshot saved" : "Could not save the screenshot", saved ? Good : Error);
}

// ---------------------------------------------------------------------------
// File dialog
// ---------------------------------------------------------------------------

void App::onFileChosen(void* userdata, const char* const* files, int) {
    App* app = static_cast<App*>(userdata);
    std::lock_guard<std::mutex> lock(app->dialogMutex_);
    if (!files) app->dialogError_ = SDL_GetError();
    else if (files[0]) app->dialogResult_ = files[0];
    app->dialogOpen_ = false;  // cancelled: nothing selected
}

void App::showFileDialog() {
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (dialogOpen_) return;
        dialogOpen_ = true;
    }
    static const SDL_DialogFileFilter filters[] = {
        {"N64 ROMs", "z64;n64;v64;rom"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(&App::onFileChosen, this, window_, filters, 2, nullptr, false);
}

void App::pollFileDialog() {
    std::string path, error;
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        path.swap(dialogResult_);
        error.swap(dialogError_);
    }
    if (!error.empty()) {
        message_ = "Could not open the file dialog: " + error;
        messageIsError_ = true;
    }
    if (!path.empty()) startGame(path);
}

// ---------------------------------------------------------------------------
// Menus and input
// ---------------------------------------------------------------------------

void App::buildMenu() {
    menu_.clear();
    menu_.push_back({MenuEntry::Open, 0, "Open ROM..."});
    for (size_t i = 0; i < settings_.recent.size() && i < MaxRecentShown; ++i) {
        menu_.push_back({MenuEntry::Recent, static_cast<int>(i), truncate(baseName(settings_.recent[i]), 34)});
    }
    menu_.push_back({MenuEntry::Settings, 0, "Settings"});
    menu_.push_back({MenuEntry::Controls, 0, "Controls"});
    menu_.push_back({MenuEntry::Quit, 0, "Quit"});
    selected_ = std::min(selected_, static_cast<int>(menu_.size()) - 1);
}

void App::activateMenuEntry(int index) {
    if (index < 0 || index >= static_cast<int>(menu_.size())) return;
    const MenuEntry& entry = menu_[static_cast<size_t>(index)];
    switch (entry.kind) {
        case MenuEntry::Open: showFileDialog(); break;
        case MenuEntry::Recent: startGame(settings_.recent[static_cast<size_t>(entry.recentIndex)]); break;
        case MenuEntry::Settings: screen_ = Screen::Settings; settingsSelected_ = 0; break;
        case MenuEntry::Controls: screen_ = Screen::Controls; break;
        case MenuEntry::Quit: running_ = false; break;
    }
}

void App::menuKey(SDL_Scancode scancode) {
    const int count = static_cast<int>(menu_.size());
    switch (scancode) {
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_W:
            selected_ = (selected_ + count - 1) % count;
            break;
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_S:
            selected_ = (selected_ + 1) % count;
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
            activateMenuEntry(selected_);
            break;
        case SDL_SCANCODE_ESCAPE:
            running_ = false;
            break;
        default:
            break;
    }
}

// direction: +1 / -1 for Right / Left, 0 for Enter (toggle or advance).
void App::adjustSetting(int row, int direction) {
    switch (row) {
        case RowForce60: settings_.force60 = !settings_.force60; break;
        case RowCpu: {
            int v = static_cast<int>(settings_.cyclesPerInstruction) + (direction == 0 ? 1 : direction);
            if (v > static_cast<int>(Settings::MaxCyclesPerInstruction)) v = Settings::MinCyclesPerInstruction;
            if (v < static_cast<int>(Settings::MinCyclesPerInstruction)) v = Settings::MaxCyclesPerInstruction;
            settings_.cyclesPerInstruction = static_cast<unsigned>(v);
            break;
        }
        case RowSmooth: settings_.smoothScaling = !settings_.smoothScaling; break;
        case RowVolume: {
            int v = settings_.volume + (direction == 0 ? 10 : direction * 10);
            if (v > 100) v = 0;
            if (v < 0) v = 100;
            settings_.volume = v;
            applyAudioVolume();
            break;
        }
        case RowFullscreen:
            settings_.fullscreen = !settings_.fullscreen;
            fullscreen_ = settings_.fullscreen;
            SDL_SetWindowFullscreen(window_, fullscreen_);
            break;
        case RowBack:
            screen_ = Screen::Menu;
            break;
    }
    saveSettings();
}

void App::settingsKey(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_W:
            settingsSelected_ = (settingsSelected_ + SettingsRowCount - 1) % SettingsRowCount;
            break;
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_S:
            settingsSelected_ = (settingsSelected_ + 1) % SettingsRowCount;
            break;
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_A:
            if (settingsSelected_ != RowBack) adjustSetting(settingsSelected_, -1);
            break;
        case SDL_SCANCODE_RIGHT:
        case SDL_SCANCODE_D:
            if (settingsSelected_ != RowBack) adjustSetting(settingsSelected_, +1);
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
            adjustSetting(settingsSelected_, 0);
            break;
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_BACKSPACE:
            screen_ = Screen::Menu;
            break;
        default:
            break;
    }
}

void App::handleKey(const SDL_KeyboardEvent& key) {
    if (key.repeat && screen_ == Screen::Playing) return;

    if (key.down && key.scancode == SDL_SCANCODE_F11) {
        fullscreen_ = !fullscreen_;
        settings_.fullscreen = fullscreen_;
        SDL_SetWindowFullscreen(window_, fullscreen_);
        saveSettings();
        return;
    }

    switch (screen_) {
        case Screen::Menu:
            if (key.down && !menuInputBlocked()) menuKey(key.scancode);
            break;
        case Screen::Settings:
            if (key.down && !menuInputBlocked()) settingsKey(key.scancode);
            break;
        case Screen::Controls:
            if (key.down && !menuInputBlocked() && (key.scancode == SDL_SCANCODE_ESCAPE || key.scancode == SDL_SCANCODE_RETURN ||
                                                    key.scancode == SDL_SCANCODE_BACKSPACE)) {
                screen_ = Screen::Menu;
            }
            break;
        case Screen::Playing:
            if (key.scancode == SDL_SCANCODE_TAB) {  // hold to fast-forward
                fastForward_ = key.down;
                return;
            }
            if (key.down) {
                switch (key.scancode) {
                    case SDL_SCANCODE_ESCAPE: stopGame(); return;
                    case SDL_SCANCODE_P: paused_ = !paused_; return;
                    case SDL_SCANCODE_F5: saveState(); return;
                    case SDL_SCANCODE_F7: loadState(); return;
                    case SDL_SCANCODE_F12: takeScreenshot(); return;
                    default: break;
                }
            }
            if (const char* name = SDL_GetScancodeName(key.scancode)) {
                if (*name) input_.keyEvent(name, key.down);
            }
            break;
    }
}

// Gamepads can drive the menus: D-pad to move, A to select, B to go back.
void App::handleGamepadButton(const SDL_GamepadButtonEvent& button) {
    if (screen_ == Screen::Playing || !button.down || menuInputBlocked()) return;
    switch (button.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            if (screen_ == Screen::Menu) menuKey(SDL_SCANCODE_UP);
            else if (screen_ == Screen::Settings) settingsKey(SDL_SCANCODE_UP);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            if (screen_ == Screen::Menu) menuKey(SDL_SCANCODE_DOWN);
            else if (screen_ == Screen::Settings) settingsKey(SDL_SCANCODE_DOWN);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            if (screen_ == Screen::Settings) settingsKey(SDL_SCANCODE_LEFT);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            if (screen_ == Screen::Settings) settingsKey(SDL_SCANCODE_RIGHT);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
        case SDL_GAMEPAD_BUTTON_START:
            if (screen_ == Screen::Menu) activateMenuEntry(selected_);
            else if (screen_ == Screen::Settings) settingsKey(SDL_SCANCODE_RETURN);
            else screen_ = Screen::Menu;
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
            if (screen_ != Screen::Menu) screen_ = Screen::Menu;
            break;
        default:
            break;
    }
}

void App::pumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                running_ = false;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                handleKey(event.key);
                break;
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data) startGame(event.drop.data);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                input_.releaseAll();
                fastForward_ = false;
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                blockMenuInput();
                break;
            case SDL_EVENT_JOYSTICK_ADDED:
                if (!SDL_IsGamepad(event.jdevice.which)) {
                    addGenericMapping(event.jdevice.which);
                    if (SDL_IsGamepad(event.jdevice.which)) openGamepad(event.jdevice.which);
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                openGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                closeGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const char* name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(event.gbutton.button));
                if (name) input_.padButton(static_cast<uint32_t>(event.gbutton.which), name, event.gbutton.down);
                handleGamepadButton(event.gbutton);
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                const char* name = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(event.gaxis.axis));
                if (name) input_.padAxis(static_cast<uint32_t>(event.gaxis.which), name, normalizeAxis(event.gaxis.value));
                break;
            }
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void App::text(float x, float y, float scale, Color color, const std::string& s) {
    SDL_SetRenderScale(renderer_, scale, scale);
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, 255);
    SDL_RenderDebugText(renderer_, x / scale, y / scale, s.c_str());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
}

void App::centeredText(float y, float scale, Color color, const std::string& s) {
    const float width = static_cast<float>(s.size()) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;
    text((LogicalWidth - width) / 2, y, scale, color, s);
}

void App::drawMessage(float y) {
    if (message_.empty()) return;
    const std::vector<std::string> lines = wrap(message_, 50);
    for (size_t i = 0; i < lines.size() && i < 2; ++i) {
        centeredText(y + static_cast<float>(i) * 16, 1.5f, messageIsError_ ? Error : Notice, lines[i]);
    }
}

void App::drawMenu() {
    if (iconTexture_) {
        const SDL_FRect dst{(LogicalWidth - 150) / 2.0f, 8, 150, 150};
        SDL_RenderTexture(renderer_, iconTexture_, nullptr, &dst);
    } else {
        centeredText(60, 4.0f, White, "Reality64");
    }

    float y = 168;
    for (size_t i = 0; i < menu_.size(); ++i) {
        const MenuEntry& entry = menu_[i];
        const bool sel = static_cast<int>(i) == selected_;
        const bool recent = entry.kind == MenuEntry::Recent;
        const float scale = recent ? 1.75f : 2.5f;
        const std::string label = std::string(sel ? "> " : "  ") + entry.label + (sel ? " <" : "  ");
        centeredText(y, scale, sel ? Accent : (recent ? Dim : White), label);
        y += recent ? 22.0f : 32.0f;
    }

    drawMessage(392);
    centeredText(440, 1.5f, Dim, "Up/Down: select   Enter: confirm   Esc: quit");
    centeredText(458, 1.5f, Dim, "Tip: drag a ROM file onto this window");

    text(8, 8, 1.5f, Dim, "Gamepads: " + std::to_string(gamepads_.size()));
}

void App::drawSettings() {
    centeredText(20, 3.0f, Accent, "Settings");

    const auto onOff = [](bool v) { return v ? "ON" : "OFF"; };
    const std::string values[SettingsRowCount] = {
        onOff(settings_.force60),
        std::to_string(settings_.cyclesPerInstruction) + " cycles/instr",
        onOff(settings_.smoothScaling),
        std::to_string(settings_.volume) + "%",
        onOff(settings_.fullscreen),
        "",
    };
    const char* names[SettingsRowCount] = {"Always 60 fps", "CPU speed", "Smooth scaling", "Volume", "Fullscreen", "Back"};
    const char* help[SettingsRowCount] = {
        "PAL games normally run at 50 Hz; ON shows them at 60 Hz (about 20% faster).",
        "Higher = lighter on your PC. Lower = more demanding but more precise timing.",
        "Smooths the picture when it is scaled up (OFF = sharp pixels).",
        "Sound volume. Left/Right to change.",
        "Also toggled with F11.",
        "Return to the main menu.",
    };

    for (int i = 0; i < SettingsRowCount; ++i) {
        const bool sel = i == settingsSelected_;
        const float y = 90.0f + 44.0f * static_cast<float>(i);
        std::string line = std::string(sel ? "> " : "  ") + names[i];
        text(40, y, 2.0f, sel ? Accent : White, line);
        if (i != RowBack) text(340, y, 2.0f, sel ? Accent : Dim, values[i]);
    }

    const std::vector<std::string> lines = wrap(help[settingsSelected_], 52);
    for (size_t i = 0; i < lines.size() && i < 2; ++i) centeredText(372 + static_cast<float>(i) * 16, 1.5f, Notice, lines[i]);
    centeredText(420, 1.5f, Dim, "Changes apply to the next game you start.");
    centeredText(440, 1.5f, Dim, "Up/Down select   Left/Right change   Esc back");
}

void App::drawControls() {
    centeredText(16, 3.0f, Accent, "Controls");

    char line[96];
    const auto row = [&](float y, Color c, const char* a, const char* b, const char* d) {
        std::snprintf(line, sizeof line, "%-15s%-15s%s", a, b, d);
        text(40, y, 1.5f, c, line);
    };
    row(72, Accent, "N64", "Keyboard", "Gamepad");
    row(98, White, "Analog stick", "Arrow keys", "Left stick");
    row(118, White, "A / B", "X / C", "A / X");
    row(138, White, "Z", "Z", "Left trigger");
    row(158, White, "L / R", "A / S", "Shoulders");
    row(178, White, "Start", "Enter", "Start");
    row(198, White, "D-pad", "T F G H", "D-pad");
    row(218, White, "C buttons", "I J K L", "Right stick");

    text(40, 258, 1.5f, Accent, "In game");
    text(40, 280, 1.5f, White, "Esc  menu            P    pause");
    text(40, 298, 1.5f, White, "F5   save state      F7   load state");
    text(40, 316, 1.5f, White, "F12  screenshot      F11  fullscreen");
    text(40, 334, 1.5f, White, "Tab  hold to fast-forward");

    text(40, 366, 1.5f, Dim, "Any gamepad works; up to 4 can be used at once.");
    text(40, 384, 1.5f, Dim, "To remap, copy input.cfg.example to input.cfg");
    text(40, 402, 1.5f, Dim, "next to the program and edit it.");

    centeredText(444, 1.5f, Dim, "Esc / Enter: back");
}

void App::drawGame() {
    if (game_->bus().captureFrame(frame_)) {
        const int w = static_cast<int>(frame_.width), h = static_cast<int>(frame_.height);
        if (!texture_ || w != textureWidth_ || h != textureHeight_) {
            if (texture_) SDL_DestroyTexture(texture_);
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
            textureWidth_ = w;
            textureHeight_ = h;
        }
        if (texture_) {
            SDL_SetTextureScaleMode(texture_, settings_.smoothScaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
            SDL_UpdateTexture(texture_, nullptr, frame_.rgba.data(), w * 4);
            SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
        }
    }

    // Status overlays, so problems are visible without a console.
    if (game_->cpu().halted()) {
        const std::vector<std::string> lines = wrap("CPU stopped: " + game_->cpu().haltReason(), 52);
        for (size_t i = 0; i < lines.size() && i < 4; ++i) {
            centeredText(8 + static_cast<float>(i) * 14, 1.5f, Error, lines[i]);
        }
        centeredText(456, 1.5f, Dim, "Esc: back to the menu");
    } else if (paused_) {
        centeredText(8, 2.0f, Accent, "PAUSED");
    } else if (fastForward_) {
        centeredText(8, 2.0f, Notice, ">> FAST FORWARD");
    }
    if (SDL_GetTicks() < toastUntil_) centeredText(452, 1.75f, toastColor_, toast_);
}

void App::updateTitle(double fps) {
    if (screen_ != Screen::Playing || !game_) return;
    const Rom* rom = game_->rom();
    std::string title = "Reality64";
    if (rom && !rom->header().name.empty()) title += " - " + rom->header().name;
    char buf[64];
    std::snprintf(buf, sizeof buf, " [%.0f fps]", fps);
    title += buf;
    SDL_SetWindowTitle(window_, title.c_str());
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

int App::run(const std::string& romPath) {
    blockMenuInput();
    if (!romPath.empty()) startGame(romPath);

    Uint64 nextFrame = SDL_GetTicksNS();
    Uint64 fpsStart = nextFrame;
    int framesSinceTitle = 0;
    Screen lastScreen = screen_;

    while (running_) {
        pumpEvents();
        pollFileDialog();

        if (screen_ != lastScreen) {  // don't try to catch up after a screen change
            lastScreen = screen_;
            nextFrame = SDL_GetTicksNS();
        }

        SDL_SetRenderDrawColor(renderer_, 18, 18, 28, 255);
        SDL_RenderClear(renderer_);

        double refresh = 60.0;
        int emulated = 0;
        if (screen_ == Screen::Playing && game_) {
            refresh = game_->bus().frameRate();
            if (!paused_ && !game_->cpu().halted()) {
                const int frames = fastForward_ ? FastForwardFrames : 1;
                for (int i = 0; i < frames && !game_->cpu().halted(); ++i) {
                    game_->runFrame();
                    ++emulated;
                }
            } else if (game_->cpu().halted() && !haltReported_) {
                haltReported_ = true;
                std::fprintf(stderr, "CPU stopped: %s\n", game_->cpu().haltReason().c_str());
            }
            drawGame();
        } else if (screen_ == Screen::Controls) {
            drawControls();
        } else if (screen_ == Screen::Settings) {
            drawSettings();
        } else {
            drawMenu();
        }
        SDL_RenderPresent(renderer_);

        framesSinceTitle += emulated > 0 ? emulated : 1;
        const Uint64 now = SDL_GetTicksNS();
        if (now - fpsStart >= 1000000000ull) {
            updateTitle(framesSinceTitle * 1e9 / static_cast<double>(now - fpsStart));
            framesSinceTitle = 0;
            fpsStart = now;
        }

        // Menus are always paced; a game can opt out with --no-limit or fast-forward.
        const bool pace = (options_.frameLimit && !fastForward_) || screen_ != Screen::Playing;
        if (pace) {
            const Uint64 frameNs = static_cast<Uint64>(1e9 / refresh);
            nextFrame += frameNs;
            const Uint64 after = SDL_GetTicksNS();
            if (nextFrame > after) SDL_DelayPrecise(nextFrame - after);
            else if (after - nextFrame > 5 * frameNs) nextFrame = after;  // fell behind: don't try to catch up
        } else {
            nextFrame = SDL_GetTicksNS();
        }
    }
    saveSettings();
    return 0;
}

}  // namespace

int runSdlFrontend(InputMapper& input, const FrontendOptions& options, const std::string& romPath) {
    App app(input, options);
    if (!app.init()) return 1;
    return app.run(romPath);
}

}  // namespace reality64
