#include "TempoReport.h"

#include "Log.h"
#include "Theme.h"

#include "../src/dsp/TempoDrift.h"

#include <algorithm>

namespace stemcel::native
{
namespace tempodrift
{
namespace
{
juce::var toVar(const stemcel::TempoDriftResult& r)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("version", 1);
    o->setProperty("mainBpm", r.mainBpm);
    o->setProperty("driftPercent", r.driftPercent);
    juce::Array<juce::var> curve;
    for (const auto& p : r.curve) curve.add(juce::Array<juce::var>{std::round(p.sec * 1000.0) / 1000.0, std::round(p.bpm * 100.0) / 100.0});
    o->setProperty("curve", curve);
    juce::Array<juce::var> sections;
    for (const auto& s : r.sections)
    {
        auto* so = new juce::DynamicObject();
        so->setProperty("startSec", s.startSec);
        so->setProperty("endSec", s.endSec);
        so->setProperty("bpm", s.bpm);
        sections.add(juce::var(so));
    }
    o->setProperty("sections", sections);
    return juce::var(o);
}

juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props) o->setProperty(k, v);
    return juce::var(o);
}
} // namespace

Drift of(const LibraryItemModel& item)
{
    Drift d;
    const auto v = item.raw.getProperty("tempoDrift", {});
    if (!v.isObject()) return d;
    d.mainBpm = v.getProperty("mainBpm", 0.0);
    d.driftPercent = v.getProperty("driftPercent", 0.0);
    if (const auto* curve = v.getProperty("curve", {}).getArray())
        for (const auto& p : *curve)
            if (const auto* pair = p.getArray(); pair != nullptr && pair->size() >= 2)
                d.curve.push_back({static_cast<double>((*pair)[0]) * 1000.0, static_cast<double>((*pair)[1])});
    if (const auto* sections = v.getProperty("sections", {}).getArray())
        for (const auto& s : *sections)
            d.sections.push_back({s.getProperty("startSec", 0.0), s.getProperty("endSec", 0.0), s.getProperty("bpm", 0.0)});
    return d;
}

bool autoWarp(TimelineActions& actions, const ProjectModel& model, const juce::String& clipId)
{
    const auto* clip = model.findClip(clipId);
    const auto* item = clip != nullptr ? model.findLibraryItem(clip->libraryItemId) : nullptr;
    if (item == nullptr) return false;
    const auto d = of(*item);
    if (!d.valid()) return false;
    // The measured tempo over the clip's window (and a bar either side, so its ends are covered).
    const double margin = 240000.0 / d.mainBpm;
    juce::Array<juce::var> points;
    for (const auto& p : d.curve)
        if (p.timeMs >= clip->inMs - margin && p.timeMs <= clip->inMs + clip->durationMs + margin)
            points.add(obj({{"sourceMs", p.timeMs}, {"bpm", p.bpm}}));
    if (points.size() < 2) return false;
    auto& engine = actions.engineClient();
    engine.inUndoGroup("Auto-warp clip tempo",
                       [&]
                       {
                           // Warped, following the project (no pinned ratio), along the measured tempo.
                           engine.send("CLIP_SET_WARP", obj({{"clipId", clipId}, {"warpEnabled", true}, {"tempoRatio", juce::var()}}));
                           engine.send("CLIP_SET_SOURCE_TEMPO", obj({{"clipId", clipId}, {"points", points}}));
                       });
    return true;
}

bool followInProjectTempo(TimelineActions& actions, const ProjectModel& model, const juce::String& clipId)
{
    const auto* clip = model.findClip(clipId);
    const auto* item = clip != nullptr ? model.findLibraryItem(clip->libraryItemId) : nullptr;
    if (item == nullptr) return false;
    const auto d = of(*item);
    if (!d.valid()) return false;
    // The clip plays as recorded, so its source runs 1:1 with the timeline from where it starts.
    const double start = clip->offsetMs;
    const double end = start + clip->durationMs;
    const auto old = model.tempoCurve();
    std::vector<TempoPoint> points;
    for (const auto& p : model.tempoMap)
        if (p.timeMs < start - 1.0 || p.timeMs > end + 1.0) points.push_back(p);
    // The tempo before and after the clip stays as it was.
    if (start > 1.0) points.push_back({start - 1.0, old.bpmAt(start)});
    for (const auto& p : d.curve)
        if (p.timeMs >= clip->inMs && p.timeMs <= clip->inMs + clip->durationMs)
            points.push_back({start + (p.timeMs - clip->inMs), juce::jlimit(20.0, 400.0, p.bpm)});
    points.push_back({end + 1.0, old.bpmAt(end)});
    std::sort(points.begin(), points.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.timeMs < b.timeMs; });
    auto& engine = actions.engineClient();
    engine.inUndoGroup("Follow clip tempo",
                       [&]
                       {
                           engine.send("CLIP_SET_SOURCE_TEMPO", obj({{"clipId", clipId}, {"points", juce::Array<juce::var>{}}}));
                           engine.send("CLIP_SET_WARP", obj({{"clipId", clipId}, {"warpEnabled", false}}));
                           actions.setTempoMap(points);
                       });
    return true;
}

void clearWarp(TimelineActions& actions, const juce::String& clipId)
{
    actions.engineClient().send("CLIP_SET_SOURCE_TEMPO", obj({{"clipId", clipId}, {"points", juce::Array<juce::var>{}}}));
}
} // namespace tempodrift

// ── the analyser ─────────────────────────────────────────────────────────────
TempoDriftAnalyser::TempoDriftAnalyser(EngineClient& client, ProjectMirror& projectMirror) : engine(client), mirror(projectMirror) {}

TempoDriftAnalyser::~TempoDriftAnalyser()
{
    alive->store(false);
    pool.removeAllJobs(true, 30000);
}

void TempoDriftAnalyser::analyse(const juce::String& itemId)
{
    const auto* item = mirror.model().findLibraryItem(itemId);
    if (item == nullptr || running.count(itemId) != 0) return;
    // A stem keeps its source's tempo: the source item's BPM is the hint.
    double bpm = item->bpm;
    if (bpm <= 0.0)
        if (const auto* source = mirror.model().findLibraryItem(item->sourceItemId)) bpm = source->bpm;
    if (bpm <= 0.0)
    {
        if (onNotify) onNotify("Analyse " + item->displayName() + "'s tempo first", true);
        return;
    }
    running.insert(itemId);
    const juce::File file(item->audioPath());
    const auto name = item->displayName();
    pool.addJob(
        [this, itemId, file, bpm, name, alive = alive]
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            const auto result = stemcel::analyseTempoDrift(file, formats, bpm, [alive] { return !alive->load(); });
            const auto stored = result.ok ? tempodrift::toVar(result) : juce::var();
            juce::MessageManager::callAsync(
                [this, itemId, name, stored, ok = result.ok, drift = result.driftPercent, main = result.mainBpm, alive]
                {
                    if (!alive->load()) return;
                    running.erase(itemId);
                    if (!ok)
                    {
                        if (onNotify) onNotify("Couldn't follow the beats of " + name, true);
                        mirror.sendChangeMessage();
                        return;
                    }
                    auto* payload = new juce::DynamicObject();
                    payload->setProperty("itemId", itemId);
                    payload->setProperty("tempoDrift", stored);
                    engine.send("LIBRARY_ITEM_SET_TEMPO_DRIFT", juce::var(payload));
                    log::info("tempo", name + ": main " + juce::String(main, 2) + " BPM, drifts " + juce::String(drift, 1) + "%");
                    mirror.refreshNow();
                });
        });
}

// ── the report ───────────────────────────────────────────────────────────────
TempoReport::TempoReport(ProjectMirror& projectMirror, TimelineActions& timelineActions, TempoDriftAnalyser& driftAnalyser,
                         const juce::String& item, const juce::String& clip)
    : mirror(projectMirror), actions(timelineActions), analyser(driftAnalyser), itemId(item), clipId(clip)
{
    summary.setFont(uiFont(13.0f));
    summary.setColour(juce::Label::textColourId, colours::text);
    summary.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(summary);
    autoWarpButton.setTooltip("Stretch the drifting stretches back to the clip's main tempo, so every beat lands on the grid");
    followButton.setTooltip("Make the project tempo follow this clip over its span; it plays as recorded and other warped clips follow");
    editButton.setTooltip("Shape the clip's warp by hand on the graph: click to add a point, drag it, double-click to remove it");
    clearButton.setTooltip("Back to the clip's one detected tempo");
    autoWarpButton.onClick = [this] { tempodrift::autoWarp(actions, mirror.model(), clipId); };
    followButton.onClick = [this] { tempodrift::followInProjectTempo(actions, mirror.model(), clipId); };
    clearButton.onClick = [this]
    {
        editing = false;
        tempodrift::clearWarp(actions, clipId);
    };
    editButton.onClick = [this]
    {
        if (!editing)
        {
            startEditing();
            return;
        }
        // Apply: the clip follows the project along the curve as drawn.
        juce::Array<juce::var> points;
        for (const auto& p : draft)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("sourceMs", p.timeMs);
            o->setProperty("bpm", p.bpm);
            points.add(juce::var(o));
        }
        auto& engine = actions.engineClient();
        engine.inUndoGroup("Warp clip tempo by hand",
                           [&]
                           {
                               auto* warp = new juce::DynamicObject();
                               warp->setProperty("clipId", clipId);
                               warp->setProperty("warpEnabled", true);
                               warp->setProperty("tempoRatio", juce::var());
                               engine.send("CLIP_SET_WARP", juce::var(warp));
                               auto* o = new juce::DynamicObject();
                               o->setProperty("clipId", clipId);
                               o->setProperty("points", points);
                               engine.send("CLIP_SET_SOURCE_TEMPO", juce::var(o));
                           });
        editing = false;
        refresh();
    };
    againButton.onClick = [this] { analyser.analyse(itemId); refresh(); };
    closeButton.onClick = [this]
    {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) window->exitModalState(0);
    };
    for (auto* b : {&autoWarpButton, &followButton, &editButton, &clearButton, &againButton, &closeButton}) addAndMakeVisible(b);
    mirror.addChangeListener(this);
    if (const auto* i = mirror.model().findLibraryItem(itemId); i != nullptr && !tempodrift::of(*i).valid()) analyser.analyse(itemId);
    refresh();
    setSize(860, 520);
}

TempoReport::~TempoReport()
{
    mirror.removeChangeListener(this);
}

void TempoReport::show(ProjectMirror& mirror, TimelineActions& actions, TempoDriftAnalyser& analyser, const juce::String& itemId,
                       const juce::String& clipId)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new TempoReport(mirror, actions, analyser, itemId, clipId));
    const auto* item = mirror.model().findLibraryItem(itemId);
    options.dialogTitle = "BPM Variability" + (item != nullptr ? ": " + item->displayName() : juce::String());
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    if (auto* window = options.launchAsync()) window->setResizeLimits(640, 420, 4000, 3000);
}

void TempoReport::changeListenerCallback(juce::ChangeBroadcaster*)
{
    refresh();
}

void TempoReport::refresh()
{
    const auto& m = mirror.model();
    const auto* item = m.findLibraryItem(itemId);
    drift = item != nullptr ? tempodrift::of(*item) : tempodrift::Drift{};
    durationMs = item != nullptr ? item->durationMs : 0.0;
    const auto* clip = m.findClip(clipId);
    juce::String text;
    if (analyser.isRunning(itemId))
        text = "Following the beats through the whole track...";
    else if (!drift.valid())
        text = "No tempo measured yet.";
    else
    {
        double low = drift.mainBpm, high = drift.mainBpm;
        for (const auto& p : drift.curve)
        {
            low = juce::jmin(low, p.bpm);
            high = juce::jmax(high, p.bpm);
        }
        text << "Main tempo " << juce::String(drift.mainBpm, 2) << " BPM. It drifts " << juce::String(drift.driftPercent, 1)
             << "% (" << juce::String(low, 1) << " to " << juce::String(high, 1) << " BPM). ";
        if (drift.sections.empty())
            text << "Nothing strays more than " << juce::String(stemcel::TempoDriftResult::kSectionThreshold * 100.0, 1)
                 << "% from it for 4 bars or more: steady enough for the grid as it is.";
        else
            text << juce::String(static_cast<int>(drift.sections.size())) << (drift.sections.size() == 1 ? " stretch strays" : " stretches stray")
                 << " (shaded): warp it back, or let the project tempo follow it.";
        if (clip != nullptr && !clip->sourceTempo.empty()) text << "\nThis clip is warped along its measured tempo.";
    }
    summary.setText(text, juce::dontSendNotification);
    const bool clipMode = clip != nullptr;
    for (auto* b : {&autoWarpButton, &followButton, &editButton, &clearButton})
    {
        b->setVisible(clipMode);
        b->setEnabled(drift.valid() || b == &editButton);
    }
    clearButton.setEnabled(clip != nullptr && !clip->sourceTempo.empty());
    editButton.setButtonText(editing ? "Apply Warp" : "Edit Warp by Hand");
    againButton.setEnabled(!analyser.isRunning(itemId));
    repaint();
}

void TempoReport::startEditing()
{
    const auto* clip = mirror.model().findClip(clipId);
    if (clip == nullptr) return;
    draft = clip->sourceTempo;
    if (draft.empty())
    {
        // Seeded from the measurement over the clip, or the clip's one tempo when there is none.
        for (const auto& p : drift.curve)
            if (p.timeMs >= clip->inMs && p.timeMs <= clip->inMs + clip->durationMs) draft.push_back(p);
        if (draft.size() < 2)
        {
            const double bpm = drift.valid() ? drift.mainBpm : juce::jmax(1.0, clip->tempoFollowBpm);
            draft = {{clip->inMs, bpm}, {clip->inMs + clip->durationMs, bpm}};
        }
    }
    editing = true;
    refresh();
}

juce::Rectangle<float> TempoReport::graph() const
{
    return getLocalBounds().reduced(16).withTrimmedTop(60).withTrimmedBottom(48).toFloat();
}

std::pair<double, double> TempoReport::bpmRange() const
{
    double low = drift.valid() ? drift.mainBpm : 120.0, high = low;
    for (const auto& p : drift.curve)
    {
        low = juce::jmin(low, p.bpm);
        high = juce::jmax(high, p.bpm);
    }
    for (const auto& p : draft)
    {
        low = juce::jmin(low, p.bpm);
        high = juce::jmax(high, p.bpm);
    }
    const double pad = juce::jmax(1.0, (high - low) * 0.15);
    return {low - pad, high + pad};
}

juce::Point<float> TempoReport::toGraph(double ms, double bpm) const
{
    const auto g = graph();
    const auto [low, high] = bpmRange();
    const double span = juce::jmax(1.0, durationMs);
    return {g.getX() + static_cast<float>(ms / span) * g.getWidth(),
            g.getBottom() - static_cast<float>((bpm - low) / (high - low)) * g.getHeight()};
}

std::pair<double, double> TempoReport::fromGraph(juce::Point<float> p) const
{
    const auto g = graph();
    const auto [low, high] = bpmRange();
    return {juce::jlimit(0.0, durationMs, static_cast<double>((p.x - g.getX()) / g.getWidth()) * durationMs),
            low + static_cast<double>((g.getBottom() - p.y) / g.getHeight()) * (high - low)};
}

void TempoReport::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    const auto area = graph();
    g.setColour(colours::zinc950);
    g.fillRect(area);
    g.setColour(colours::border);
    g.drawRect(area);
    if (durationMs <= 0.0) return;

    // Minutes along the bottom.
    g.setFont(uiFont(10.0f));
    const double step = durationMs > 600000.0 ? 120000.0 : durationMs > 120000.0 ? 30000.0 : 10000.0;
    for (double ms = 0.0; ms <= durationMs; ms += step)
    {
        const float x = toGraph(ms, 0.0).x;
        g.setColour(colours::grey700.withAlpha(0.5f));
        g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
        g.setColour(colours::textFaint);
        const auto s = static_cast<int>(ms / 1000.0);
        g.drawText(juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2), static_cast<int>(x) + 3,
                   static_cast<int>(area.getBottom()) + 2, 50, 14, juce::Justification::centredLeft);
    }
    // The clip's window of the file.
    if (const auto* clip = mirror.model().findClip(clipId))
    {
        const float x0 = toGraph(clip->inMs, 0.0).x, x1 = toGraph(clip->inMs + clip->durationMs, 0.0).x;
        g.setColour(colours::sky500.withAlpha(0.08f));
        g.fillRect(juce::Rectangle<float>(x0, area.getY(), x1 - x0, area.getHeight()));
    }
    if (!drift.valid()) return;
    // Stretches that stray, shaded.
    for (const auto& s : drift.sections)
    {
        const float x0 = toGraph(s.startSec * 1000.0, 0.0).x, x1 = toGraph(s.endSec * 1000.0, 0.0).x;
        g.setColour(colours::amber500.withAlpha(0.16f));
        g.fillRect(juce::Rectangle<float>(x0, area.getY(), x1 - x0, area.getHeight()));
    }
    // The main tempo, dashed, and the measured tempo.
    {
        const float y = toGraph(0.0, drift.mainBpm).y;
        juce::Path line;
        line.startNewSubPath(area.getX(), y);
        line.lineTo(area.getRight(), y);
        const float dash[] = {5.0f, 4.0f};
        juce::Path dashed;
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, line, dash, 2);
        g.setColour(colours::textDim);
        g.fillPath(dashed);
        g.drawText(juce::String(drift.mainBpm, 1) + " BPM", static_cast<int>(area.getX()) + 4, static_cast<int>(y) - 15, 90, 14,
                   juce::Justification::centredLeft);
    }
    juce::Path curve;
    for (size_t i = 0; i < drift.curve.size(); ++i)
    {
        const auto p = toGraph(drift.curve[i].timeMs, drift.curve[i].bpm);
        if (i == 0) curve.startNewSubPath(p);
        else curve.lineTo(p);
    }
    g.setColour(colours::sky400);
    g.strokePath(curve, juce::PathStrokeType(1.6f));
    // The clip's warp: as drawn by hand while editing, or as applied.
    const auto* clip = mirror.model().findClip(clipId);
    const auto& warp = editing ? draft : (clip != nullptr ? clip->sourceTempo : draft);
    if (!warp.empty())
    {
        juce::Path w;
        for (size_t i = 0; i < warp.size(); ++i)
        {
            const auto p = toGraph(warp[i].timeMs, warp[i].bpm);
            if (i == 0) w.startNewSubPath(p);
            else w.lineTo(p);
        }
        g.setColour(colours::amber400);
        g.strokePath(w, juce::PathStrokeType(editing ? 2.0f : 1.2f));
        if (editing)
            for (const auto& p : warp)
            {
                const auto c = toGraph(p.timeMs, p.bpm);
                g.fillEllipse(c.x - 4.0f, c.y - 4.0f, 8.0f, 8.0f);
            }
    }
}

void TempoReport::resized()
{
    auto area = getLocalBounds().reduced(16);
    summary.setBounds(area.removeFromTop(52));
    auto buttons = area.removeFromBottom(30);
    closeButton.setBounds(buttons.removeFromRight(80));
    buttons.removeFromRight(6);
    againButton.setBounds(buttons.removeFromRight(120));
    for (auto* b : {&autoWarpButton, &followButton, &editButton, &clearButton})
    {
        b->setBounds(buttons.removeFromLeft(juce::jmin(180, buttons.getWidth())));
        buttons.removeFromLeft(6);
    }
}

int TempoReport::handleAt(juce::Point<float> p) const
{
    for (size_t i = 0; i < draft.size(); ++i)
        if (toGraph(draft[i].timeMs, draft[i].bpm).getDistanceFrom(p) < 7.0f) return static_cast<int>(i);
    return -1;
}

void TempoReport::mouseDown(const juce::MouseEvent& e)
{
    if (!editing || !graph().contains(e.position)) return;
    dragging = handleAt(e.position);
    if (dragging >= 0) return;
    const auto [ms, bpm] = fromGraph(e.position);
    draft.push_back({ms, bpm});
    std::sort(draft.begin(), draft.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.timeMs < b.timeMs; });
    dragging = handleAt(e.position);
    repaint();
}

void TempoReport::mouseDrag(const juce::MouseEvent& e)
{
    if (!editing || dragging < 0 || dragging >= static_cast<int>(draft.size())) return;
    auto [ms, bpm] = fromGraph(e.position);
    // Between its neighbours, so dragging never reorders the points.
    const auto i = static_cast<size_t>(dragging);
    if (i > 0) ms = juce::jmax(ms, draft[i - 1].timeMs + 1.0);
    if (i + 1 < draft.size()) ms = juce::jmin(ms, draft[i + 1].timeMs - 1.0);
    draft[i] = {ms, juce::jlimit(20.0, 400.0, bpm)};
    repaint();
}

void TempoReport::mouseUp(const juce::MouseEvent&)
{
    dragging = -1;
}

void TempoReport::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (!editing) return;
    if (const int i = handleAt(e.position); i >= 0 && draft.size() > 2)
    {
        draft.erase(draft.begin() + i);
        repaint();
    }
}

} // namespace stemcel::native
