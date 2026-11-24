// The Play tab: preset bar, the sound in four sections (oscillator, filter, envelope, output),
// the voices. Right-click a knob, filter / waveform button, preset arrow or Silence for MIDI learn.
#pragma once

#include "PluginProcessor.h"
#include "Ui.h"

class PlayPage : public juce::Component
{
public:
    explicit PlayPage (SidBridgeProcessor&);
    ~PlayPage() override;

    /// Editor timer: status, parameters -> buttons, badges, banner.
    void refresh (const BridgeStatus&, bool setupDone);
    std::function<void()> onGoToSetup;
    /// Right-click menu for a learnable target (also used for Silence in the header).
    void learnMenu (const juce::String& target, juce::Component& near);

    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    void fillPresets();
    void choosePresetsFile();
    void setChoice (const char* paramId, int index);
    int choice (const char* paramId) const;
    void clickWave (int i);
    void clickMode (int i);
    static juce::String badgeFor (const juce::String& target, const BridgeStatus& st, bool& learning);

    SidBridgeProcessor& proc;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    ui::RightClickable<juce::TextButton> prevPreset, nextPreset;
    juce::ComboBox preset;
    juce::Label info;
    ui::RightClickable<juce::TextButton> bannerButton;

    ui::Card osc { "Oscillator" }, filter { "Filter" }, env { "Envelope" }, out { "Output" };
    std::array<ui::Chip, 4> waves;
    std::array<ui::Chip, 3> modes;
    juce::Label waveHint, modeHint, velocityLabel, amountLabel;
    std::array<std::unique_ptr<ui::Knob>, msid::kNumParams> knobs;
    ui::EnvelopeGraph graph;
    ui::Segmented velocity { { "Off", "Sustain", "Volume" } };
    juce::Slider amount;
    ui::VoiceStrip voices;

    juce::TextButton goSetup { "Go to Setup" };
    bool showOverlay = false;
    int shownPresetsVersion = -1;
    juce::String lastLearnMessage;
    double learnMessageUntil = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    static constexpr int kCmdLoadPresets = 10001, kCmdBuiltinPresets = 10002;
    // last: attachments must go before the sliders they attach to
    std::vector<std::unique_ptr<SliderAttachment>> attachments;
};
