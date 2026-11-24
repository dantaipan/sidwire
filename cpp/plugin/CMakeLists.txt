# SID Bridge: VST3 / AU / Standalone instrument that plays the C64U's SID chips.
juce_add_plugin(SidBridge
    COMPANY_NAME             "MidiSid"
    BUNDLE_ID                "com.midisid.sidbridge"
    PLUGIN_MANUFACTURER_CODE Msid
    PLUGIN_CODE              Sidb
    FORMATS                  VST3 AU Standalone
    PRODUCT_NAME             "SID Bridge"
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD  ${MSID_COPY_PLUGINS}
    AU_MAIN_TYPE             kAudioUnitType_MusicDevice
    VST3_CATEGORIES          Instrument Synth
    HARDENED_RUNTIME_ENABLED TRUE
    ICON_BIG                 ${CMAKE_SOURCE_DIR}/../docs/icon.png   # source: docs/icon.svg
    # macOS 15 asks before an app talks to the local network; this is the reason it shows
    PLIST_TO_MERGE           "<plist><dict><key>NSLocalNetworkUsageDescription</key><string>SID Bridge finds your C64 Ultimate on your local network and plays its SID chips over it.</string></dict></plist>")

juce_add_binary_data(SidBridgeData
    SOURCES ${CMAKE_SOURCE_DIR}/../presets.toml
            ${CMAKE_SOURCE_DIR}/../profiles/generic.toml
            ${CMAKE_SOURCE_DIR}/../profiles/oxygen-49-mkv.toml)

target_sources(SidBridge PRIVATE
    Source/AudioStreamReceiver.cpp
    Source/AudioStreamReceiver.h
    Source/BridgeController.cpp
    Source/BridgeController.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/About.cpp
    Source/About.h
    Source/Updates.cpp
    Source/Updates.h
    Source/PlayPage.cpp
    Source/PlayPage.h
    Source/SetupPage.cpp
    Source/SetupPage.h
    Source/Ui.cpp
    Source/Ui.h
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/Profiles.cpp
    Source/Profiles.h)

# SID Bridge's own standalone window (Source/StandaloneApp.cpp) instead of JUCE's generic one
target_sources(SidBridge_Standalone PRIVATE Source/StandaloneApp.cpp)
target_compile_definitions(SidBridge_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)

target_compile_definitions(SidBridge PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SidBridge
    PRIVATE
        msid::core
        SidBridgeData
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Headless integration tests: the real processor against a fake C64U on localhost.
if(MSID_BUILD_TESTS)
    juce_add_console_app(sidbridge-tests PRODUCT_NAME "sidbridge-tests")
    target_sources(sidbridge-tests PRIVATE
        tests/PluginTests.cpp
        Source/AudioStreamReceiver.cpp
        Source/BridgeController.cpp
        Source/PluginEditor.cpp
        Source/About.cpp
        Source/Updates.cpp
        Source/PlayPage.cpp
        Source/SetupPage.cpp
        Source/Ui.cpp
        Source/PluginProcessor.cpp
        Source/Profiles.cpp)
    target_include_directories(sidbridge-tests PRIVATE Source ${CMAKE_SOURCE_DIR}/core/tests)
    target_compile_definitions(sidbridge-tests PRIVATE
        JucePlugin_Name="SID Bridge"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1)   # pump messages (write-back timer)
    target_link_libraries(sidbridge-tests
        PRIVATE
            msid::core
            SidBridgeData
            juce::juce_audio_utils
            Catch2::Catch2
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    catch_discover_tests(sidbridge-tests)
endif()
