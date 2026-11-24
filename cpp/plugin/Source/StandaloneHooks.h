// What the standalone app shares with the editor. In a plugin host these stay empty.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace standalone {

/// The standalone app's audio device manager (StandaloneApp.cpp sets it; nullptr in a DAW).
inline juce::AudioDeviceManager*& deviceManager()
{
    static juce::AudioDeviceManager* dm = nullptr;
    return dm;
}

} // namespace standalone
