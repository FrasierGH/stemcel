#pragma once

#include "AudioImport.h"
#include "LiveState.h"
#include "BeatSync.h"
#include "ProjectModel.h"
#include "Theme.h"
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
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;
    // For tests: a move drag of this clip under way, as a press on it would start.
    void beginMoveDragForTest(const juce::String& clipId);
    bool isBeatSyncedForTest() const { return beatLock; }
    double dragPreviewStartForTest() const { return dragPreviewStartMs; }
    void endDragForTest();
    // For tests: Space held or let go (no real key), the hold menu opened now, and the pointer
    // over its option `index` (-1: away from it).
    void setSpaceHeldForTest(bool held);
    void openBeatMenuForTest() { openBeatMenu(); }
    bool beatMenuOpenForTest() const { return beatMenu.has_value(); }
    int beatMenuOptionCountForTest() const { return beatMenu ? static_cast<int>(beatMenu->options.size()) : 0; }
    double beatMenuOptionStartForTest(int index) const;
    void hoverBeatMenuForTest(int index);
    // Space while dragging a clip. A tap locks the clip to the beat: it moves the least that
    // puts its beats on the beats of the clip it overlaps (else the grid), and keeps doing so as
    // it is dragged on; another tap lets it go. Held past 0.35 s, a menu of placements opens at
    // the pointer and stays while Space is held: the option under the pointer shows a ghost of
    // the clip where it would go, letting Space go over an option places the clip there, and
    // letting it go away from the menu closes it (the drag carries on). False when no clip is
    // being dragged (Space then plays as usual).
    bool beatSyncKey();
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
    // The BPM variability check for a clip's source (TempoReport).
    std::function<void(const juce::String& itemId, const juce::String& clipId)> onTempoReport;
    std::function<void(const juce::String& clipId)> onOpenScratch;
    // Quick Mashup of these library items (the source tracks of the selected clips).
    std::function<void(std::vector<juce::String> itemIds)> onQuickMashup;
    // Several library items dropped on the timeline (track empty: below the tracks), and placing
    // them one after another there (a new track when none is given).
    std::function<void(const juce::StringArray& itemIds, const juce::String& trackId, double atMs)> onLibraryItemsDropped;
    void placeLibraryItems(const juce::StringArray& ids, juce::String trackId, double at);
    // For tests: the mouse let go where it was pressed (the end of a drag that never moved).
    void releaseMouseForTest();
    // For tests: the mouse pressed, dragged and let go at points on the timeline (real handlers).
    void pressForTest(juce::Point<int> at, juce::ModifierKeys mods = {});
    void dragForTest(juce::Point<int> to);
    void releaseForTest(juce::Point<int> at);
    juce::Rectangle<int> clipScreenBoundsForTest(const juce::String& clipId) const;
    juce::String clipIdAtForTest(juce::Point<int> at) const
    {
        const auto hit = clipAt(at);
        return hit.has_value() ? hit->clip->id : juce::String();
    }
    std::function<void(const juce::String& clipId, const juce::StringArray& channels)> onSplitChannels;

    // Zoom keeping `anchorMs` under the same x (or the playhead/centre when unset).
    void zoomBy(double factor, std::optional<double> anchorMs = std::nullopt);
    void zoomToFit();
    // Where a transition is drawn on the outgoing (or incoming) clip's row, if it is on screen.
    std::optional<juce::Rectangle<float>> transitionArea(const juce::String& transitionId, bool outgoing) const;
    // The furthest the view zooms out: half the zoom that fits the whole project (and never
    // closer than 1.25 px/s), so a long mix always fits with room to spare.
    double minPxPerSecond() const;
    // What Fit shows: the clips, the project length and the markers.
    double fitLengthMs() const;
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
        marker,
        pan, // middle button held: the view follows the mouse
        transitionMove,  // a transition's label, dragged: the window moves
        transitionStart, // a transition's left edge
        transitionEnd,   // its right edge
        boxSelect        // dragged from empty space: a box selecting the clips it touches
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
    void paintClipHeader(juce::Graphics& g, const ClipModel& clip, juce::Rectangle<float> header, const PaletteEntry& palette,
                         float alpha);
    std::vector<int> clipHeaderBadges(const ClipModel& clip) const;
    bool isWarpPending(const ClipModel& clip) const;
    bool ownsBeatRepeat(const ClipModel& clip) const;
    juce::Rectangle<float> clipBounds(const ClipModel& clip, const TrackRow& row) const;
    std::optional<ClipHit> clipAt(juce::Point<int> p) const;
    const MarkerModel* markerAt(juce::Point<int> p) const;
    double contentLengthMs() const;
    void paintOverChildren(juce::Graphics&) override;
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
    // The other selected clips moving with the dragged one: where each started, and its track's
    // place in the project's track order (they keep their spacing and track offsets).
    struct GroupClip
    {
        juce::String id;
        double startMs = 0.0, endMs = 0.0;
        int trackIndex = 0;
    };
    std::vector<GroupClip> dragGroup;
    std::vector<juce::String> trackOrder() const;
    // How many tracks the group moves (the dragged clip's track change, kept on the tracks).
    int groupRowShift() const;
    // Keeps the group on the timeline (none before zero) and on the tracks.
    void clampGroupDrag();
    // The box being dragged (anchored in content coordinates, so it holds as the view scrolls).
    double boxAnchorMs = 0.0;
    double boxAnchorContentY = 0.0;
    std::optional<juce::Rectangle<float>> boxPreview;
    std::set<juce::String> boxBase;
    void selectInBox(juce::Rectangle<float> box);
    juce::Point<int> testDownPos;
    juce::MouseEvent testMouseEvent(juce::Point<int> at, juce::ModifierKeys mods) const;
    juce::String dragMarkerId;
    double dragAnchorMs = 0.0;
    double dragOriginStartMs = 0.0;
    double dragOriginEndMs = 0.0;
    double dragPreviewStartMs = 0.0;
    double dragPreviewEndMs = 0.0;
    juce::String dragPreviewTrackId;
    bool spaceHeld = false;           // Space is down during a drag (its key repeats are ignored)
    bool spaceHeldForTest = false;
    juce::uint32 spacePressedAt = 0;
    bool unlockOnTap = false;         // this press began with the lock on: a short tap turns it off
    bool beatLock = false;            // the dragged clip stays on the beat as it moves
    juce::String beatSyncLabel;       // what the lock did, shown on the dragged clip
    bool spaceIsDown() const;
    void spaceReleased();
    void applyBeatLock(const ClipModel& clip, double rawStartMs);
    // The placements menu while Space is held, drawn over the timeline at the pointer.
    struct BeatMenu
    {
        std::vector<beatsync::Placement> options;
        juce::Rectangle<int> bounds;
        int hovered = -1;
        bool mouseReleased = false; // the drag's button came up while it was open
    };
    std::optional<BeatMenu> beatMenu;
    void openBeatMenu();
    int beatMenuRowAt(juce::Point<int> p) const;
    juce::Rectangle<int> beatMenuRow(int index) const;
    void hoverBeatMenu(juce::Point<int> p);
    void placeFromBeatMenu(int index);
    void closeBeatMenu();
    void commitMoveDrag(double startMs);
    // The tempo lane's bottom edge, dragged to make it taller (Ruler + lane height).
    bool tempoResizing = false;
    int tempoResizeStartHeight = 0;
    bool overTempoLaneEdge(int y) const;
    // Right-click on a transition's fades over the clips: the curve of the level segment
    // there, and the transition editor.
    bool showTransitionCurveMenu(juce::Point<float> p);
    bool dragDuplicates = false; // Alt/Option held as a clip move began: drop a copy, leave the clip
    double panStartScrollX = 0.0, panStartScrollY = 0.0;
    std::optional<std::pair<double, double>> rangePreview;
    std::optional<std::pair<juce::String, double>> dropPreview; // track id, start ms
    // Where the playhead is drawn: set with the repaint that covers it, so a paint never
    // draws it anywhere it was not invalidated (no stale copies left behind).
    double playheadX = -1.0;
    bool wasPlaying = false;
    int tick = 0;
    // A scroll the user made, which pauses follow-playback.
    void userScrolled();
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
    // `window` stands in for the record's own while it is being dragged.
    std::optional<TransitionShape> transitionShape(const juce::var& record,
                                                   std::optional<std::pair<double, double>> window = std::nullopt) const;
    // The transition handle under `p`: the label (move) or a band edge (resize). An edge that
    // lies on a clip's own edge is left to trimming that clip.
    std::optional<std::pair<juce::var, DragKind>> transitionHandleAt(juce::Point<float> p) const;
    // A clip moved or trimmed: its transitions follow (mix::retimeAfterClipEdit).
    void retimeTransitions(const juce::String& clipId, double oldStartMs, double oldEndMs);
    juce::var txDragRecord;
    std::pair<double, double> txOriginWindow{0.0, 0.0}, txLimits{0.0, 0.0};
    std::optional<std::pair<double, double>> txPreview;

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
