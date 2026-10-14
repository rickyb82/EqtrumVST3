cmake_minimum_required(VERSION 3.22)

# Must be set before project() to take effect on macOS.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

project(Eqtrum VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-config generators (Ninja, Makefiles): default to Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.6
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(Eqtrum
    PRODUCT_NAME "Eqtrum"
    COMPANY_NAME "Eqtrum"
    BUNDLE_ID "com.eqtrum.eqtrum"
    PLUGIN_MANUFACTURER_CODE Eqtm
    PLUGIN_CODE Eqt1
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES Fx EQ Analyzer
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Eqtrum Standalone uses the audio input for testing.")

target_sources(Eqtrum
    PRIVATE
        Source/Params.cpp
        Source/EqMath.cpp
        Source/EqEngine.cpp
        Source/InstanceRegistry.cpp
        Source/SpectrumAnalyzer.cpp
        Source/LinearPhaseBuilder.cpp
        Source/PresetManager.cpp
        Source/PluginProcessor.cpp
        Source/SpectrumView.cpp
        Source/TrackListPanel.cpp
        Source/PluginEditor.cpp)

target_include_directories(Eqtrum PRIVATE Source)

target_compile_definitions(Eqtrum
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Eqtrum
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
