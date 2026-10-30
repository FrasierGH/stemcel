#include "TransportBar.h"

#include "Db.h"
#include "Theme.h"
#include "TimelineView.h"

#include <cmath>

namespace stemcel::native
{
namespace
{
const juce::StringArray kGrids{"bar", "beat", "half", "quarter", "free"};
const juce::StringArray kGridLabels{"Bar", "Beat", "Half beat", "Quarter beat", "Free"};
constexpr double kMinBpm = 20.0;
constexpr double kMaxBpm = 300.0;
constexpr float kFloorDb = -60.0f; // the bottom of the master meter
} // namespace

BpmField::BpmField()
{
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

void BpmField::show(double bpm)
{
    pending = juce::jlimit(kMinBpm, kMaxBpm, std::round(bpm * 100.0) / 100.0);
    setText(juce::String(pending, 2), juce::dontSendNotification);
}

void BpmField::mouseDown(const juce::MouseEvent&)
{
    dragStart = current ? current() : getText().getDoubleValue();
    dragging = false;
}

void BpmField::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging && std::abs(e.getDistanceFromDragStartY()) < 3) return;
    dragging = true;
    // Up is faster: a quarter BPM a pixel, a hundredth with Shift.
    show(dragStart - e.getDistanceFromDragStartY() * (e.mods.isShiftDown() ? 0.01 : 0.25));
}

void BpmField::mouseUp(const juce::MouseEvent&)
{
    if (!dragging)
    {
        showEditor();
        return;
    }
    dragging = false;
    if (onCommit) onCommit(pending);
}

void BpmField::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (w.deltaY == 0.0f || isBeingEdited()) return;
    const double base = timerRunning ? pending : (current ? current() : getText().getDoubleValue());
    show(base + (w.deltaY > 0.0f ? 1.0 : -1.0) * (e.mods.isShiftDown() ? 0.1 : 1.0));
    timerRunning = true;
    startTimer(450);
}

void BpmField::timerCallback()
{
    stopTimer();
    timerRunning = false;
    if (onCommit) onCommit(pending);
}

TransportBar::TransportBar(ProjectMirror& projectMirror, TimelineViewState& viewState, LiveState& liveState,
                           TimelineActions& timelineActions, AppPreferences& preferences)
    : mirror(projectMirror), view(viewState), live(liveState), actions(timelineActions), prefs(preferences)
{
    playButton.onClick = [this] { togglePlay(); };
    playButton.setTooltip("Play / Pause (Space)");
    stopButton.onClick = [this] { actions.stop(); };
    stopButton.setTooltip("Stop and return to the start");
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    skipBack.onClick = [this] { skip(false); };
    skipForward.onClick = [this] { skip(true); };
    addAndMakeVisible(skipBack);
    addAndMakeVisible(skipForward);

    position.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 16.0f, juce::Font::plain)));
    position.setColour(juce::Label::textColourId, colours::zinc100);
    position.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(position);

    bpmLabel.setFont(uiFont(11.0f));
    bpmLabel.setColour(juce::Label::textColourId, colours::textFaint);
    addAndMakeVisible(bpmLabel);
    bpmValue.setFont(uiFont(14.0f, true));
    bpmValue.setColour(juce::Label::textColourId, colours::zinc100);
    bpmValue.setColour(juce::Label::backgroundColourId, colours::zinc900);
    bpmValue.setColour(juce::Label::outlineColourId, colours::zinc700);
    bpmValue.setTooltip("Project tempo: click to type, drag up or down or scroll to change it (Shift for fine steps)");
    bpmValue.current = [this]
    {
        const auto& m = mirror.model();
        return m.tempoMap.empty() ? m.bpm : m.tempoCurve().bpmAt(live.positionMs);
    };
    bpmValue.onTextChange = [this] { applyTempo(bpmValue.getText().getDoubleValue()); };
    bpmValue.onCommit = [this](double bpm) { applyTempo(bpm); };
    addAndMakeVisible(bpmValue);

    for (int i = 0; i < kGrids.size(); ++i) snapBox.addItem(kGridLabels[i], i + 1);
    snapBox.setTooltip("Snap grid");
    snapBox.onChange = [this]
    {
        const int index = snapBox.getSelectedItemIndex();
        if (index >= 0) actions.setSnapGrid(kGrids[index]);
    };
    addAndMakeVisible(snapBox);

    loopButton.setClickingTogglesState(false);
    loopButton.setTooltip("Loop the timeline selection");
    loopButton.onClick = [this]
    {
        const auto& m = mirror.model();
        if (!m.timelineSelection.has_value()) return;
        actions.setTimelineSelection(m.timelineSelection, !m.loopSelection);
    };
    addAndMakeVisible(loopButton);

    followButton.setClickingTogglesState(true);
    followButton.setToggleState(view.followPlayback, juce::dontSendNotification);
    followButton.setTooltip("Scroll to follow the playhead");
    followButton.onClick = [this]
    {
        view.followPlayback = followButton.getToggleState();
        prefs.set("ui", "followPlayback", view.followPlayback);
        prefs.save();
    };
    addAndMakeVisible(followButton);

    volumeButton.setClickingTogglesState(true);
    volumeButton.setTooltip("Volume mode: click a clip to add envelope points, drag them, double-click to remove");
    volumeButton.onClick = [this]
    {
        view.volumeMode = volumeButton.getToggleState();
        volumeButton.setColour(juce::TextButton::buttonColourId, view.volumeMode ? colours::amber500 : colours::zinc800);
        view.changed();
    };
    addAndMakeVisible(volumeButton);

    zoomIn.onClick = [this] { if (timeline != nullptr) timeline->zoomBy(1.25); };
    zoomOut.onClick = [this] { if (timeline != nullptr) timeline->zoomBy(0.8); };
    zoomFit.onClick = [this] { if (timeline != nullptr) timeline->zoomToFit(); };
    zoomIn.setTooltip("Zoom in");
    zoomOut.setTooltip("Zoom out");
    zoomFit.setTooltip("Zoom to fit the arrangement");
    for (auto* b : {&zoomIn, &zoomOut, &zoomFit}) addAndMakeVisible(b);

    // The timeline zoom, on a log scale from the whole project to the closest view.
    zoomSlider.setRange(std::log(TimelineViewState::kMinPxPerSecond), std::log(TimelineViewState::kMaxPxPerSecond), 0.001);
    zoomSlider.setTooltip("Timeline zoom");
    zoomSlider.onValueChange = [this]
    {
        if (timeline == nullptr || !zoomSlider.isMouseButtonDown()) return;
        timeline->zoomBy(std::exp(zoomSlider.getValue()) / view.pxPerSecond);
    };
    addAndMakeVisible(zoomSlider);
    mixerButton.setTooltip("Show the mixer (F9)");
    mixerButton.onClick = [this] { if (onToggleMixer) onToggleMixer(); };
    addAndMakeVisible(mixerButton);
    masterLabel.setFont(uiFont(9.0f, true));
    masterLabel.setColour(juce::Label::textColourId, colours::textFaint);
    masterLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(masterLabel);
    master.setRange(0.0, 1.0);
    master.setDoubleClickReturnValue(true, 1.0);
    master.onValueChange = [this]
    {
        const double gain = db::positionToLinear(master.getValue(), db::kMaxMasterDb);
        master.setTooltip("Master volume: " + db::format(db::linearToDb(gain)));
        actions.setMasterVolume(gain);
    };
    addAndMakeVisible(master);

    mirror.addChangeListener(this);
    sync();
    startTimerHz(30);
}

TransportBar::~TransportBar()
{
    mirror.removeChangeListener(this);
}

void TransportBar::togglePlay()
{
    if (live.playing) actions.pause();
    else actions.play();
}

void TransportBar::changeListenerCallback(juce::ChangeBroadcaster*)
{
    sync();
}

void TransportBar::sync()
{
    const auto& m = mirror.model();
    showTempo();
    const int gridIndex = kGrids.indexOf(m.snapGrid.isNotEmpty() ? m.snapGrid : juce::String("quarter"));
    snapBox.setSelectedItemIndex(juce::jmax(0, gridIndex), juce::dontSendNotification);
    loopButton.setEnabled(m.timelineSelection.has_value());
    loopButton.setToggleState(m.loopSelection, juce::dontSendNotification);
    loopButton.setColour(juce::TextButton::buttonColourId, m.loopSelection ? colours::amber500 : colours::zinc800);
    if (!master.isMouseButtonDown())
        master.setValue(db::linearToPosition(m.masterVolume, db::kMaxMasterDb), juce::dontSendNotification);
}

juce::String TransportBar::positionText() const
{
    const double ms = juce::jmax(0.0, live.positionMs);
    const double beats = mirror.model().tempoCurve().beatAt(ms); // along the tempo map
    const int bar = static_cast<int>(beats / 4.0) + 1;
    const int beat = static_cast<int>(std::fmod(beats, 4.0)) + 1;
    const int sixteenth = static_cast<int>(std::fmod(beats * 4.0, 4.0)) + 1;
    const int totalSeconds = static_cast<int>(ms / 1000.0);
    return juce::String(bar) + "." + juce::String(beat) + "." + juce::String(sixteenth) + "   "
           + juce::String(totalSeconds / 60) + ":" + juce::String(totalSeconds % 60).paddedLeft('0', 2) + "."
           + juce::String(static_cast<int>(std::fmod(ms, 1000.0) / 10.0)).paddedLeft('0', 2);
}

void TransportBar::showTempo()
{
    if (!bpmValue.busy()) bpmValue.setText(juce::String(bpmValue.current(), 2), juce::dontSendNotification);
}

void TransportBar::applyTempo(double typed)
{
    // With a tempo map the box shows the tempo at the playhead, and a new value scales the
    // whole map (the project tempo is its base) so that the playhead's tempo is the one typed.
    const auto& m = mirror.model();
    const double shown = bpmValue.current();
    const double bpm = shown > 0.0 ? m.bpm * typed / shown : typed;
    if (typed >= kMinBpm && typed <= kMaxBpm && bpm >= kMinBpm && bpm <= kMaxBpm && std::abs(bpm - m.bpm) > 1.0e-6)
        actions.setBpm(bpm, static_cast<bool>(prefs.get("ui", "matchProjectTempoOnDrop", true)));
    sync();
}

void TransportBar::timerCallback()
{
    position.setText(positionText(), juce::dontSendNotification);
    if (!mirror.model().tempoMap.empty()) showTempo(); // it moves with the playhead
    skipBack.setTooltip(view.skipToMarkers ? "Skip to previous marker" : "Skip to start");
    skipForward.setTooltip(view.skipToMarkers ? "Skip to next marker" : "Skip to end");
    playButton.setButtonText(live.playing ? "Pause" : "Play");
    playButton.setColour(juce::TextButton::buttonColourId, live.playing ? colours::sky600 : colours::zinc800);
    if (!zoomSlider.isMouseButtonDown()) zoomSlider.setValue(std::log(view.pxPerSecond), juce::dontSendNotification);
    // Peak hold: the highest peak stays for 1.5 s, then falls to the live level.
    const auto now = juce::Time::getMillisecondCounter();
    auto hold = [now](float peak, float& held, juce::uint32& at)
    {
        if (peak >= held || now - at > 1500)
        {
            held = peak;
            at = now;
        }
    };
    hold(live.masterPeakL, holdL, holdTimeL);
    hold(live.masterPeakR, holdR, holdTimeR);
    clipped = clipped || live.masterPeakL >= 1.0f || live.masterPeakR >= 1.0f;
    repaint(meterArea.expanded(2));
}

void TransportBar::setMixerShown(bool shown)
{
    mixerButton.setColour(juce::TextButton::buttonColourId, shown ? colours::sky600 : colours::zinc800);
    mixerButton.setTooltip(shown ? "Back to the library panel (F9)" : "Show the mixer (F9)");
}

void TransportBar::mouseDown(const juce::MouseEvent& e)
{
    // A click on the meter clears its clip light.
    if (meterArea.expanded(2).contains(e.getPosition()))
    {
        clipped = false;
        repaint(meterArea.expanded(2));
    }
}

void TransportBar::paint(juce::Graphics& g)
{
    g.fillAll(colours::zinc900);
    g.setColour(colours::border);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));

    // The master meter: L and R bars from -60 to 0 dBFS in segments (green, then amber
    // above -12, red above -3), each with its peak hold, a dB scale beneath, and a clip light.
    auto area = meterArea;
    auto clip = area.removeFromRight(10);
    g.setColour(clipped ? colours::red500 : colours::zinc800);
    g.fillRect(clip.reduced(1, 2));
    area.removeFromRight(3);
    auto labels = area.removeFromLeft(10);
    auto scale = area.removeFromBottom(9);
    g.setColour(colours::zinc950);
    g.fillRect(area);
    auto toX = [&area](float dbValue) { return area.getX() + (juce::jlimit(kFloorDb, 0.0f, dbValue) - kFloorDb) / -kFloorDb * area.getWidth(); };
    g.setFont(uiFont(8.0f, true));
    for (int ch = 0; ch < 2; ++ch)
    {
        const float peak = ch == 0 ? live.masterPeakL : live.masterPeakR;
        const float held = ch == 0 ? holdL : holdR;
        const auto lane = area.withHeight(area.getHeight() / 2).withY(area.getY() + ch * area.getHeight() / 2).reduced(0, 1).toFloat();
        g.setColour(colours::textFaint);
        g.drawText(ch == 0 ? "L" : "R", labels.withHeight(lane.toNearestInt().getHeight()).withY(lane.toNearestInt().getY()), juce::Justification::centred);
        const float level = toX(static_cast<float>(db::linearToDb(peak)));
        // Segments two pixels wide with a gap, coloured by where they sit.
        for (float x = lane.getX(); x < level; x += 3.0f)
        {
            const float at = kFloorDb + (x - area.getX()) / area.getWidth() * -kFloorDb;
            g.setColour(at > -3.0f ? colours::red500 : at > -12.0f ? colours::amber400 : colours::emerald500);
            g.fillRect(x, lane.getY(), 2.0f, lane.getHeight());
        }
        if (held > 0.0f)
        {
            g.setColour(colours::zinc100);
            g.fillRect(toX(static_cast<float>(db::linearToDb(held))) - 1.0f, lane.getY(), 2.0f, lane.getHeight());
        }
    }
    g.setColour(colours::textFaint);
    g.setFont(uiFont(7.5f));
    for (const int mark : {-48, -24, -12, -6, 0})
    {
        const float x = toX(static_cast<float>(mark));
        g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(scale.getY()), static_cast<float>(scale.getY() + 2));
        g.drawText(juce::String(mark), juce::Rectangle<float>(x - 10.0f, static_cast<float>(scale.getY() + 1), 20.0f, 8.0f),
                   juce::Justification::centred);
    }
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced(8, 7);
    skipBack.setBounds(area.removeFromLeft(30));
    area.removeFromLeft(2);
    playButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(2);
    skipForward.setBounds(area.removeFromLeft(30));
    area.removeFromLeft(4);
    stopButton.setBounds(area.removeFromLeft(52));
    area.removeFromLeft(12);
    position.setBounds(area.removeFromLeft(220));
    area.removeFromLeft(12);
    bpmLabel.setBounds(area.removeFromLeft(30));
    bpmValue.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(10);
    snapBox.setBounds(area.removeFromLeft(120));
    area.removeFromLeft(8);
    loopButton.setBounds(area.removeFromLeft(52));
    area.removeFromLeft(4);
    followButton.setBounds(area.removeFromLeft(60));
    area.removeFromLeft(4);
    volumeButton.setBounds(area.removeFromLeft(44));
    area.removeFromLeft(4);
    mixerButton.setBounds(area.removeFromLeft(58));

    auto right = area.removeFromRight(520);
    meterArea = right.removeFromRight(150).reduced(0, 2);
    right.removeFromRight(6);
    master.setBounds(right.removeFromRight(100));
    masterLabel.setBounds(right.removeFromRight(46));
    right.removeFromRight(12);
    zoomFit.setBounds(right.removeFromRight(36));
    right.removeFromRight(2);
    zoomIn.setBounds(right.removeFromRight(24));
    zoomSlider.setBounds(right.removeFromRight(100));
    zoomOut.setBounds(right.removeFromRight(24));
}

void TransportBar::skip(bool forward)
{
    const auto& m = mirror.model();
    double target = forward ? m.contentEndMs() : 0.0;
    if (view.skipToMarkers)
    {
        // The nearest marker beyond the playhead (a little slack so repeated presses walk on).
        const double from = live.positionMs + (forward ? 5.0 : -5.0);
        for (const auto& marker : m.markers)
        {
            if (forward && marker.positionMs > from && marker.positionMs < target) target = marker.positionMs;
            if (!forward && marker.positionMs < from && marker.positionMs > target) target = marker.positionMs;
        }
    }
    actions.seek(juce::jmax(0.0, target));
    if (timeline != nullptr) timeline->reveal(target);
}

} // namespace stemcel::native
