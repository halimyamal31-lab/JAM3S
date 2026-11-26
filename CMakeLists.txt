cmake_minimum_required(VERSION 3.22)
project(JAM3S VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.6 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
juce_add_plugin(JAM3S
    COMPANY_NAME "JAM3S"
    BUNDLE_ID com.jam3s.vocalengine
    PLUGIN_MANUFACTURER_CODE Jm3s
    PLUGIN_CODE Jv01
    FORMATS VST3
    PRODUCT_NAME "JAM3S Vocal Engine"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)
juce_generate_juce_header(JAM3S)
target_sources(JAM3S PRIVATE Source/Plugin.cpp)
target_compile_definitions(JAM3S PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(JAM3S PRIVATE juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
