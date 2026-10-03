# Reality64
A modern Nintendo 64 emulator.

## Status

Early work in progress. The CPU, memory system and input are in place, but the
graphics and sound processors are not, so commercial games do not render yet.
Software that draws with the CPU (framebuffer homebrew, test ROMs) can already
show output.

| Component | State |
|-----------|-------|
| ROM loading (`.z64` / `.v64` / `.n64`), header, CIC detection | done |
| VR4300 CPU: integer ISA, 64-bit ops, delay slots, exceptions, interrupts, Count/Compare | done |
| FPU (COP1): single/double arithmetic, conversions with FCSR rounding, compares, FR=0/1 | done; FPU exceptions are never raised |
| TLB: 32 entries, refill/invalid/modified exceptions | done |
| Memory bus: RDRAM, SP memory, cartridge, PIF RAM | done |
| MI, VI (timing, vblank interrupt, framebuffer output), PI (ROM DMA), SI, AI (audio DMA to the sound device), SP DMA | done |
| PIF Joybus: controller info + button state for 4 ports | done; Controller Pak / EEPROM report as absent |
| Input: keyboard + any gamepad, hot-plug, configurable bindings | done |
| Boot: high-level (PIF/IPL3 skipped), region (NTSC/PAL) from the game code | done; CIC table not yet checked against real dumps |
| RSP (microcode), RDP (rasteriser) | **missing** - starting the RSP completes a null task at once so games don't deadlock |
| Save states (one slot per game), screenshots, fast-forward | done |
| Settings (always-60-fps, CPU speed model, scaling, volume) and a recent-games list | done |
| Cartridge save data (EEPROM / SRAM / Flash), Controller Pak, rumble | missing |

The window frontend (SDL3) is built on Windows, Linux and macOS. The Android and
iOS builds are headless (core + command line): SDL needs an app wrapper on those
platforms, and the wrapper can drive the platform-neutral `InputMapper`.

## Building

Requires CMake 3.16+ and a C++17 compiler. The first configure downloads and builds
SDL3 (statically linked, so the result is a single binary).

```sh
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Linux install SDL's build dependencies first (X11/Wayland, ALSA/PulseAudio, udev,
...; see `.github/workflows/build.yml` for the exact list). Options:

| Option | Default | |
|--------|---------|---|
| `REALITY64_ENABLE_SDL` | ON (desktop) | build the window/audio/gamepad frontend |
| `REALITY64_FETCH_SDL` | ON | download SDL3 if it isn't installed |
| `REALITY64_BUILD_TESTS` | ON | build the unit tests |

## Running

Double-click `Reality64` (or run it with no arguments) to open the menu: pick a ROM
with the file dialog, or drag a ROM file onto the window. The menu works with the
keyboard (arrows, Enter, Esc) and with a gamepad (D-pad, A). There is also a
Controls screen that lists the default bindings.

```sh
Reality64                               # open the menu
Reality64 game.z64                      # play straight away
Reality64 --info game.z64               # print the ROM header
Reality64 --headless --frames 60 --screenshot out.ppm game.z64
Reality64 --headless --steps 100000 game.z64   # run N instructions, dump registers
Reality64 --trace game.z64              # print every executed instruction
Reality64 --cpi 3 --force60 off game.z64   # CPU speed model, real PAL refresh rate
```

In a game: `Esc` back to the menu (and quit from the menu), `P` pause, `F5` save state, `F7` load
state, `F12` screenshot, `F11` fullscreen, hold `Tab` to fast-forward. If the emulated CPU hits
something unsupported, the reason is shown on screen.

Settings, save states and screenshots live in your user data folder (`%APPDATA%\Reality64Crew\Reality64`
on Windows). Save states only load into the same game they were made from.

## Speed and 60 fps

The emulator is built to hold 60 fps. The interpreter is tuned for the host (compare-chain
instruction dispatch, byte-swapped loads, per-batch timers) and time is modelled as a number of
**cycles per instruction** (default 2, adjustable in Settings or with `--cpi`). The real VR4300
averages well over one cycle per instruction once cache misses and RDRAM latency count, so this
is closer to the hardware than 1, and it halves the host speed needed. The pacing is exact
(precise sleeps), and **Always 60 fps** (on by default) shows PAL games at 60 Hz instead of 50 Hz,
which makes them run about 20% faster than on a real console.

On the development PC a trivial program runs at ~180 fps unthrottled, and the 60 fps lock holds
with a wide margin. Real games execute more per frame and do work the emulator does not yet
(RSP/RDP), so how fast they run depends on your CPU. If a game cannot keep up, raise the CPU
speed model in Settings. Hold `Tab` to fast-forward.

## Controls

Any gamepad SDL knows about works out of the box (Xbox, PlayStation, Switch Pro,
8BitDo, ...), and others get a generic layout. Up to four pads are assigned to
controllers 1-4 in the order they connect, and can be plugged in or removed while
playing. The keyboard always drives controller 1.

| N64 | Keyboard | Gamepad |
|-----|----------|---------|
| Analog stick | Arrow keys | Left stick |
| A / B | X / C | A (south) / X (west) |
| Z | Z | Left trigger |
| L / R | A / S | Left / right shoulder (right trigger also R) |
| Start | Enter | Start |
| D-pad | T F G H | D-pad |
| C buttons | I J K L | Right stick |

Everything can be remapped: copy [input.cfg.example](input.cfg.example) to
`input.cfg` next to the executable (or pass `--input-config`). Extra controller
mappings in SDL's `gamecontrollerdb.txt` format are picked up from a file of that
name next to the executable.

## CI

`.github/workflows/build.yml` builds Windows (x64), Linux (x64), macOS (universal),
Android (arm64-v8a, armeabi-v7a, x86_64, x86) and iOS (arm64) and uploads each as
an artifact; the desktop jobs also run the unit tests.
