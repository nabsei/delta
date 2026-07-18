cmake_minimum_required(VERSION 3.22)
project(DeltaZero VERSION 0.5.0)

add_subdirectory(libs/JUCE)

juce_add_plugin(DeltaZero
    COMPANY_NAME "Bumpin Audio"
    BUNDLE_ID "com.bumpinaudio.deltazero"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Nabs
    PLUGIN_CODE DZro
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "Delta Zero")

target_sources(DeltaZero PRIVATE
    Source/DeltaProcessor.cpp
    Source/PluginEditor.cpp
    Source/PluginEntry.cpp)

target_compile_definitions(DeltaZero PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(DeltaZero
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
