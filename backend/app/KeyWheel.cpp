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

KeyWheel::KeyWheel(ProjectMirror& projectMirror, TimelineActions& timelineActions, std::vector<juce::String> clips,
                   std::vector<juce::String> items)
    : mirror(projectMirror), actions(timelineActions), clipIds(std::move(clips)), itemIds(std::move(items))
{
    info.setFont(uiFont(12.0f));
    info.setColour(juce::Label::textColourId, colours::text);
    info.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(info);
    mirror.addChangeListener(this);
    rebuild();
    setSize(560, 680);
}

KeyWheel::~KeyWheel()
{
    mirror.removeChangeListener(this);
}

void KeyWheel::show(ProjectMirror& mirror, TimelineActions& actions, std::vector<juce::String> clipIds, std::vector<juce::String> itemIds)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new KeyWheel(mirror, actions, std::move(clipIds), std::move(itemIds)));
    options.dialogTitle = "Key Wheel";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    if (auto* window = options.launchAsync()) window->setResizeLimits(420, 520, 2000, 2000);
}

void KeyWheel::changeListenerCallback(juce::ChangeBroadcaster*)
{
    rebuild();
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
            subjects.push_back({clip->id, clip->name.isNotEmpty() ? clip->name : item != nullptr ? item->displayName() : clip->id,
                                automix::itemKey(m, item), automix::clipKey(m, *clip), paletteSwatch(juce::jmax(0, colour))});
        }
    for (const auto& id : itemIds)
        if (const auto* item = m.findLibraryItem(id))
        {
            const auto key = automix::itemKey(m, item);
            subjects.push_back({{}, item->displayName(), key, key, colours::sky400});
        }
    std::vector<juce::String> keys;
    for (const auto& s : subjects)
        if (automix::camelot(s.sourceKey).valid()) keys.push_back(s.sourceKey);
    best = keys.size() >= 2 ? recommended(keys) : keys.size() == 1 ? automix::camelot(keys.front()) : automix::Camelot{};
    juce::String text;
    for (const auto& s : subjects)
    {
        const auto c = automix::camelot(s.currentKey);
        text << s.name << ": " << (c.valid() ? s.currentKey + " (" + c.text() + ")" : juce::String("key unknown")) << "\n";
    }
    if (best.valid() && subjects.size() >= 2) text << "\nRecommended: " << fullKeyName(best) << " (" << best.text() << ")";
    if (hover.valid()) text << "\n" << planFor(hover);
    info.setText(text, juce::dontSendNotification);
    repaint();
}

juce::String KeyWheel::planFor(automix::Camelot target) const
{
    juce::String plan = "To " + fullKeyName(target) + " (" + target.text() + "): ";
    juce::StringArray moves;
    for (const auto& s : subjects)
    {
        if (s.clipId.isEmpty()) continue;
        const auto shift = shiftTo(s.sourceKey, target, kMaxApplyShift);
        moves.add(s.name + (shift ? " " + juce::String(*shift > 0 ? "+" : "") + juce::String(*shift) : juce::String(" stays")));
    }
    return moves.isEmpty() ? juce::String() : plan + moves.joinIntoString(", ");
}

void KeyWheel::resized()
{
    auto area = getLocalBounds().reduced(12);
    info.setBounds(area.removeFromBottom(150));
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
            if (key.number == best.number && key.major == best.major && best.valid()) fill = colours::amber500.withAlpha(0.75f);
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
    for (const auto& s : subjects)
    {
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
    if (const auto target = segmentAt(e.position); target.valid() && !clipIds.empty()) apply(target);
}

void KeyWheel::apply(automix::Camelot target)
{
    actions.engineClient().inUndoGroup("Shift to " + fullKeyName(target),
                                       [&]
                                       {
                                           for (const auto& s : subjects)
                                               if (s.clipId.isNotEmpty())
                                                   if (const auto shift = shiftTo(s.sourceKey, target, kMaxApplyShift))
                                                       actions.setClipKeyShift(s.clipId, *shift);
                                       });
}

} // namespace stemcel::native
