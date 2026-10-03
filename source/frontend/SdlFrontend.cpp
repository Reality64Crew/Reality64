#include "frontend/SdlFrontend.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Emulator.h"
#include "frontend/AppIcon.h"

namespace reality64 {

namespace {

constexpr int LogicalWidth = 640;
constexpr int LogicalHeight = 480;

enum class Screen { Menu, Controls, Playing };

struct Color {
    Uint8 r, g, b;
};
constexpr Color White{235, 235, 240};
constexpr Color Dim{140, 140, 155};
constexpr Color Accent{255, 214, 64};
constexpr Color Error{255, 96, 96};
constexpr Color Notice{120, 200, 255};

const char* const MenuItems[] = {"Open ROM...", "Controls", "Quit"};
constexpr int MenuItemCount = 3;
constexpr Uint64 MenuInputGraceMs = 600;

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

    // events
    void pumpEvents();
    void handleKey(const SDL_KeyboardEvent& key);
    void handleGamepadButton(const SDL_GamepadButtonEvent& button);
    void menuKey(SDL_Scancode scancode);
    // The key that launched the program (Enter in a terminal or Explorer) can
    // arrive at the new window; ignore menu input briefly after it appears or
    // regains focus so that it doesn't activate an item by accident.
    void blockMenuInput() { menuBlockedUntil_ = SDL_GetTicks() + MenuInputGraceMs; }
    bool menuInputBlocked() const { return SDL_GetTicks() < menuBlockedUntil_; }
    void activateMenuItem(int index);
    void openGamepad(SDL_JoystickID id);
    void closeGamepad(SDL_JoystickID id);
    void addGenericMapping(SDL_JoystickID id);
    void pollFileDialog();
    void showFileDialog();
    static void SDLCALL onFileChosen(void* userdata, const char* const* files, int filter);

    // game lifecycle
    bool startGame(const std::string& path);
    void stopGame();
    void onAudio(const uint8_t* samples, uint32_t bytes, uint32_t rate);

    // drawing
    void text(float x, float y, float scale, Color color, const std::string& s);
    void centeredText(float y, float scale, Color color, const std::string& s);
    void drawMenu();
    void drawControls();
    void drawGame();
    void drawMessage(float y);
    void updateTitle(double fps);

    InputMapper& input_;
    FrontendOptions options_;

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
    VideoFrame frame_;
    Screen screen_ = Screen::Menu;
    int selected_ = 0;
    Uint64 menuBlockedUntil_ = 0;
    bool running_ = true;
    bool paused_ = false;
    bool haltReported_ = false;
    bool fullscreen_ = false;

    std::string message_;
    bool messageIsError_ = false;

    // The file dialog may call back from another thread.
    std::mutex dialogMutex_;
    bool dialogOpen_ = false;
    std::string dialogResult_;
    std::string dialogError_;
};

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

bool App::init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "error: cannot initialise SDL: %s\n", SDL_GetError());
        return false;
    }

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

    if (options_.fullscreen) {
        fullscreen_ = true;
        SDL_SetWindowFullscreen(window_, true);
    }

    // Audio is optional: carry on silently if there is no output device.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_S16BE;
        spec.channels = 2;
        spec.freq = 44100;
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_) {
            SDL_ResumeAudioStreamDevice(audio_);
            audioRate_ = 44100;
        } else {
            std::fprintf(stderr, "warning: no audio output: %s\n", SDL_GetError());
        }
    }

    loadControllerDatabase();
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
// Game lifecycle
// ---------------------------------------------------------------------------

bool App::startGame(const std::string& path) {
    auto emu = std::make_unique<Emulator>();
    std::string error;
    if (!emu->loadRom(path, error) || !emu->boot(error)) {
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
    input_.releaseAll();
    screen_ = Screen::Playing;
    paused_ = false;
    haltReported_ = false;
    message_.clear();
    return true;
}

void App::stopGame() {
    game_.reset();
    if (audio_) SDL_ClearAudioStream(audio_);
    input_.releaseAll();
    screen_ = Screen::Menu;
    SDL_SetWindowTitle(window_, "Reality64");
}

void App::onAudio(const uint8_t* samples, uint32_t bytes, uint32_t rate) {
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
// Input
// ---------------------------------------------------------------------------

void App::activateMenuItem(int index) {
    switch (index) {
        case 0: showFileDialog(); break;
        case 1: screen_ = Screen::Controls; break;
        case 2: running_ = false; break;
    }
}

void App::menuKey(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_W:
            selected_ = (selected_ + MenuItemCount - 1) % MenuItemCount;
            break;
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_S:
            selected_ = (selected_ + 1) % MenuItemCount;
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
            activateMenuItem(selected_);
            break;
        case SDL_SCANCODE_ESCAPE:
            running_ = false;
            break;
        default:
            break;
    }
}

void App::handleKey(const SDL_KeyboardEvent& key) {
    if (key.repeat && screen_ == Screen::Playing) return;

    if (key.down && key.scancode == SDL_SCANCODE_F11) {
        fullscreen_ = !fullscreen_;
        SDL_SetWindowFullscreen(window_, fullscreen_);
        return;
    }

    switch (screen_) {
        case Screen::Menu:
            if (key.down && !menuInputBlocked()) menuKey(key.scancode);
            break;
        case Screen::Controls:
            if (key.down && !menuInputBlocked() && (key.scancode == SDL_SCANCODE_ESCAPE || key.scancode == SDL_SCANCODE_RETURN ||
                             key.scancode == SDL_SCANCODE_BACKSPACE)) {
                screen_ = Screen::Menu;
            }
            break;
        case Screen::Playing:
            if (key.down && key.scancode == SDL_SCANCODE_ESCAPE) {
                stopGame();
                return;
            }
            if (key.down && key.scancode == SDL_SCANCODE_P) {
                paused_ = !paused_;
                return;
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
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            if (screen_ == Screen::Menu) menuKey(SDL_SCANCODE_DOWN);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
        case SDL_GAMEPAD_BUTTON_START:
            if (screen_ == Screen::Menu) activateMenuItem(selected_);
            else screen_ = Screen::Menu;
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
            if (screen_ == Screen::Controls) screen_ = Screen::Menu;
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
    for (size_t i = 0; i < lines.size() && i < 3; ++i) {
        centeredText(y + static_cast<float>(i) * 16, 1.5f, messageIsError_ ? Error : Notice, lines[i]);
    }
}

void App::drawMenu() {
    if (iconTexture_) {
        const SDL_FRect dst{(LogicalWidth - 210) / 2.0f, 14, 210, 210};
        SDL_RenderTexture(renderer_, iconTexture_, nullptr, &dst);
    } else {
        centeredText(100, 5.0f, White, "Reality64");
    }

    for (int i = 0; i < MenuItemCount; ++i) {
        const bool sel = i == selected_;
        const std::string label = std::string(sel ? "> " : "  ") + MenuItems[i] + (sel ? " <" : "  ");
        centeredText(246.0f + 42.0f * static_cast<float>(i), 3.0f, sel ? Accent : White, label);
    }

    drawMessage(384);
    centeredText(432, 1.5f, Dim, "Up/Down: select   Enter: confirm   Esc: quit");
    centeredText(452, 1.5f, Dim, "Tip: drag a ROM file onto this window");

    const std::string pads = "Gamepads: " + std::to_string(gamepads_.size());
    text(8, 8, 1.5f, Dim, pads);
}

void App::drawControls() {
    centeredText(24, 3.0f, Accent, "Controls");

    char line[96];
    const auto row = [&](float y, Color c, const char* a, const char* b, const char* d) {
        std::snprintf(line, sizeof line, "%-15s%-15s%s", a, b, d);
        text(40, y, 1.5f, c, line);
    };
    row(90, Accent, "N64", "Keyboard", "Gamepad");
    row(116, White, "Analog stick", "Arrow keys", "Left stick");
    row(136, White, "A / B", "X / C", "A / X");
    row(156, White, "Z", "Z", "Left trigger");
    row(176, White, "L / R", "A / S", "Shoulders");
    row(196, White, "Start", "Enter", "Start");
    row(216, White, "D-pad", "T F G H", "D-pad");
    row(236, White, "C buttons", "I J K L", "Right stick");

    text(40, 282, 1.5f, Dim, "In game:  Esc = menu   P = pause   F11 = fullscreen");
    text(40, 308, 1.5f, Dim, "Any gamepad works and up to 4 can be used at once.");
    text(40, 328, 1.5f, Dim, "To remap buttons, copy input.cfg.example to");
    text(40, 348, 1.5f, Dim, "input.cfg next to the program and edit it.");

    centeredText(440, 1.5f, Dim, "Esc / Enter: back");
}

void App::drawGame() {
    if (game_->bus().captureFrame(frame_)) {
        const int w = static_cast<int>(frame_.width), h = static_cast<int>(frame_.height);
        if (!texture_ || w != textureWidth_ || h != textureHeight_) {
            if (texture_) SDL_DestroyTexture(texture_);
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
            if (texture_) SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
            textureWidth_ = w;
            textureHeight_ = h;
        }
        if (texture_) {
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
    }
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
        if (screen_ == Screen::Playing && game_) {
            refresh = game_->bus().frameRate();
            if (!paused_ && !game_->cpu().halted()) {
                game_->runFrame();
            } else if (game_->cpu().halted() && !haltReported_) {
                haltReported_ = true;
                std::fprintf(stderr, "CPU stopped: %s\n", game_->cpu().haltReason().c_str());
            }
            drawGame();
        } else if (screen_ == Screen::Controls) {
            drawControls();
        } else {
            drawMenu();
        }
        SDL_RenderPresent(renderer_);

        ++framesSinceTitle;
        const Uint64 now = SDL_GetTicksNS();
        if (now - fpsStart >= 1000000000ull) {
            updateTitle(framesSinceTitle * 1e9 / static_cast<double>(now - fpsStart));
            framesSinceTitle = 0;
            fpsStart = now;
        }

        // The menus are always paced; a game can opt out with --no-limit.
        if (options_.frameLimit || screen_ != Screen::Playing) {
            nextFrame += static_cast<Uint64>(1e9 / refresh);
            const Uint64 after = SDL_GetTicksNS();
            if (nextFrame > after) SDL_DelayNS(nextFrame - after);
            else if (after - nextFrame > 5 * static_cast<Uint64>(1e9 / refresh)) nextFrame = after;
        }
    }
    return 0;
}

}  // namespace

int runSdlFrontend(InputMapper& input, const FrontendOptions& options, const std::string& romPath) {
    App app(input, options);
    if (!app.init()) return 1;
    return app.run(romPath);
}

}  // namespace reality64
