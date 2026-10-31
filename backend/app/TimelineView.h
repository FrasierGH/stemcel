#pragma once

#include "AudioImport.h"
#include "LiveState.h"
#include "ProjectModel.h"
#include "TimelineActions.h"
#include "WaveformStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <vector>

namespace stemcel::native
{

struct LaneRow
{
    juce::String paramId;
    int top = 0; // offset from the track row's top
    int height = 0;
};

struct TrackRow
{
    juce::String trackId;
    int top = 0;        // content y (below the ruler, before vertical scroll)
    int height = 0;     // clips + automation lanes
    int clipHeight = 0; // the clip lane alone
    std::vector<LaneRow> lanes;
};

// Row geometry shared by the track headers and the canvas.
std::vector<TrackRow> layoutRows(const ProjectModel& model);
int contentHeight(const std::vector<TrackRow>& rows);
// The master's row above its lanes (its name and lane menu are in the track headers).
constexpr int kMasterRowHeight = 40;

// The arrange canvas (the renderer's PixiJS timeline): ruler, grid, clips with waveforms,
// crossfades, markers, range selection and playhead, with select / move / trim / seek.
class TimelineView final : public juce::Component,
                           public juce::DragAndDropTarget,
                           public juce::FileDragAndDropTarget,
                           private juce::ChangeListener,
                           private juce::ScrollBar::Listener,
                           private juce::Timer
{
  public:
    // Clips on other tracks this one could hand over to, or take over from, without
    // overlapping yet: the nearest starting within 16 bars after it ends, and the nearest
    // ending within 16 bars before it starts. As (outgoing, incoming).
    std::vector<std::pair<juce::String, juce::String>> nearbyPairs(const juce::String& clipId) const;
    // Applies a preset; when the clips do not overlap yet, the incoming one first moves so the
    // preset's length of bars fits (on a bar). One undo step.
    void applyTransitionPlacing(const juce::String& outId, const juce::String& inId, const juce::String& presetId);
    TimelineView(ProjectMirror& mirror, TimelineViewState& view, LiveState& live, TimelineActions& actions,
                 WaveformStore& waveforms);
    ~TimelineView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scaleFactor) override;

    // Library items dragged in as "libraryItem:<id>".
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;
    // Files from the OS, and file/rekordbox rows that still need importing.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    std::function<void(std::vector<ImportRequest>, const juce::String& trackId, double positionMs)> onImportDrop;
    // Opens the clip editor (set by the workspace).
    std::function<void(const juce::String& clipId)> onOpenClipEditor;
    // Opens the inline name field over a clip's title strip.
    void startClipRename(const juce::String& clipId);
    std::function<void(const juce::String& clipId)> onSeparateStems;
    std::function<void(const juce::String& itemId)> onAnalyseSections;
    std::function<void(const juce::String& clipId)> onOpenScratch;
    std::function<void(const juce::String& clipId, const juce::StringArray& channels)> onSplitChannels;

    // Zoom keeping `anchorMs` under the same x (or the playhead/centre when unset).
    void zoomBy(double factor, std::optional<double> anchorMs = std::nullopt);
    void zoomToFit();
    // Zooms to show [fromMs, toMs] with a margin, scrolled down to `trackId` when given.
    void revealRange(double fromMs, double toMs, const juce::String& trackId = {});
    // Scroll so `ms` is visible.
    void reveal(double ms);
    // Clamp scrolls to the content and persist the view to the project.
    void clampScroll();

  private:
    enum class DragKind
    {
        none,
        seek,
        range,
        move,
        trimStart,
        trimEnd,
        automationPoint,
        automationBend,
        envelopePoint,
        marker
    };

    struct ClipHit
    {
        const ClipModel* clip = nullptr;
        juce::Rectangle<float> bounds;
        DragKind edge = DragKind::none;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override;
    // Places the scrollbars and matches their thumbs to the view's scroll and zoom.
    void syncScrollBars();

    const ProjectModel& model() const { return mirror.model(); }
    int rowIndexAtY(int y) const;
    // rowIndexAtY for clips: -1 on the master's row.
    int trackRowIndexAtY(int y) const;
    void showClipMenu(const juce::String& clipId, double atMs);
    void showAutomationMenu(const juce::String& trackId, const juce::String& paramId, int pointIndex, double atMs);
    void paintLanes(juce::Graphics& g, const TrackRow& row, const TrackModel& track);
    struct LaneHit
    {
        const TrackRow* row = nullptr;
        const LaneRow* lane = nullptr;
        juce::Rectangle<float> area;
    };
    std::optional<LaneHit> laneAt(juce::Point<int> p) const;
    juce::Rectangle<float> laneArea(const TrackRow& row, const LaneRow& lane) const;
    int pointAt(const LaneHit& hit, juce::Point<int> p) const;
    automation::Point pointFrom(const LaneHit& hit, juce::Point<int> p, bool snapTime) const;
    // Envelope editing (volume mode): points in clip-local timeline ms, gain 0..1.5 shown.
    int envelopePointAt(const ClipModel& clip, juce::Rectangle<float> body, juce::Point<int> p) const;
    std::vector<EnvelopePoint> envPoints;
    int envIndex = -1;
    juce::String envClipId;
    // Automation point drag state.
    juce::String autoTrackId, autoParamId, autoGestureId;
    std::vector<automation::Point> autoPoints;
    int autoIndex = -1;
    // A Shift-drag bending one segment: its first point, and its shape before the drag.
    int bendIndex = -1;
    double bendStartTension = 0.0;
    juce::String bendStartCurve;
    void showLaneMenu(const juce::String& trackId, double atMs);
    juce::PopupMenu beatRepeatMenu(const juce::String& trackId, double atMs, const ClipModel* clip);
    juce::PopupMenu stutterMenu(const juce::String& trackId, double atMs);
    void paintClipHeader(juce::Graphics& g, const ClipModel& clip, juce::Rectangle<float> header, juce::Colour border,
                         float alpha);
    std::vector<int> clipHeaderBadges(const ClipModel& clip) const;
    bool isWarpPending(const ClipModel& clip) const;
    bool ownsBeatRepeat(const ClipModel& clip) const;
    juce::Rectangle<float> clipBounds(const ClipModel& clip, const TrackRow& row) const;
    std::optional<ClipHit> clipAt(juce::Point<int> p) const;
    const MarkerModel* markerAt(juce::Point<int> p) const;
    double contentLengthMs() const;
    void paintRuler(juce::Graphics&, juce::Rectangle<int> area);
    void paintGrid(juce::Graphics&, juce::Rectangle<int> area);
    void paintClip(juce::Graphics&, const ClipModel& clip, juce::Rectangle<float> r, bool silenced, int trackColor,
                   float alpha);
    void persistView();

    ProjectMirror& mirror;
    TimelineViewState& view;
    LiveState& live;
    TimelineActions& actions;
    WaveformStore& waveforms;
    std::vector<TrackRow> rows;

    DragKind drag = DragKind::none;
    juce::String dragClipId;
    juce::String dragMarkerId;
    double dragAnchorMs = 0.0;
    double dragOriginStartMs = 0.0;
    double dragOriginEndMs = 0.0;
    double dragPreviewStartMs = 0.0;
    double dragPreviewEndMs = 0.0;
    juce::String dragPreviewTrackId;
    std::optional<std::pair<double, double>> rangePreview;
    std::optional<std::pair<juce::String, double>> dropPreview; // track id, start ms
    double lastDrawnPlayheadX = -1.0;
    bool viewDirty = false;
    bool warpSpinnerVisible = false; // a clip header drew the pending-warp spinner this frame
    juce::uint32 lastViewChangeMs = 0;

    // Inline clip rename: a field over the clip's title strip that follows it while the
    // view scrolls or zooms (the renderer's useClipRename).
    // The tempo lane (under the ruler while shown): the tempo map's curve, with points to add
    // (double-click the line), drag and delete (double-click a point), like an automation lane.
    juce::Rectangle<int> tempoLaneArea() const;
    void paintTempoLane(juce::Graphics& g);
    bool tempoLaneMouseDown(const juce::MouseEvent& e);
    bool tempoLaneDoubleClick(const juce::MouseEvent& e);
    std::vector<TempoPoint> shownTempoMap() const; // the drag draft while one is under way
    std::pair<double, double> tempoRange(const std::vector<TempoPoint>& points) const;
    float tempoToY(double bpm, std::pair<double, double> range) const;
    double yToTempo(float y, std::pair<double, double> range) const;
    int tempoPointAt(juce::Point<int> p) const;
    std::vector<TempoPoint> tempoDraft;
    int tempoDragIndex = -1;
    std::pair<double, double> tempoDragRange; // the scale stays put while a point moves

    void paintMixTransitions(juce::Graphics& g);
    void paintClipSections(juce::Graphics& g, const ClipModel& clip, juce::Rectangle<float> body, float alpha);
    // Where a recorded transition is drawn: its window over the outgoing and incoming clips,
    // the connector between them, and its name. Double-click any of it to edit.
    struct TransitionShape
    {
        juce::Rectangle<float> out, in, connector, pill;
        bool contains(juce::Point<float> p) const
        {
            return out.contains(p) || in.contains(p) || connector.contains(p) || pill.contains(p);
        }
    };
    std::optional<TransitionShape> transitionShape(const juce::var& record) const;

    juce::String transitionDropClipId; // the clip a dragged transition would land on

    void placeClipNameEditor();
    void commitClipRename(bool keep);
    juce::TextEditor clipNameEditor;
    juce::String renamingClipId;

    // Over the canvas's bottom and right edges, hidden while everything fits.
    juce::ScrollBar hScroll{false};
    juce::ScrollBar vScroll{true};
};

} // namespace stemcel::native
