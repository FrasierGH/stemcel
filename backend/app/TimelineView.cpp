#include "TimelineView.h"

#include "LibraryPanel.h"

#include "AutoMix.h"
#include "BeatStutter.h"
#include "Automation.h"
#include "MixTransitions.h"
#include "TransitionEditor.h"
#include "Theme.h"

#include <cmath>
#include <limits>
#include <utility>

namespace stemcel::native
{
namespace
{
using VS = TimelineViewState;
constexpr float kEdgeGrabPx = 6.0f;
constexpr float kClipHeaderPx = 16.0f;
constexpr float kClipPadding = 2.0f;
constexpr int kDragThresholdPx = 3;
constexpr double kBeatsPerBar = 4.0;
constexpr double kTailPaddingMs = 30000.0;
constexpr int kViewPersistDelayMs = 400;

bool anySoloed(const ProjectModel& m)
{
    for (const auto& t : m.tracks)
        if (t.soloed) return true;
    return false;
}

// Clip header status badges, as the Electron timeline draws them (clipHeaderRenderer.ts):
// the same order, sizes and colours, right of the name in a strip that hugs both.
constexpr float kBadgeH = 14.0f;
constexpr float kBadgeRadius = 5.0f;
constexpr float kBadgeGap = 4.0f;
constexpr float kNameBadgeGap = 6.0f;
constexpr float kHeaderPadX = 4.0f;

enum class Badge
{
    link,
    lock,
    pitch,
    reversed,
    repeat,
    brake,
    spin,
    warpPending,
    warp
};

float badgeWidth(Badge b)
{
    switch (b)
    {
    case Badge::link: return 18.0f;
    case Badge::lock: return 14.0f;
    case Badge::pitch: return 18.0f;
    case Badge::reversed: return 30.0f;
    case Badge::repeat: return 48.0f;
    case Badge::brake: return 44.0f;
    case Badge::spin: return 38.0f;
    case Badge::warpPending:
    case Badge::warp: return 40.0f;
    }
    return 0.0f;
}

void drawBadge(juce::Graphics& g, Badge b, juce::Rectangle<float> r, float alpha)
{
    struct Style
    {
        juce::uint32 fill;
        float fillAlpha;
        juce::uint32 stroke;
        const char* text;
        juce::uint32 textColour;
    };
    Style st{0xff18181b, 0.95f, 0xffffffff, nullptr, 0};
    switch (b)
    {
    case Badge::link: st = {0xff09090b, 0.85f, 0xffffffff, nullptr, 0}; break;
    case Badge::lock:
    case Badge::pitch: break;
    case Badge::reversed: st = {0xff134e4a, 0.95f, 0xffffffff, "REV", 0xff5eead4}; break;
    case Badge::repeat: st = {0xff0c4a6e, 0.95f, 0xff7dd3fc, "REPEAT", 0xffe0f2fe}; break;
    case Badge::brake: st = {0xff3f1d1d, 0.95f, 0xffffffff, "BRAKE", 0xfffca5a5}; break;
    case Badge::spin: st = {0xff2e1065, 0.95f, 0xffffffff, "SPIN", 0xffc4b5fd}; break;
    case Badge::warpPending: st = {0xff0f172a, 0.95f, 0xffffffff, nullptr, 0}; break;
    case Badge::warp: st = {0xff0f172a, 0.95f, 0xffffffff, "WARP", 0xfffacc15}; break;
    }
    g.setColour(juce::Colour(st.fill).withAlpha(st.fillAlpha * alpha));
    g.fillRoundedRectangle(r, kBadgeRadius);
    g.setColour(juce::Colour(st.stroke).withAlpha(0.95f * alpha));
    g.drawRoundedRectangle(r.reduced(0.5f), kBadgeRadius, 1.0f);

    const auto c = r.getCentre();
    const auto white = juce::Colours::white.withAlpha(alpha);
    if (st.text != nullptr)
    {
        g.setColour(juce::Colour(st.textColour).withAlpha(alpha));
        g.setFont(uiFont(9.0f, true));
        g.drawText(st.text, r, juce::Justification::centred, false);
        return;
    }
    switch (b)
    {
    case Badge::link:
        // Two linked rings: a saved clip shares its edits across its placements.
        g.setColour(white);
        g.drawEllipse(c.x - 2.5f - 2.3f, c.y - 2.3f, 4.6f, 4.6f, 1.5f);
        g.drawEllipse(c.x + 2.5f - 2.3f, c.y - 2.3f, 4.6f, 4.6f, 1.5f);
        break;
    case Badge::lock:
    {
        g.setColour(white);
        g.fillRoundedRectangle(c.x - 3.0f, c.y - 1.0f, 6.0f, 5.0f, 1.0f);
        juce::Path shackle;
        shackle.addCentredArc(c.x, c.y - 1.0f, 2.2f, 2.2f, 0.0f, -juce::MathConstants<float>::halfPi,
                              juce::MathConstants<float>::halfPi, true);
        g.strokePath(shackle, juce::PathStrokeType(1.2f));
        break;
    }
    case Badge::pitch:
    {
        // An eighth note, drawn so it never depends on the font having the glyph.
        g.setColour(white);
        g.fillEllipse(c.x - 3.2f, c.y + 0.6f, 4.2f, 3.2f);
        g.fillRect(c.x + 0.2f, c.y - 4.2f, 1.1f, 6.4f);
        juce::Path flag;
        flag.startNewSubPath(c.x + 1.3f, c.y - 4.2f);
        flag.quadraticTo(c.x + 3.8f, c.y - 3.0f, c.x + 2.8f, c.y - 0.6f);
        g.strokePath(flag, juce::PathStrokeType(1.1f));
        break;
    }
    case Badge::warpPending:
    {
        // Eight dots chasing round: the clip waits for its tempo analysis.
        const int phase = static_cast<int>(juce::Time::getMillisecondCounter() / 125) % 8;
        for (int i = 0; i < 8; ++i)
        {
            const float angle = static_cast<float>(i - phase) / 8.0f * juce::MathConstants<float>::twoPi;
            g.setColour(juce::Colours::white.withAlpha((0.25f + (static_cast<float>(i + 1) / 8.0f) * 0.65f) * alpha));
            g.fillEllipse(c.x + std::cos(angle) * 4.2f - 1.1f, c.y + std::sin(angle) * 4.2f - 1.1f, 2.2f, 2.2f);
        }
        break;
    }
    default: break;
    }
}
} // namespace

std::vector<TrackRow> layoutRows(const ProjectModel& model)
{
    std::vector<TrackRow> out;
    int y = 0;
    for (const auto& t : model.tracks)
    {
        const int h = t.heightPx > 0.0
                          ? juce::jlimit(VS::kMinTrackHeight, VS::kMaxTrackHeight, static_cast<int>(t.heightPx))
                          : VS::kDefaultTrackHeight;
        TrackRow row{t.id, y, h, h, {}};
        for (const auto& lane : automation::lanesOf(t))
        {
            row.lanes.push_back({lane.paramId, row.height, lane.heightPx});
            row.height += lane.heightPx;
        }
        out.push_back(row);
        y += row.height + VS::kTrackGap;
    }
    // The master's row, for its automation lanes: a short strip with its lanes beneath.
    if (model.master.id.isNotEmpty())
    {
        TrackRow row{model.master.id, y, kMasterRowHeight, kMasterRowHeight, {}};
        for (const auto& lane : automation::lanesOf(model.master))
        {
            row.lanes.push_back({lane.paramId, row.height, lane.heightPx});
            row.height += lane.heightPx;
        }
        out.push_back(row);
    }
    return out;
}

int contentHeight(const std::vector<TrackRow>& rows)
{
    return rows.empty() ? 0 : rows.back().top + rows.back().height;
}

TimelineView::TimelineView(ProjectMirror& projectMirror, TimelineViewState& viewState, LiveState& liveState,
                           TimelineActions& timelineActions, WaveformStore& waveformStore)
    : mirror(projectMirror), view(viewState), live(liveState), actions(timelineActions), waveforms(waveformStore)
{
    setWantsKeyboardFocus(true);
    setOpaque(true);
    mirror.addChangeListener(this);
    view.addChangeListener(this);
    waveforms.addChangeListener(this);
    rows = layoutRows(model());
    clipNameEditor.setFont(uiFont(11.0f, true));
    clipNameEditor.setIndents(4, 1);
    clipNameEditor.setColour(juce::TextEditor::backgroundColourId, colours::zinc950);
    clipNameEditor.setColour(juce::TextEditor::outlineColourId, colours::sky500);
    clipNameEditor.setColour(juce::TextEditor::focusedOutlineColourId, colours::sky500);
    clipNameEditor.onReturnKey = [this] { commitClipRename(true); };
    clipNameEditor.onEscapeKey = [this] { commitClipRename(false); };
    clipNameEditor.onFocusLost = [this] { commitClipRename(true); };
    addChildComponent(clipNameEditor);
    for (auto* bar : {&hScroll, &vScroll})
    {
        bar->setAutoHide(true);
        bar->addListener(this);
        addAndMakeVisible(*bar);
    }
    if (model().pxPerSecond > 0.0) view.pxPerSecond = model().pxPerSecond;
    view.scrollXPx = model().scrollX;
    startTimerHz(30);
}

TimelineView::~TimelineView()
{
    mirror.removeChangeListener(this);
    view.removeChangeListener(this);
    waveforms.removeChangeListener(this);
    hScroll.removeListener(this);
    vScroll.removeListener(this);
}

void TimelineView::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &mirror) rows = layoutRows(model());
    if (renamingClipId.isNotEmpty()) placeClipNameEditor();
    syncScrollBars();
    repaint();
}

void TimelineView::resized()
{
    syncScrollBars();
}

void TimelineView::syncScrollBars()
{
    constexpr int kBar = 10;
    const int top = view.headerHeight();
    hScroll.setBounds(0, getHeight() - kBar, getWidth() - kBar, kBar);
    vScroll.setBounds(getWidth() - kBar, top, kBar, juce::jmax(0, getHeight() - top - kBar));
    // The same limits clampScroll() keeps the view within.
    const double contentPx = contentLengthMs() / 1000.0 * view.pxPerSecond;
    hScroll.setRangeLimits(0.0, juce::jmax(contentPx, static_cast<double>(getWidth())), juce::dontSendNotification);
    hScroll.setCurrentRange(view.scrollXPx, getWidth(), juce::dontSendNotification);
    const int visibleY = getHeight() - top;
    vScroll.setRangeLimits(0.0, juce::jmax(contentHeight(rows) + 40, visibleY), juce::dontSendNotification);
    vScroll.setCurrentRange(view.scrollY, visibleY, juce::dontSendNotification);
}

void TimelineView::scrollBarMoved(juce::ScrollBar* bar, double newRangeStart)
{
    if (bar == &hScroll)
        view.scrollXPx = newRangeStart;
    else
        view.scrollY = newRangeStart;
    clampScroll();
    view.changed();
    persistView();
}

void TimelineView::startClipRename(const juce::String& clipId)
{
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr) return;
    juce::String name = clip->name.trim();
    if (name.isEmpty())
        if (const auto* item = model().findLibraryItem(clip->libraryItemId)) name = item->displayName();
    renamingClipId = clipId;
    clipNameEditor.setText(name, false);
    placeClipNameEditor();
    clipNameEditor.setVisible(true);
    clipNameEditor.grabKeyboardFocus();
    clipNameEditor.selectAll();
}

void TimelineView::placeClipNameEditor()
{
    const auto* clip = model().findClip(renamingClipId);
    if (clip == nullptr)
    {
        commitClipRename(false);
        return;
    }
    for (const auto& row : rows)
        if (row.trackId == clip->trackId)
        {
            // At least wide enough to type in, as the renderer's minimum.
            const auto header = clipBounds(*clip, row).withHeight(kClipHeaderPx + 2.0f);
            clipNameEditor.setBounds(header.withWidth(juce::jmax(header.getWidth(), 120.0f)).toNearestInt());
            return;
        }
}

void TimelineView::commitClipRename(bool keep)
{
    if (renamingClipId.isEmpty()) return;
    const auto id = std::exchange(renamingClipId, {});
    clipNameEditor.setVisible(false);
    const auto* clip = model().findClip(id);
    const auto name = clipNameEditor.getText().trim();
    if (!keep || clip == nullptr || name.isEmpty() || name == clip->name.trim()) return;
    actions.renameClip(id, name);
}

double TimelineView::contentLengthMs() const
{
    return juce::jmax(model().projectLengthMs, model().contentEndMs()) + kTailPaddingMs;
}

void TimelineView::clampScroll()
{
    const double contentPx = contentLengthMs() / 1000.0 * view.pxPerSecond;
    view.scrollXPx = juce::jlimit(0.0, juce::jmax(0.0, contentPx - getWidth()), view.scrollXPx);
    const double maxY = juce::jmax(0, contentHeight(rows) - (getHeight() - view.headerHeight()) + 40);
    view.scrollY = juce::jlimit(0.0, juce::jmax(0.0, maxY), view.scrollY);
}

void TimelineView::persistView()
{
    viewDirty = true;
    lastViewChangeMs = juce::Time::getMillisecondCounter();
}

void TimelineView::timerCallback()
{
    // The pending-warp spinner turns while any clip on screen waits on its tempo analysis.
    if (std::exchange(warpSpinnerVisible, false)) repaint();

    // Persist zoom/scroll once the user stops changing them, as the renderer does.
    if (viewDirty && juce::Time::getMillisecondCounter() - lastViewChangeMs > kViewPersistDelayMs)
    {
        viewDirty = false;
        actions.setView(view.pxPerSecond, view.scrollXPx, view.selectedTrackId);
    }

    const double x = view.msToX(live.positionMs);
    if (live.playing && view.followPlayback && drag == DragKind::none
        && (x > getWidth() - 40 || x < 0))
    {
        // Page-follow: jump so the playhead sits near the left edge.
        view.scrollXPx = juce::jmax(0.0, live.positionMs / 1000.0 * view.pxPerSecond - 60.0);
        clampScroll();
        view.changed();
        return;
    }
    if (std::abs(x - lastDrawnPlayheadX) >= 0.5)
    {
        const auto oldX = static_cast<int>(lastDrawnPlayheadX);
        repaint(oldX - 2, 0, 5, getHeight());
        repaint(static_cast<int>(x) - 2, 0, 5, getHeight());
    }
}

int TimelineView::trackRowIndexAtY(int y) const
{
    // The master's row takes no clips.
    const int index = rowIndexAtY(y);
    return index >= 0 && rows[static_cast<size_t>(index)].trackId == kMasterTrackId ? -1 : index;
}

int TimelineView::rowIndexAtY(int y) const
{
    const double contentY = y - view.headerHeight() + view.scrollY;
    for (size_t i = 0; i < rows.size(); ++i)
        if (contentY >= rows[i].top && contentY < rows[i].top + rows[i].height + VS::kTrackGap)
            return static_cast<int>(i);
    return -1;
}

juce::Rectangle<float> TimelineView::clipBounds(const ClipModel& clip, const TrackRow& row) const
{
    const auto x0 = static_cast<float>(view.msToX(clip.offsetMs));
    const auto x1 = static_cast<float>(view.msToX(clip.endMs()));
    const auto y = static_cast<float>(row.top + view.headerHeight() - view.scrollY) + kClipPadding;
    return {x0, y, juce::jmax(1.0f, x1 - x0), static_cast<float>(row.clipHeight) - kClipPadding * 2.0f};
}

std::optional<TimelineView::ClipHit> TimelineView::clipAt(juce::Point<int> p) const
{
    const int index = rowIndexAtY(p.y);
    if (index < 0) return std::nullopt;
    const auto& row = rows[static_cast<size_t>(index)];
    const auto* track = model().findTrack(row.trackId);
    if (track == nullptr) return std::nullopt;
    // Topmost (last drawn) first.
    for (auto it = track->clips.rbegin(); it != track->clips.rend(); ++it)
    {
        const auto r = clipBounds(*it, row);
        if (!r.contains(p.toFloat())) continue; // lanes below the clips are not clip area
        ClipHit hit{&*it, r, DragKind::move};
        if (!it->locked && r.getWidth() > kEdgeGrabPx * 3.0f)
        {
            if (static_cast<float>(p.x) - r.getX() < kEdgeGrabPx) hit.edge = DragKind::trimStart;
            else if (r.getRight() - static_cast<float>(p.x) < kEdgeGrabPx) hit.edge = DragKind::trimEnd;
        }
        return hit;
    }
    return std::nullopt;
}

const MarkerModel* TimelineView::markerAt(juce::Point<int> p) const
{
    if (p.y >= VS::kRulerHeight) return nullptr;
    for (const auto& m : model().markers)
        if (std::abs(view.msToX(m.positionMs) - p.x) <= 5.0) return &m;
    return nullptr;
}

// ── painting ────────────────────────────────────────────────────────────────
void TimelineView::paintGrid(juce::Graphics& g, juce::Rectangle<int> area)
{
    // Beats along the tempo map (evenly spaced at the constant tempo without one).
    const auto curve = model().tempoCurve();
    const double startMs = view.xToMs(0);
    const double endMs = view.xToMs(getWidth());
    const double beatPx = 60.0 / curve.bpmAt(startMs) * view.pxPerSecond;
    const auto grid = actions.snapGrid();
    int subdivisions = grid == "half" ? 2 : (grid == "quarter" || grid == "free") ? 4 : 1;
    // Thin out lines that would crowd together at low zoom.
    while (subdivisions > 1 && beatPx / subdivisions < 8.0) subdivisions /= 2;
    const bool drawBeats = beatPx >= 6.0;

    const double stepBeats = 1.0 / subdivisions;
    const auto firstStep = static_cast<juce::int64>(std::floor(curve.beatAt(startMs) / stepBeats));
    for (auto i = firstStep;; ++i)
    {
        const double ms = curve.msAtBeat(static_cast<double>(i) * stepBeats);
        if (ms > endMs) break;
        const auto x = static_cast<float>(view.msToX(ms));
        const bool isBeat = i % subdivisions == 0;
        const bool isBar = isBeat && (i / subdivisions) % static_cast<juce::int64>(kBeatsPerBar) == 0;
        if (!isBeat && !drawBeats) continue;
        if (isBeat && !isBar && !drawBeats) continue;
        g.setColour(isBar ? colours::grey500.withAlpha(0.7f)
                          : isBeat ? colours::grey600.withAlpha(0.7f) : colours::grey700.withAlpha(0.6f));
        g.drawVerticalLine(static_cast<int>(x), static_cast<float>(area.getY()), static_cast<float>(area.getBottom()));
    }
}

void TimelineView::paintRuler(juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour(colours::grey900);
    g.fillRect(area);
    // Bars along the tempo map.
    const auto curve = model().tempoCurve();
    const double startMs = view.xToMs(0);
    const double endMs = view.xToMs(getWidth());
    const double barPx = 60.0 * kBeatsPerBar / curve.bpmAt(startMs) * view.pxPerSecond;
    int labelEvery = 1;
    while (barPx * labelEvery < 40.0) labelEvery *= 2;

    g.setFont(uiFont(11.0f));
    for (auto bar = static_cast<juce::int64>(std::floor(curve.beatAt(startMs) / kBeatsPerBar));; ++bar)
    {
        const double barMs = curve.msAtBeat(static_cast<double>(bar) * kBeatsPerBar);
        if (barMs > endMs) break;
        if (bar < 0) continue;
        const auto x = static_cast<int>(view.msToX(barMs));
        g.setColour(colours::grey500);
        g.drawVerticalLine(x, static_cast<float>(area.getBottom() - 10), static_cast<float>(area.getBottom()));
        if (bar % labelEvery == 0)
        {
            g.setColour(colours::zinc400);
            g.drawText(juce::String(bar + 1), x + 3, area.getY() + 2, 40, 14, juce::Justification::centredLeft);
        }
    }

    // Range selection band.
    const auto selection = rangePreview.has_value() ? rangePreview : model().timelineSelection;
    if (selection.has_value())
    {
        const auto x0 = static_cast<float>(view.msToX(selection->first));
        const auto x1 = static_cast<float>(view.msToX(selection->second));
        g.setColour((model().loopSelection ? colours::amber400 : colours::sky400).withAlpha(0.35f));
        g.fillRect(juce::Rectangle<float>(x0, static_cast<float>(area.getBottom() - 8), x1 - x0, 8.0f));
    }

    // Markers.
    for (const auto& m : model().markers)
    {
        const auto x = static_cast<float>(view.msToX(m.positionMs));
        juce::Path flag;
        flag.addTriangle(x - 5.0f, static_cast<float>(area.getY()), x + 5.0f, static_cast<float>(area.getY()), x,
                         static_cast<float>(area.getY()) + 9.0f);
        g.setColour(colours::emerald500);
        g.fillPath(flag);
    }
    g.setColour(colours::zinc600);
    g.drawHorizontalLine(area.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
}

void TimelineView::paintClip(juce::Graphics& g, const ClipModel& clip, juce::Rectangle<float> r, bool silenced,
                             int trackColor, float alpha)
{
    const auto palette = paletteEntry(clip.colorIndex >= 0 ? clip.colorIndex : trackColor, silenced);
    const bool selected = view.selectedClipIds.count(clip.id) != 0;
    g.setColour(palette.fill.withAlpha(alpha));
    g.fillRoundedRectangle(r, 3.0f);

    const auto header = r.withHeight(kClipHeaderPx);

    // Waveform of the clip's source window.
    if (const auto* item = model().findLibraryItem(clip.libraryItemId); item != nullptr && !clip.unresolved)
    {
        if (const auto peaks = waveforms.get(item->audioPath()))
        {
            const auto body = r.withTrimmedTop(kClipHeaderPx + 1.0f).reduced(0.0f, 2.0f);
            const auto visible = body.getIntersection(getLocalBounds().toFloat());
            if (!visible.isEmpty() && body.getWidth() > 0.0f)
            {
                const double msPerPx = clip.durationMs / body.getWidth();
                const double fromMs = clip.inMs + (visible.getX() - body.getX()) * msPerPx;
                const auto colour = palette.wave.withAlpha(0.9f * alpha);
                // A clip following the tempo map runs through its source faster where the tempo
                // is higher, so it is drawn in narrow strips, each showing the source the curve
                // puts under it (evenly spread otherwise).
                const auto curve = clip.tempoFollowBpm > 0.0 && !model().tempoMap.empty() ? std::optional<TempoCurve>(model().tempoCurve())
                                                                                        : std::nullopt;
                const auto drawWave = [&](juce::Rectangle<float> area, int lane)
                {
                    if (!curve.has_value())
                    {
                        drawPeaks(g, *peaks, area, fromMs, area.getWidth() * msPerPx, clip.reversed, colour, lane);
                        return;
                    }
                    const double bodyStartMs = view.xToMs(body.getX());
                    constexpr float kStrip = 6.0f;
                    for (float x = area.getX(); x < area.getRight(); x += kStrip)
                    {
                        const float w = juce::jmin(kStrip, area.getRight() - x);
                        const double s0 = curve->sourceMsBetween(bodyStartMs, view.xToMs(x), clip.tempoFollowBpm);
                        const double s1 = curve->sourceMsBetween(bodyStartMs, view.xToMs(x + w), clip.tempoFollowBpm);
                        drawPeaks(g, *peaks, {x, area.getY(), w, area.getHeight()}, clip.inMs + s0, s1 - s0, clip.reversed,
                                  colour, lane);
                    }
                };
                if (view.stereoWaveforms && peaks->laneCount >= 3)
                {
                    const auto top = visible.withHeight(visible.getHeight() / 2.0f);
                    drawWave(top.reduced(0.0f, 1.0f), 1);
                    drawWave(top.withY(top.getBottom()).reduced(0.0f, 1.0f), 2);
                }
                else
                {
                    drawWave(visible, 0);
                }
            }
        }
    }

    // The clip whose envelope is being edited and has none yet: its unity line, to click on.
    if (view.envelopeClipId == clip.id && clip.envelope.size() < 2)
    {
        const auto body = r.withTrimmedTop(kClipHeaderPx);
        const float y = body.getBottom() - body.getHeight() / 1.5f;
        g.setColour(colours::amber400.withAlpha(0.8f));
        const float dashes[] = {4.0f, 3.0f};
        g.drawDashedLine({body.getX(), y, body.getRight(), y}, dashes, 2, 1.5f);
    }
    // Volume envelope, drawn over the body.
    if (clip.envelope.size() >= 2)
    {
        juce::Path env;
        const auto body = r.withTrimmedTop(kClipHeaderPx);
        for (size_t i = 0; i < clip.envelope.size(); ++i)
        {
            const auto& p = clip.envelope[i];
            const float x = body.getX() + static_cast<float>(p.timeMs / juce::jmax(1.0, clip.effectiveDurationMs))
                                              * body.getWidth();
            const float y = body.getBottom() - static_cast<float>(juce::jlimit(0.0, 1.5, p.gain) / 1.5)
                                                   * body.getHeight();
            if (i == 0) env.startNewSubPath(x, y);
            else env.lineTo(x, y);
        }
        g.setColour(colours::amber400.withAlpha(0.8f * alpha));
        g.strokePath(env, juce::PathStrokeType(1.5f));
        if (view.envelopeClipId == clip.id)
            for (const auto& p : clip.envelope)
            {
                const float x = body.getX() + static_cast<float>(p.timeMs / juce::jmax(1.0, clip.effectiveDurationMs)) * body.getWidth();
                const float y = body.getBottom() - static_cast<float>(juce::jlimit(0.0, 1.5, p.gain) / 1.5) * body.getHeight();
                g.setColour(colours::zinc950);
                g.fillEllipse(x - 4.0f, y - 4.0f, 8.0f, 8.0f);
                g.setColour(colours::amber400);
                g.drawEllipse(x - 4.0f, y - 4.0f, 8.0f, 8.0f, 1.5f);
            }
    }

    g.setColour(selected ? colours::zinc100 : palette.border.withAlpha(alpha));
    g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, selected ? 2.0f : 1.0f);

    paintClipHeader(g, clip, header, palette.border, alpha);
}

std::vector<int> TimelineView::clipHeaderBadges(const ClipModel& clip) const
{
    std::vector<int> out;
    const auto add = [&out](Badge b) { out.push_back(static_cast<int>(b)); };
    // Only a saved clip shares state across its placements, so only it earns the link badge.
    if (const auto* item = model().findLibraryItem(clip.libraryItemId); item != nullptr && item->kind == "clip")
        add(Badge::link);
    if (clip.locked) add(Badge::lock);
    if (clip.semitones != 0.0 || clip.cents != 0.0) add(Badge::pitch);
    if (clip.reversed) add(Badge::reversed);
    if (ownsBeatRepeat(clip)) add(Badge::repeat);
    if (clip.brake) add(Badge::brake);
    if (clip.backspin) add(Badge::spin);
    if (isWarpPending(clip)) add(Badge::warpPending);
    else if (clip.effectiveWarpActive) add(Badge::warp);
    return out;
}

bool TimelineView::isWarpPending(const ClipModel& clip) const
{
    // lib/warp.ts isWarpPending: auto-warp waiting on analysis, or warp on with no tempo to stretch to.
    if (clip.pendingAutoWarp) return true;
    if (!clip.warpEnabled) return false;
    if (static_cast<double>(clip.raw.getProperty("tempoRatio", 0.0)) > 0.0) return false;
    double sourceBpm = 0.0;
    const auto* cursor = model().findLibraryItem(clip.libraryItemId);
    for (int hops = 0; cursor != nullptr && hops < 8 && sourceBpm <= 0.0; ++hops)
    {
        sourceBpm = cursor->bpm;
        cursor = cursor->sourceItemId.isNotEmpty() ? model().findLibraryItem(cursor->sourceItemId) : nullptr;
    }
    return sourceBpm <= 0.0 || model().bpm <= 0.0;
}

bool TimelineView::ownsBeatRepeat(const ClipModel& clip) const
{
    // A region belongs to the one clip on its track it overlaps most (beatRepeatAttribution.ts):
    // it is created from that clip but stored in beats, and neighbours often touch it by a few ms.
    const auto* track = model().findTrack(clip.trackId);
    if (track == nullptr || model().bpm <= 0.0) return false;
    const auto* regions = track->raw.getProperty("beatRepeats", {}).getArray();
    if (regions == nullptr || regions->isEmpty()) return false;
    const auto curve = model().tempoCurve();
    const auto toBeat = [&curve](double ms) { return curve.beatAt(ms); };
    for (const auto& r : *regions)
    {
        const double start = static_cast<double>(r.getProperty("startBeat", 0.0));
        const double end = start + static_cast<double>(r.getProperty("lengthBeats", 0.0));
        const ClipModel* owner = nullptr;
        double widest = 0.0;
        for (const auto& c : track->clips)
        {
            const double overlap = juce::jmin(end, toBeat(c.endMs())) - juce::jmax(start, toBeat(c.offsetMs));
            // Strictly greater, so an exact tie keeps the earlier clip in track order.
            if (overlap > widest)
            {
                widest = overlap;
                owner = &c;
            }
        }
        if (owner != nullptr && owner->id == clip.id) return true;
    }
    return false;
}

void TimelineView::paintClipHeader(juce::Graphics& g, const ClipModel& clip, juce::Rectangle<float> header,
                                   juce::Colour border, float alpha)
{
    if (header.getWidth() < 20.0f) return;
    juce::String name = clip.name.trim();
    if (name.isEmpty())
        if (const auto* item = model().findLibraryItem(clip.libraryItemId)) name = item->displayName();
    if (clip.unresolved) name = "! " + name;

    const auto badges = clipHeaderBadges(clip);
    float badgesW = 0.0f;
    for (const int b : badges) badgesW += badgeWidth(static_cast<Badge>(b));
    if (!badges.empty()) badgesW += kNameBadgeGap + static_cast<float>(badges.size() - 1) * kBadgeGap;

    // The strip hugs its label and badges, as in the Electron timeline.
    const auto font = uiFont(11.0f, true);
    const float maxTextW = juce::jmax(0.0f, header.getWidth() - kHeaderPadX * 2.0f - badgesW);
    const float textW = juce::jmin(maxTextW, juce::GlyphArrangement::getStringWidth(font, name));
    const float stripW = juce::jmin(header.getWidth(), std::ceil(textW) + kHeaderPadX * 2.0f + badgesW);
    g.setColour(border.withAlpha(0.95f * alpha));
    g.fillRoundedRectangle(header.withWidth(stripW), 3.0f);

    if (maxTextW > 6.0f)
    {
        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.setFont(font);
        g.drawText(name, header.withX(header.getX() + kHeaderPadX).withWidth(maxTextW), juce::Justification::centredLeft, true);
    }

    float right = header.getX() + stripW - kHeaderPadX;
    for (const int b : badges)
    {
        const float w = badgeWidth(static_cast<Badge>(b));
        drawBadge(g, static_cast<Badge>(b), {right - w, header.getCentreY() - kBadgeH / 2.0f, w, kBadgeH}, alpha);
        right -= w + kBadgeGap;
        if (static_cast<Badge>(b) == Badge::warpPending) warpSpinnerVisible = true;
    }
}

void TimelineView::paint(juce::Graphics& g)
{
    g.fillAll(colours::grey950);
    const auto tracksArea = getLocalBounds().withTrimmedTop(view.headerHeight());
    const bool soloing = anySoloed(model());

    // Rows.
    for (const auto& row : rows)
    {
        const int y = row.top + view.headerHeight() - static_cast<int>(view.scrollY);
        if (y > getHeight() || y + row.height < view.headerHeight()) continue;
        const auto* track = model().findTrack(row.trackId);
        g.setColour(track != nullptr && track->id == view.selectedTrackId ? colours::grey800
                                                                          : colours::grey900);
        g.fillRect(0, y, getWidth(), row.height);
    }
    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(tracksArea);
        paintGrid(g, tracksArea);

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            const auto* track = model().findTrack(row.trackId);
            if (track == nullptr) continue;
            const bool silenced = track->muted || (soloing && !track->soloed);
            const int trackColor = track->colorIndex >= 0 ? track->colorIndex : static_cast<int>(i);

            for (const auto& c : track->clips)
            {
                auto r = clipBounds(c, row);
                if (r.getRight() < 0.0f || r.getX() > static_cast<float>(getWidth())) continue;
                const bool dragging = drag != DragKind::none && drag != DragKind::seek && drag != DragKind::range
                                      && drag != DragKind::marker && c.id == dragClipId;
                paintClip(g, c, r, silenced, trackColor, dragging ? 0.35f : 1.0f);
            }

            // Crossfades: the overlap between each transition's two clips.
            for (const auto& t : track->transitions)
            {
                const auto* left = model().findClip(t.leftClipId);
                const auto* right = model().findClip(t.rightClipId);
                if (left == nullptr || right == nullptr) continue;
                const auto x0 = static_cast<float>(view.msToX(right->offsetMs));
                const auto x1 = static_cast<float>(view.msToX(left->endMs()));
                if (x1 <= x0) continue;
                const auto band = clipBounds(*left, row).withX(x0).withWidth(x1 - x0);
                g.setColour(colours::sky400.withAlpha(0.18f));
                g.fillRect(band);
                g.setColour(juce::Colour(0xff7dd3fc).withAlpha(0.9f));
                g.drawLine(x0, band.getBottom(), x1, band.getY(), 1.5f);
                g.drawLine(x0, band.getY(), x1, band.getBottom(), 1.5f);
            }
            paintLanes(g, row, *track);
        }

        // Drag ghost for move / trim.
        if (drag == DragKind::move || drag == DragKind::trimStart || drag == DragKind::trimEnd)
        {
            if (const auto* c = model().findClip(dragClipId))
            {
                const auto trackId = drag == DragKind::move ? dragPreviewTrackId : c->trackId;
                for (size_t i = 0; i < rows.size(); ++i)
                {
                    if (rows[i].trackId != trackId) continue;
                    auto ghost = *c;
                    ghost.offsetMs = dragPreviewStartMs;
                    ghost.effectiveDurationMs = dragPreviewEndMs - dragPreviewStartMs;
                    const auto ratio = c->effectiveWarpActive && c->effectiveTempoRatio > 0 ? c->effectiveTempoRatio : 1.0;
                    if (drag == DragKind::trimStart) ghost.inMs = c->inMs + (dragPreviewStartMs - c->offsetMs) * ratio;
                    ghost.durationMs = ghost.effectiveDurationMs * ratio;
                    const auto* track = model().findTrack(trackId);
                    paintClip(g, ghost, clipBounds(ghost, rows[i]), false,
                              track != nullptr && track->colorIndex >= 0 ? track->colorIndex : static_cast<int>(i), 0.85f);
                }
            }
        }

        // Library drop preview.
        if (dropPreview.has_value())
        {
            for (const auto& row : rows)
            {
                if (row.trackId != dropPreview->first) continue;
                const auto x = static_cast<float>(view.msToX(dropPreview->second));
                const auto y = static_cast<float>(row.top + view.headerHeight() - view.scrollY);
                g.setColour(colours::sky400.withAlpha(0.6f));
                g.fillRect(x - 1.0f, y, 2.0f, static_cast<float>(row.height));
            }
        }

        // Selection range over the tracks, and marker lines.
        const auto selection = rangePreview.has_value() ? rangePreview : model().timelineSelection;
        if (selection.has_value())
        {
            const auto x0 = static_cast<float>(view.msToX(selection->first));
            const auto x1 = static_cast<float>(view.msToX(selection->second));
            g.setColour(colours::sky400.withAlpha(0.08f));
            g.fillRect(juce::Rectangle<float>(x0, static_cast<float>(tracksArea.getY()), x1 - x0,
                                              static_cast<float>(tracksArea.getHeight())));
        }
        for (const auto& m : model().markers)
        {
            g.setColour(colours::emerald500.withAlpha(0.5f));
            g.drawVerticalLine(static_cast<int>(view.msToX(m.positionMs)), static_cast<float>(tracksArea.getY()),
                               static_cast<float>(tracksArea.getBottom()));
        }
        paintMixTransitions(g);
    }

    paintRuler(g, getLocalBounds().withHeight(VS::kRulerHeight));
    if (view.tempoLaneVisible) paintTempoLane(g);

    if (model().tracks.empty())
    {
        g.setColour(colours::textFaint);
        g.setFont(uiFont(14.0f));
        g.drawText("Add a track, then drag audio here from the library", tracksArea, juce::Justification::centred);
    }

    // Playhead.
    const auto px = static_cast<float>(view.msToX(live.positionMs));
    lastDrawnPlayheadX = px;
    g.setColour(colours::red500);
    g.drawLine(px, 0.0f, px, static_cast<float>(getHeight()), 1.5f);
}

// ── DJ transitions ──────────────────────────────────────────────────────────
// Each applied transition: a labelled bar across its window, along the bottom of the
// incoming clip (its automation is drawn in the tracks' lanes as usual).
void TimelineView::paintMixTransitions(juce::Graphics& g)
{
    if (transitionDropClipId.isNotEmpty())
        if (const auto* clip = model().findClip(transitionDropClipId))
            for (const auto& row : rows)
                if (row.trackId == clip->trackId)
                {
                    g.setColour(colours::violet500);
                    g.drawRect(clipBounds(*clip, row).expanded(1.0f), 2.0f);
                }
    const auto* list = model().raw.getProperty("mixTransitions", {}).getArray();
    if (list == nullptr) return;
    const auto violet = colours::violet500;
    for (const auto& t : *list)
    {
        const auto shape = transitionShape(t);
        if (!shape) continue;
        // The window over both clips, the outgoing one fading out and the incoming one in.
        for (const auto& [area, outgoing] : {std::pair{shape->out, true}, std::pair{shape->in, false}})
        {
            if (area.isEmpty()) continue;
            g.setColour(violet.withAlpha(0.18f));
            g.fillRect(area);
            g.setColour(violet.withAlpha(0.7f));
            g.drawRect(area, 1.0f);
            juce::Path fade;
            const auto body = area.reduced(2.0f, 6.0f);
            constexpr int kSteps = 24;
            for (int i = 0; i <= kSteps; ++i)
            {
                const float u = static_cast<float>(i) / kSteps;
                // Equal-power shape: the curve the default blend follows.
                const float gain = outgoing ? std::cos(u * juce::MathConstants<float>::halfPi)
                                            : std::sin(u * juce::MathConstants<float>::halfPi);
                const float x = body.getX() + u * body.getWidth();
                const float y = body.getBottom() - gain * body.getHeight();
                if (i == 0) fade.startNewSubPath(x, y);
                else fade.lineTo(x, y);
            }
            g.setColour(colours::zinc100.withAlpha(0.85f));
            g.strokePath(fade, juce::PathStrokeType(1.6f));
        }
        if (!shape->connector.isEmpty())
        {
            g.setColour(violet.withAlpha(0.55f));
            g.fillRect(shape->connector);
        }
        g.setColour(violet.withAlpha(0.95f));
        g.fillRoundedRectangle(shape->pill, 4.0f);
        g.setColour(colours::zinc100);
        g.setFont(uiFont(10.0f, true));
        g.drawText(juce::String(juce::CharPointer_UTF8("\xe2\x87\x84 ")) + mix::labelOf(t), shape->pill.reduced(5.0f, 0.0f).toNearestInt(),
                   juce::Justification::centred, true);
    }
}

std::optional<TimelineView::TransitionShape> TimelineView::transitionShape(const juce::var& record) const
{
    const auto* in = model().findClip(record.getProperty("inClipId", {}).toString());
    const auto* out = model().findClip(record.getProperty("outClipId", {}).toString());
    if (in == nullptr || out == nullptr) return std::nullopt;
    const auto x0 = static_cast<float>(view.msToX(static_cast<double>(record.getProperty("startMs", 0.0))));
    const auto x1 = juce::jmax(x0 + 4.0f, static_cast<float>(view.msToX(static_cast<double>(record.getProperty("endMs", 0.0)))));
    std::optional<juce::Rectangle<float>> outArea, inArea;
    for (const auto& row : rows)
    {
        if (row.trackId == out->trackId)
        {
            const auto b = clipBounds(*out, row);
            outArea = juce::Rectangle<float>(x0, b.getY(), x1 - x0, b.getHeight()).getIntersection(b);
        }
        if (row.trackId == in->trackId)
        {
            const auto b = clipBounds(*in, row);
            inArea = juce::Rectangle<float>(x0, b.getY(), x1 - x0, b.getHeight()).getIntersection(b);
        }
    }
    if (!outArea || !inArea) return std::nullopt;
    TransitionShape shape{*outArea, *inArea, {}, {}};
    // The connector runs between the two clips across whatever rows lie between them.
    const auto& upper = outArea->getY() <= inArea->getY() ? *outArea : *inArea;
    const auto& lower = outArea->getY() <= inArea->getY() ? *inArea : *outArea;
    const float cx = (x0 + x1) / 2.0f;
    if (lower.getY() > upper.getBottom()) shape.connector = {cx - 3.0f, upper.getBottom(), 6.0f, lower.getY() - upper.getBottom()};
    // The name straddles the join: across the boundary between the two clips.
    const float pillWidth = juce::jlimit(70.0f, 240.0f, x1 - x0);
    const float boundary = lower.getY() > upper.getBottom() ? (upper.getBottom() + lower.getY()) / 2.0f : upper.getBottom();
    shape.pill = {cx - pillWidth / 2.0f, boundary - 9.0f, pillWidth, 18.0f};
    return shape;
}

std::vector<std::pair<juce::String, juce::String>> TimelineView::nearbyPairs(const juce::String& clipId) const
{
    std::vector<std::pair<juce::String, juce::String>> out;
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr) return out;
    const auto curve = model().tempoCurve();
    constexpr double kReachBeats = 16.0 * 4.0;
    const ClipModel* next = nullptr;
    const ClipModel* previous = nullptr;
    double nextGap = kReachBeats, previousGap = kReachBeats;
    for (const auto& track : model().tracks)
    {
        if (track.id == clip->trackId) continue;
        for (const auto& other : track.clips)
        {
            if (other.offsetMs < clip->endMs() && clip->offsetMs < other.endMs()) continue; // overlapping: pairsFor's
            if (const double gap = curve.beatAt(other.offsetMs) - curve.beatAt(clip->endMs()); gap >= 0.0 && gap <= nextGap)
            {
                nextGap = gap;
                next = &other;
            }
            if (const double gap = curve.beatAt(clip->offsetMs) - curve.beatAt(other.endMs()); gap >= 0.0 && gap <= previousGap)
            {
                previousGap = gap;
                previous = &other;
            }
        }
    }
    if (next != nullptr) out.emplace_back(clip->id, next->id);
    if (previous != nullptr) out.emplace_back(previous->id, clip->id);
    return out;
}

void TimelineView::applyTransitionPlacing(const juce::String& outId, const juce::String& inId, const juce::String& presetId)
{
    const auto* out = model().findClip(outId);
    const auto* in = model().findClip(inId);
    const auto* preset = mix::find(presetId);
    if (out == nullptr || in == nullptr || preset == nullptr) return;
    if (mix::overlap(*out, *in))
    {
        mix::apply(actions.engineClient(), actions, model(), outId, inId, presetId);
        return;
    }
    // The incoming clip moves so it starts the preset's bars before the outgoing one ends, on
    // a bar, and at least a bar into the outgoing clip.
    const auto curve = model().tempoCurve();
    const double endBeat = curve.beatAt(out->endMs());
    double startBeat = std::round((endBeat - preset->bars * 4.0) / 4.0) * 4.0;
    startBeat = juce::jmax(startBeat, std::ceil(curve.beatAt(out->offsetMs) / 4.0) * 4.0 + 4.0);
    if (startBeat >= endBeat) startBeat = endBeat - 4.0;
    const double offsetMs = juce::jmax(0.0, curve.msAtBeat(startBeat));
    const auto trackId = in->trackId;
    actions.engineClient().inUndoGroup("Transition", [&]
    {
        actions.moveClip(inId, offsetMs, trackId);
        mirror.refreshNow();
        mix::apply(actions.engineClient(), actions, model(), outId, inId, presetId);
    });
}

// ── tempo lane ──────────────────────────────────────────────────────────────
juce::Rectangle<int> TimelineView::tempoLaneArea() const
{
    if (!view.tempoLaneVisible) return {};
    return {0, VS::kRulerHeight, getWidth(), VS::kTempoLaneHeight};
}

std::vector<TempoPoint> TimelineView::shownTempoMap() const
{
    return tempoDragIndex >= 0 ? tempoDraft : model().tempoMap;
}

std::pair<double, double> TimelineView::tempoRange(const std::vector<TempoPoint>& points) const
{
    // Around the tempos in play, in steps of 10 BPM, at least 20 BPM tall.
    double lo = model().bpm, hi = model().bpm;
    for (const auto& p : points)
    {
        lo = juce::jmin(lo, p.bpm);
        hi = juce::jmax(hi, p.bpm);
    }
    lo = std::floor((lo - 5.0) / 10.0) * 10.0;
    hi = std::ceil((hi + 5.0) / 10.0) * 10.0;
    if (hi - lo < 20.0) hi = lo + 20.0;
    return {juce::jmax(20.0, lo), juce::jmin(400.0, hi)};
}

float TimelineView::tempoToY(double bpm, std::pair<double, double> range) const
{
    const auto area = tempoLaneArea().toFloat().reduced(0.0f, 6.0f);
    const double f = (bpm - range.first) / juce::jmax(1.0, range.second - range.first);
    return area.getBottom() - static_cast<float>(juce::jlimit(0.0, 1.0, f)) * area.getHeight();
}

double TimelineView::yToTempo(float y, std::pair<double, double> range) const
{
    const auto area = tempoLaneArea().toFloat().reduced(0.0f, 6.0f);
    const double f = (area.getBottom() - y) / juce::jmax(1.0f, area.getHeight());
    return juce::jlimit(20.0, 400.0, range.first + juce::jlimit(0.0, 1.0, f) * (range.second - range.first));
}

int TimelineView::tempoPointAt(juce::Point<int> p) const
{
    const auto points = shownTempoMap();
    const auto range = tempoRange(points);
    for (int i = 0; i < static_cast<int>(points.size()); ++i)
    {
        const auto& t = points[static_cast<size_t>(i)];
        if (std::abs(view.msToX(t.timeMs) - p.x) <= 6.0 && std::abs(tempoToY(t.bpm, range) - static_cast<float>(p.y)) <= 6.0f)
            return i;
    }
    return -1;
}

void TimelineView::paintTempoLane(juce::Graphics& g)
{
    const auto area = tempoLaneArea();
    g.setColour(colours::grey950);
    g.fillRect(area);
    g.setColour(colours::grey700);
    g.drawHorizontalLine(area.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));

    const auto points = shownTempoMap();
    const auto range = tempoRange(points);
    // Guide lines every 10 BPM, labelled at the left.
    g.setFont(uiFont(9.0f));
    for (double bpm = range.first; bpm <= range.second + 0.01; bpm += 10.0)
    {
        const float y = tempoToY(bpm, range);
        g.setColour(colours::grey800);
        g.drawHorizontalLine(static_cast<int>(y), 0.0f, static_cast<float>(getWidth()));
        g.setColour(colours::zinc600);
        g.drawText(juce::String(static_cast<int>(bpm)), 4, static_cast<int>(y) - 10, 40, 10, juce::Justification::bottomLeft);
    }

    // The curve, sampled across the view (straight between points, flat past the ends).
    const TempoCurve curve(points, model().bpm);
    juce::Path path;
    for (int x = 0; x <= getWidth(); x += 2)
    {
        const float y = tempoToY(curve.bpmAt(view.xToMs(x)), range);
        if (x == 0) path.startNewSubPath(0.0f, y);
        else path.lineTo(static_cast<float>(x), y);
    }
    g.setColour(points.empty() ? colours::sky500.withAlpha(0.35f) : colours::sky500);
    g.strokePath(path, juce::PathStrokeType(1.5f));

    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto& p = points[i];
        const float x = static_cast<float>(view.msToX(p.timeMs));
        const float y = tempoToY(p.bpm, range);
        g.setColour(colours::zinc950);
        g.fillEllipse(x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        g.setColour(static_cast<int>(i) == tempoDragIndex ? colours::amber400 : colours::sky500);
        g.drawEllipse(x - 4.0f, y - 4.0f, 8.0f, 8.0f, 1.5f);
        g.setColour(colours::zinc300);
        g.drawText(juce::String(p.bpm, 1), static_cast<int>(x) + 6, static_cast<int>(y) - 14, 48, 12, juce::Justification::centredLeft);
    }
    if (points.empty())
    {
        g.setColour(colours::textFaint);
        g.drawText("Tempo " + juce::String(model().bpm, 2) + " BPM - double-click the line to add a tempo point",
                   area.reduced(48, 0), juce::Justification::centredLeft);
    }
}

bool TimelineView::tempoLaneMouseDown(const juce::MouseEvent& e)
{
    if (!tempoLaneArea().contains(e.getPosition())) return false;
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        const double atMs = actions.snap(juce::jmax(0.0, view.xToMs(e.x)));
        const auto points = model().tempoMap;
        menu.addItem("Add Tempo Point Here", [this, atMs, points]
                     {
                         auto next = points;
                         next.push_back({atMs, model().tempoCurve().bpmAt(atMs)});
                         actions.setTempoMap(next);
                     });
        if (const int index = tempoPointAt(e.getPosition()); index >= 0)
            menu.addItem("Delete Tempo Point", [this, index, points]
                         {
                             auto next = points;
                             next.erase(next.begin() + index);
                             actions.setTempoMap(next);
                         });
        menu.addItem("Clear Tempo Map", !points.empty(), false, [this] { actions.setTempoMap({}); });
        menu.addSeparator();
        menu.addItem("Hide Tempo Lane", [this]
                     {
                         view.tempoLaneVisible = false;
                         view.changed();
                     });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
        return true;
    }
    if (const int index = tempoPointAt(e.getPosition()); index >= 0)
    {
        tempoDraft = model().tempoMap;
        tempoDragIndex = index;
        tempoDragRange = tempoRange(tempoDraft);
        repaint();
    }
    return true;
}

bool TimelineView::tempoLaneDoubleClick(const juce::MouseEvent& e)
{
    if (!tempoLaneArea().contains(e.getPosition())) return false;
    auto points = model().tempoMap;
    if (const int index = tempoPointAt(e.getPosition()); index >= 0)
        points.erase(points.begin() + index);
    else
    {
        // On the line: a point where the tempo already is, ready to drag up or down.
        const double atMs = e.mods.isAltDown() ? juce::jmax(0.0, view.xToMs(e.x)) : actions.snap(juce::jmax(0.0, view.xToMs(e.x)));
        points.push_back({atMs, std::round(model().tempoCurve().bpmAt(atMs) * 10.0) / 10.0});
    }
    actions.setTempoMap(points);
    return true;
}

// ── mouse ───────────────────────────────────────────────────────────────────
void TimelineView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag = DragKind::none;
    rangePreview.reset();
    const double ms = juce::jmax(0.0, view.xToMs(e.x));

    if (tempoLaneMouseDown(e)) return;
    if (e.y < VS::kRulerHeight)
    {
        if (const auto* m = markerAt(e.getPosition()))
        {
            drag = DragKind::marker;
            dragMarkerId = m->id;
            return;
        }
        drag = DragKind::seek;
        dragAnchorMs = actions.snap(ms);
        actions.seek(e.mods.isAltDown() ? ms : dragAnchorMs);
        return;
    }

    if (const auto lane = laneAt(e.getPosition()))
    {
        const auto* track = model().findTrack(lane->row->trackId);
        if (track == nullptr) return;
        view.selectedTrackId = track->id;
        view.changed();
        autoTrackId = track->id;
        autoParamId = lane->lane->paramId;
        autoPoints = automation::pointsOf(*track, autoParamId);
        autoIndex = pointAt(*lane, e.getPosition());
        if (e.mods.isPopupMenu())
        {
            showAutomationMenu(autoTrackId, autoParamId, autoIndex, view.xToMs(e.x));
            return;
        }
        autoGestureId = juce::Uuid().toDashedString();
        if (autoIndex < 0 && e.mods.isShiftDown())
        {
            // Shift-drag on a segment bends it: up for a fast start, down for a slow start.
            const double ms = view.xToMs(e.x);
            bendIndex = -1;
            for (size_t i = 0; i + 1 < autoPoints.size(); ++i)
                if (autoPoints[i].timeMs <= ms && ms < autoPoints[i + 1].timeMs) bendIndex = static_cast<int>(i);
            if (bendIndex >= 0)
            {
                bendStartTension = autoPoints[static_cast<size_t>(bendIndex)].tension;
                bendStartCurve = autoPoints[static_cast<size_t>(bendIndex)].curve;
                drag = DragKind::automationBend;
                return;
            }
        }
        if (autoIndex < 0)
        {
            // Click on the curve's lane: add a point there and drag it.
            autoPoints.push_back(pointFrom(*lane, e.getPosition(), !e.mods.isAltDown()));
            std::sort(autoPoints.begin(), autoPoints.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
            for (size_t i = 0; i < autoPoints.size(); ++i)
                if (std::abs(autoPoints[i].timeMs - pointFrom(*lane, e.getPosition(), !e.mods.isAltDown()).timeMs) < 0.001)
                    autoIndex = static_cast<int>(i);
            actions.setAutomation(autoTrackId, autoParamId, autoPoints, autoGestureId, false);
        }
        drag = DragKind::automationPoint;
        return;
    }

    if (const auto hit = clipAt(e.getPosition()); hit.has_value() && hit->clip->id == view.envelopeClipId && !e.mods.isPopupMenu()
                                                   && !hit->clip->locked)
    {
        const auto& clip = *hit->clip;
        const auto body = hit->bounds.withTrimmedTop(kClipHeaderPx);
        envClipId = clip.id;
        envPoints = clip.envelope;
        const double length = juce::jmax(1.0, clip.effectiveDurationMs);
        if (envPoints.size() < 2) envPoints = {{0.0, 1.0}, {length, 1.0}};
        envIndex = envelopePointAt(clip, body, e.getPosition());
        if (envIndex < 0)
        {
            const double t = juce::jlimit(0.0, length, (static_cast<double>(e.x) - body.getX()) / body.getWidth() * length);
            const double gain = juce::jlimit(0.0, 1.5, (body.getBottom() - static_cast<float>(e.y)) / body.getHeight() * 1.5);
            envPoints.push_back({t, gain});
            std::sort(envPoints.begin(), envPoints.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
            for (size_t i = 0; i < envPoints.size(); ++i)
                if (std::abs(envPoints[i].timeMs - t) < 1e-6 && std::abs(envPoints[i].gain - gain) < 1e-9) envIndex = static_cast<int>(i);
            actions.setClipEnvelope(envClipId, envPoints);
        }
        drag = DragKind::envelopePoint;
        return;
    }

    if (const auto hit = clipAt(e.getPosition()))
    {
        const auto& clip = *hit->clip;
        if (e.mods.isShiftDown() || e.mods.isCommandDown())
        {
            if (view.selectedClipIds.count(clip.id) != 0) view.selectedClipIds.erase(clip.id);
            else view.selectedClipIds.insert(clip.id);
        }
        else if (view.selectedClipIds.count(clip.id) == 0)
        {
            view.selectedClipIds = {clip.id};
        }
        view.selectedTrackId = clip.trackId;
        view.changed();

        if (e.mods.isPopupMenu())
        {
            showClipMenu(clip.id, ms);
            return;
        }
        if (clip.locked) return;
        drag = hit->edge;
        dragClipId = clip.id;
        dragAnchorMs = ms;
        dragOriginStartMs = clip.offsetMs;
        dragOriginEndMs = clip.endMs();
        dragPreviewStartMs = clip.offsetMs;
        dragPreviewEndMs = clip.endMs();
        dragPreviewTrackId = clip.trackId;
        return;
    }

    // Empty area: select the track, clear clip selection.
    view.selectedClipIds.clear();
    if (const int index = rowIndexAtY(e.y); index >= 0) view.selectedTrackId = rows[static_cast<size_t>(index)].trackId;
    view.changed();
    persistView();
    if (e.mods.isPopupMenu() && model().findTrack(view.selectedTrackId) != nullptr) showLaneMenu(view.selectedTrackId, ms);
}

void TimelineView::mouseDrag(const juce::MouseEvent& e)
{
    const double ms = juce::jmax(0.0, view.xToMs(e.x));
    const bool free = e.mods.isAltDown();
    auto snapped = [&](double t) { return free ? juce::jmax(0.0, t) : actions.snap(t); };
    if (tempoDragIndex >= 0 && tempoDragIndex < static_cast<int>(tempoDraft.size()))
    {
        // Between its neighbours, so dragging never reorders the map.
        const auto i = static_cast<size_t>(tempoDragIndex);
        const double lo = i > 0 ? tempoDraft[i - 1].timeMs + 1.0 : 0.0;
        const double hi = i + 1 < tempoDraft.size() ? tempoDraft[i + 1].timeMs - 1.0 : std::numeric_limits<double>::max();
        tempoDraft[i].timeMs = juce::jlimit(lo, juce::jmax(lo, hi), snapped(ms));
        tempoDraft[i].bpm = std::round(yToTempo(static_cast<float>(e.y), tempoDragRange) * 10.0) / 10.0;
        repaint();
        return;
    }

    switch (drag)
    {
    case DragKind::seek:
    case DragKind::range:
        if (std::abs(e.getDistanceFromDragStartX()) > kDragThresholdPx)
        {
            drag = DragKind::range;
            const double a = dragAnchorMs;
            const double b = snapped(ms);
            rangePreview = std::make_pair(juce::jmin(a, b), juce::jmax(a, b));
            repaint();
        }
        break;
    case DragKind::marker:
        if (std::abs(e.getDistanceFromDragStartX()) > kDragThresholdPx)
        {
            dragPreviewStartMs = snapped(ms);
            repaint();
        }
        break;
    case DragKind::envelopePoint:
    {
        const auto* clip = model().findClip(envClipId);
        if (clip == nullptr || envIndex < 0 || envIndex >= static_cast<int>(envPoints.size())) return;
        for (const auto& row : rows)
        {
            if (row.trackId != clip->trackId) continue;
            const auto body = clipBounds(*clip, row).withTrimmedTop(kClipHeaderPx);
            const double length = juce::jmax(1.0, clip->effectiveDurationMs);
            auto& p = envPoints[static_cast<size_t>(envIndex)];
            p.timeMs = juce::jlimit(0.0, length, (static_cast<double>(e.x) - body.getX()) / body.getWidth() * length);
            if (envIndex > 0) p.timeMs = juce::jmax(p.timeMs, envPoints[static_cast<size_t>(envIndex - 1)].timeMs + 1.0);
            if (envIndex + 1 < static_cast<int>(envPoints.size()))
                p.timeMs = juce::jmin(p.timeMs, envPoints[static_cast<size_t>(envIndex + 1)].timeMs - 1.0);
            p.gain = juce::jlimit(0.0, 1.5, (body.getBottom() - static_cast<float>(e.y)) / body.getHeight() * 1.5);
            actions.setClipEnvelope(envClipId, envPoints);
            repaint();
        }
        return;
    }
    case DragKind::automationBend:
    {
        if (bendIndex < 0 || bendIndex + 1 >= static_cast<int>(autoPoints.size())) return;
        auto& p = autoPoints[static_cast<size_t>(bendIndex)];
        const int dy = e.getDistanceFromDragStartY();
        if (std::abs(dy) < 3)
        {
            p.curve = bendStartCurve;
            p.tension = bendStartTension;
        }
        else
        {
            // The further the drag, the harder the bend (from nearly straight to steep).
            p.curve = dy < 0 ? "fastStart" : "slowStart";
            p.tension = juce::jlimit(-1.0, 1.0, -1.0 + std::abs(dy) / 60.0);
        }
        actions.setAutomation(autoTrackId, autoParamId, autoPoints, autoGestureId, false);
        return;
    }
    case DragKind::automationPoint:
    {
        for (const auto& row : rows)
        {
            if (row.trackId != autoTrackId) continue;
            for (const auto& lane : row.lanes)
            {
                if (lane.paramId != autoParamId) continue;
                LaneHit hit{&row, &lane, laneArea(row, lane)};
                if (autoIndex < 0 || autoIndex >= static_cast<int>(autoPoints.size())) return;
                auto moved = pointFrom(hit, e.getPosition(), !e.mods.isAltDown());
                // Points keep their order: a dragged point stays between its neighbours.
                if (autoIndex > 0) moved.timeMs = juce::jmax(moved.timeMs, autoPoints[static_cast<size_t>(autoIndex - 1)].timeMs + 1.0);
                if (autoIndex + 1 < static_cast<int>(autoPoints.size()))
                    moved.timeMs = juce::jmin(moved.timeMs, autoPoints[static_cast<size_t>(autoIndex + 1)].timeMs - 1.0);
                autoPoints[static_cast<size_t>(autoIndex)] = moved;
                actions.setAutomation(autoTrackId, autoParamId, autoPoints, autoGestureId, false);
                return;
            }
        }
        break;
    }
    case DragKind::move:
    {
        if (e.getDistanceFromDragStart() <= kDragThresholdPx) return;
        dragPreviewStartMs = snapped(dragOriginStartMs + (ms - dragAnchorMs));
        dragPreviewEndMs = dragPreviewStartMs + (dragOriginEndMs - dragOriginStartMs);
        if (const int index = trackRowIndexAtY(juce::jlimit(view.headerHeight(), getHeight() - 1, e.y)); index >= 0)
            dragPreviewTrackId = rows[static_cast<size_t>(index)].trackId;
        repaint();
        break;
    }
    case DragKind::trimStart:
        dragPreviewStartMs = juce::jmin(snapped(dragOriginStartMs + (ms - dragAnchorMs)), dragOriginEndMs - 10.0);
        repaint();
        break;
    case DragKind::trimEnd:
        dragPreviewEndMs = juce::jmax(snapped(dragOriginEndMs + (ms - dragAnchorMs)), dragOriginStartMs + 10.0);
        repaint();
        break;
    case DragKind::none:
        break;
    }

    // Auto-scroll near the edges while dragging.
    if (drag != DragKind::none && (e.x > getWidth() - 20 || e.x < 20))
    {
        view.scrollXPx += e.x < 20 ? -20.0 : 20.0;
        clampScroll();
        view.changed();
    }
}

void TimelineView::mouseUp(const juce::MouseEvent& e)
{
    if (tempoDragIndex >= 0)
    {
        tempoDragIndex = -1;
        if (e.getDistanceFromDragStart() > 0) actions.setTempoMap(tempoDraft);
        tempoDraft.clear();
        repaint();
        return;
    }
    const auto kind = drag;
    drag = DragKind::none;
    const bool moved = e.getDistanceFromDragStart() > kDragThresholdPx;
    switch (kind)
    {
    case DragKind::range:
        if (rangePreview.has_value() && rangePreview->second - rangePreview->first > 1.0)
            actions.setTimelineSelection(rangePreview, model().loopSelection);
        rangePreview.reset();
        break;
    case DragKind::seek:
        // A plain click on the ruler clears the range.
        if (model().timelineSelection.has_value()) actions.setTimelineSelection(std::nullopt, false);
        break;
    case DragKind::marker:
        if (moved) actions.moveMarker(dragMarkerId, dragPreviewStartMs);
        break;
    case DragKind::automationBend:
    case DragKind::automationPoint:
        // Close the gesture so the whole add/drag is one undo step.
        actions.setAutomation(autoTrackId, autoParamId, autoPoints, autoGestureId, true);
        autoGestureId.clear();
        break;
    case DragKind::move:
        if (moved
            && (std::abs(dragPreviewStartMs - dragOriginStartMs) > 0.01
                || dragPreviewTrackId != (model().findClip(dragClipId) != nullptr
                                              ? model().findClip(dragClipId)->trackId
                                              : dragPreviewTrackId)))
            actions.moveClip(dragClipId, dragPreviewStartMs, dragPreviewTrackId);
        break;
    case DragKind::trimStart:
    case DragKind::trimEnd:
        if (moved)
        {
            actions.trimClip(dragClipId, dragPreviewStartMs, dragPreviewEndMs);
            // A trim into a neighbour crossfades the pair (renderer: maybeCreateTransitionAfterTrim).
            mirror.refreshNow();
            actions.maybeCreateTransitionAfterTrim(dragClipId, kind == DragKind::trimEnd);
        }
        break;
    case DragKind::none:
        break;
    }
    repaint();
}

void TimelineView::mouseMove(const juce::MouseEvent& e)
{
    if (e.y < VS::kRulerHeight)
    {
        setMouseCursor(markerAt(e.getPosition()) != nullptr ? juce::MouseCursor::LeftRightResizeCursor
                                                            : juce::MouseCursor::PointingHandCursor);
        return;
    }
    const auto hit = clipAt(e.getPosition());
    if (hit && (hit->edge == DragKind::trimStart || hit->edge == DragKind::trimEnd))
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    else
        setMouseCursor(hit ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void TimelineView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (tempoLaneDoubleClick(e)) return;
    if (const auto* list = model().raw.getProperty("mixTransitions", {}).getArray())
        for (const auto& t : *list)
            if (const auto shape = transitionShape(t); shape.has_value() && shape->contains(e.position))
            {
                drag = DragKind::none;
                TransitionEditor::show(mirror, actions, t.getProperty("outClipId", {}).toString(), t.getProperty("inClipId", {}).toString());
                return;
            }
    if (view.envelopeClipId.isNotEmpty())
        if (const auto hit = clipAt(e.getPosition()); hit.has_value() && hit->clip->id == view.envelopeClipId)
        {
            auto points = hit->clip->envelope;
            const int index = envelopePointAt(*hit->clip, hit->bounds.withTrimmedTop(kClipHeaderPx), e.getPosition());
            if (index >= 0 && index < static_cast<int>(points.size()))
            {
                points.erase(points.begin() + index);
                // Two flat unity points are no envelope at all.
                actions.setClipEnvelope(hit->clip->id, envelope::isFlatUnity(points) ? std::vector<EnvelopePoint>{} : points);
            }
            drag = DragKind::none;
            return;
        }
    if (const auto lane = laneAt(e.getPosition()))
    {
        if (const auto* track = model().findTrack(lane->row->trackId))
        {
            auto points = automation::pointsOf(*track, lane->lane->paramId);
            const int index = pointAt(*lane, e.getPosition());
            if (index >= 0)
            {
                points.erase(points.begin() + index);
                actions.setAutomation(track->id, lane->lane->paramId, points);
            }
        }
        return;
    }
    // Double-click a marker to remove it (the renderer toggles markers at the playhead).
    if (const auto* m = markerAt(e.getPosition()))
    {
        actions.removeMarker(m->id);
        return;
    }
    // Double-click a clip's title strip to rename it, its body to open it in the Clip
    // Editor (the renderer's behaviour).
    if (const auto hit = clipAt(e.getPosition());
        hit.has_value() && static_cast<float>(e.y) <= hit->bounds.getY() + kClipHeaderPx)
    {
        drag = DragKind::none;
        startClipRename(hit->clip->id);
        return;
    }
    if (const auto hit = clipAt(e.getPosition()); hit.has_value() && onOpenClipEditor)
    {
        const auto id = hit->clip->id;
        drag = DragKind::none;
        juce::MessageManager::callAsync([this, id] { if (onOpenClipEditor) onOpenClipEditor(id); });
    }
}

void TimelineView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomBy(w.deltaY > 0 ? 1.2 : 1.0 / 1.2, view.xToMs(e.x));
        return;
    }
    const bool horizontal = std::abs(w.deltaX) > std::abs(w.deltaY) || e.mods.isShiftDown();
    if (horizontal)
        view.scrollXPx -= (std::abs(w.deltaX) > 0.0f ? w.deltaX : w.deltaY) * 300.0;
    else
        view.scrollY -= w.deltaY * 200.0;
    clampScroll();
    view.changed();
    persistView();
}

void TimelineView::mouseMagnify(const juce::MouseEvent& e, float scaleFactor)
{
    zoomBy(static_cast<double>(scaleFactor), view.xToMs(e.x));
}

void TimelineView::zoomBy(double factor, std::optional<double> anchorMs)
{
    const double anchor = anchorMs.value_or(live.positionMs);
    const double anchorX = view.msToX(anchor);
    view.pxPerSecond = juce::jlimit(VS::kMinPxPerSecond, VS::kMaxPxPerSecond, view.pxPerSecond * factor);
    view.scrollXPx = anchor / 1000.0 * view.pxPerSecond - anchorX;
    clampScroll();
    view.changed();
    persistView();
}

void TimelineView::zoomToFit()
{
    const double length = juce::jmax(10000.0, model().contentEndMs());
    view.pxPerSecond = juce::jlimit(VS::kMinPxPerSecond, VS::kMaxPxPerSecond,
                                    (getWidth() - 40) / (length / 1000.0));
    view.scrollXPx = 0.0;
    clampScroll();
    view.changed();
    persistView();
}

void TimelineView::revealRange(double fromMs, double toMs, const juce::String& trackId)
{
    const double span = juce::jmax(1000.0, toMs - fromMs);
    const double margin = span * 0.15;
    view.pxPerSecond = juce::jlimit(VS::kMinPxPerSecond, VS::kMaxPxPerSecond, getWidth() / ((span + 2.0 * margin) / 1000.0));
    view.scrollXPx = juce::jmax(0.0, (fromMs - margin) / 1000.0 * view.pxPerSecond);
    for (const auto& row : rows)
        if (row.trackId == trackId) view.scrollY = row.top;
    clampScroll();
    view.changed();
    persistView();
}

void TimelineView::reveal(double ms)
{
    const double x = view.msToX(ms);
    if (x < 0 || x > getWidth())
    {
        view.scrollXPx = juce::jmax(0.0, ms / 1000.0 * view.pxPerSecond - getWidth() / 3.0);
        clampScroll();
        view.changed();
    }
}

// ── library drops ──────────────────────────────────────────────────────────
bool TimelineView::isInterestedInDragSource(const SourceDetails& details)
{
    const auto text = details.description.toString();
    return text.startsWith("libraryItem:") || text.startsWith("transition:") || details.description.hasProperty("import");
}

void TimelineView::itemDragMove(const SourceDetails& details)
{
    if (details.description.toString().startsWith("transition:"))
    {
        // A transition goes on the clip under the pointer: outline it.
        const auto hit = clipAt(details.localPosition);
        const auto id = hit.has_value() ? hit->clip->id : juce::String();
        if (id != transitionDropClipId)
        {
            transitionDropClipId = id;
            repaint();
        }
        return;
    }
    const int index = trackRowIndexAtY(details.localPosition.y);
    if (index < 0)
    {
        dropPreview.reset();
    }
    else
    {
        dropPreview = std::make_pair(rows[static_cast<size_t>(index)].trackId,
                                     actions.snap(view.xToMs(details.localPosition.x)));
    }
    repaint();
}

void TimelineView::itemDragExit(const SourceDetails&)
{
    dropPreview.reset();
    transitionDropClipId.clear();
    repaint();
}

void TimelineView::itemDropped(const SourceDetails& details)
{
    dropPreview.reset();
    transitionDropClipId.clear();
    if (const auto what = details.description.toString(); what.startsWith("transition:"))
    {
        const auto hit = clipAt(details.localPosition);
        const auto why = hit.has_value() ? mix::drop(actions.engineClient(), actions, model(), hit->clip->id,
                                                     view.xToMs(details.localPosition.x), what)
                                         : juce::String("Drop a transition onto a clip.");
        if (why.isNotEmpty() && actions.onNotify) actions.onNotify(why, true);
        repaint();
        return;
    }
    if (details.description.hasProperty("import"))
    {
        juce::String target;
        if (const int index = trackRowIndexAtY(details.localPosition.y); index >= 0)
            target = rows[static_cast<size_t>(index)].trackId;
        if (onImportDrop)
            onImportDrop(importRequestsFromDrag(details.description), target,
                         actions.snap(view.xToMs(details.localPosition.x)));
        repaint();
        return;
    }
    const auto itemId = details.description.toString().fromFirstOccurrenceOf("libraryItem:", false, false);
    const auto* item = model().findLibraryItem(itemId);
    if (item == nullptr) return;
    juce::String trackId;
    if (const int index = trackRowIndexAtY(details.localPosition.y); index >= 0)
        trackId = rows[static_cast<size_t>(index)].trackId;
    else
        trackId = actions.addTrack();
    const auto clipId = actions.addClip(trackId, *item, actions.snap(view.xToMs(details.localPosition.x)));
    view.selectedClipIds = {clipId};
    view.selectedTrackId = trackId;
    view.changed();
}

bool TimelineView::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (AudioImporter::isAudioFile(juce::File(f))) return true;
    return false;
}

void TimelineView::fileDragMove(const juce::StringArray&, int x, int y)
{
    const int index = trackRowIndexAtY(y);
    dropPreview = std::make_pair(index >= 0 ? rows[static_cast<size_t>(index)].trackId : juce::String(),
                                 actions.snap(view.xToMs(x)));
    repaint();
}

void TimelineView::fileDragExit(const juce::StringArray&)
{
    dropPreview.reset();
    repaint();
}

void TimelineView::filesDropped(const juce::StringArray& files, int x, int y)
{
    dropPreview.reset();
    repaint();
    std::vector<ImportRequest> requests;
    for (const auto& f : files)
        if (AudioImporter::isAudioFile(juce::File(f))) requests.push_back({juce::File(f), {}});
    juce::String target;
    if (const int index = trackRowIndexAtY(y); index >= 0) target = rows[static_cast<size_t>(index)].trackId;
    if (onImportDrop && !requests.empty()) onImportDrop(std::move(requests), target, actions.snap(view.xToMs(x)));
}

// ── context menus ──────────────────────────────────────────────────────────
juce::PopupMenu TimelineView::stutterMenu(const juce::String& trackId, double atMs)
{
    // From the clicked beat, or across the timeline selection when there is one.
    const auto curve = model().tempoCurve();
    double from = std::floor(curve.beatAt(juce::jmax(0.0, atMs)));
    double to = -1.0;
    if (const auto& selection = model().timelineSelection; selection.has_value() && selection->second > selection->first)
    {
        from = curve.beatAt(selection->first);
        to = curve.beatAt(selection->second);
    }
    juce::PopupMenu menu;
    for (const auto& p : stutter::presets())
        menu.addItem(juce::String(p.label) + (to > from ? " (over the selection)" : ""),
                     [this, trackId, from, to, id = juce::String(p.id)] { stutter::apply(actions, model(), trackId, id, from, to); });
    return menu;
}

juce::PopupMenu TimelineView::beatRepeatMenu(const juce::String& trackId, double atMs, const ClipModel* clip)
{
    juce::PopupMenu menu;
    const auto* track = model().findTrack(trackId);
    if (track == nullptr) return menu;
    const auto curve = model().tempoCurve();
    const double startBeat = std::floor(curve.beatAt(atMs));
    struct Region
    {
        juce::String id, division;
        double startBeat, lengthBeats;
    };
    std::vector<Region> regions;
    if (const auto* list = track->raw.getProperty("beatRepeats", {}).getArray())
        for (const auto& r : *list)
            regions.push_back({r.getProperty("id", {}).toString(), r.getProperty("division", "1/8").toString(),
                               static_cast<double>(r.getProperty("startBeat", 0.0)),
                               static_cast<double>(r.getProperty("lengthBeats", 0.0))});
    const std::vector<std::pair<double, juce::String>> lengths{{0.5, "1/2 Beat"}, {1.0, "1 Beat"}, {2.0, "2 Beats"}, {4.0, "1 Bar"}};
    for (const auto& [beats, label] : lengths)
    {
        const double endBeat = startBeat + beats;
        bool overlaps = false;
        for (const auto& r : regions) overlaps = overlaps || (startBeat < r.startBeat + r.lengthBeats && endBeat > r.startBeat);
        juce::PopupMenu divisions;
        for (const auto* division : {"1/4", "1/8", "1/16", "1/32"})
            divisions.addItem(division, [this, trackId, startBeat, beats = beats, d = juce::String(division)]
                              { actions.addBeatRepeat(trackId, startBeat, beats, d); });
        menu.addSubMenu(label, divisions, !overlaps);
    }
    // Regions this clip (or the click) touches can be removed.
    const double fromBeat = clip != nullptr ? curve.beatAt(clip->offsetMs) : startBeat;
    const double toBeat = clip != nullptr ? curve.beatAt(clip->endMs()) : startBeat + 1.0;
    bool first = true;
    for (const auto& r : regions)
        if (r.startBeat < toBeat && r.startBeat + r.lengthBeats > fromBeat)
        {
            if (first) menu.addSeparator();
            first = false;
            // Beat numbers are whole for beat/bar regions, halves for 1/2-beat ones.
            const double beatNumber = r.startBeat + 1.0;
            const auto beatText = std::abs(beatNumber - std::round(beatNumber)) < 1e-6 ? juce::String(juce::roundToInt(beatNumber))
                                                                                       : juce::String(beatNumber, 1);
            menu.addItem(juce::String::fromUTF8("\xe2\x9c\x93 Beat ") + beatText + " - " + r.division,
                         [this, trackId, id = r.id] { actions.deleteBeatRepeat(trackId, id); });
        }
    return menu;
}

void TimelineView::showLaneMenu(const juce::String& trackId, double atMs)
{
    juce::PopupMenu menu;
    menu.addItem("Add marker here", [this, atMs] { actions.addMarker(actions.snap(atMs)); });
    menu.addSubMenu("Beat Repeat", beatRepeatMenu(trackId, atMs, nullptr));
    menu.addSubMenu("Beat Stutter", stutterMenu(trackId, atMs));
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void TimelineView::showClipMenu(const juce::String& clipId, double atMs)
{
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr) return;
    const auto trackId = clip->trackId;
    juce::PopupMenu menu;

    // Several clips selected: the bulk actions.
    if (view.selectedClipIds.size() > 1)
    {
        const std::vector<juce::String> ids(view.selectedClipIds.begin(), view.selectedClipIds.end());
        const int count = static_cast<int>(ids.size());
        bool allLocked = true;
        for (const auto& id : ids)
            if (const auto* c = model().findClip(id)) allLocked = allLocked && c->locked;
        menu.addItem((allLocked ? "Unlock " : "Lock ") + juce::String(count) + " Clips", [this, ids, allLocked]
                     {
                         actions.engineClient().inUndoGroup(allLocked ? "Unlock clips" : "Lock clips", [&]
                                                            { for (const auto& id : ids) actions.setClipLocked(id, !allLocked); });
                     });
        juce::PopupMenu colour;
        for (int i = 0; i < kPaletteSize; ++i)
            colour.addColouredItem(1000 + i, "Colour " + juce::String(i + 1), paletteSwatch(i), true, false, nullptr);
        menu.addSubMenu("Colour", colour);
        menu.addItem("Duplicate " + juce::String(count) + " Clips", [this, ids]
                     {
                         actions.engineClient().inUndoGroup("Duplicate clips", [&] { for (const auto& id : ids) actions.duplicateClip(id); });
                     });
        menu.addItem("Delete " + juce::String(count) + " Clips", [this, ids]
                     {
                         actions.removeClips(ids);
                         view.selectedClipIds.clear();
                         view.changed();
                     });
        menu.addSeparator();
        menu.addItem("Deselect", [this] { view.selectedClipIds.clear(); view.changed(); });
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [this, ids](int result)
                           {
                               if (result < 1000 || result >= 1000 + kPaletteSize) return;
                               actions.engineClient().inUndoGroup("Recolour clips", [&]
                                                                  { for (const auto& id : ids) actions.setClipColor(id, result - 1000); });
                           });
        return;
    }

    menu.addItem("Clip Editor...", onOpenClipEditor != nullptr, false, [this, clipId] { if (onOpenClipEditor) onOpenClipEditor(clipId); });
    menu.addItem("Scratch Editor...", onOpenScratch != nullptr && !clip->unresolved, false, [this, clipId] { if (onOpenScratch) onOpenScratch(clipId); });
    menu.addSeparator();
    const bool inside = live.positionMs > clip->offsetMs && live.positionMs < clip->endMs();
    menu.addItem(clip->locked ? "Split at Playhead (clip is locked)" : "Split at Playhead", inside && !clip->locked, false,
                 [this, clipId] { actions.splitClipAt(clipId, live.positionMs); });
    menu.addItem("Split Here", atMs > clip->offsetMs && atMs < clip->endMs() && !clip->locked, false,
                 [this, clipId, atMs] { actions.splitClipAt(clipId, actions.snap(atMs)); });
    menu.addItem("Duplicate", [this, clipId] { actions.duplicateClip(clipId); });
    // Clicks on the clip add and drag gain points; double-click one to remove it.
    menu.addItem("Edit Volume Envelope", !clip->locked, view.envelopeClipId == clipId,
                 [this, clipId]
                 {
                     view.envelopeClipId = view.envelopeClipId == clipId ? juce::String() : clipId;
                     view.changed();
                     repaint();
                 });
    menu.addItem(clip->locked ? "Unlock" : "Lock", [this, clipId, locked = clip->locked] { actions.setClipLocked(clipId, !locked); });
    // Key lock, as on a DJ deck: off, a tempo change moves the pitch with it.
    const bool masterTempo = clip->warpMode != "repitch";
    menu.addItem("Master Tempo", true, masterTempo, [this, clipId, masterTempo] { actions.setClipMasterTempo(clipId, !masterTempo); });
    menu.addItem("Rename", [this, clipId] { startClipRename(clipId); });
    juce::PopupMenu colour;
    colour.addItem("Track colour", [this, clipId] { actions.setClipColor(clipId, -1); });
    for (int i = 0; i < kPaletteSize; ++i)
        colour.addColouredItem(2000 + i, "Colour " + juce::String(i + 1), paletteSwatch(i), true, clip->colorIndex == i, nullptr);
    menu.addSubMenu("Colour", colour);

    juce::PopupMenu transform;
    transform.addItem("Reverse", true, clip->reversed, [this, clipId, on = clip->reversed] { actions.setClipReversed(clipId, !on); });
    transform.addItem("Brake", !clip->backspin, clip->brake, [this, clipId, on = clip->brake] { actions.setClipBrake(clipId, !on); });
    transform.addItem("Backspin", !clip->brake, clip->backspin, [this, clipId, on = clip->backspin] { actions.setClipBackspin(clipId, !on); });
    menu.addSubMenu("Transform", transform);

    juce::PopupMenu effects;
    effects.addSubMenu("Beat Repeat", beatRepeatMenu(trackId, atMs, clip));
    effects.addSubMenu("Beat Stutter", stutterMenu(trackId, atMs));
    menu.addSubMenu("Effects", effects);

    // Key: shift it, or match a clip it overlaps (the smallest shift that puts them in key).
    {
        juce::PopupMenu keyMenu;
        const auto key = automix::clipKey(model(), *clip);
        const int shift = static_cast<int>(std::lround(clip->semitones));
        const auto wheel = automix::camelot(key);
        keyMenu.addItem(key.isEmpty() ? juce::String("Key unknown")
                                      : "Key: " + key + " (" + wheel.text() + ")"
                                            + (shift != 0 ? ", shifted " + juce::String(shift > 0 ? "+" : "") + juce::String(shift) : juce::String()),
                        false, false, nullptr);
        keyMenu.addItem("Up a Semitone", shift < 12, false, [this, clipId, shift] { actions.setClipKeyShift(clipId, shift + 1); });
        keyMenu.addItem("Down a Semitone", shift > -12, false, [this, clipId, shift] { actions.setClipKeyShift(clipId, shift - 1); });
        keyMenu.addItem("Reset Key", shift != 0, false, [this, clipId] { actions.setClipKeyShift(clipId, 0); });
        bool first = true;
        for (const auto& [outId, inId] : mix::pairsFor(model(), clipId))
        {
            const auto* other = model().findClip(outId == clipId ? inId : outId);
            if (other == nullptr) continue;
            const auto target = automix::clipKey(model(), *other);
            const auto ownUnshifted = automix::transposeKey(key, -shift);
            const auto match = automix::keyMatchShift(ownUnshifted, target, 6);
            if (first) keyMenu.addSeparator();
            first = false;
            const auto name = other->name.isNotEmpty() ? other->name : juce::String("overlapping clip");
            keyMenu.addItem("Match " + name + " (" + (target.isEmpty() ? juce::String("key unknown") : target) + ")"
                                + (match.has_value() ? ": " + juce::String(*match > 0 ? "+" : "") + juce::String(*match) : juce::String()),
                            match.has_value(), false, [this, clipId, m = match.value_or(0)] { actions.setClipKeyShift(clipId, m); });
        }
        menu.addSubMenu("Key", keyMenu);
    }

    // DJ transitions: with the clips this one overlaps on other tracks, and the nearest ones
    // it could hand over to or take over from (they move into place when one is applied).
    auto pairs = mix::pairsFor(model(), clipId);
    const auto nearby = nearbyPairs(clipId);
    pairs.insert(pairs.end(), nearby.begin(), nearby.end());
    {
        juce::PopupMenu transitions;
        if (pairs.empty())
            transitions.addItem("No clip on another track within 16 bars", false, false, [] {});
        for (const auto& [outId, inId] : pairs)
        {
            const auto* other = model().findClip(outId == clipId ? inId : outId);
            const auto* record = mix::recordFor(model(), outId, inId);
            const auto current = record != nullptr ? record->getProperty("preset", {}).toString() : juce::String();
            juce::PopupMenu presetsMenu;
            juce::String recommended;
            {
                const auto* outClip = model().findClip(outId);
                const auto* inClip = model().findClip(inId);
                const auto pick = automix::choose(model(), waveforms, outClip != nullptr ? model().findLibraryItem(outClip->libraryItemId) : nullptr,
                                                  inClip != nullptr ? model().findLibraryItem(inClip->libraryItemId) : nullptr, {}, "Auto",
                                                  outClip != nullptr ? static_cast<int>(std::lround(outClip->semitones)) : 0,
                                                  inClip != nullptr ? static_cast<int>(std::lround(inClip->semitones)) : 0, outClip, inClip);
                const auto* picked = mix::find(pick.presetId);
                recommended = pick.presetId;
                presetsMenu.addItem("Recommended: " + (picked != nullptr ? picked->name : pick.presetId) + " (" + pick.why + ")",
                                    [this, outId = outId, inId = inId, id = pick.presetId] { applyTransitionPlacing(outId, inId, id); });
                presetsMenu.addSeparator();
            }
            for (const auto& category : mix::categories())
            {
                juce::PopupMenu sub;
                for (const auto& p : mix::presets())
                    if (p.category == category)
                        sub.addItem(p.name, true, p.id == current,
                                    [this, outId = outId, inId = inId, id = p.id] { applyTransitionPlacing(outId, inId, id); });
                presetsMenu.addSubMenu(category, sub);
            }
            presetsMenu.addSeparator();
            presetsMenu.addItem(record != nullptr ? "Edit Transition..." : "New Transition (Out and In)...",
                                [this, outId = outId, inId = inId, recommended]
                                {
                                    // Clips that do not overlap yet are placed with the recommendation first.
                                    const auto* o = model().findClip(outId);
                                    const auto* i = model().findClip(inId);
                                    if (o != nullptr && i != nullptr && !mix::overlap(*o, *i)) applyTransitionPlacing(outId, inId, recommended);
                                    TransitionEditor::show(mirror, actions, outId, inId);
                                });
            if (record != nullptr && mix::find(current) != nullptr)
                presetsMenu.addItem("Copy Transition", [preset = current] { mix::copyPreset(preset); });
            if (const auto* copied = mix::find(mix::copiedPreset()))
                presetsMenu.addItem("Paste Transition (" + copied->name + ")", [this, outId = outId, inId = inId, id = copied->id]
                                    { applyTransitionPlacing(outId, inId, id); });
            if (record != nullptr)
                presetsMenu.addItem("Remove Transition", [this, id = record->getProperty("id", {}).toString()]
                                    { mix::remove(actions.engineClient(), actions, model(), id); });
            const auto otherName = other != nullptr && other->name.isNotEmpty() ? other->name : juce::String("the other clip");
            if (pairs.size() == 1) transitions = presetsMenu;
            else transitions.addSubMenu((outId == clipId ? "Into " : "From ") + otherName, presetsMenu);
        }
        menu.addSubMenu("Transition", transitions);
    }
    // Several clips selected: give every join among them the preset that suits it.
    if (view.selectedClipIds.size() >= 2 && view.selectedClipIds.count(clipId) != 0)
    {
        const std::vector<juce::String> selected(view.selectedClipIds.begin(), view.selectedClipIds.end());
        juce::PopupMenu styles;
        for (const auto& style : automix::styles())
            styles.addItem(style, [this, selected, style]
                           {
                               const int n = automix::autoTransition(actions.engineClient(), mirror, actions, waveforms, selected, style);
                               if (actions.onNotify)
                                   actions.onNotify(n > 0 ? "Set " + juce::String(n) + " transition" + (n == 1 ? "" : "s")
                                                          : juce::String("The selected clips do not overlap across tracks"),
                                                    n == 0);
                           });
        menu.addSubMenu("Auto Transition Selected Clips", styles);
    }

    // Crossfades this clip takes part in.
    if (const auto* track = model().findTrack(trackId))
    {
        juce::PopupMenu crossfade;
        auto group = [&](const TransitionModel& tr, const juce::String& side, const juce::String& removeLabel)
        {
            for (const auto& [kind, label] : std::vector<std::pair<juce::String, juce::String>>{{"smooth", "Smooth blend"}, {"linear", "Fade out / in"}})
                crossfade.addItem(side + ": " + label, true, tr.recipe == kind,
                                  [this, trackId, id = tr.id, kind = kind] { actions.setTransitionRecipe(trackId, id, kind); });
            crossfade.addItem(removeLabel, [this, trackId, id = tr.id] { actions.deleteTransition(trackId, id); });
        };
        for (const auto& tr : track->transitions)
            if (tr.leftClipId == clipId) group(tr, "Crossfade to next", "Remove Crossfade to Next Clip");
        for (const auto& tr : track->transitions)
            if (tr.rightClipId == clipId)
            {
                if (crossfade.getNumItems() > 0) crossfade.addSeparator();
                group(tr, "Crossfade from previous", "Remove Crossfade from Previous Clip");
            }
        if (crossfade.getNumItems() > 0) menu.addSubMenu("Crossfade", crossfade);
    }

    menu.addItem("Separate Stems...", onSeparateStems != nullptr && !clip->unresolved, false,
                 [this, clipId] { if (onSeparateStems) onSeparateStems(clipId); });
    {
        juce::PopupMenu channels;
        channels.addItem("Left and right", [this, clipId] { if (onSplitChannels) onSplitChannels(clipId, {"left", "right"}); });
        channels.addItem("Left only", [this, clipId] { if (onSplitChannels) onSplitChannels(clipId, {"left"}); });
        channels.addItem("Right only", [this, clipId] { if (onSplitChannels) onSplitChannels(clipId, {"right"}); });
        const auto* source = model().findLibraryItem(clip->libraryItemId);
        menu.addSubMenu("Split Stereo Channels", channels, onSplitChannels != nullptr && source != nullptr && source->channelCount >= 2);
    }

    juce::PopupMenu library;
    const auto* item = model().findLibraryItem(clip->libraryItemId);
    const bool linked = item != nullptr && item->kind == "clip";
    library.addItem("Save Clip to Library", !linked, false, [this, clipId] { actions.saveClipToLibrary(clipId); });
    library.addItem("Save as Sample (one-shot)", [this, clipId] { actions.saveClipAsSample(clipId, "simple"); });
    library.addItem("Save as Sample (music)", [this, clipId] { actions.saveClipAsSample(clipId, "music"); });
    juce::PopupMenu chop;
    for (const auto& [beats, label] : std::vector<std::pair<double, juce::String>>{{4.0, "1 bar"}, {2.0, "1/2 bar"}, {1.0, "1/4"}, {0.5, "1/8"}, {0.25, "1/16"}})
        chop.addItem(label, [this, clipId, beats = beats]
                     {
                         const int n = actions.chopClipToGrid(clipId, beats, "simple");
                         if (actions.onNotify) actions.onNotify(juce::String(n) + " slices saved to the library", false);
                     });
    library.addSubMenu("Chop to Grid", chop);
    menu.addSubMenu("Library", library);
    menu.addSeparator();
    menu.addItem("Delete", [this, clipId]
                 {
                     actions.removeClips({clipId});
                     view.selectedClipIds.erase(clipId);
                     view.changed();
                 });
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [this, clipId](int result)
                       {
                           if (result >= 2000 && result < 2000 + kPaletteSize) actions.setClipColor(clipId, result - 2000);
                       });
}

// ── automation lanes ───────────────────────────────────────────────────────
juce::Rectangle<float> TimelineView::laneArea(const TrackRow& row, const LaneRow& lane) const
{
    const auto y = static_cast<float>(row.top + lane.top + view.headerHeight() - view.scrollY);
    return {0.0f, y, static_cast<float>(getWidth()), static_cast<float>(lane.height)};
}

std::optional<TimelineView::LaneHit> TimelineView::laneAt(juce::Point<int> p) const
{
    if (p.y < view.headerHeight()) return std::nullopt;
    for (const auto& row : rows)
        for (const auto& lane : row.lanes)
        {
            const auto area = laneArea(row, lane);
            if (area.contains(p.toFloat())) return LaneHit{&row, &lane, area};
        }
    return std::nullopt;
}

automation::Point TimelineView::pointFrom(const LaneHit& hit, juce::Point<int> p, bool snapTime) const
{
    const auto* param = automation::find(hit.lane->paramId);
    const auto body = hit.area.reduced(0.0f, 6.0f);
    const double fraction = (body.getBottom() - static_cast<float>(p.y)) / juce::jmax(1.0f, body.getHeight());
    const double t = juce::jmax(0.0, view.xToMs(p.x));
    return {snapTime ? actions.snap(t) : t, param != nullptr ? automation::fromFraction(*param, fraction) : 0.0};
}

int TimelineView::pointAt(const LaneHit& hit, juce::Point<int> p) const
{
    const auto* track = model().findTrack(hit.row->trackId);
    const auto* param = automation::find(hit.lane->paramId);
    if (track == nullptr || param == nullptr) return -1;
    const auto points = automation::pointsOf(*track, hit.lane->paramId);
    const auto body = hit.area.reduced(0.0f, 6.0f);
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto x = static_cast<float>(view.msToX(points[i].timeMs));
        const auto y = body.getBottom() - static_cast<float>(automation::toFraction(*param, points[i].value)) * body.getHeight();
        if (std::abs(x - static_cast<float>(p.x)) <= 6.0f && std::abs(y - static_cast<float>(p.y)) <= 6.0f)
            return static_cast<int>(i);
    }
    return -1;
}

void TimelineView::paintLanes(juce::Graphics& g, const TrackRow& row, const TrackModel& track)
{
    // Beat Repeat regions: a strip along the bottom of the clip lane.
    if (const auto* regions = track.raw.getProperty("beatRepeats", {}).getArray())
    {
        const auto curve = model().tempoCurve();
        const auto y = static_cast<float>(row.top + row.clipHeight + view.headerHeight() - view.scrollY) - 10.0f;
        for (const auto& r : *regions)
        {
            const double startBeat = static_cast<double>(r.getProperty("startBeat", 0.0));
            const double start = curve.msAtBeat(startBeat);
            const double end = curve.msAtBeat(startBeat + static_cast<double>(r.getProperty("lengthBeats", 0.0)));
            const auto x0 = static_cast<float>(view.msToX(start));
            const auto x1 = static_cast<float>(view.msToX(end));
            g.setColour(colours::amber500.withAlpha(0.75f));
            g.fillRoundedRectangle(x0, y, juce::jmax(2.0f, x1 - x0), 7.0f, 2.0f);
            g.setColour(colours::zinc950);
            g.setFont(uiFont(9.0f));
            g.drawText(r.getProperty("division", "").toString(), juce::Rectangle<float>(x0 + 2.0f, y - 1.0f, x1 - x0, 9.0f),
                       juce::Justification::centredLeft, false);
        }
    }

    for (const auto& lane : row.lanes)
    {
        const auto area = laneArea(row, lane);
        if (area.getBottom() < view.headerHeight() || area.getY() > getHeight()) continue;
        const auto* param = automation::find(lane.paramId);
        if (param == nullptr) continue;
        g.setColour(colours::grey950.withAlpha(0.9f));
        g.fillRect(area);
        g.setColour(colours::border);
        g.drawHorizontalLine(static_cast<int>(area.getY()), 0.0f, area.getRight());
        const auto body = area.reduced(0.0f, 6.0f);
        // The parameter's resting value as a guide line.
        const auto restY = body.getBottom() - static_cast<float>(automation::toFraction(*param, param->defaultValue)) * body.getHeight();
        g.setColour(colours::grey600);
        g.drawHorizontalLine(static_cast<int>(restY), 0.0f, area.getRight());

        auto points = automation::pointsOf(track, lane.paramId);
        if (drag == DragKind::automationPoint && autoTrackId == track.id && autoParamId == lane.paramId) points = autoPoints;
        const auto colour = colours::amber400;
        if (!points.empty())
        {
            juce::Path curve;
            auto yOf = [&](double v) { return body.getBottom() - static_cast<float>(automation::toFraction(*param, v)) * body.getHeight(); };
            curve.startNewSubPath(0.0f, yOf(points.front().value));
            for (size_t i = 0; i < points.size(); ++i)
            {
                const auto& p = points[i];
                const auto x = static_cast<float>(view.msToX(p.timeMs));
                curve.lineTo(x, yOf(p.value));
                // Each segment along its shape: sampled every few pixels where it is on screen.
                if (i + 1 < points.size() && p.curve != "linear")
                {
                    const auto& next = points[i + 1];
                    const auto x1 = static_cast<float>(view.msToX(next.timeMs));
                    if (x1 < 0.0f || x > static_cast<float>(getWidth())) continue;
                    const int steps = juce::jlimit(2, 200, static_cast<int>((x1 - x) / 3.0f));
                    for (int s = 1; s < steps; ++s)
                    {
                        const double t = p.timeMs + (next.timeMs - p.timeMs) * s / steps;
                        curve.lineTo(static_cast<float>(view.msToX(t)), yOf(automation::valueAt({p, next}, t, p.value)));
                    }
                    if (p.curve == "hold" || p.curve == "stairs") curve.lineTo(x1, yOf(automation::valueAt({p, next}, next.timeMs - 0.01, p.value)));
                }
            }
            curve.lineTo(area.getRight(), yOf(points.back().value));
            g.setColour(colour.withAlpha(0.9f));
            g.strokePath(curve, juce::PathStrokeType(1.6f));
            for (const auto& p : points)
            {
                const auto x = static_cast<float>(view.msToX(p.timeMs));
                if (x < -6.0f || x > getWidth() + 6.0f) continue;
                g.setColour(colours::zinc950);
                g.fillEllipse(x - 4.0f, yOf(p.value) - 4.0f, 8.0f, 8.0f);
                g.setColour(colour);
                g.drawEllipse(x - 4.0f, yOf(p.value) - 4.0f, 8.0f, 8.0f, 1.5f);
            }
        }
        g.setColour(colours::textDim);
        g.setFont(uiFont(11.0f));
        const double current = automation::valueAt(points, live.positionMs, param->defaultValue);
        g.drawText(juce::String(param->label) + "  " + automation::format(lane.paramId, current),
                   area.reduced(6.0f, 2.0f).toNearestInt(), juce::Justification::topLeft, false);
    }
}

void TimelineView::showAutomationMenu(const juce::String& trackId, const juce::String& paramId, int pointIndex, double atMs)
{
    juce::PopupMenu menu;
    const auto* track = model().findTrack(trackId);
    if (track == nullptr) return;
    const auto points = automation::pointsOf(*track, paramId);
    // The segment to shape: the one leaving the point, or the one under the click.
    int segment = pointIndex;
    if (segment < 0)
        for (size_t i = 0; i + 1 < points.size(); ++i)
            if (points[i].timeMs <= atMs && atMs < points[i + 1].timeMs) segment = static_cast<int>(i);
    if (segment >= 0 && segment + 1 < static_cast<int>(points.size()))
    {
        juce::PopupMenu shapes;
        const auto& current = points[static_cast<size_t>(segment)];
        for (const auto& [id, label] : automation::curveShapes())
            shapes.addItem(label, true, current.curve == id,
                           [this, trackId, paramId, segment, shapeId = juce::String(id)]
                           {
                               if (const auto* t = model().findTrack(trackId))
                               {
                                   auto next = automation::pointsOf(*t, paramId);
                                   if (segment >= static_cast<int>(next.size())) return;
                                   next[static_cast<size_t>(segment)].curve = shapeId;
                                   next[static_cast<size_t>(segment)].tension = 0.0;
                                   actions.setAutomation(trackId, paramId, next);
                               }
                           });
        shapes.addSeparator();
        shapes.addItem("Shift-drag a segment to bend it", false, false, [] {});
        menu.addSubMenu("Curve", shapes);
    }
    if (pointIndex >= 0)
        menu.addItem("Delete point", [this, trackId, paramId, pointIndex]
                     {
                         if (const auto* t = model().findTrack(trackId))
                         {
                             auto points = automation::pointsOf(*t, paramId);
                             if (pointIndex < static_cast<int>(points.size())) points.erase(points.begin() + pointIndex);
                             actions.setAutomation(trackId, paramId, points);
                         }
                     });
    // Patterns over the timeline selection, or the four bars from the clicked bar.
    {
        const auto curve = model().tempoCurve();
        double from = 0.0, to = 0.0;
        juce::String where;
        if (const auto& selection = model().timelineSelection; selection.has_value() && selection->second > selection->first)
        {
            from = selection->first;
            to = selection->second;
            where = "over the selection";
        }
        else
        {
            const double bar = std::floor(curve.beatAt(juce::jmax(0.0, atMs)) / 4.0) * 4.0;
            from = curve.msAtBeat(bar);
            to = curve.msAtBeat(bar + 16.0);
            where = "over 4 bars from here";
        }
        const double beatMs = 60000.0 / juce::jmax(1.0, curve.bpmAt(from));
        juce::PopupMenu patterns;
        for (const auto& p : automation::patterns())
            patterns.addItem(p.label, [this, trackId, paramId, from, to, beatMs, patternId = juce::String(p.id)]
                             {
                                 const auto* t = model().findTrack(trackId);
                                 const auto* param = automation::find(paramId);
                                 if (t == nullptr || param == nullptr) return;
                                 actions.setAutomation(trackId, paramId,
                                                       automation::applyPattern(automation::pointsOf(*t, paramId), *param, patternId, from, to, beatMs));
                             });
        menu.addSubMenu("Insert pattern " + where, patterns);
    }
    menu.addItem("Clear curve", [this, trackId, paramId] { actions.setAutomation(trackId, paramId, {}); });
    menu.addItem("Hide lane", [this, trackId, paramId]
                 {
                     if (const auto* t = model().findTrack(trackId))
                     {
                         auto lanes = automation::lanesOf(*t);
                         lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [&](const auto& l) { return l.paramId == paramId; }), lanes.end());
                         actions.setAutomationLanes(trackId, lanes);
                     }
                 });
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

int TimelineView::envelopePointAt(const ClipModel& clip, juce::Rectangle<float> body, juce::Point<int> p) const
{
    const double length = juce::jmax(1.0, clip.effectiveDurationMs);
    for (size_t i = 0; i < clip.envelope.size(); ++i)
    {
        const auto& pt = clip.envelope[i];
        const float x = body.getX() + static_cast<float>(pt.timeMs / length) * body.getWidth();
        const float y = body.getBottom() - static_cast<float>(juce::jlimit(0.0, 1.5, pt.gain) / 1.5) * body.getHeight();
        if (std::abs(x - static_cast<float>(p.x)) <= 6.0f && std::abs(y - static_cast<float>(p.y)) <= 6.0f) return static_cast<int>(i);
    }
    return -1;
}

} // namespace stemcel::native
