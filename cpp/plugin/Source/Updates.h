// Finding out whether a newer SID Bridge is out, from its GitHub releases (the same way as
// RoughTrack). Nothing is sent beyond an ordinary request for the latest release.
//
// The standalone app checks by itself once a day for each version (it can be switched off) and
// from its menu; the plugin only checks when asked (About panel), never on its own in a DAW.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace updates {

/// Where to ask for the latest release. Left empty, no request is ever made.
inline constexpr auto latestReleaseUrl = "https://api.github.com/repos/dantaipan/sid-bridge/releases/latest";

inline bool isUpdateCheckingAvailable() { return juce::String (latestReleaseUrl).isNotEmpty(); }

/// This build's version (the CMake project version), "0.0.0" in test builds.
juce::String currentVersion();

struct Release
{
    juce::String version;     // as tagged, such as "v0.2.0"
    juce::String notes;       // the release notes, in Markdown
    juce::String pageUrl;
    juce::String downloadUrl; // the disk image / zip, or the release page if there isn't one
};

/// Compares version numbers part by part, so 0.10 is newer than 0.9. A leading "v" is ignored.
bool isNewerVersion (const juce::String& candidate, const juce::String& current);

/// Reads GitHub's answer for the latest release, or nothing if it isn't one.
std::optional<Release> parseLatestRelease (const juce::var& json);

/// Whether to tell the player about a release. Automatic checks leave out a skipped version;
/// asking for it always shows it.
bool shouldOffer (const Release&, const juce::String& currentVersion, const juce::String& skippedVersion, bool isManual);

/// What the last automatic check is stored as: the version that checked, and when.
juce::String lastCheckRecord (const juce::String& version, juce::int64 nowMs);

/// Whether an automatic check is due: once a day for each version (another version's check,
/// such as a newer test build's, doesn't count).
bool isAutomaticCheckDue (const juce::String& lastCheck, const juce::String& currentVersion, juce::int64 nowMs);

/// A stretch of release notes in one style.
struct NoteRun
{
    juce::String text;
    bool bold = false;
    bool heading = false;
};

/// Release notes' Markdown as styled text: headings, bullets, bold and link text come through;
/// images, the download section and the checksum (for the web page) are left out.
std::vector<NoteRun> notesToRuns (const juce::String& markdown);
juce::String runsToText (const std::vector<NoteRun>&);

/// Asks GitHub for the latest release. Blocks for up to 10 s: run it off the message thread.
std::optional<Release> fetchLatestRelease (juce::String& error);

/// One per process (juce::SharedResourcePointer): runs checks in the background and keeps the
/// preferences (automatic on/off, last check, skipped version) in
/// ~/Library/Application Support/SID Bridge.updates.
class UpdateChecker
{
public:
    UpdateChecker();
    ~UpdateChecker();

    struct Outcome
    {
        std::optional<Release> release;   // the latest release, if the check got one
        juce::String error;
        bool manual = false;
        bool offer = false;               // newer, and not skipped (or asked for)
    };

    /// Starts a check in the background (ignored while one is running).
    void check (bool manual);
    /// The automatic check, if it's on and due.
    void checkIfDue();
    /// The finished check's outcome, once (message thread polls).
    std::optional<Outcome> takeOutcome();
    bool isRunning() const { return running.load(); }

    bool checksAutomatically() const;
    void setChecksAutomatically (bool);
    void skip (const juce::String& version);
    juce::String skippedVersion() const;

    /// Tests: replace the GitHub request, and the preferences file.
    static std::function<std::optional<Release> (juce::String& error)> fetchOverride;
    static juce::File prefsFileOverride;

private:
    std::unique_ptr<juce::PropertiesFile> prefs;
    std::thread worker;
    std::atomic<bool> running { false };
    mutable std::mutex mutex;
    std::optional<Outcome> outcome;
};

} // namespace updates
