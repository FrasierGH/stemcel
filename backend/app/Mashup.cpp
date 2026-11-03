#include "Mashup.h"

#include "AutoMix.h"
#include "Stems.h"
#include "Theme.h"
#include "TimelineActions.h"

#include <cmath>

namespace stemcel::native
{
namespace
{
constexpr double kBeatsPerBar = 4.0;
constexpr int kMaxKeyShift = 6; // any key is within a tritone

juce::String titleOf(const ProjectModel& m, const juce::String& itemId)
{
    const auto* item = m.findLibraryItem(itemId);
    return item != nullptr ? item->displayName() : juce::String("Track");
}

// The leader first, then the rest as given.
std::vector<size_t> leaderFirst(size_t count, int leader)
{
    std::vector<size_t> order{static_cast<size_t>(leader)};
    for (size_t i = 0; i < count; ++i)
        if (static_cast<int>(i) != leader) order.push_back(i);
    return order;
}
} // namespace

const juce::StringArray& MashupBuilder::partNames()
{
    static const juce::StringArray names{"All", "Vocals", "Drums", "Bass", "Other", "Guitar", "Piano"};
    return names;
}

MashupBuilder::MashupBuilder(EngineClient& e, ProjectMirror& m, TimelineActions& a, StemService& s)
    : engine(e), mirror(m), actions(a), stems(s)
{
}

juce::String MashupBuilder::findStem(const ProjectModel& model, const juce::String& itemId, const juce::String& label)
{
    for (const auto& item : model.library)
        if (item.kind == "stem" && item.sourceItemId == itemId && item.name.startsWithIgnoreCase(label)) return item.id;
    return {};
}

void MashupBuilder::start(const juce::String& vocalItemId, const juce::String& beatItemId)
{
    juce::StringArray beatParts{"Drums", "Bass", "Other"};
    for (const auto* extra : {"Guitar", "Piano"})
        if (findStem(mirror.model(), beatItemId, extra).isNotEmpty()) beatParts.add(extra);
    start({{vocalItemId, {"Vocals"}}, {beatItemId, beatParts}}, 1, false);
}

void MashupBuilder::start(std::vector<Request> list, int leaderIndex, bool wheel)
{
    if (busy())
    {
        if (onNotify) onNotify("A mashup is already being prepared", true);
        return;
    }
    list.erase(std::remove_if(list.begin(), list.end(), [](const Request& r) { return r.parts.isEmpty(); }), list.end());
    if (list.size() < 2)
    {
        if (onNotify) onNotify("A mashup needs parts of two tracks or more", true);
        return;
    }
    requests = std::move(list);
    requested.assign(requests.size(), false);
    leader = juce::jlimit(0, static_cast<int>(requests.size()) - 1, leaderIndex);
    openWheel = wheel;
    timerCallback();
    if (busy()) startTimer(500);
}

void MashupBuilder::timerCallback()
{
    const auto& m = mirror.model();
    for (const auto& r : requests)
        if (m.findLibraryItem(r.itemId) == nullptr)
        {
            finish("The mashup's tracks are no longer in the library", true);
            return;
        }
    if (stems.busy()) return; // a separation is under way (ours or the user's)
    for (size_t i = 0; i < requests.size(); ++i)
    {
        juce::StringArray missing;
        for (const auto& part : requests[i].parts)
            if (part != "All" && findStem(m, requests[i].itemId, part).isEmpty()) missing.add(part.toLowerCase());
        if (missing.isEmpty()) continue;
        const auto title = titleOf(m, requests[i].itemId);
        if (requested[i])
        {
            finish("Could not separate " + missing.joinIntoString(", ") + " of " + title + " for the mashup", true);
            return;
        }
        requested[i] = true;
        if (onNotify) onNotify("Mashup: separating " + missing.joinIntoString(", ") + " of " + title + "...", false);
        // Vocals alone: the vocal model; anything else: the six-stem one, keeping what's needed.
        if (missing.size() == 1 && missing[0] == "vocals")
            stems.startUvr(requests[i].itemId, {}, "vocalsTop", {}, {"vocals"});
        else
            stems.startUvr(requests[i].itemId, {}, "six", {}, missing);
        return;
    }
    place();
}

void MashupBuilder::place()
{
    const auto list = requests;
    const int lead = leader;
    const bool wheel = openWheel;
    stopTimer();
    requests.clear();
    juce::String firstTrack;
    double from = 0.0, to = 0.0;
    std::vector<std::vector<juce::String>> clipsOf(list.size());
    const auto order = leaderFirst(list.size(), lead);
    engine.inUndoGroup("Mashup", [&]
    {
        const auto curve = mirror.model().tempoCurve();
        const double end = mirror.model().contentEndMs();
        from = end > 0.0 ? curve.msAtBeat(std::ceil(curve.beatAt(end) / kBeatsPerBar) * kBeatsPerBar) : 0.0;
        // The leader's parts first, so they sit at the top.
        for (const auto i : order)
            for (const auto& part : list[i].parts)
            {
                const auto itemId = part == "All" ? list[i].itemId : findStem(mirror.model(), list[i].itemId, part);
                if (mirror.model().findLibraryItem(itemId) == nullptr) continue;
                const auto title = titleOf(mirror.model(), list[i].itemId);
                const auto trackId = actions.addTrack();
                mirror.refreshNow();
                actions.renameTrack(trackId, part == "All" ? title : title + " " + part);
                if (firstTrack.isEmpty()) firstTrack = trackId;
                mirror.refreshNow();
                clipsOf[i].push_back(actions.addClip(trackId, *mirror.model().findLibraryItem(itemId), from));
                mirror.refreshNow();
            }
        // On the grid, so every part's bars line up.
        for (const auto& clips : clipsOf)
            for (const auto& c : clips) actions.alignClipToBarGrid(c);
        mirror.refreshNow();
        // Every other track into the leader's key (the nearest exact shift).
        const auto& leadClips = clipsOf[static_cast<size_t>(lead)];
        const auto* lc = leadClips.empty() ? nullptr : mirror.model().findClip(leadClips.front());
        const auto leadKey = lc != nullptr ? automix::clipKey(mirror.model(), *lc) : juce::String();
        std::vector<std::pair<size_t, int>> shifts;
        for (size_t i = 0; i < list.size() && leadKey.isNotEmpty(); ++i)
        {
            if (static_cast<int>(i) == lead || clipsOf[i].empty()) continue;
            const auto key = automix::itemKey(mirror.model(), mirror.model().findLibraryItem(list[i].itemId));
            if (const auto shift = automix::keyMatchShift(key, leadKey, kMaxKeyShift); shift && *shift != 0) shifts.emplace_back(i, *shift);
        }
        for (const auto& [i, shift] : shifts)
            for (const auto& id : clipsOf[i]) actions.setClipKeyShift(id, shift);
        mirror.refreshNow();
        for (const auto& clips : clipsOf)
            for (const auto& c : clips)
                if (const auto* clip = mirror.model().findClip(c)) to = juce::jmax(to, clip->endMs());
    });
    juce::StringArray names;
    for (const auto& r : list) names.add(titleOf(mirror.model(), r.itemId));
    finish("Mashup: " + names.joinIntoString(" + "), false);
    if (onPlaced && to > from) onPlaced(from, to, firstTrack);
    if (wheel && onKeys)
    {
        // One wheel entry per track (its first part); its other parts follow its shifts.
        std::vector<juce::String> keyed;
        std::map<juce::String, std::vector<juce::String>> companions;
        for (const auto i : order)
            if (!clipsOf[i].empty())
            {
                keyed.push_back(clipsOf[i].front());
                companions[clipsOf[i].front()] = std::vector<juce::String>(clipsOf[i].begin() + 1, clipsOf[i].end());
            }
        if (keyed.size() >= 2) onKeys(keyed, companions);
    }
}

void MashupBuilder::finish(const juce::String& message, bool isError)
{
    stopTimer();
    requests.clear();
    if (onNotify) onNotify(message, isError);
}

namespace quickmashup
{
namespace
{
class Dialog final : public juce::Component
{
  public:
    Dialog(ProjectMirror& m, std::vector<juce::String> ids, std::function<void(std::vector<MashupBuilder::Request>, int)> onBuild)
        : itemIds(std::move(ids)), build(std::move(onBuild))
    {
        for (size_t r = 0; r < itemIds.size(); ++r)
        {
            auto row = std::make_unique<Row>();
            row->name.setText(titleOf(m.model(), itemIds[r]), juce::dontSendNotification);
            row->name.setFont(uiFont(13.0f, true));
            row->name.setColour(juce::Label::textColourId, colours::text);
            addAndMakeVisible(row->name);
            row->leader.setButtonText("Leader");
            row->leader.setRadioGroupId(7201);
            row->leader.setTooltip("Its key and tempo are the ones the others follow");
            row->leader.setToggleState(r == 0, juce::dontSendNotification);
            addAndMakeVisible(row->leader);
            auto* rowPtr = row.get();
            for (const auto& n : MashupBuilder::partNames())
            {
                auto* b = row->parts.add(new juce::ToggleButton(n));
                // The first track whole, the vocals of the others, to start with.
                b->setToggleState(r == 0 ? n == "All" : n == "Vocals", juce::dontSendNotification);
                b->onClick = [rowPtr, b, n]
                {
                    // "All" is the whole track: it and the parts exclude each other.
                    if (!b->getToggleState()) return;
                    for (auto* other : rowPtr->parts)
                        if (other != b && (n == "All" || other->getButtonText() == "All")) other->setToggleState(false, juce::dontSendNotification);
                };
                addAndMakeVisible(b);
            }
            rows.push_back(std::move(row));
        }
        hint.setText("Each track's chosen parts go on tracks of their own after the arrangement, on the bar grid, in the "
                     "leader's key. Stems not yet in the library are separated first.",
                     juce::dontSendNotification);
        hint.setFont(uiFont(12.0f));
        hint.setColour(juce::Label::textColourId, colours::textDim);
        addAndMakeVisible(hint);
        buildButton.onClick = [this]
        {
            std::vector<MashupBuilder::Request> list;
            int leader = 0;
            for (size_t r = 0; r < rows.size(); ++r)
            {
                MashupBuilder::Request req{itemIds[r], {}};
                for (auto* b : rows[r]->parts)
                    if (b->getToggleState()) req.parts.add(b->getButtonText());
                if (rows[r]->leader.getToggleState()) leader = static_cast<int>(r);
                list.push_back(req);
            }
            auto fn = build;
            if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(1);
            if (fn) fn(list, leader);
        };
        cancelButton.onClick = [this]
        {
            if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
        };
        addAndMakeVisible(buildButton);
        addAndMakeVisible(cancelButton);
        setSize(720, 110 + static_cast<int>(rows.size()) * 58);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(14);
        for (auto& row : rows)
        {
            auto line = area.removeFromTop(22);
            row->leader.setBounds(line.removeFromRight(90));
            row->name.setBounds(line);
            auto parts = area.removeFromTop(28);
            const int w = parts.getWidth() / juce::jmax(1, row->parts.size());
            for (auto* b : row->parts) b->setBounds(parts.removeFromLeft(w));
            area.removeFromTop(8);
        }
        auto buttons = area.removeFromBottom(28);
        cancelButton.setBounds(buttons.removeFromRight(90));
        buttons.removeFromRight(8);
        buildButton.setBounds(buttons.removeFromRight(120));
        hint.setBounds(area);
    }

  private:
    struct Row
    {
        juce::Label name;
        juce::ToggleButton leader;
        juce::OwnedArray<juce::ToggleButton> parts;
    };
    std::vector<juce::String> itemIds;
    std::function<void(std::vector<MashupBuilder::Request>, int)> build;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Label hint;
    juce::TextButton buildButton{"Build Mashup"}, cancelButton{"Cancel"};
};
} // namespace

void show(ProjectMirror& mirror, std::vector<juce::String> itemIds, std::function<void(std::vector<MashupBuilder::Request>, int)> build)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new Dialog(mirror, std::move(itemIds), std::move(build)));
    options.dialogTitle = "Quick Mashup";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}
} // namespace quickmashup

} // namespace stemcel::native
