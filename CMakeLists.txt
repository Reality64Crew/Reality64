cmake_minimum_required(VERSION 3.16)
project(Reality64 VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The window, audio and gamepad frontend uses SDL3. It is only wired up on the
# desktop platforms; Android and iOS get the headless core and CLI, and an app
# wrapper can feed the platform-neutral InputMapper itself.
if(ANDROID OR IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(REALITY64_SDL_DEFAULT OFF)
else()
    set(REALITY64_SDL_DEFAULT ON)
endif()
option(REALITY64_ENABLE_SDL "Build the SDL3 window/audio/gamepad frontend" ${REALITY64_SDL_DEFAULT})
option(REALITY64_FETCH_SDL "Download and build SDL3 if it is not installed" ON)
option(REALITY64_BUILD_TESTS "Build the unit tests" ON)

add_library(reality64_core STATIC
    source/core/Cic.cpp
    source/core/Emulator.cpp
    source/core/MemoryBus.cpp
    source/core/Pif.cpp
    source/core/Rom.cpp
    source/core/Video.cpp
    source/cpu/VR4300.cpp
    source/cpu/VR4300Fpu.cpp
    source/cpu/VR4300Tlb.cpp
    source/input/InputMapper.cpp
)
target_include_directories(reality64_core PUBLIC source)
if(MSVC)
    target_compile_options(reality64_core PRIVATE /W4)
else()
    target_compile_options(reality64_core PRIVATE -Wall -Wextra)
endif()

add_executable(Reality64 source/Main.cpp)
target_link_libraries(Reality64 PRIVATE reality64_core)
target_compile_definitions(Reality64 PRIVATE REALITY64_VERSION="${PROJECT_VERSION}")

if(REALITY64_ENABLE_SDL)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        if(NOT REALITY64_FETCH_SDL)
            message(FATAL_ERROR "SDL3 not found. Install it, or configure with -DREALITY64_FETCH_SDL=ON "
                                "(or -DREALITY64_ENABLE_SDL=OFF for a headless build).")
        endif()
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.4.18
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()

    target_sources(Reality64 PRIVATE source/frontend/SdlFrontend.cpp)
    target_compile_definitions(Reality64 PRIVATE REALITY64_SDL=1)
    if(TARGET SDL3::SDL3)
        target_link_libraries(Reality64 PRIVATE SDL3::SDL3)
    else()
        target_link_libraries(Reality64 PRIVATE SDL3::SDL3-static)
    endif()
endif()

install(TARGETS Reality64
    RUNTIME DESTINATION .
    BUNDLE DESTINATION .
)
install(FILES input.cfg.example DESTINATION .)

if(REALITY64_BUILD_TESTS)
    enable_testing()
    add_executable(Reality64Tests tests/Tests.cpp)
    target_link_libraries(Reality64Tests PRIVATE reality64_core)
    add_test(NAME Reality64Tests COMMAND Reality64Tests)
endif()
