#include "SectionAnalyser.h"

#include "Log.h"
#include "SectionAnalysis.h"

namespace stemcel::native
{

SectionAnalyser::SectionAnalyser(EngineClient& client, ProjectMirror& projectMirror, AppPreferences& preferences)
    : engine(client), mirror(projectMirror), prefs(preferences)
{
    mirror.addChangeListener(this);
}

SectionAnalyser::~SectionAnalyser()
{
    alive->store(false);
    mirror.removeChangeListener(this);
    pool.removeAllJobs(true, 30000);
}

void SectionAnalyser::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (!static_cast<bool>(prefs.get("ui", "detectSections", false))) return;
    for (const auto& item : mirror.model().library)
    {
        // Whole tracks with a tempo (the bars are on its grid), not yet analysed.
        if (item.kind != "source" || item.bpm <= 0.0 || item.unresolved || tried.count(item.id) != 0) continue;
        if (!sections::of(item).empty()) continue;
        tried.insert(item.id);
        analyse(item.id, false);
    }
}

void SectionAnalyser::analyse(const juce::String& itemId, bool force)
{
    const auto* item = mirror.model().findLibraryItem(itemId);
    if (item == nullptr) return;
    const juce::File audio(item->audioPath());
    const double bpm = item->bpm, anchor = item->beatAnchorSec;
    const auto name = item->displayName();
    // The item's Vocals stem, when it has one, says where the voice is.
    juce::File vocals;
    for (const auto& other : mirror.model().library)
        if (other.kind == "stem" && other.sourceItemId == itemId && other.name.startsWithIgnoreCase("Vocals"))
            vocals = juce::File(other.audioPath());
    const auto cache = sections::cacheFileFor(audio, bpm, anchor, vocals.existsAsFile());
    // Automatic analysis is quiet; one asked for says what it found.
    auto store = [this, itemId, name, force, alive = alive](const juce::var& list, bool fromCache)
    {
        if (!alive->load()) return;
        auto* payload = new juce::DynamicObject();
        payload->setProperty("itemId", itemId);
        payload->setProperty("sections", list);
        engine.send("LIBRARY_ITEM_SET_SECTIONS", juce::var(payload));
        const int count = list.isArray() ? list.getArray()->size() : 0;
        log::info("sections", name + ": " + juce::String(count) + " sections" + (fromCache ? " (cached)" : ""));
        if (onNotify && force) onNotify(juce::String(count) + " sections in " + name, false);
    };
    if (!force && cache.existsAsFile())
        if (const auto cached = juce::JSON::parse(cache); cached.isArray())
        {
            store(cached, true);
            return;
        }
    ++running;
    if (onNotify && force) onNotify("Analysing the sections of " + name + "...", false);
    pool.addJob(
        [this, audio, bpm, anchor, vocals, cache, store, name, force, alive = alive]
        {
            const auto found = sections::analyseFile(audio, bpm, anchor, vocals);
            const auto list = found.has_value() ? sections::toVar(*found) : juce::var();
            if (found.has_value())
            {
                cache.getParentDirectory().createDirectory();
                cache.replaceWithText(juce::JSON::toString(list, true));
            }
            juce::MessageManager::callAsync(
                [this, list, store, name, force, ok = found.has_value(), alive]
                {
                    if (!alive->load()) return;
                    --running;
                    if (!ok)
                    {
                        if (onNotify && force) onNotify("Couldn't read " + name + " to find its sections", true);
                        return;
                    }
                    store(list, false);
                });
        });
}

} // namespace stemcel::native
