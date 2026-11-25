cmake_minimum_required(VERSION 3.22)

# Must be set before project() to take effect on macOS
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")

project(MidiSid VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MSID_BUILD_TESTS  "Build the core unit + parity tests (Catch2)" ON)
option(MSID_BUILD_PLUGIN "Build the JUCE plugin (VST3/AU/Standalone)" ON)
option(MSID_COPY_PLUGINS "Copy built plugins to ~/Library/Audio/Plug-Ins" ON)

if(MSID_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

if(MSID_BUILD_PLUGIN)
    # JUCE 9.0.4 — local clone at external/JUCE (same commit as roughstomp)
    add_subdirectory(external/JUCE)
    add_subdirectory(plugin)
endif()
