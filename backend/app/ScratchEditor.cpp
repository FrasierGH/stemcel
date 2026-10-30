#include "ScratchEditor.h"

#include "Theme.h"

#include <cmath>

namespace stemcel::native
{
namespace
{
ScratchEditor* openScratchEditor = nullptr; // the one open editor, for the MIDI Cue button
constexpr int kProtocol = 1;
constexpr int kDeck = 1;

juce::var payload(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        if (!v.isVoid()) o->setProperty(k, v);
    return juce::var(o);
}

juce::String clock(juce::int64 us)
{
    const double s = static_cast<double>(us) / 1e6;
    return juce::String(static_cast<int>(s) / 60) + ":" + juce::String(std::fmod(s, 60.0), 2).paddedLeft('0', 5);
}
} // namespace

// ── The deck: a record you drag round ──────────────────────────────────────
class ScratchEditor::Deck final : public juce::Component
{
  public:
    explicit Deck(ScratchEditor& owner) : editor(owner) {}

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced(6.0f);
        const float size = juce::jmin(bounds.getWidth(), bounds.getHeight());
        const auto disc = bounds.withSizeKeepingCentre(size, size);
        const auto centre = disc.getCentre();
        g.setColour(colours::zinc800);
        g.fillEllipse(disc.expanded(4.0f));
        g.setColour(juce::Colour(0xff111113));
        g.fillEllipse(disc);
        // Grooves.
        g.setColour(colours::zinc700.withAlpha(0.5f));
        for (float r = size * 0.22f; r < size * 0.49f; r += 5.0f)
            g.drawEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(centre), 0.6f);
        // Label, rotated with the platter.
        const float angle = static_cast<float>(editor.platterTurns * juce::MathConstants<double>::twoPi);
        g.setColour(editor.touched ? colours::amber500 : colours::sky600);
        g.fillEllipse(juce::Rectangle<float>(size * 0.36f, size * 0.36f).withCentre(centre));
        juce::Path marker;
        marker.addRectangle(-2.0f, -size * 0.47f, 4.0f, size * 0.26f);
        g.setColour(colours::zinc100);
        g.fillPath(marker, juce::AffineTransform::rotation(angle).translated(centre));
        g.setColour(colours::zinc950);
        g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre(centre));
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        lastAngle = angleAt(e.position);
        editor.control("platterTouch", {{"deck", kDeck}, {"touched", true}});
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const double angle = angleAt(e.position);
        double delta = angle - lastAngle;
        // Unwrap across the +/-pi seam.
        if (delta > juce::MathConstants<double>::pi) delta -= juce::MathConstants<double>::twoPi;
        if (delta < -juce::MathConstants<double>::pi) delta += juce::MathConstants<double>::twoPi;
        lastAngle = angle;
        if (std::abs(delta) < 1e-6) return;
        editor.control("platterMove", {{"deck", kDeck},
                                       {"deltaTurns", delta / juce::MathConstants<double>::twoPi},
                                       {"clientTimeMs", juce::Time::getMillisecondCounterHiRes()}});
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        editor.control("platterTouch", {{"deck", kDeck}, {"touched", false}});
    }

  private:
    double angleAt(juce::Point<float> p) const
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        return std::atan2(static_cast<double>(p.x - c.x), static_cast<double>(c.y - p.y));
    }

    ScratchEditor& editor;
    double lastAngle = 0.0;
};

// ── Notation: the take's platter and crossfader curves, with crop handles ───
class ScratchEditor::Notation final : public juce::Component
{
  public:
    explicit Notation(ScratchEditor& owner) : editor(owner) {}

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::zinc950);
        const auto& p = editor.pattern;
        if (!p.isObject())
        {
            g.setColour(colours::textFaint);
            g.setFont(uiFont(12.0f));
            g.drawText("Record a take to see its notation here", getLocalBounds(), juce::Justification::centred);
            return;
        }
        const double duration = juce::jmax(1.0, static_cast<double>(p.getProperty("durationUs", 1)));
        auto area = getLocalBounds().toFloat().reduced(0.0f, 4.0f);
        const auto platterArea = area.removeFromTop(area.getHeight() * 0.7f);
        const auto faderArea = area.reduced(0.0f, 3.0f);
        auto x = [&](double us) { return static_cast<float>(us / duration) * static_cast<float>(getWidth()); };

        // Platter position (turns), scaled to the lane.
        if (const auto* keys = p.getProperty("platter", {}).getArray(); keys != nullptr && !keys->isEmpty())
        {
            double lo = 1e9, hi = -1e9;
            for (const auto& k : *keys)
            {
                const double t = static_cast<double>(k.getProperty("turns", 0.0));
                lo = juce::jmin(lo, t);
                hi = juce::jmax(hi, t);
            }
            const double span = juce::jmax(1e-6, hi - lo);
            juce::Path curve;
            bool first = true;
            for (const auto& k : *keys)
            {
                const float px = x(static_cast<double>(k.getProperty("timeUs", 0)));
                const float py = platterArea.getBottom() - static_cast<float>((static_cast<double>(k.getProperty("turns", 0.0)) - lo) / span) * platterArea.getHeight();
                if (first) curve.startNewSubPath(px, py);
                else curve.lineTo(px, py);
                first = false;
            }
            g.setColour(colours::sky400);
            g.strokePath(curve, juce::PathStrokeType(1.5f));
        }
        // Crossfader: open (1) drawn as a filled bar.
        if (const auto* keys = p.getProperty("crossfader", {}).getArray())
            for (int i = 0; i < keys->size(); ++i)
            {
                const auto& k = (*keys)[i];
                const double from = static_cast<double>(k.getProperty("timeUs", 0));
                const double to = i + 1 < keys->size() ? static_cast<double>((*keys)[i + 1].getProperty("timeUs", 0)) : duration;
                const float v = static_cast<float>(k.getProperty("value", 0.0));
                g.setColour(colours::emerald500.withAlpha(0.25f + 0.6f * v));
                g.fillRect(juce::Rectangle<float>(x(from), faderArea.getY(), juce::jmax(1.0f, x(to) - x(from)), faderArea.getHeight()));
            }
        // Crop: outside dimmed, handles amber.
        const float c0 = x(static_cast<double>(p.getProperty("cropStartUs", 0)));
        const float c1 = x(static_cast<double>(p.getProperty("cropEndUs", duration)));
        g.setColour(colours::zinc950.withAlpha(0.6f));
        g.fillRect(0.0f, 0.0f, c0, static_cast<float>(getHeight()));
        g.fillRect(c1, 0.0f, static_cast<float>(getWidth()) - c1, static_cast<float>(getHeight()));
        g.setColour(colours::amber400);
        g.drawLine(c0, 0.0f, c0, static_cast<float>(getHeight()), 2.0f);
        g.drawLine(c1, 0.0f, c1, static_cast<float>(getHeight()), 2.0f);
        if (editor.replaying)
        {
            const float rx = static_cast<float>(editor.replayPosition) * static_cast<float>(getWidth());
            g.setColour(colours::red500);
            g.drawLine(rx, 0.0f, rx, static_cast<float>(getHeight()), 1.5f);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        auto& p = editor.pattern;
        if (!p.isObject()) return;
        const double duration = juce::jmax(1.0, static_cast<double>(p.getProperty("durationUs", 1)));
        const float c0 = static_cast<float>(static_cast<double>(p.getProperty("cropStartUs", 0)) / duration) * getWidth();
        const float c1 = static_cast<float>(static_cast<double>(p.getProperty("cropEndUs", duration)) / duration) * getWidth();
        dragging = std::abs(e.position.x - c0) < std::abs(e.position.x - c1) ? 0 : 1;
        mouseDrag(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        auto& p = editor.pattern;
        if (!p.isObject() || dragging < 0) return;
        const double duration = juce::jmax(1.0, static_cast<double>(p.getProperty("durationUs", 1)));
        const auto us = static_cast<juce::int64>(juce::jlimit(0.0, duration, e.position.x / juce::jmax(1, getWidth()) * duration));
        auto* o = p.getDynamicObject();
        const auto start = static_cast<juce::int64>(o->getProperty("cropStartUs"));
        const auto end = static_cast<juce::int64>(o->hasProperty("cropEndUs") ? o->getProperty("cropEndUs") : juce::var(duration));
        // Keep at least 50 ms between the handles.
        if (dragging == 0) o->setProperty("cropStartUs", juce::jmin(us, end - 50000));
        else o->setProperty("cropEndUs", juce::jmax(us, start + 50000));
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override { dragging = -1; }

  private:
    ScratchEditor& editor;
    int dragging = -1;
};

// ── Editor ─────────────────────────────────────────────────────────────────
ScratchEditor::ScratchEditor(EngineClient& client, ProjectMirror& projectMirror, WaveformStore& waveformStore,
                             const juce::String& clip, const juce::String& item, std::function<void(const juce::String&, bool)> notifyFn)
    : engine(client), mirror(projectMirror), waveforms(waveformStore), clipId(clip), libraryItemId(item),
      notify(std::move(notifyFn)), subscriptions(client)
{
    openScratchEditor = this;
    deck = std::make_unique<Deck>(*this);
    addAndMakeVisible(*deck);
    notation = std::make_unique<Notation>(*this);
    addAndMakeVisible(*notation);
    replayButton.setTooltip("Play the take back as recorded (within its crop)");
    replayButton.onClick = [this]
    {
        if (replaying) engine.send("SCRATCH_PATTERN_REPLAY_STOP", payload({{"protocolVersion", kProtocol}}));
        else if (pattern.isObject()) engine.send("SCRATCH_PATTERN_REPLAY_START", payload({{"protocolVersion", kProtocol}, {"pattern", pattern}}));
    };
    addAndMakeVisible(replayButton);

    // Backing: a bed rendered from the chosen tracks to scratch over (useScratchBacking.ts).
    backingHeading.setFont(uiFont(12.0f, true));
    backingHeading.setColour(juce::Label::textColourId, colours::textDim);
    backingFrom.addItem("From: Start", 1);
    backingFrom.addItem("From: Playhead", 2);
    backingFrom.setSelectedId(1, juce::dontSendNotification);
    backingLength.addItem("60s", 60);
    backingLength.addItem("120s", 120);
    backingLength.addItem("Full", 1000);
    backingLength.setSelectedId(120, juce::dontSendNotification);
    backingTracksButton.onClick = [this] { showBackingTracksMenu(); };
    backingButton.onClick = [this]
    {
        juce::Array<juce::var> trackIds;
        for (const auto& id : audibleBackingTracks()) trackIds.add(id);
        if (trackIds.isEmpty()) return;
        const int length = backingLength.getSelectedId();
        engine.send("SCRATCH_BACKING_PREPARE", payload({{"protocolVersion", kProtocol},
                                                         {"sessionId", sessionId},
                                                         {"trackIds", trackIds},
                                                         {"startAnchor", backingFrom.getSelectedId() == 2 ? "playhead" : "arrangement"},
                                                         {"durationSec", length == 1000 ? 0 : length}}));
    };
    for (auto* comp : std::initializer_list<juce::Component*>{&backingHeading, &backingTracksButton, &backingFrom, &backingLength})
        addAndMakeVisible(comp);
    backingClear.onClick = [this] { engine.send("SCRATCH_BACKING_CLEAR", payload({{"protocolVersion", kProtocol}, {"sessionId", sessionId}})); };
    backingLoop.onClick = [this] { control("backingLoop", {{"loop", backingLoop.getToggleState()}}); };
    for (auto* sl : {&backingGain, &scratchGain})
    {
        sl->setSliderStyle(juce::Slider::LinearHorizontal);
        sl->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        sl->setRange(0.0, 1.0, 0.01);
        sl->setValue(1.0, juce::dontSendNotification);
    }
    backingGain.setTooltip("Backing level (monitor only)");
    scratchGain.setTooltip("Scratch level (monitor only)");
    backingGain.onValueChange = [this] { control("backingGain", {{"gain", backingGain.getValue()}}); };
    scratchGain.onValueChange = [this] { control("scratchGain", {{"gain", scratchGain.getValue()}}); };
    backingLabel.setFont(uiFont(12.0f));
    backingLabel.setColour(juce::Label::textColourId, colours::textDim);
    gainLabel.setFont(uiFont(12.0f));
    gainLabel.setColour(juce::Label::textColourId, colours::textDim);
    for (auto* comp : std::initializer_list<juce::Component*>{&backingButton, &backingClear, &backingLoop, &backingGain, &scratchGain, &backingLabel, &gainLabel})
        addAndMakeVisible(comp);
    title.setFont(uiFont(15.0f, true));
    title.setColour(juce::Label::textColourId, colours::text);
    statusLabel.setFont(uiFont(12.0f));
    statusLabel.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(title);
    addAndMakeVisible(statusLabel);
    const auto* c = mirror.model().findClip(clipId);
    const auto* li = mirror.model().findLibraryItem(c != nullptr ? c->libraryItemId : libraryItemId);
    title.setText("Scratch: " + (c != nullptr && c->name.isNotEmpty() ? c->name : li != nullptr ? li->displayName() : juce::String("audio")),
                  juce::dontSendNotification);

    playButton.onClick = [this] { control(status == "playing" ? "pause" : "play"); };
    startButton.setTooltip("Back to the start");
    startButton.onClick = [this] { control("seek", {{"positionUs", 0}}); };
    armButton.setClickingTogglesState(false);
    armButton.setTooltip("Arm: the take starts with your first move");
    armButton.onClick = [this] { control(armed ? "recordDisarm" : "recordArm"); };
    recordButton.setColour(juce::TextButton::buttonColourId, colours::red600);
    recordButton.onClick = [this] { control(status == "recording" ? "recordStop" : "recordStart"); };
    crossfader.setSliderStyle(juce::Slider::LinearHorizontal);
    crossfader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    crossfader.setRange(0.0, 1.0, 0.001);
    crossfader.setValue(1.0, juce::dontSendNotification);
    crossfader.setTooltip("Crossfader: cut the scratch in and out");
    crossfader.onValueChange = [this] { control("crossfader", {{"value", crossfader.getValue()}}); };
    realism.addItem("Realism: off", 1);
    realism.addItem("Realism: medium", 2);
    realism.addItem("Realism: high", 3);
    realism.setSelectedId(2, juce::dontSendNotification);
    realism.setTooltip("Vinyl character added to the scratch sound");
    realism.onChange = [this]
    {
        const juce::StringArray levels{"off", "medium", "high"};
        engine.send("SCRATCH_REALISM_SET", payload({{"level", levels[realism.getSelectedId() - 1]}}));
    };
    takeName.setTextToShowWhenEmpty("Name this scratch", colours::textFaint);
    saveSample.setColour(juce::TextButton::buttonColourId, colours::sky600);
    saveSample.onClick = [this] { saveAsSample(); };
    savePatternButton.onClick = [this] { savePattern(false); };
    applyButton.onClick = [this] { savePattern(true); };
    closeButton.onClick = [this] { close(); };
    for (auto* comp : std::initializer_list<juce::Component*>{&playButton, &startButton, &armButton, &recordButton, &crossfader, &realism,
                                                              &takeName, &saveSample, &savePatternButton, &applyButton, &closeButton})
        addAndMakeVisible(comp);
    savedLabel.setColour(juce::Label::textColourId, colours::textDim);
    savedLabel.setFont(uiFont(13.0f));
    savedList.setTextWhenNothingSelected("No saved patterns");
    savedList.onChange = [this] { timerCallback(); };
    loadSavedButton.onClick = [this] { loadSaved(); };
    renameSavedButton.onClick = [this] { renameSaved(); };
    deleteSavedButton.onClick = [this] { deleteSaved(); };
    for (auto* comp : std::initializer_list<juce::Component*>{&savedLabel, &savedList, &loadSavedButton, &renameSavedButton, &deleteSavedButton})
        addAndMakeVisible(comp);
    refreshSaved();

    subscriptions.on("SCRATCH_SESSION_STATE", [this](const juce::var& s) { state(s); });
    subscriptions.on("SCRATCH_PATTERN_RECORDED",
                     [this](const juce::var& p)
                     {
                         if (p.getProperty("sessionId", {}).toString() != sessionId) return;
                         pattern = p.getProperty("pattern", {});
                         loadedPatternId = {}; // a new take is a new pattern until saved
                         if (takeName.isEmpty()) takeName.setText(pattern.getProperty("name", "Scratch").toString(), false);
                         repaint();
                     });
    engine.send("SCRATCH_SESSION_OPEN", payload({{"protocolVersion", kProtocol},
                                                 {"clipId", clipId.isNotEmpty() ? juce::var(clipId) : juce::var()},
                                                 {"libraryItemId", clipId.isEmpty() ? juce::var(libraryItemId) : juce::var()}}));
    startTimerHz(30);
    setWantsKeyboardFocus(true);
    setSize(760, 640);
}

bool ScratchEditor::cueBuildBacking()
{
    if (openScratchEditor == nullptr) return false;
    if (openScratchEditor->backingButton.isEnabled()) openScratchEditor->backingButton.triggerClick();
    return true;
}

ScratchEditor::~ScratchEditor()
{
    if (openScratchEditor == this) openScratchEditor = nullptr;
    if (sessionId.isNotEmpty())
        engine.send("SCRATCH_SESSION_CLOSE", payload({{"protocolVersion", kProtocol}, {"sessionId", sessionId}}));
}

void ScratchEditor::show(EngineClient& engine, ProjectMirror& mirror, WaveformStore& waveforms, const juce::String& clipId,
                         const juce::String& libraryItemId, std::function<void(const juce::String&, bool)> notify)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new ScratchEditor(engine, mirror, waveforms, clipId, libraryItemId, std::move(notify)));
    options.dialogTitle = "Scratch Editor";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

juce::String ScratchEditor::sourceItemId() const
{
    if (const auto* c = mirror.model().findClip(clipId)) return c->libraryItemId;
    return libraryItemId;
}

void ScratchEditor::control(const juce::String& action, std::initializer_list<std::pair<const char*, juce::var>> props)
{
    if (sessionId.isEmpty()) return;
    auto p = payload(props);
    p.getDynamicObject()->setProperty("protocolVersion", kProtocol);
    p.getDynamicObject()->setProperty("sessionId", sessionId);
    p.getDynamicObject()->setProperty("action", action);
    engine.send("SCRATCH_SESSION_CONTROL", p);
}

void ScratchEditor::state(const juce::var& s)
{
    sessionId = s.getProperty("sessionId", {}).toString();
    status = s.getProperty("status", {}).toString();
    error = s.getProperty("error", {}).toString();
    progress = static_cast<double>(s.getProperty("preparationProgress", 0.0));
    positionUs = static_cast<juce::int64>(s.getProperty("positionUs", 0));
    durationUs = static_cast<juce::int64>(s.getProperty("durationUs", 0));
    platterTurns = static_cast<double>(s.getProperty("platterTurns", 0.0));
    playbackRate = static_cast<double>(s.getProperty("playbackRate", 0.0));
    armed = static_cast<bool>(s.getProperty("armed", false));
    replaying = static_cast<bool>(s.getProperty("replaying", false));
    replayPosition = static_cast<double>(s.getProperty("replayPositionNormalized", 0.0));
    backingStatus = s.getProperty("backingStatus", {}).toString();
    backingPositionUs = static_cast<juce::int64>(s.getProperty("backingPositionUs", 0));
    backingDurationUs = static_cast<juce::int64>(s.getProperty("backingDurationUs", 0));
    backingLoop.setToggleState(static_cast<bool>(s.getProperty("backingLoop", false)), juce::dontSendNotification);
    if (!backingGain.isMouseButtonDown()) backingGain.setValue(static_cast<double>(s.getProperty("backingGain", 1.0)), juce::dontSendNotification);
    if (!scratchGain.isMouseButtonDown()) scratchGain.setValue(static_cast<double>(s.getProperty("scratchMonitorGain", 1.0)), juce::dontSendNotification);
    if (s.hasProperty("backingError") && notify) notify("Backing: " + s.getProperty("backingError", {}).toString(), true);
    touched = static_cast<bool>(s.getProperty("touched", false));
    if (!crossfader.isMouseButtonDown()) crossfader.setValue(static_cast<double>(s.getProperty("crossfader", 1.0)), juce::dontSendNotification);
}

void ScratchEditor::timerCallback()
{
    refreshSaved();
    const bool picked = savedList.getSelectedId() > 0;
    loadSavedButton.setEnabled(picked && status != "recording");
    renameSavedButton.setEnabled(picked);
    deleteSavedButton.setEnabled(picked);
    savePatternButton.setButtonText(loadedPatternId.isNotEmpty() ? "Update pattern" : "Keep pattern");
    const bool ready = sessionId.isNotEmpty() && status != "preparing" && status != "error";
    playButton.setButtonText(status == "playing" ? "Pause" : "Play");
    playButton.setEnabled(ready && status != "recording");
    armButton.setEnabled(ready && status != "recording");
    armButton.setColour(juce::TextButton::buttonColourId, armed ? colours::amber500 : colours::zinc800);
    recordButton.setEnabled(ready);
    recordButton.setButtonText(status == "recording" ? "Stop" : "Record");
    const bool hasTake = pattern.isObject();
    for (auto* b : {&saveSample, &savePatternButton}) b->setEnabled(hasTake && status != "recording");
    applyButton.setEnabled(hasTake && clipId.isNotEmpty() && status != "recording");
    takeName.setEnabled(hasTake);
    statusLabel.setText(status == "preparing" ? "Preparing the record... " + juce::String(juce::roundToInt(progress * 100.0)) + "%"
                        : status == "error"   ? "Scratch error: " + error
                        : sessionId.isEmpty() ? juce::String("Opening...")
                                              : clock(positionUs) + " / " + clock(durationUs) + "   " + status
                                                    + (std::abs(playbackRate) > 0.01 ? "   x" + juce::String(playbackRate, 2) : juce::String()),
                        juce::dontSendNotification);
    replayButton.setEnabled(hasTake && status != "recording");
    replayButton.setButtonText(replaying ? "Stop take" : "Play take");
    // One control carries the whole lifecycle: Build, Building, Ready (click to rebuild), Retry.
    const auto audible = audibleBackingTracks();
    backingButton.setButtonText(backingStatus == "preparing" ? "Building" : backingStatus == "error" ? "Retry"
                                : backingStatus == "ready"   ? "Ready"
                                                             : "Build");
    backingButton.setEnabled(ready && !audible.empty() && backingStatus != "preparing");
    const int total = static_cast<int>(mirror.model().tracks.size());
    backingTracksButton.setButtonText(total == 0 ? juce::String("No tracks")
                                                 : juce::String(static_cast<int>(audible.size())) + " of " + juce::String(total) + " tracks");
    // The prepared bed is fixed while it plays; its makeup changes only with a rebuild.
    const bool bedPlaying = backingPositionUs > 0 && status == "playing";
    for (auto* c : std::initializer_list<juce::Component*>{&backingTracksButton, &backingFrom, &backingLength})
        c->setEnabled(ready && !bedPlaying);
    backingClear.setEnabled(ready && backingStatus.isNotEmpty() && backingStatus != "none");
    backingLabel.setText(backingStatus.isEmpty() || backingStatus == "none" ? juce::String("No backing")
                         : "Backing " + backingStatus + (backingDurationUs > 0 ? "  " + clock(backingPositionUs) + " / " + clock(backingDurationUs) : juce::String()),
                         juce::dontSendNotification);
    deck->repaint();
    notation->repaint();
    repaint(waveArea);
}

void ScratchEditor::saveAsSample()
{
    if (!pattern.isObject() || sessionId.isEmpty()) return;
    auto saved = juce::JSON::parse(juce::JSON::toString(pattern)); // a copy we can rename
    const auto name = takeName.getText().trim().isNotEmpty() ? takeName.getText().trim() : juce::String("Scratch");
    saved.getDynamicObject()->setProperty("name", name);
    const auto id = saved.getProperty("id", juce::Uuid().toDashedString()).toString();
    const auto* c = mirror.model().findClip(clipId);
    engine.send("SCRATCH_SAVE_AS_SAMPLE",
                payload({{"protocolVersion", kProtocol},
                         {"sessionId", sessionId},
                         {"itemId", "scratch-" + id},
                         {"sampleName", name},
                         {"sourceItemId", sourceItemId().isNotEmpty() ? juce::var(sourceItemId()) : juce::var()},
                         {"sourceInMs", c != nullptr ? juce::var(c->inMs) : juce::var()},
                         {"sourceDurationMs", c != nullptr ? juce::var(c->durationMs) : juce::var()},
                         {"pattern", saved}}));
    if (notify) notify("Saved \"" + name + "\" to the library", false);
}

void ScratchEditor::savePattern(bool applyToClip)
{
    if (!pattern.isObject() || sessionId.isEmpty()) return;
    auto saved = juce::JSON::parse(juce::JSON::toString(pattern));
    const auto name = takeName.getText().trim().isNotEmpty() ? takeName.getText().trim() : juce::String("Scratch");
    saved.getDynamicObject()->setProperty("name", name);
    if (loadedPatternId.isNotEmpty()) saved.getDynamicObject()->setProperty("id", loadedPatternId);
    loadedPatternId = saved.getProperty("id", {}).toString();
    engine.inUndoGroup(applyToClip ? "Apply scratch" : "Save scratch pattern",
                       [&]
                       {
                           engine.send("SCRATCH_PATTERN_SAVE", payload({{"protocolVersion", kProtocol}, {"sessionId", sessionId}, {"pattern", saved}}));
                           if (applyToClip && clipId.isNotEmpty())
                               engine.send("SCRATCH_PATTERN_APPLY", payload({{"protocolVersion", kProtocol},
                                                                             {"clipId", clipId},
                                                                             {"patternId", saved.getProperty("id", {})}}));
                       });
    if (notify) notify(applyToClip ? "Scratch applied to the clip" : "Scratch pattern kept with the project", false);
}

juce::Array<juce::var> ScratchEditor::savedPatterns() const
{
    if (const auto* list = mirror.model().raw.getProperty("scratchPatterns", {}).getArray()) return *list;
    return {};
}

void ScratchEditor::refreshSaved()
{
    const auto patterns = savedPatterns();
    juce::String signature;
    for (const auto& p : patterns) signature << p.getProperty("id", {}).toString() << "=" << p.getProperty("name", {}).toString() << ";";
    if (signature == savedSignature) return;
    savedSignature = signature;
    const auto keep = savedList.getSelectedId() > 0 ? savedList.getText() : juce::String();
    savedList.clear(juce::dontSendNotification);
    for (int i = 0; i < patterns.size(); ++i)
    {
        const auto name = patterns[i].getProperty("name", "Untitled Scratch").toString();
        savedList.addItem(name, i + 1);
        if (name == keep) savedList.setSelectedId(i + 1, juce::dontSendNotification);
    }
}

void ScratchEditor::loadSaved()
{
    const auto patterns = savedPatterns();
    const int index = savedList.getSelectedId() - 1;
    if (index < 0 || index >= patterns.size()) return;
    // The saved pattern becomes the take: replay, save and apply all work on it.
    pattern = juce::JSON::parse(juce::JSON::toString(patterns[index]));
    loadedPatternId = pattern.getProperty("id", {}).toString();
    takeName.setText(pattern.getProperty("name", "Scratch").toString(), false);
    notation->repaint();
    repaint();
}

void ScratchEditor::renameSaved()
{
    const auto patterns = savedPatterns();
    const int index = savedList.getSelectedId() - 1;
    if (index < 0 || index >= patterns.size()) return;
    const auto id = patterns[index].getProperty("id", {}).toString();
    auto* window = new juce::AlertWindow("Rename pattern", "A new name for this scratch pattern.", juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("name", patterns[index].getProperty("name", {}).toString());
    window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create(
                                      [safe = juce::Component::SafePointer<ScratchEditor>(this), window, id](int result)
                                      {
                                          const auto name = window->getTextEditorContents("name").trim();
                                          if (safe == nullptr || result != 1 || name.isEmpty()) return;
                                          safe->engine.send("SCRATCH_PATTERN_RENAME",
                                                            payload({{"protocolVersion", kProtocol}, {"patternId", id}, {"name", name}}));
                                          if (safe->loadedPatternId == id) safe->takeName.setText(name, false);
                                      }),
                           true);
}

void ScratchEditor::deleteSaved()
{
    const auto patterns = savedPatterns();
    const int index = savedList.getSelectedId() - 1;
    if (index < 0 || index >= patterns.size()) return;
    const auto id = patterns[index].getProperty("id", {}).toString();
    const auto name = patterns[index].getProperty("name", {}).toString();
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::WarningIcon)
                                     .withTitle("Delete pattern")
                                     .withMessage("Delete the saved scratch pattern \"" + name + "\"?")
                                     .withButton("Delete")
                                     .withButton("Cancel"),
                                 [safe = juce::Component::SafePointer<ScratchEditor>(this), id](int result)
                                 {
                                     if (safe == nullptr || result != 1) return;
                                     safe->engine.send("SCRATCH_PATTERN_DELETE", payload({{"protocolVersion", kProtocol}, {"patternId", id}}));
                                     if (safe->loadedPatternId == id) safe->loadedPatternId = {};
                                 });
}

std::vector<juce::String> ScratchEditor::audibleBackingTracks() const
{
    // Seeded once with every track but the one being scratched; silenced tracks never join.
    const auto& model = mirror.model();
    if (!backingTracksSeeded && !model.tracks.empty())
    {
        const auto* c = model.findClip(clipId);
        for (const auto& t : model.tracks)
            if (c == nullptr || t.id != c->trackId) backingTrackIds.insert(t.id);
        backingTracksSeeded = true;
    }
    bool anySoloed = false;
    for (const auto& t : model.tracks) anySoloed = anySoloed || t.soloed;
    std::vector<juce::String> out;
    for (const auto& t : model.tracks)
        if (backingTrackIds.count(t.id) != 0 && !t.muted && (!anySoloed || t.soloed)) out.push_back(t.id);
    return out;
}

void ScratchEditor::showBackingTracksMenu()
{
    audibleBackingTracks(); // seeds the default selection
    bool anySoloed = false;
    for (const auto& t : mirror.model().tracks) anySoloed = anySoloed || t.soloed;
    juce::PopupMenu menu;
    for (const auto& t : mirror.model().tracks)
    {
        const bool silenced = t.muted || (anySoloed && !t.soloed);
        menu.addItem(t.name + (silenced ? "  (muted)" : ""), !silenced, !silenced && backingTrackIds.count(t.id) != 0,
                     [this, id = t.id]
                     {
                         if (!backingTrackIds.erase(id)) backingTrackIds.insert(id);
                         timerCallback();
                     });
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&backingTracksButton));
}

void ScratchEditor::close()
{
    if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
}

bool ScratchEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        control(status == "playing" ? "pause" : "play");
        return true;
    }
    return false;
}

void ScratchEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    // The source with the needle position.
    g.setColour(colours::zinc950);
    g.fillRect(waveArea);
    if (const auto* li = mirror.model().findLibraryItem(sourceItemId()))
        if (const auto peaks = waveforms.get(li->audioPath()))
        {
            const auto* c = mirror.model().findClip(clipId);
            const double inMs = c != nullptr ? c->inMs : 0.0;
            const double lengthMs = c != nullptr ? c->durationMs : li->durationMs;
            drawPeaks(g, *peaks, waveArea.toFloat().reduced(0.0f, 3.0f), inMs, lengthMs, false, colours::sky400.withAlpha(0.8f));
        }
    if (durationUs > 0)
    {
        const auto x = waveArea.getX() + static_cast<float>(positionUs) / static_cast<float>(durationUs) * waveArea.getWidth();
        g.setColour(colours::red500);
        g.drawLine(x, static_cast<float>(waveArea.getY()), x, static_cast<float>(waveArea.getBottom()), 1.5f);
    }
    if (pattern.isObject())
    {
        g.setColour(colours::emerald500);
        g.setFont(uiFont(12.0f));
        g.drawText("Take recorded: " + clock(static_cast<juce::int64>(pattern.getProperty("durationUs", 0))), waveArea.translated(0, waveArea.getHeight() + 2).withHeight(16),
                   juce::Justification::centredLeft);
    }
}

void ScratchEditor::resized()
{
    auto area = getLocalBounds().reduced(14);
    auto header = area.removeFromTop(26);
    realism.setBounds(header.removeFromRight(150).reduced(0, 1));
    title.setBounds(header);
    statusLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);
    waveArea = area.removeFromTop(60);
    area.removeFromTop(22);
    auto bottom = area.removeFromBottom(30);
    area.removeFromBottom(6);
    auto savedRow = area.removeFromBottom(28);
    savedLabel.setBounds(savedRow.removeFromLeft(50));
    deleteSavedButton.setBounds(savedRow.removeFromRight(70));
    savedRow.removeFromRight(6);
    renameSavedButton.setBounds(savedRow.removeFromRight(90));
    savedRow.removeFromRight(6);
    loadSavedButton.setBounds(savedRow.removeFromRight(70));
    savedRow.removeFromRight(8);
    savedList.setBounds(savedRow.reduced(0, 2));
    closeButton.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(8);
    applyButton.setBounds(bottom.removeFromRight(110));
    bottom.removeFromRight(6);
    savePatternButton.setBounds(bottom.removeFromRight(110));
    bottom.removeFromRight(6);
    saveSample.setBounds(bottom.removeFromRight(120));
    bottom.removeFromRight(8);
    takeName.setBounds(bottom.reduced(0, 2));
    area.removeFromBottom(10);
    auto controls = area.removeFromBottom(30);
    startButton.setBounds(controls.removeFromLeft(40));
    controls.removeFromLeft(4);
    playButton.setBounds(controls.removeFromLeft(80));
    controls.removeFromLeft(12);
    armButton.setBounds(controls.removeFromLeft(70));
    controls.removeFromLeft(4);
    recordButton.setBounds(controls.removeFromLeft(90));
    controls.removeFromLeft(16);
    crossfader.setBounds(controls);
    area.removeFromBottom(10);
    auto backingRow = area.removeFromBottom(26);
    backingLoop.setBounds(backingRow.removeFromLeft(60));
    backingGain.setBounds(backingRow.removeFromLeft(110));
    backingRow.removeFromLeft(8);
    gainLabel.setBounds(backingRow.removeFromLeft(80));
    scratchGain.setBounds(backingRow.removeFromLeft(110));
    backingLabel.setBounds(backingRow);
    area.removeFromBottom(6);
    auto buildRow = area.removeFromBottom(26);
    backingHeading.setBounds(buildRow.removeFromLeft(60));
    backingTracksButton.setBounds(buildRow.removeFromLeft(130));
    buildRow.removeFromLeft(4);
    backingFrom.setBounds(buildRow.removeFromLeft(130).reduced(0, 1));
    buildRow.removeFromLeft(4);
    backingLength.setBounds(buildRow.removeFromLeft(80).reduced(0, 1));
    buildRow.removeFromLeft(8);
    backingButton.setBounds(buildRow.removeFromLeft(90));
    buildRow.removeFromLeft(4);
    backingClear.setBounds(buildRow.removeFromLeft(60));
    area.removeFromBottom(8);
    auto notationRow = area.removeFromBottom(70);
    replayButton.setBounds(notationRow.removeFromRight(90).withSizeKeepingCentre(90, 28));
    notationRow.removeFromRight(6);
    notation->setBounds(notationRow);
    area.removeFromBottom(8);
    deck->setBounds(area);
}

} // namespace stemcel::native
