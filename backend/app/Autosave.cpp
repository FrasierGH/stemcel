#include "Autosave.h"

#include "AppPaths.h"
#include "Log.h"

#include <array>
#include <cstdint>

namespace stemcel::native
{
namespace
{
constexpr const char* kAutosaveFile = "autosave.stemcel";
constexpr const char* kManifestFile = "manifest.json";
constexpr int kDefaultIntervalSeconds = 30;
constexpr int kMinIntervalSeconds = 5;

bool isValidId(const juce::String& id)
{
    return id.isNotEmpty() && id.length() <= 64
           && id.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
}

// SHA-1 (FIPS 180-1), only for ids matching the renderer's `deriveProjectIdFromPath`.
std::array<std::uint8_t, 20> sha1(const std::string& text)
{
    auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<std::uint8_t> msg(text.begin(), text.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
    {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(msg[chunk + 4 * i]) << 24) | (std::uint32_t(msg[chunk + 4 * i + 1]) << 16)
                   | (std::uint32_t(msg[chunk + 4 * i + 2]) << 8) | std::uint32_t(msg[chunk + 4 * i + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 20; ++i) out[static_cast<size_t>(i)] = static_cast<std::uint8_t>(h[i / 4] >> (24 - 8 * (i % 4)));
    return out;
}

juce::String freshId()
{
    return juce::Uuid().toString().substring(0, 24);
}
} // namespace

Autosave::Autosave(EngineClient& client, AppPreferences& preferences)
    : engine(client), prefs(preferences), subscriptions(client)
{
    subscriptions.on("PROJECT_STATE", [this](const juce::var& p) { identityChanged(p); });
    subscriptions.on("PROJECT_DIRTY",
                     [this](const juce::var& p)
                     {
                         dirty = static_cast<bool>(p.getProperty("dirty", false));
                         preferencesChanged();
                     });
    subscriptions.on("PROJECT_AUTOSAVED",
                     [this](const juce::var& p)
                     {
                         if (!inFlight) return;
                         inFlight = false;
                         if (static_cast<bool>(p.getProperty("ok", false))) writeManifest(false);
                         else log::warn("autosave", "PROJECT_AUTOSAVE failed: " + p.getProperty("error", "").toString());
                     });
    subscriptions.on("PROJECT_SAVED",
                     [this](const juce::var& p)
                     {
                         // An explicit save supersedes the bucket.
                         if (static_cast<bool>(p.getProperty("ok", false)) && projectId.isNotEmpty()
                             && p.getProperty("filePath", filePath).toString() == filePath)
                             clear(projectId);
                     });
}

Autosave::~Autosave()
{
    stopTimer();
}

juce::File Autosave::root()
{
    return userAppDataDirectory().getChildFile(appFolderName()).getChildFile("autosave");
}

juce::String Autosave::projectIdForPath(const juce::String& path)
{
    // UTF-8 of the trimmed, lower-cased path; first 8 bytes of its SHA-1 as hex.
    const auto digest = sha1(path.trim().toLowerCase().toStdString());
    return juce::String::toHexString(digest.data(), 8, 0);
}

void Autosave::clear(const juce::String& id)
{
    if (!isValidId(id)) return;
    const auto dir = root().getChildFile(id);
    if (dir.isAChildOf(root())) dir.deleteRecursively();
}

std::vector<Autosave::Entry> Autosave::listRecoverable()
{
    std::vector<Entry> out;
    for (const auto& entry : juce::RangedDirectoryIterator(root(), false, "*", juce::File::findDirectories))
    {
        const auto dir = entry.getFile();
        if (!isValidId(dir.getFileName())) continue;
        const auto manifest = juce::JSON::parse(dir.getChildFile(kManifestFile));
        if (!manifest.isObject() || static_cast<bool>(manifest.getProperty("pending", true))) continue;
        const auto autosave = dir.getChildFile(kAutosaveFile);
        if (!autosave.existsAsFile()) continue;
        Entry e;
        e.projectId = manifest.getProperty("projectId", {}).toString();
        if (!isValidId(e.projectId)) continue;
        const auto original = manifest.getProperty("originalPath", {});
        e.originalPath = original.isString() ? original.toString() : juce::String();
        e.projectName = manifest.getProperty("projectName", "Untitled").toString();
        e.savedAtIso = manifest.getProperty("savedAtIso", {}).toString();
        e.autosaveFile = autosave;
        bool recoverable = e.originalPath.isEmpty();
        if (e.originalPath.isNotEmpty())
        {
            const juce::File originalFile(e.originalPath);
            e.originalExists = originalFile.existsAsFile();
            recoverable = !e.originalExists
                          || autosave.getLastModificationTime().toMilliseconds()
                                 > originalFile.getLastModificationTime().toMilliseconds() + 500;
        }
        if (recoverable) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.savedAtIso > b.savedAtIso; });
    return out;
}

void Autosave::restore(const Entry& entry)
{
    pendingRecoveredId = entry.projectId;
    auto* p = new juce::DynamicObject();
    p->setProperty("autosavePath", entry.autosaveFile.getFullPathName());
    p->setProperty("originalPath", entry.originalPath.isNotEmpty() ? juce::var(entry.originalPath) : juce::var());
    engine.send("PROJECT_LOAD_RECOVERY", juce::var(p));
    log::info("autosave", "recovering " + entry.projectId + " from " + entry.autosaveFile.getFullPathName());
}

void Autosave::identityChanged(const juce::var& state)
{
    const auto nextPath = state.getProperty("filePath", {}).toString();
    const bool reset = static_cast<bool>(state.getProperty("reset", false));
    projectName = state.getProperty("name", {}).toString();
    dirty = static_cast<bool>(state.getProperty("dirty", false));
    if (reset || nextPath != filePath)
    {
        const auto previous = projectId;
        filePath = nextPath;
        if (pendingRecoveredId.isNotEmpty())
            projectId = nextPath.isNotEmpty() ? projectIdForPath(nextPath) : pendingRecoveredId;
        else
            projectId = nextPath.isNotEmpty() ? projectIdForPath(nextPath) : freshId();
        // A recovery keeps every bucket (the user may still want another); otherwise the
        // project being replaced no longer needs its own.
        if (previous.isNotEmpty() && previous != projectId && pendingRecoveredId.isEmpty()) clear(previous);
        pendingRecoveredId.clear();
        inFlight = false;
    }
    preferencesChanged();
}

void Autosave::preferencesChanged()
{
    const auto settings = prefs.get("autosave", "enabled", true);
    // The smoke test drives its own autosave; it must never leave buckets in the user's profile.
    const bool enabled = static_cast<bool>(settings)
                         && !juce::JUCEApplicationBase::getCommandLineParameters().contains("--smoke-test");
    const int seconds = juce::jmax(kMinIntervalSeconds, static_cast<int>(prefs.get("autosave", "intervalSeconds", kDefaultIntervalSeconds)));
    if (!enabled || !dirty || projectId.isEmpty())
    {
        stopTimer();
        return;
    }
    if (!isTimerRunning() || getTimerInterval() != seconds * 1000)
    {
        startTimer(seconds * 1000);
        tick(); // a newly dirty project gets a prompt first snapshot
    }
}

void Autosave::timerCallback()
{
    tick();
}

void Autosave::writeManifest(bool pending)
{
    if (!isValidId(projectId)) return;
    const auto dir = root().getChildFile(projectId);
    dir.createDirectory();
    auto* m = new juce::DynamicObject();
    m->setProperty("projectId", projectId);
    m->setProperty("originalPath", filePath.isNotEmpty() ? juce::var(filePath) : juce::var());
    m->setProperty("projectName", projectName.isNotEmpty() ? projectName : juce::String("Untitled"));
    m->setProperty("savedAtIso", juce::Time::getCurrentTime().toISO8601(true));
    m->setProperty("pending", pending);
    m->setProperty("appVersion", juce::JUCEApplicationBase::getInstance() != nullptr
                                     ? juce::JUCEApplicationBase::getInstance()->getApplicationVersion()
                                     : juce::String());
    dir.getChildFile(kManifestFile).replaceWithText(juce::JSON::toString(juce::var(m)));
}

void Autosave::tick()
{
    if (!dirty || projectId.isEmpty() || inFlight) return;
    writeManifest(true);
    inFlight = true;
    auto* p = new juce::DynamicObject();
    p->setProperty("filePath", root().getChildFile(projectId).getChildFile(kAutosaveFile).getFullPathName());
    engine.send("PROJECT_AUTOSAVE", juce::var(p));
}

} // namespace stemcel::native
