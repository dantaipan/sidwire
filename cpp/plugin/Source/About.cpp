#include "About.h"

#include "StandaloneHooks.h"
#include "Ui.h"

using namespace ui::colours;

AboutPanel::AboutPanel()
{
    setComponentID ("about");
    setWantsKeyboardFocus (true);
    for (auto* l : { &juceSite, &agpl, &juceLicence })
    {
        l->setColour (juce::HyperlinkButton::textColourId, juce::Colour (0xffaab3ff));
        l->setFont (ui::font (13.0f), false, juce::Justification::centredLeft);
        addAndMakeVisible (*l);
    }
    close.setComponentID ("aboutClose");
    close.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (close);

    checkUpdates.setComponentID ("checkUpdates");
    checkUpdates.onClick = [this] {
        updateStatus = "Checking...";
        repaint();
        if (onCheckForUpdates)
            onCheckForUpdates();
    };
    addAndMakeVisible (checkUpdates);
    automatic.setComponentID ("checkAutomatically");
    automatic.setTooltip ("The app asks GitHub once a day whether a newer SID Bridge is out. Nothing else is sent.");
    automatic.onClick = [this] { checker->setChecksAutomatically (automatic.getToggleState()); };
    addChildComponent (automatic);
}

void AboutPanel::visibilityChanged()
{
    // the automatic check is the standalone app's; in a DAW the plugin only checks when asked
    automatic.setVisible (standalone::deviceManager() != nullptr);
    automatic.setToggleState (checker->checksAutomatically(), juce::dontSendNotification);
    if (isVisible())
        startTimerHz (4);
    else
        stopTimer();
}

void AboutPanel::timerCallback()
{
    checkUpdates.setEnabled (! checker->isRunning());
}

juce::String AboutPanel::version()
{
#ifdef JucePlugin_VersionString
    return JucePlugin_VersionString;
#else
    return "dev";
#endif
}

juce::Rectangle<int> AboutPanel::card() const
{
    return getLocalBounds().withSizeKeepingCentre (500, 400);
}

void AboutPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    auto c = card();
    g.setColour (juce::Colour (0xff2a2e47));
    g.fillRoundedRectangle (c.toFloat(), 14.0f);
    g.setColour (juce::Colour (0xff454b73));
    g.drawRoundedRectangle (c.toFloat().reduced (0.5f), 14.0f, 1.0f);

    auto r = c.reduced (28, 24);
    g.setColour (bright);
    g.setFont (ui::font (22.0f, true));
    g.drawText ("SID Bridge", r.removeFromTop (30), juce::Justification::centredLeft);
    g.setColour (dim);
    g.setFont (ui::font (13.0f));
    g.drawText ("Version " + version(), r.removeFromTop (20), juce::Justification::centredLeft);
    r.removeFromTop (12);
    g.setColour (text);
    g.drawFittedText ("Plays the real SID chips in a C64 Ultimate from MIDI, over your network.",
                      r.removeFromTop (20), juce::Justification::centredLeft, 1);
    r.removeFromTop (22);
    g.setColour (label);
    g.setFont (ui::font (11.0f, true));
    g.drawText ("BUILT WITH JUCE", r.removeFromTop (16), juce::Justification::centredLeft);
    r.removeFromTop (6);
    g.setColour (text);
    g.setFont (ui::font (13.0f));
    g.drawFittedText ("SID Bridge uses the JUCE framework. The JUCE modules are dual-licensed under the "
                      "GNU AGPLv3 and the commercial JUCE licence.",
                      r.removeFromTop (40), juce::Justification::topLeft, 2);
    r.removeFromTop (6 + 24 + 22);
    g.setColour (label);
    g.setFont (ui::font (11.0f, true));
    g.drawText ("UPDATES", r.removeFromTop (16), juce::Justification::centredLeft);
    r.removeFromTop (6 + 32 + 4);
    g.setColour (dim);
    g.setFont (ui::font (12.0f));
    g.drawText (updateStatus, r.removeFromTop (18), juce::Justification::centredLeft);
}

void AboutPanel::resized()
{
    auto r = card().reduced (28, 24);
    r.removeFromTop (30 + 20 + 12 + 20 + 22 + 16 + 6 + 40 + 6);
    auto links = r.removeFromTop (24);
    juceSite.setBounds (links.removeFromLeft (80));
    links.removeFromLeft (16);
    agpl.setBounds (links.removeFromLeft (100));
    links.removeFromLeft (16);
    juceLicence.setBounds (links.removeFromLeft (110));
    r.removeFromTop (22 + 16 + 6);
    auto row = r.removeFromTop (32);
    checkUpdates.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (16);
    automatic.setBounds (row);
    close.setBounds (r.removeFromBottom (34).removeFromRight (100));
}

void AboutPanel::mouseDown (const juce::MouseEvent& e)
{
    if (! card().contains (e.getPosition()) && onClose)
        onClose();
}

bool AboutPanel::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && onClose)
    {
        onClose();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- update panel

UpdatePanel::UpdatePanel()
{
    setComponentID ("updatePanel");
    setWantsKeyboardFocus (true);
    notes.setMultiLine (true, true);
    notes.setReadOnly (true);
    notes.setScrollbarsShown (true);
    notes.setCaretVisible (false);
    notes.setFont (ui::font (13.0f));
    notes.setColour (juce::TextEditor::backgroundColourId, field);
    addAndMakeVisible (notes);
    download.setComponentID ("updateDownload");
    skip.setComponentID ("updateSkip");
    later.setComponentID ("updateLater");
    download.setColour (juce::TextButton::buttonColourId, selBg);
    download.getProperties().set ("border", (int) selBorder.getARGB());
    download.onClick = [this] {
        juce::URL (downloadUrl).launchInDefaultBrowser();
        if (onClose) onClose();
    };
    skip.onClick = [this] {
        checker->skip (version);
        if (onClose) onClose();
    };
    later.onClick = [this] { if (onClose) onClose(); };
    for (auto* b : { &download, &skip, &later })
        addAndMakeVisible (*b);
}

void UpdatePanel::show (const updates::UpdateChecker::Outcome& o)
{
    hasRelease = o.offer && o.release.has_value();
    if (hasRelease)
    {
        version = o.release->version;
        downloadUrl = o.release->downloadUrl;
        title = "SID Bridge " + version.trimCharactersAtStart ("vV") + " is available";
        subtitle = "You have " + updates::currentVersion() + ".";
        notes.setText (updates::runsToText (updates::notesToRuns (o.release->notes)), false);
    }
    else
    {
        title = o.error.isNotEmpty() ? juce::String ("Couldn't check for updates") : juce::String ("SID Bridge is up to date");
        subtitle = o.error.isNotEmpty() ? o.error : "Version " + updates::currentVersion() + " is the latest.";
        notes.clear();
    }
    notes.setVisible (hasRelease);
    download.setVisible (hasRelease);
    skip.setVisible (hasRelease);
    later.setButtonText (hasRelease ? "Later" : "OK");
    resized();
    repaint();
}

juce::Rectangle<int> UpdatePanel::card() const
{
    return getLocalBounds().withSizeKeepingCentre (560, hasRelease ? 420 : 190);
}

void UpdatePanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    auto c = card();
    g.setColour (juce::Colour (0xff2a2e47));
    g.fillRoundedRectangle (c.toFloat(), 14.0f);
    g.setColour (juce::Colour (0xff454b73));
    g.drawRoundedRectangle (c.toFloat().reduced (0.5f), 14.0f, 1.0f);
    auto r = c.reduced (28, 24);
    g.setColour (bright);
    g.setFont (ui::font (19.0f, true));
    g.drawText (title, r.removeFromTop (28), juce::Justification::centredLeft);
    g.setColour (dim);
    g.setFont (ui::font (13.0f));
    g.drawFittedText (subtitle, r.removeFromTop (36), juce::Justification::topLeft, 2);
}

void UpdatePanel::resized()
{
    auto r = card().reduced (28, 24);
    r.removeFromTop (28 + 36);
    auto buttons = r.removeFromBottom (34);
    later.setBounds (buttons.removeFromRight (100));
    buttons.removeFromRight (8);
    download.setBounds (buttons.removeFromRight (110));
    skip.setBounds (buttons.removeFromLeft (150));
    r.removeFromBottom (14);
    notes.setBounds (r);
}

bool UpdatePanel::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && onClose)
    {
        onClose();
        return true;
    }
    return false;
}
