#include "KeyWheel.h"

#include "Theme.h"

#include <cmath>
#include <limits>
#include <map>

namespace stemcel::native
{
namespace
{
constexpr int kMaxRecommendShift = 3; // further, and a voice starts to sound pitched
constexpr int kMaxApplyShift = 6;     // any key is within a tritone

// "C major" / "A minor" for a wheel position.
juce::String keyName(automix::Camelot c)
{
    static const char* names[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    const int majorRoot = ((c.number - 8) * 7 % 12 + 12) % 12;
    const int root = c.major ? majorRoot : (majorRoot + 9) % 12;
    return juce::String(names[root]) + (c.major ? "" : "m");
}

juce::String fullKeyName(automix::Camelot c)
{
    auto name = keyName(c);
    return name.endsWith("m") ? name.dropLastCharacters(1) + " minor" : name + " major";
}

float angleOf(int number)
{
    // 12 at the top, numbers clockwise (as the Camelot wheel is drawn).
    return juce::MathConstants<float>::twoPi * static_cast<float>(number % 12) / 12.0f;
}
} // namespace

std::optional<int> KeyWheel::shiftTo(const juce::String& sourceKey, automix::Camelot target, int maxShift)
{
    if (!automix::camelot(sourceKey).valid() || !target.valid()) return std::nullopt;
    for (int step = 0; step <= maxShift; ++step)
        for (const int shift : {step, -step})
            if (automix::keyDistance(automix::camelot(automix::transposeKey(sourceKey, shift)), target) == 0) return shift;
    return std::nullopt;
}

automix::Camelot KeyWheel::recommended(const std::vector<juce::String>& sourceKeys)
{
    automix::Camelot bestKey;
    double bestCost = std::numeric_limits<double>::max();
    for (int n = 1; n <= 12; ++n)
        for (const bool major : {false, true})
        {
            const automix::Camelot target{n, major};
            double cost = 0.0;
            bool reachable = true;
            for (const auto& key : sourceKeys)
            {
                const auto shift = shiftTo(key, target, kMaxRecommendShift);
                if (!shift)
                {
                    reachable = false;
                    break;
                }
                cost += std::abs(*shift);
            }
            if (reachable && cost < bestCost)
            {
                bestCost = cost;
                bestKey = target;
            }
        }
    return bestKey;
}

std::vector<automix::Camelot> KeyWheel::commonKeys(const std::vector<juce::String>& keys, int leader, int limit)
{
    std::vector<std::pair<double, automix::Camelot>> close, far;
    for (int n = 1; n <= 12; ++n)
        for (const bool major : {false, true})
        {
            const automix::Camelot target{n, major};
            bool closeOk = true, farOk = true;
            double closeCost = 0.0, farCost = 0.0;
            for (size_t i = 0; i < keys.size(); ++i)
            {
                if (!automix::camelot(keys[i]).valid()) continue; // unknown: nothing to match
                const bool isLeader = static_cast<int>(i) == leader;
                if (const auto near = shiftTo(keys[i], target, isLeader ? 0 : kMaxRecommendShift))
                    closeCost += std::abs(*near);
                else
                    closeOk = false;
                if (const auto any = shiftTo(keys[i], target, kMaxApplyShift))
                    farCost += std::abs(*any) * (isLeader ? 3.0 : 1.0);
                else
                    farOk = false;
            }
            if (closeOk) close.push_back({closeCost, target});
            if (farOk) far.push_back({farCost, target});
        }
    auto& pick = close.empty() ? far : close;
    std::stable_sort(pick.begin(), pick.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<automix::Camelot> out;
    for (const auto& entry : pick)
        if (static_cast<int>(out.size()) < limit) out.push_back(entry.second);
    return out;
}

KeyWheel::KeyWheel(ProjectMirror& projectMirror, TimelineActions& timelineActions, std::vector<juce::String> clips,
                   std::vector<juce::String> items)
    : mirror(projectMirror), actions(timelineActions), clipIds(std::move(clips)), itemIds(std::move(items))
{
    info.setFont(uiFont(12.0f));
    info.setColour(juce::Label::textColourId, colours::text);
    info.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(info);
    commonLabel.setText("Common keys", juce::dontSendNotification);
    commonLabel.setFont(uiFont(12.0f));
    commonLabel.setColour(juce::Label::textColourId, colours::textDim);
    addChildComponent(commonLabel);
    for (int i = 0; i < 4; ++i)
    {
        auto* b = commonButtons.add(new juce::TextButton());
        b->onClick = [this, i]
        {
            if (i < static_cast<int>(common.size()) && !clipIds.empty()) apply(common[static_cast<size_t>(i)], true);
        };
        addChildComponent(b);
    }
    leaderBox.setTooltip("The dominant track: it stays in its key (or shifts least), the others follow");
    leaderBox.onChange = [this]
    {
        leader = juce::jmax(0, leaderBox.getSelectedItemIndex());
        rebuild();
    };
    addChildComponent(leaderBox);
    fromCurrent.setTooltip("Count shifts from where each clip is now, not from its original key");
    fromCurrent.onClick = [this] { rebuild(); };
    addChildComponent(fromCurrent);
    mirror.addChangeListener(this);
    rebuild();
    setSize(560, 780);
}

KeyWheel::~KeyWheel()
{
    mirror.removeChangeListener(this);
}

void KeyWheel::show(ProjectMirror& mirror, TimelineActions& actions, std::vector<juce::String> clipIds, std::vector<juce::String> itemIds,
                    int leader, std::map<juce::String, std::vector<juce::String>> companions)
{
    juce::DialogWindow::LaunchOptions options;
    auto* wheel = new KeyWheel(mirror, actions, std::move(clipIds), std::move(itemIds));
    wheel->companions = std::move(companions);
    wheel->setLeaderForTest(leader);
    options.content.setOwned(wheel);
    options.dialogTitle = "Key Wheel";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    if (auto* window = options.launchAsync()) window->setResizeLimits(420, 620, 2000, 2000);
}

void KeyWheel::changeListenerCallback(juce::ChangeBroadcaster*)
{
    rebuild();
}

juce::String KeyWheel::baseKey(const Subject& s) const
{
    return fromCurrent.getToggleState() ? s.currentKey : s.sourceKey;
}

std::optional<int> KeyWheel::targetShift(const Subject& s, automix::Camelot target) const
{
    const auto shift = shiftTo(baseKey(s), target, kMaxApplyShift);
    if (!shift) return std::nullopt;
    return fromCurrent.getToggleState() ? s.shift + *shift : *shift;
}

bool KeyWheel::applies(size_t index) const
{
    return index < subjects.size() && subjects[index].clipId.isNotEmpty() && (chosen < 0 || chosen == static_cast<int>(index));
}

void KeyWheel::rebuildControls()
{
    clipButtons.clear();
    leaderBox.clear(juce::dontSendNotification);
    if (!clipIds.empty() && subjects.size() >= 2)
    {
        auto* all = clipButtons.add(new juce::TextButton("All"));
        all->setTooltip("A click on the wheel shifts every clip into that key");
        all->setClickingTogglesState(true);
        all->setRadioGroupId(7101);
        all->onClick = [this]
        {
            chosen = -1;
            rebuild();
        };
        addAndMakeVisible(all);
        for (size_t i = 0; i < subjects.size(); ++i)
        {
            auto* b = clipButtons.add(new juce::TextButton());
            b->setTooltip("A click on the wheel shifts this clip only");
            b->setClickingTogglesState(true);
            b->setRadioGroupId(7101);
            b->setColour(juce::TextButton::buttonOnColourId, subjects[i].colour.withAlpha(0.85f));
            b->setColour(juce::TextButton::buttonColourId, subjects[i].colour.withAlpha(0.25f));
            b->onClick = [this, i]
            {
                chosen = static_cast<int>(i);
                rebuild();
            };
            addAndMakeVisible(b);
        }
        chosen = juce::jlimit(-1, static_cast<int>(subjects.size()) - 1, chosen);
        clipButtons[chosen + 1]->setToggleState(true, juce::dontSendNotification);
    }
    for (size_t i = 0; i < subjects.size(); ++i) leaderBox.addItem("Leader: " + subjects[i].name, static_cast<int>(i) + 1);
    leader = juce::jlimit(0, juce::jmax(0, static_cast<int>(subjects.size()) - 1), leader);
    leaderBox.setSelectedItemIndex(leader, juce::dontSendNotification);
    leaderBox.setVisible(subjects.size() >= 2);
    builtFor = subjects.size();
    resized();
}

void KeyWheel::rebuild()
{
    const auto& m = mirror.model();
    subjects.clear();
    for (const auto& id : clipIds)
        if (const auto* clip = m.findClip(id))
        {
            const auto* item = m.findLibraryItem(clip->libraryItemId);
            const auto* track = m.findTrack(clip->trackId);
            const int colour = clip->colorIndex >= 0 ? clip->colorIndex : track != nullptr ? track->colorIndex : 0;
            Subject s{clip->id, clip->name.isNotEmpty() ? clip->name : item != nullptr ? item->displayName() : clip->id,
                      automix::itemKey(m, item), automix::clipKey(m, *clip), paletteSwatch(juce::jmax(0, colour))};
            s.shift = static_cast<int>(std::lround(clip->semitones));
            subjects.push_back(s);
        }
    for (const auto& id : itemIds)
        if (const auto* item = m.findLibraryItem(id))
        {
            const auto key = automix::itemKey(m, item);
            subjects.push_back({{}, item->displayName(), key, key, colours::sky400});
        }
    if (subjects.size() != builtFor) rebuildControls();
    // Each clip's button: its name and the key it is in now.
    for (size_t i = 0; i < subjects.size() && static_cast<int>(i) + 1 < clipButtons.size(); ++i)
    {
        const auto c = automix::camelot(subjects[i].currentKey);
        clipButtons[static_cast<int>(i) + 1]->setButtonText(subjects[i].name + "  " + (c.valid() ? c.text() : juce::String("?")));
    }
    bool anyShifted = false;
    for (const auto& s : subjects) anyShifted = anyShifted || s.shift != 0;
    fromCurrent.setVisible(anyShifted && !clipIds.empty());

    std::vector<juce::String> keys;
    keys.reserve(subjects.size());
    for (const auto& s : subjects) keys.push_back(baseKey(s));
    common = subjects.size() >= 2 ? commonKeys(keys, leader) : std::vector<automix::Camelot>{};
    best = !common.empty() ? common.front() : subjects.size() == 1 ? automix::camelot(subjects.front().currentKey) : automix::Camelot{};
    for (int i = 0; i < commonButtons.size(); ++i)
    {
        auto* b = commonButtons[i];
        const bool shown = i < static_cast<int>(common.size());
        b->setVisible(shown);
        if (!shown) continue;
        const auto key = common[static_cast<size_t>(i)];
        b->setButtonText(key.text() + " " + keyName(key));
        b->setColour(juce::TextButton::buttonColourId, colours::amber500.withAlpha(i == 0 ? 0.75f : 0.3f));
        b->setTooltip(planFor(key));
    }
    commonLabel.setVisible(!common.empty());

    juce::String text;
    for (const auto& s : subjects)
    {
        const auto c = automix::camelot(s.currentKey);
        text << s.name << ": " << (c.valid() ? s.currentKey + " (" + c.text() + ")" : juce::String("key unknown"));
        if (s.shift != 0) text << ", shifted " << (s.shift > 0 ? "+" : "") << s.shift << " from " << s.sourceKey;
        text << "\n";
    }
    if (hover.valid()) text << "\n" << planFor(hover);
    info.setText(text, juce::dontSendNotification);
    repaint();
}

juce::String KeyWheel::planFor(automix::Camelot target) const
{
    juce::String plan = "To " + fullKeyName(target) + " (" + target.text() + "): ";
    juce::StringArray moves;
    for (size_t i = 0; i < subjects.size(); ++i)
    {
        const auto& s = subjects[i];
        if (!applies(i)) continue;
        const auto shift = targetShift(s, target);
        moves.add(s.name + (shift ? " " + juce::String(*shift > 0 ? "+" : "") + juce::String(*shift) : juce::String(" can't reach it")));
    }
    return moves.isEmpty() ? juce::String() : plan + moves.joinIntoString(", ");
}

void KeyWheel::resized()
{
    auto area = getLocalBounds().reduced(12);
    info.setBounds(area.removeFromBottom(110));
    auto commonRow = area.removeFromBottom(30).reduced(0, 2);
    commonLabel.setBounds(commonRow.removeFromLeft(96));
    for (auto* b : commonButtons) b->setBounds(commonRow.removeFromLeft(96).reduced(2, 0));
    auto leaderRow = area.removeFromBottom(30).reduced(0, 2);
    leaderBox.setBounds(leaderRow.removeFromLeft(juce::jmin(260, leaderRow.getWidth() / 2)));
    fromCurrent.setBounds(leaderRow.withTrimmedLeft(10));
    if (!clipButtons.isEmpty())
    {
        auto clipRow = area.removeFromBottom(30).reduced(0, 2);
        const int w = clipRow.getWidth() / clipButtons.size();
        for (auto* b : clipButtons) b->setBounds(clipRow.removeFromLeft(w).reduced(2, 0));
    }
    area.removeFromBottom(6);
    const auto wheel = area.toFloat();
    centre = wheel.getCentre();
    radius = juce::jmin(wheel.getWidth(), wheel.getHeight()) / 2.0f - 6.0f;
}

juce::Path KeyWheel::segmentPath(automix::Camelot key, float inner, float outer) const
{
    const float half = juce::MathConstants<float>::pi / 12.0f;
    const float a = angleOf(key.number);
    juce::Path p;
    p.addPieSegment(centre.x - outer, centre.y - outer, outer * 2.0f, outer * 2.0f, a - half, a + half, inner / outer);
    return p;
}

automix::Camelot KeyWheel::segmentAt(juce::Point<float> p) const
{
    const float d = p.getDistanceFrom(centre);
    if (d < radius * 0.3f || d > radius) return {};
    // Angle clockwise from the top.
    float a = std::atan2(p.x - centre.x, centre.y - p.y);
    if (a < 0.0f) a += juce::MathConstants<float>::twoPi;
    int n = static_cast<int>(std::round(a / (juce::MathConstants<float>::twoPi / 12.0f))) % 12;
    if (n == 0) n = 12;
    return {n, d > radius * 0.65f};
}

void KeyWheel::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    const float outerR = radius, middleR = radius * 0.65f, innerR = radius * 0.3f;
    std::vector<automix::Camelot> current;
    for (const auto& s : subjects)
        if (const auto c = automix::camelot(s.currentKey); c.valid()) current.push_back(c);
    for (int n = 1; n <= 12; ++n)
        for (const bool major : {true, false})
        {
            const automix::Camelot key{n, major};
            const auto path = segmentPath(key, major ? middleR : innerR, major ? outerR : middleR);
            // Lit when it mixes with every chosen key; brightest when it is the recommendation.
            int worst = 0;
            for (const auto& c : current) worst = juce::jmax(worst, automix::keyDistance(c, key));
            const bool compatible = !current.empty() && worst <= 1;
            juce::Colour fill = major ? colours::zinc800 : colours::zinc700;
            if (compatible) fill = colours::sky500.withAlpha(0.35f).overlaidWith(fill.withAlpha(0.0f));
            for (size_t i = 0; i < common.size(); ++i)
                if (key.number == common[i].number && key.major == common[i].major)
                    fill = colours::amber500.withAlpha(i == 0 ? 0.75f : 0.35f);
            if (common.empty() && key.number == best.number && key.major == best.major && best.valid()) fill = colours::amber500.withAlpha(0.75f);
            if (key.number == hover.number && key.major == hover.major && hover.valid()) fill = fill.brighter(0.35f);
            g.setColour(fill);
            g.fillPath(path);
            g.setColour(colours::background);
            g.strokePath(path, juce::PathStrokeType(1.5f));
            // Camelot code above the key name, at the segment's middle.
            const float r = major ? (middleR + outerR) / 2.0f : (innerR + middleR) / 2.0f;
            const float a = angleOf(n);
            const juce::Point<float> at{centre.x + r * std::sin(a), centre.y - r * std::cos(a)};
            g.setColour(colours::zinc100);
            g.setFont(uiFont(major ? 13.0f : 12.0f, true));
            g.drawText(key.text(), juce::Rectangle<float>(at.x - 30.0f, at.y - 16.0f, 60.0f, 15.0f), juce::Justification::centred);
            g.setColour(colours::textDim);
            g.setFont(uiFont(11.0f));
            g.drawText(keyName(key), juce::Rectangle<float>(at.x - 30.0f, at.y, 60.0f, 14.0f), juce::Justification::centred);
        }
    // Each chosen clip's key: a dot in its colour on the ring (several on one key fan out).
    std::map<int, int> onKey;
    for (size_t index = 0; index < subjects.size(); ++index)
    {
        const auto& s = subjects[index];
        const auto c = automix::camelot(s.currentKey);
        if (!c.valid()) continue;
        // Beside the key's label, inside its segment.
        const int slot = onKey[c.number * 2 + (c.major ? 1 : 0)]++;
        const float r = (c.major ? (middleR + outerR) / 2.0f : (innerR + middleR) / 2.0f) + 22.0f - static_cast<float>(slot) * 14.0f;
        const float a = angleOf(c.number);
        const juce::Point<float> at{centre.x + r * std::sin(a), centre.y - r * std::cos(a)};
        g.setColour(s.colour);
        g.fillEllipse(at.x - 6.0f, at.y - 6.0f, 12.0f, 12.0f);
        g.setColour(colours::zinc100);
        g.drawEllipse(at.x - 6.0f, at.y - 6.0f, 12.0f, 12.0f, 1.5f);
        // The clip a click shifts, ringed.
        if (chosen == static_cast<int>(index)) g.drawEllipse(at.x - 9.0f, at.y - 9.0f, 18.0f, 18.0f, 2.0f);
    }
    g.setColour(colours::textFaint);
    g.setFont(uiFont(11.0f));
    g.drawText(juce::String(clipIds.empty() ? "" : "Click a key to shift"), juce::Rectangle<float>(centre.x - innerR, centre.y - 8.0f, innerR * 2.0f, 16.0f),
               juce::Justification::centred);
}

void KeyWheel::mouseMove(const juce::MouseEvent& e)
{
    const auto at = segmentAt(e.position);
    if (at.number != hover.number || at.major != hover.major)
    {
        hover = at;
        rebuild();
    }
}

void KeyWheel::mouseExit(const juce::MouseEvent&)
{
    hover = {};
    rebuild();
}

void KeyWheel::mouseUp(const juce::MouseEvent& e)
{
    if (const auto target = segmentAt(e.position); target.valid() && !clipIds.empty()) apply(target, false);
}

void KeyWheel::apply(automix::Camelot target, bool all)
{
    const auto list = subjects; // a shift refreshes the mirror, and so the subjects
    actions.engineClient().inUndoGroup("Shift to " + fullKeyName(target),
                                       [&]
                                       {
                                           for (size_t i = 0; i < list.size(); ++i)
                                               if (list[i].clipId.isNotEmpty() && (all || applies(i)))
                                                   if (const auto shift = targetShift(list[i], target))
                                                   {
                                                       actions.setClipKeyShift(list[i].clipId, *shift);
                                                       if (const auto it = companions.find(list[i].clipId); it != companions.end())
                                                           for (const auto& part : it->second) actions.setClipKeyShift(part, *shift);
                                                   }
                                       });
}

} // namespace stemcel::native
