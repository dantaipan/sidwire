// SIDwire's own standalone app (JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP): JUCE's audio engine
// for standalone plugins (StandalonePluginHolder), in a plain window without JUCE's generic
// "Options" bar. Setup chooses everything instead: the audio output (StandaloneHooks) and the
// MIDI ports, which SIDwire opens itself. The holder's own MIDI inputs stay off, so nothing
// can arrive twice (e.g. the Oxygen's Mackie port as keys).
//
// Settings live where JUCE's default app kept them (~/Library/Application Support/
// SIDwire.settings; copied from SID Bridge.settings on the first run after the rename).
#include "PluginEditor.h"      // ours first: the plugin-client headers below change the namespace context
#include "Migration.h"
#include "StandaloneHooks.h"
#include "Ui.h"

// the same includes JUCE's own standalone app uses before this header
#include <juce_core/system/juce_TargetPlatform.h>
#include <juce_audio_plugin_client/detail/juce_CheckSettingMacros.h>
#include <juce_audio_plugin_client/detail/juce_IncludeSystemHeaders.h>
#include <juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h>
#include <juce_audio_plugin_client/detail/juce_PluginUtilities.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>


namespace {

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, juce::StandalonePluginHolder& h)
        : DocumentWindow (name, ui::colours::bg, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          holder (h)
    {
        setUsingNativeTitleBar (true);
        if (auto* editor = holder.processor->createEditorAndMakeActive())
            setContentNonOwned (editor, true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
    }

    ~MainWindow() override
    {
        if (auto* editor = holder.processor->getActiveEditor())
        {
            clearContentComponent();
            holder.processor->editorBeingDeleted (editor);
            delete editor;
        }
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    juce::StandalonePluginHolder& holder;
};

/// The app menu ("SIDwire") gets "About SIDwire...".
class AppMenu final : public juce::MenuBarModel
{
public:
    std::function<void()> onAbout;
    juce::StringArray getMenuBarNames() override { return {}; }
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override { return {}; }
    std::function<void()> onCheckForUpdates;
    void menuItemSelected (int id, int) override
    {
        if (id == 1 && onAbout)
            onAbout();
        if (id == 2 && onCheckForUpdates)
            onCheckForUpdates();
    }
};

class SidBridgeApp final : public juce::JUCEApplication
{
public:
    SidBridgeApp()
    {
        migration::migrate (juce::CharPointer_UTF8 (JucePlugin_Name), ".settings");   // from SID Bridge 0.1.x
        juce::PropertiesFile::Options options;
        options.applicationName = juce::CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = "";
        properties.setStorageParameters (options);
    }

    const juce::String getApplicationName() override { return juce::CharPointer_UTF8 (JucePlugin_Name); }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted (const juce::String&) override {}

    void initialise (const juce::String&) override
    {
        holder = std::make_unique<juce::StandalonePluginHolder> (properties.getUserSettings(), false);
        for (auto& d : juce::MidiInput::getAvailableDevices())   // SIDwire opens its own ports
            holder->deviceManager.setMidiInputDeviceEnabled (d.identifier, false);
        standalone::deviceManager() = &holder->deviceManager;
        if (! juce::Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            window = std::make_unique<MainWindow> (getApplicationName(), *holder);
            window->setVisible (true);
        }
       #if JUCE_MAC
        menu.onAbout = [this] {
            if (auto* editor = dynamic_cast<SidBridgeEditor*> (holder != nullptr ? holder->processor->getActiveEditor() : nullptr))
                editor->showAbout();
        };
        menu.onCheckForUpdates = [this] {
            if (auto* editor = dynamic_cast<SidBridgeEditor*> (holder != nullptr ? holder->processor->getActiveEditor() : nullptr))
                editor->checkForUpdates();
        };
        juce::PopupMenu extra;
        extra.addItem (1, "About SIDwire...");
        extra.addItem (2, "Check for Updates...");
        juce::MenuBarModel::setMacMainMenu (&menu, &extra);
       #endif
        updateChecker->checkIfDue();   // once a day for each version, unless switched off
    }

    void shutdown() override
    {
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
       #endif
        window = nullptr;
        standalone::deviceManager() = nullptr;
        holder = nullptr;
        properties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (holder != nullptr)
            holder->savePluginState();
        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
            juce::Timer::callAfterDelay (100, [] {
                if (auto* app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        else
            quit();
    }

private:
    juce::ApplicationProperties properties;
    AppMenu menu;
    juce::SharedResourcePointer<updates::UpdateChecker> updateChecker;
    std::unique_ptr<juce::StandalonePluginHolder> holder;
    std::unique_ptr<MainWindow> window;
};

} // namespace

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new SidBridgeApp(); }
