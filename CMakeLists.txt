cmake_minimum_required(VERSION 3.22)

project(Bassifier VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
option(COPY_PLUGIN_AFTER_BUILD "Copy AU and VST3 bundles after building the plug-in target" OFF)

if(MSVC)
    set(BASSIFIER_COMPILER_OPTIONS /W4)
else()
    set(BASSIFIER_COMPILER_OPTIONS -Wall -Wextra)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Prefer a local JUCE checkout for day-to-day development. A fresh clone remains buildable:
# when third_party/JUCE is absent CMake fetches the exact release used by this project.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# The bass engine has no JUCE dependency, so it can be tested offline.
add_library(bassifier_dsp STATIC
    Source/dsp/BassEngine.cpp)
target_include_directories(bassifier_dsp PUBLIC Source)
set_target_properties(bassifier_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(bassifier_dsp PRIVATE ${BASSIFIER_COMPILER_OPTIONS})

add_executable(bassifier_eval tests/eval.cpp)
target_link_libraries(bassifier_eval PRIVATE bassifier_dsp)
target_compile_options(bassifier_eval PRIVATE ${BASSIFIER_COMPILER_OPTIONS})
add_test(NAME bassifier_dsp_eval COMMAND bassifier_eval)

juce_add_plugin(Bassifier
    PRODUCT_NAME "Bassifier"
    COMPANY_NAME "codycodie"
    BUNDLE_ID "com.c0dypeng.bassifier"
    PLUGIN_MANUFACTURER_CODE Cdpg
    PLUGIN_CODE Bsfr
    FORMATS AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD})

# Inter and JetBrains Mono, both SIL Open Font License (licences are next to the fonts).
juce_add_binary_data(BassifierAssets SOURCES
    Resources/fonts/Inter-SemiBold.ttf
    Resources/fonts/JetBrainsMono-Medium.ttf)
set_target_properties(BassifierAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(Bassifier PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Bassifier PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Bassifier
    PRIVATE
        bassifier_dsp
        BassifierAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Exercises the processor as a host would: layouts, parameters, audio, meters and state.
juce_add_console_app(bassifier_plugin_smoke)
target_sources(bassifier_plugin_smoke PRIVATE
    tests/plugin_smoke.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_compile_definitions(bassifier_plugin_smoke PRIVATE
    JucePlugin_Name="Bassifier"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(bassifier_plugin_smoke PRIVATE
    bassifier_dsp BassifierAssets juce::juce_audio_utils juce::juce_dsp)
add_test(NAME bassifier_plugin_smoke COMMAND bassifier_plugin_smoke)

# Renders the editor to a PNG (see tests/snapshot.cpp).
juce_add_console_app(bassifier_snapshot)
target_sources(bassifier_snapshot PRIVATE
    tests/snapshot.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_compile_definitions(bassifier_snapshot PRIVATE
    JucePlugin_Name="Bassifier"
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(bassifier_snapshot PRIVATE bassifier_dsp BassifierAssets juce::juce_audio_utils juce::juce_dsp)
add_test(NAME bassifier_snapshot_render
    COMMAND bassifier_snapshot ${CMAKE_CURRENT_BINARY_DIR}/bassifier_snapshot.png)
