#pragma once

#include "BridgeController.h"

#include <juce_audio_processors/juce_audio_processors.h>

/// SID Bridge: incoming MIDI plays the real SID chip(s) in a C64 Ultimate over the network
/// (see BridgeController). Its audio output is silent unless "C64 audio" is on: then it plays
/// the C64U's network audio stream (AudioStreamReceiver).
class SidBridgeProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    SidBridgeProcessor();
    ~SidBridgeProcessor() override;

    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // host programs = presets
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    BridgeController& bridge() { return *controller; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static std::vector<msid::Preset> builtinPresets();

private:
    void timerCallback() override { controller->applyWritebacks(); }
    static BridgeSettings withEnvOverrides (BridgeSettings);
    static bool envOverridesActive();
    BridgeSettings stateSettings;   // as loaded, before env overrides (what gets saved then)

    std::unique_ptr<BridgeController> controller;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidBridgeProcessor)
};
