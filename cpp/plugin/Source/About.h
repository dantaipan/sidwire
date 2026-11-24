// "About SID Bridge": version, what it is, and the JUCE licensing statement. Shown over the
// window (plugin and standalone alike); the standalone app also opens it from its app menu.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Updates.h"

class AboutPanel : public juce::Component, private juce::Timer
{
public:
    AboutPanel();
    std::function<void()> onClose;
    std::function<void()> onCheckForUpdates;   // the window runs the check and shows the result
    void setUpdateStatus (const juce::String& s) { updateStatus = s; repaint(); }

    static juce::String version();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;   // a click outside the card closes it
    bool keyPressed (const juce::KeyPress&) override;    // Escape closes it

private:
    juce::Rectangle<int> card() const;
    void timerCallback() override;
    void visibilityChanged() override;
    juce::TextButton checkUpdates { "Check for updates" };
    juce::ToggleButton automatic { "Check automatically (daily)" };
    juce::String updateStatus;
    juce::SharedResourcePointer<updates::UpdateChecker> checker;
    juce::HyperlinkButton juceSite { "juce.com", juce::URL ("https://juce.com") },
                          agpl { "GNU AGPLv3", juce::URL ("https://www.gnu.org/licenses/agpl-3.0.en.html") },
                          juceLicence { "JUCE licence", juce::URL ("https://juce.com/legal/juce-9-licence/") };
    juce::TextButton close { "Close" };
};

/// "SID Bridge x.y is available": the release notes with Download / Skip This Version / Later,
/// or the answer to a check that found nothing newer.
class UpdatePanel : public juce::Component
{
public:
    UpdatePanel();
    void show (const updates::UpdateChecker::Outcome&);
    std::function<void()> onClose;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    const juce::String& headline() const { return title; }

private:
    juce::Rectangle<int> card() const;
    juce::TextEditor notes;
    juce::TextButton download { "Download" }, skip { "Skip This Version" }, later { "Later" };
    juce::String title, subtitle, version, downloadUrl;
    bool hasRelease = false;
    juce::SharedResourcePointer<updates::UpdateChecker> checker;
};
