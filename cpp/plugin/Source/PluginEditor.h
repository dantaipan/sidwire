// The SID Bridge window: a header (Play / Setup tabs, connection status, Stream C64U audio,
// Silence) over the Play or the Setup page. Opens on Setup until it has connected once.
#pragma once

#include "About.h"
#include "PlayPage.h"
#include "PluginProcessor.h"
#include "SetupPage.h"
#include "Ui.h"

class SidBridgeEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SidBridgeEditor (SidBridgeProcessor&);
    ~SidBridgeEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    enum class Tab { play, setup };
    void showTab (Tab);
    Tab currentTab() const { return tab; }
    void showAbout();
    /// Ask GitHub now; the answer appears over the window.
    void checkForUpdates();
    bool isUpdateShowing() const { return updatePanel.isVisible(); }
    const juce::String& updateHeadline() const { return updatePanel.headline(); }
    bool isAboutShowing() const { return about.isVisible(); }

private:
    void timerCallback() override;

    SidBridgeProcessor& proc;
    ui::LookAndFeel laf;
    juce::TooltipWindow tooltips { this, 600 };

    juce::TextButton title, playTab { "Play" }, setupTab { "Setup" }, statusPill;
    juce::ToggleButton stream { "Stream C64U audio" };
    ui::RightClickable<juce::TextButton> silence;
    PlayPage play;
    SetupPage setup;
    AboutPanel about;
    UpdatePanel updatePanel;
    juce::SharedResourcePointer<updates::UpdateChecker> updateChecker;
    Tab tab = Tab::setup;
    BridgeStatus shown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidBridgeEditor)
};
