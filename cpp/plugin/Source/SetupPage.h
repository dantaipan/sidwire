// The Setup tab: 1 your C64 Ultimate (address + Find), 2 your keyboard (profile, ports),
// 3 how you play. A "Ready to play" bar with "Start playing" once connected.
#pragma once

#include "PluginProcessor.h"
#include "StandaloneHooks.h"
#include "Ui.h"

class SetupPage : public juce::Component
{
public:
    explicit SetupPage (SidBridgeProcessor&);
    ~SetupPage() override;

    void refresh (const BridgeStatus&);
    /// First run: search the network by itself when no address is set yet.
    void autoFindIfNeeded();
    std::function<void()> onStartPlaying;
    std::function<void()> onAbout;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void loadSettings();
    void pushSettings();
    void fillPortBox (juce::ComboBox&, const juce::String& noneLabel, const juce::String& current);
    juce::String portFrom (const juce::ComboBox&) const;
    void refreshPorts();
    void fillProfiles();
    juce::String selectedProfile() const;
    void startFind();
    void showFound (const BridgeController::FindState&);
    void useDevice (const msid::FoundDevice&);
    void choosePresetsFile();

    SidBridgeProcessor& proc;
    ui::Card c64 { "Your C64 Ultimate" }, keyboard { "Your keyboard" }, playing { "How you play" };

    // 1: C64 Ultimate
    juce::Label addressLabel { {}, "Address" }, passwordLabel { {}, "Network password" }, basesLabel { {}, "SID addresses" },
                viaLabel { {}, "Send via" }, videoLabel { {}, "Video" }, findText, c64Info;
    juce::TextEditor host, password, bases;
    juce::TextButton find { "Find" }, more, connectButton;
    std::vector<std::unique_ptr<juce::TextButton>> foundButtons;
    std::vector<msid::FoundDevice> found;
    ui::Segmented via { { "DMA", "HTTP" } }, video { { "PAL", "NTSC" } };
    bool showMore = false, findWasRunning = false, autoUse = false;

    // 2: keyboard
    juce::Label profileLabel { {}, "Profile" }, notePortLabel { {}, "Notes in" }, encoderPortLabel { {}, "Controls in" }, mappings;
    juce::ComboBox profileBox, notePort, encoderPort;
    juce::TextButton openFolder { "Open profiles folder" };
    std::vector<ProfileEntry> shownProfiles;
    juce::StringArray knownPorts;

    // 3: how you play
    juce::Label layoutLabel { {}, "Layout" }, targetLabel { {}, "Knobs and pads change" },
                presetsLabel { {}, "Presets" }, presetsName;
    ui::Segmented layout { { "Poly", "Split ch1 / ch2" } }, target { { "Both SIDs", "SID 1", "SID 2" } };
    juce::TextButton loadPresets { "Load file..." }, builtinPresets { "Built-in" };
    // standalone app only: where the streamed C64U audio plays
    juce::Label audioOutLabel { {}, "Audio output (for Stream C64U audio)" };
    juce::ComboBox audioOut;
    void fillAudioOutputs();
    juce::String notesNoneLabel() const;

    juce::TextButton footnote;   // "Made with JUCE · About SIDwire"
    // old "SID Bridge" bundles (from before the rename) still installed: offer to show them
    juce::TextButton oldInstall;
    juce::Array<juce::File> oldBundles;
    void checkOldInstall();

    // ready bar
    juce::TextButton startPlaying { "Start playing" };
    juce::String readyTitle, readyLine;
    bool ready = false;
    int ticks = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    static constexpr int kCmdOpenProfiles = 10001;
};
