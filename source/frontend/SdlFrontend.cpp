#include "frontend/SdlFrontend.h"

#include <SDL3/SDL.h>

#include "frontend/AppIcon.h"

#include <cstdio>
#include <map>
#include <string>

namespace reality64 {

namespace {

constexpr int LogicalWidth = 640;
constexpr int LogicalHeight = 480;

class Frontend {
public:
    Frontend(Emulator& emu, InputMapper& input, const FrontendOptions& options)
        : emu_(emu), input_(input), options_(options) {}
    ~Frontend() { shutdown(); }

    bool init();
    int run();

private:
    void pumpEvents();
    void handleKey(const SDL_KeyboardEvent& key);
    void openGamepad(SDL_JoystickID id);
    void closeGamepad(SDL_JoystickID id);
    void addGenericMapping(SDL_JoystickID id);
    void loadControllerDatabase();
    void present();
    void onAudio(const uint8_t* samples, uint32_t bytes, uint32_t rate);
    void updateTitle(double fps);
    void shutdown();

    Emulator& emu_;
    InputMapper& input_;
    FrontendOptions options_;

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int textureWidth_ = 0;
    int textureHeight_ = 0;
    SDL_AudioStream* audio_ = nullptr;
    uint32_t audioRate_ = 0;
    std::map<SDL_JoystickID, SDL_Gamepad*> gamepads_;

    VideoFrame frame_;
    bool running_ = true;
    bool paused_ = false;
    bool haltReported_ = false;
    bool fullscreen_ = false;
};

float normalizeAxis(Sint16 value) {
    return value < 0 ? static_cast<float>(value) / 32768.0f : static_cast<float>(value) / 32767.0f;
}

bool Frontend::init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "error: cannot initialise SDL: %s\n", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow("Reality64", LogicalWidth, LogicalHeight, SDL_WINDOW_RESIZABLE);
    if (!window_) {
        std::fprintf(stderr, "error: cannot create window: %s\n", SDL_GetError());
        return false;
    }
    if (SDL_IOStream* io = SDL_IOFromConstMem(kAppIconPng, static_cast<size_t>(kAppIconPngSize))) {
        if (SDL_Surface* icon = SDL_LoadPNG_IO(io, true)) {
            SDL_SetWindowIcon(window_, icon);
            SDL_DestroySurface(icon);
        } else {
            std::fprintf(stderr, "warning: cannot load the window icon: %s\n", SDL_GetError());
        }
    }
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        std::fprintf(stderr, "error: cannot create renderer: %s\n", SDL_GetError());
        return false;
    }
    // Always present a 4:3 picture, letterboxed in whatever size the window is.
    SDL_SetRenderLogicalPresentation(renderer_, LogicalWidth, LogicalHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);

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
            emu_.bus().setAudioSink([this](const uint8_t* samples, uint32_t bytes, uint32_t rate) {
                onAudio(samples, bytes, rate);
            });
        } else {
            std::fprintf(stderr, "warning: no audio output: %s\n", SDL_GetError());
        }
    }

    loadControllerDatabase();
    return true;
}

void Frontend::shutdown() {
    emu_.bus().setAudioSink(nullptr);
    for (auto& [id, pad] : gamepads_) SDL_CloseGamepad(pad);
    gamepads_.clear();
    if (audio_) SDL_DestroyAudioStream(audio_);
    audio_ = nullptr;
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
void Frontend::loadControllerDatabase() {
    std::string path = "gamecontrollerdb.txt";
    if (const char* base = SDL_GetBasePath()) path = std::string(base) + path;
    const int added = SDL_AddGamepadMappingsFromFile(path.c_str());
    if (added > 0) std::fprintf(stderr, "Loaded %d gamepad mappings from %s\n", added, path.c_str());
}

void Frontend::openGamepad(SDL_JoystickID id) {
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

void Frontend::closeGamepad(SDL_JoystickID id) {
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
void Frontend::addGenericMapping(SDL_JoystickID id) {
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

void Frontend::handleKey(const SDL_KeyboardEvent& key) {
    if (key.repeat) return;
    if (key.down) {
        switch (key.scancode) {
            case SDL_SCANCODE_ESCAPE: running_ = false; return;
            case SDL_SCANCODE_F11:
                fullscreen_ = !fullscreen_;
                SDL_SetWindowFullscreen(window_, fullscreen_);
                return;
            case SDL_SCANCODE_P:
                paused_ = !paused_;
                return;
            default: break;
        }
    }
    const char* name = SDL_GetScancodeName(key.scancode);
    if (name && *name) input_.keyEvent(name, key.down);
}

void Frontend::pumpEvents() {
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
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                input_.releaseAll();
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

void Frontend::onAudio(const uint8_t* samples, uint32_t bytes, uint32_t rate) {
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

void Frontend::present() {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);

    if (emu_.bus().captureFrame(frame_)) {
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
    SDL_RenderPresent(renderer_);
}

void Frontend::updateTitle(double fps) {
    const Rom* rom = emu_.rom();
    std::string title = "Reality64";
    if (rom && !rom->header().name.empty()) title += " - " + rom->header().name;
    char buf[64];
    std::snprintf(buf, sizeof buf, " [%.0f fps]", fps);
    title += buf;
    if (paused_) title += " (paused)";
    if (emu_.cpu().halted()) title += " (CPU halted)";
    SDL_SetWindowTitle(window_, title.c_str());
}

int Frontend::run() {
    const double refresh = emu_.bus().frameRate();
    const Uint64 frameNs = static_cast<Uint64>(1e9 / refresh);
    Uint64 nextFrame = SDL_GetTicksNS();
    Uint64 fpsStart = nextFrame;
    int framesSinceTitle = 0;

    while (running_) {
        pumpEvents();

        if (!paused_ && !emu_.cpu().halted()) {
            emu_.runFrame();
        } else if (emu_.cpu().halted() && !haltReported_) {
            haltReported_ = true;
            std::fprintf(stderr, "CPU halted: %s\n", emu_.cpu().haltReason().c_str());
        }

        present();
        ++framesSinceTitle;

        const Uint64 now = SDL_GetTicksNS();
        if (now - fpsStart >= 1000000000ull) {
            updateTitle(framesSinceTitle * 1e9 / static_cast<double>(now - fpsStart));
            framesSinceTitle = 0;
            fpsStart = now;
        }

        if (options_.frameLimit) {
            nextFrame += frameNs;
            const Uint64 after = SDL_GetTicksNS();
            if (nextFrame > after) SDL_DelayNS(nextFrame - after);
            else if (after - nextFrame > 5 * frameNs) nextFrame = after;  // fell behind: don't try to catch up
        }
    }
    return emu_.cpu().halted() ? 2 : 0;
}

}  // namespace

int runSdlFrontend(Emulator& emu, InputMapper& input, const FrontendOptions& options) {
    Frontend frontend(emu, input, options);
    if (!frontend.init()) return 1;
    return frontend.run();
}

}  // namespace reality64
