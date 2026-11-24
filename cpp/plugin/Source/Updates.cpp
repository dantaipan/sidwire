#include "Updates.h"

namespace updates {

namespace {

constexpr int timeoutMs = 10000;

juce::Array<int> versionParts (const juce::String& version)
{
    juce::Array<int> parts;
    for (const auto& part : juce::StringArray::fromTokens (version.trim().trimCharactersAtStart ("vV"), ".", {}))
        parts.add (part.getIntValue());
    return parts;
}

/// A line's text, with **bold** spans, [links](url) as their text, and images and `code` marks taken out.
void addInline (std::vector<NoteRun>& runs, const juce::String& line)
{
    juce::String plain;
    auto bold = false;
    const auto flush = [&] {
        if (plain.isNotEmpty())
            runs.push_back ({ plain, bold, false });
        plain.clear();
    };
    for (int i = 0; i < line.length();)
    {
        if (line.substring (i).startsWith ("**"))
        {
            flush();
            bold = ! bold;
            i += 2;
            continue;
        }
        const auto isImage = line.substring (i).startsWith ("![");
        if (isImage || line[i] == '[')
        {
            const auto textStart = i + (isImage ? 2 : 1);
            const auto textEnd = line.indexOf (textStart, "](");
            const auto urlEnd = textEnd >= 0 ? line.indexOfChar (textEnd + 2, ')') : -1;
            if (urlEnd >= 0)
            {
                if (! isImage)
                    plain << line.substring (textStart, textEnd);
                i = urlEnd + 1;
                continue;
            }
        }
        if (line[i] != '`')
            plain << line[i];
        ++i;
    }
    flush();
}

} // namespace

juce::String currentVersion()
{
#ifdef JucePlugin_VersionString
    return JucePlugin_VersionString;
#else
    return "0.0.0";
#endif
}

bool isNewerVersion (const juce::String& candidate, const juce::String& current)
{
    const auto newer = versionParts (candidate);
    const auto older = versionParts (current);
    for (int i = 0; i < juce::jmax (newer.size(), older.size()); ++i)
        if (newer[i] != older[i])
            return newer[i] > older[i];
    return false;
}

std::optional<Release> parseLatestRelease (const juce::var& json)
{
    if (! json.isObject())
        return std::nullopt;
    Release release;
    release.version = json["tag_name"].toString();
    release.notes = json["body"].toString();
    release.pageUrl = json["html_url"].toString();
    release.downloadUrl = release.pageUrl;
    if (release.version.isEmpty() || release.pageUrl.isEmpty() || versionParts (release.version).isEmpty()
        || static_cast<bool> (json["draft"]) || static_cast<bool> (json["prerelease"]))
        return std::nullopt;
    if (const auto* assets = json["assets"].getArray())
    {
        // a disk image if there is one, else a zip, else the release page
        for (auto* ext : { ".dmg", ".zip" })
        {
            for (const auto& asset : *assets)
                if (asset["name"].toString().endsWithIgnoreCase (ext))
                {
                    release.downloadUrl = asset["browser_download_url"].toString();
                    return release;
                }
        }
    }
    return release;
}

juce::String lastCheckRecord (const juce::String& version, juce::int64 nowMs)
{
    return version + " " + juce::String (nowMs);
}

bool isAutomaticCheckDue (const juce::String& lastCheck, const juce::String& version, juce::int64 nowMs)
{
    constexpr juce::int64 dayMs = 24 * 60 * 60 * 1000;
    const auto checkedVersion = lastCheck.upToFirstOccurrenceOf (" ", false, false);
    const auto checkedAt = lastCheck.fromFirstOccurrenceOf (" ", false, false).getLargeIntValue();
    // a clock set back makes a check "in the future": that isn't a reason to wait
    return checkedVersion != version || nowMs - checkedAt >= dayMs || nowMs < checkedAt;
}

bool shouldOffer (const Release& release, const juce::String& version, const juce::String& skippedVersion, bool isManual)
{
    if (! isNewerVersion (release.version, version))
        return false;
    return isManual || release.version != skippedVersion;
}

std::vector<NoteRun> notesToRuns (const juce::String& markdown)
{
    std::vector<NoteRun> runs;
    auto skippingSection = false;
    auto lastWasBlank = true;
    for (auto line : juce::StringArray::fromLines (markdown))
    {
        line = line.trimEnd();
        if (line.startsWith ("#"))
        {
            const auto title = line.trimCharactersAtStart ("#").trim();
            skippingSection = title.equalsIgnoreCase ("Download");   // the panel has its own Download button
            if (! skippingSection)
            {
                if (! lastWasBlank)
                    runs.push_back ({ "\n" });
                runs.push_back ({ title + "\n", true, true });
                lastWasBlank = true;
            }
            continue;
        }
        const auto trimmed = line.trimStart();
        if (skippingSection || trimmed.startsWith ("![") || trimmed.startsWithIgnoreCase ("SHA-256"))
            continue;
        if (trimmed.isEmpty())
        {
            if (! lastWasBlank)
                runs.push_back ({ "\n" });
            lastWasBlank = true;
            continue;
        }
        if (trimmed.startsWith ("- ") || trimmed.startsWith ("* "))
        {
            runs.push_back ({ juce::String::fromUTF8 ("\xe2\x80\xa2 ") });
            addInline (runs, trimmed.substring (2));
        }
        else
            addInline (runs, trimmed);
        runs.push_back ({ "\n" });
        lastWasBlank = false;
    }
    while (! runs.empty() && runs.back().text.trim().isEmpty())
        runs.pop_back();
    return runs;
}

juce::String runsToText (const std::vector<NoteRun>& runs)
{
    juce::String text;
    for (const auto& run : runs)
        text << run.text;
    return text;
}

std::optional<Release> fetchLatestRelease (juce::String& error)
{
    if (! isUpdateCheckingAvailable())
    {
        error = "Checking for updates isn't set up.";
        return std::nullopt;
    }
    // GitHub asks every request to say what it comes from
    int status = 0;
    const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                             .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: SID-Bridge/" + currentVersion())
                             .withConnectionTimeoutMs (timeoutMs)
                             .withStatusCode (&status);
    const auto stream = juce::URL (latestReleaseUrl).createInputStream (options);
    if (stream == nullptr)
    {
        error = "Couldn't reach GitHub. Check your internet connection.";
        return std::nullopt;
    }
    const auto body = stream->readEntireStreamAsString();
    if (status == 404)
    {
        error = "No SID Bridge release has been published yet.";
        return std::nullopt;
    }
    const auto release = parseLatestRelease (juce::JSON::parse (body));
    if (! release.has_value())
        error = "GitHub's answer didn't include a release.";
    return release;
}

// ---------------------------------------------------------------- checker

std::function<std::optional<Release> (juce::String&)> UpdateChecker::fetchOverride;
juce::File UpdateChecker::prefsFileOverride;

namespace {
constexpr auto kAutomatic = "checkAutomatically";
constexpr auto kLastCheck = "lastCheck";
constexpr auto kSkipped = "skippedVersion";
}

UpdateChecker::UpdateChecker()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "SID Bridge";
    o.filenameSuffix = ".updates";
    o.osxLibrarySubFolder = "Application Support";
    o.folderName = "";
    prefs = prefsFileOverride != juce::File() ? std::make_unique<juce::PropertiesFile> (prefsFileOverride, o)
                                              : std::make_unique<juce::PropertiesFile> (o);
}

UpdateChecker::~UpdateChecker()
{
    if (worker.joinable())
        worker.join();
    prefs->saveIfNeeded();
}

void UpdateChecker::check (bool manual)
{
    if (running.exchange (true))
        return;
    if (worker.joinable())
        worker.join();
    if (! manual)
    {
        std::lock_guard g (mutex);
        prefs->setValue (kLastCheck, lastCheckRecord (currentVersion(), juce::Time::currentTimeMillis()));
        prefs->saveIfNeeded();
    }
    worker = std::thread ([this, manual] {
        Outcome o;
        o.manual = manual;
        o.release = fetchOverride ? fetchOverride (o.error) : fetchLatestRelease (o.error);
        if (o.release)
            o.offer = shouldOffer (*o.release, currentVersion(), skippedVersion(), manual);
        {
            std::lock_guard g (mutex);
            outcome = o;
        }
        running = false;
    });
}

void UpdateChecker::checkIfDue()
{
    juce::String last;
    {
        std::lock_guard g (mutex);
        last = prefs->getValue (kLastCheck);
    }
    if (checksAutomatically() && isAutomaticCheckDue (last, currentVersion(), juce::Time::currentTimeMillis()))
        check (false);
}

std::optional<UpdateChecker::Outcome> UpdateChecker::takeOutcome()
{
    std::lock_guard g (mutex);
    auto o = outcome;
    outcome.reset();
    return o;
}

bool UpdateChecker::checksAutomatically() const
{
    std::lock_guard g (mutex);
    return prefs->getBoolValue (kAutomatic, true);
}

void UpdateChecker::setChecksAutomatically (bool on)
{
    std::lock_guard g (mutex);
    prefs->setValue (kAutomatic, on);
    prefs->saveIfNeeded();
}

void UpdateChecker::skip (const juce::String& version)
{
    std::lock_guard g (mutex);
    prefs->setValue (kSkipped, version);
    prefs->saveIfNeeded();
}

juce::String UpdateChecker::skippedVersion() const
{
    std::lock_guard g (mutex);
    return prefs->getValue (kSkipped);
}

} // namespace updates
