#include "ClipEditor.h"
#include "WarpModeDefaults.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace stemcel::native
{
namespace
{
constexpr int kRulerPx = 22;
constexpr double kMinWindowMs = 20.0;
const juce::StringArray kWarpModes{"rhythmic", "tonal", "complex", "pro", "repitch"};
const juce::StringArray kWarpModeLabels{"Rhythmic (beats)", "Tonal (melodic)", "Complex (full mix)", "Pro (full mix, Signalsmith)", "Re-Pitch (master tempo off)"};
const std::vector<std::pair<double, juce::String>> kSlices{{4.0, "1 bar"}, {2.0, "1/2 bar"}, {1.0, "1/4"}, {0.5, "1/8"}, {0.25, "1/16"}};

juce::var payload(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props) o->setProperty(k, v); // void kept: `tempoRatio: null` means follow
    return juce::var(o);
}

juce::String time(double ms)
{
    const double s = ms / 1000.0;
    return juce::String(static_cast<int>(s) / 60) + ":" + juce::String(std::fmod(s, 60.0), 3).paddedLeft('0', 6);
}

// pitchKey.ts shiftedKey: the key a pitch shift lands on ("C minor" +2 -> "D minor").
juce::String shiftedKey(const juce::String& sourceKey, int semitones, int cents)
{
    static const juce::StringArray notes{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const std::map<juce::String, juce::String> aliases{{"B#", "C"}, {"Db", "C#"}, {"Eb", "D#"}, {"E#", "F"},
                                                              {"Gb", "F#"}, {"Ab", "G#"}, {"Bb", "A#"}, {"Cb", "B"}};
    const auto t = sourceKey.trim();
    if (t.isEmpty() || !juce::String("ABCDEFGabcdefg").containsChar(t[0])) return {};
    juce::String note = juce::String::charToString(juce::CharacterFunctions::toUpperCase(t[0]));
    if (t.length() > 1 && (t[1] == '#' || t[1] == 'b')) note += juce::String::charToString(t[1]);
    if (const auto a = aliases.find(note); a != aliases.end()) note = a->second;
    const int pitchClass = notes.indexOf(note);
    if (pitchClass < 0) return {};
    const int at = note.length();
    const bool minor = t.containsIgnoreCase("minor") || (t.length() > at && t[at] == 'm' && !t.substring(at).startsWithIgnoreCase("maj"));
    const int index = ((pitchClass + semitones) % 12 + 12) % 12;
    return notes[index] + (minor ? " minor" : " major") + (cents == 0 ? juce::String() : " " + juce::String(cents > 0 ? "+" : "") + juce::String(cents) + "c");
}

void setupSlider(juce::Slider& s, double min, double max, double step)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    s.setRange(min, max, step);
}
} // namespace

// ── Waveform with trim window, beat grid and preview playhead ───────────────
class ClipEditor::WaveView final : public juce::Component, private juce::ChangeListener
{
  public:
    explicit WaveView(ClipEditor& owner) : editor(owner) { editor.waveforms.addChangeListener(this); }
    ~WaveView() override { editor.waveforms.removeChangeListener(this); }

    double viewInMs = 0.0;
    double viewDurationMs = 1000.0;
    std::function<void()> onDraftChanged;

    void fit()
    {
        const auto* item = editor.item();
        const double total = item != nullptr ? item->durationMs : editor.draft.durationMs;
        // Show the window with some context either side.
        const double pad = juce::jmax(500.0, editor.draft.durationMs * 0.1);
        viewInMs = juce::jmax(0.0, editor.draft.inMs - pad);
        viewDurationMs = juce::jmin(total - viewInMs, editor.draft.durationMs + pad * 2.0);
        if (viewDurationMs <= 0.0) viewDurationMs = juce::jmax(1000.0, total);
        repaint();
    }

    void zoom(double factor, double anchorMs)
    {
        const auto* item = editor.item();
        const double total = item != nullptr ? item->durationMs : viewDurationMs;
        const double next = juce::jlimit(50.0, juce::jmax(50.0, total), viewDurationMs / factor);
        const double frac = (anchorMs - viewInMs) / viewDurationMs;
        viewInMs = juce::jlimit(0.0, juce::jmax(0.0, total - next), anchorMs - frac * next);
        viewDurationMs = next;
        repaint();
    }

    double xToMs(float x) const { return viewInMs + static_cast<double>(x) / juce::jmax(1, getWidth()) * viewDurationMs; }
    float msToX(double ms) const { return static_cast<float>((ms - viewInMs) / viewDurationMs * getWidth()); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::zinc950);
        const auto* item = editor.item();
        const auto body = getLocalBounds().toFloat().withTrimmedTop(static_cast<float>(kRulerPx));
        // The trim window.
        const auto x0 = msToX(editor.draft.inMs);
        const auto x1 = msToX(editor.draft.inMs + editor.draft.durationMs);
        g.setColour(colours::zinc800);
        g.fillRect(juce::Rectangle<float>(x0, body.getY(), x1 - x0, body.getHeight()));
        if (item != nullptr)
            if (const auto peaks = editor.waveforms.get(item->audioPath()))
                drawPeaks(g, *peaks, body.reduced(0.0f, 4.0f), viewInMs, viewDurationMs, editor.draft.reversed,
                          colours::sky400.withAlpha(0.85f));

        // Beat grid: bars brighter, the first downbeat (the anchor) amber.
        const auto beats = editor.gridBeatsMs();
        for (size_t i = 0; i < beats.size(); ++i)
        {
            const auto x = msToX(beats[i]);
            if (x < 0.0f || x > static_cast<float>(getWidth())) continue;
            const bool anchor = std::abs(beats[i] - editor.draft.anchorSec * 1000.0) < 0.5;
            const bool bar = (i % 4) == (static_cast<size_t>(std::llround(anchorIndex(beats))) % 4);
            g.setColour(anchor ? colours::amber400 : bar ? colours::zinc400.withAlpha(0.7f) : colours::zinc600.withAlpha(0.5f));
            g.drawVerticalLine(static_cast<int>(x), body.getY(), body.getBottom());
        }
        // Slice markers.
        for (const double m : editor.sliceMarkers)
        {
            const auto x = msToX(m);
            g.setColour(colours::emerald500);
            g.drawLine(x, body.getY(), x, body.getBottom(), 1.5f);
        }
        // Ruler with the anchor handle.
        g.setColour(colours::zinc900);
        g.fillRect(getLocalBounds().withHeight(kRulerPx));
        g.setColour(colours::textFaint);
        g.setFont(uiFont(10.0f));
        // Labels at least ~90 px apart, on 1/2/5 steps.
        const double minStep = viewDurationMs * 90.0 / juce::jmax(1, getWidth());
        double step = std::pow(10.0, std::floor(std::log10(juce::jmax(1.0, minStep))));
        for (const double m : {1.0, 2.0, 5.0, 10.0})
            if (step * m >= minStep)
            {
                step *= m;
                break;
            }
        for (double t = std::ceil(viewInMs / step) * step; t < viewInMs + viewDurationMs; t += step)
            g.drawText(time(t), static_cast<int>(msToX(t)) + 2, 2, 70, 12, juce::Justification::left);
        const auto ax = msToX(editor.draft.anchorSec * 1000.0);
        juce::Path handle;
        handle.addTriangle(ax - 6.0f, 8.0f, ax + 6.0f, 8.0f, ax, static_cast<float>(kRulerPx));
        g.setColour(colours::amber400);
        g.fillPath(handle);

        // Window edges.
        g.setColour(colours::zinc100);
        g.drawLine(x0, body.getY(), x0, body.getBottom(), 2.0f);
        g.drawLine(x1, body.getY(), x1, body.getBottom(), 2.0f);
        g.setColour(colours::zinc950.withAlpha(0.55f));
        g.fillRect(juce::Rectangle<float>(0.0f, body.getY(), juce::jmax(0.0f, x0), body.getHeight()));
        g.fillRect(juce::Rectangle<float>(x1, body.getY(), juce::jmax(0.0f, static_cast<float>(getWidth()) - x1), body.getHeight()));

        // Playback selection.
        if (editor.hasSelection())
        {
            const auto s0 = msToX(editor.selInMs), s1 = msToX(editor.selInMs + editor.selDurMs);
            g.setColour(colours::sky500.withAlpha(0.18f));
            g.fillRect(juce::Rectangle<float>(s0, body.getY(), s1 - s0, body.getHeight()));
            g.setColour(colours::sky400);
            g.drawLine(s0, body.getY(), s0, body.getBottom(), 1.5f);
            g.drawLine(s1, body.getY(), s1, body.getBottom(), 1.5f);
        }

        // Volume shape, over the window; handles while editing it.
        const auto& env = editor.draft.envelope;
        if (!env.empty() && (editor.volumeMode || !envelope::isFlatUnity(env)))
        {
            juce::Path line;
            for (size_t i = 0; i < env.size(); ++i)
            {
                const auto p = pointXY(env[i], body);
                if (i == 0) line.startNewSubPath(p);
                else line.lineTo(p);
            }
            g.setColour(colours::amber400.withAlpha(editor.volumeMode ? 0.95f : 0.6f));
            g.strokePath(line, juce::PathStrokeType(1.5f));
            if (editor.volumeMode)
                for (const auto& point : env)
                {
                    const auto p = pointXY(point, body);
                    g.setColour(colours::zinc950);
                    g.fillEllipse(p.x - 4.0f, p.y - 4.0f, 8.0f, 8.0f);
                    g.setColour(colours::amber400);
                    g.drawEllipse(p.x - 4.0f, p.y - 4.0f, 8.0f, 8.0f, 1.5f);
                }
        }

        // Preview playhead (preview positions are relative to the window).
        if (editor.previewPlaying || editor.previewLoaded)
        {
            const auto px = msToX(editor.draft.inMs + editor.previewPositionMs);
            g.setColour(colours::red500);
            g.drawLine(px, 0.0f, px, static_cast<float>(getHeight()), 1.5f);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const double ms = juce::jmax(0.0, xToMs(static_cast<float>(e.x)));
        const auto x0 = msToX(editor.draft.inMs);
        const auto x1 = msToX(editor.draft.inMs + editor.draft.durationMs);
        if (editor.sliceMode && e.y >= kRulerPx)
        {
            auto& markers = editor.sliceMarkers;
            if (e.mods.isAltDown())
            {
                // Alt-click removes the nearest marker within reach.
                const double reach = 6.0 / juce::jmax(1, getWidth()) * viewDurationMs;
                for (auto it = markers.begin(); it != markers.end(); ++it)
                    if (std::abs(*it - ms) <= reach)
                    {
                        markers.erase(it);
                        break;
                    }
            }
            else
            {
                markers.push_back(snapToBeat(ms, e.mods.isShiftDown()));
            }
            dragging = Drag::none;
            repaint();
            if (onDraftChanged) onDraftChanged();
            return;
        }
        if (editor.volumeMode && e.y >= kRulerPx)
        {
            volumeDown(e);
            return;
        }
        const auto s0 = msToX(editor.selInMs), s1 = msToX(editor.selInMs + editor.selDurMs);
        if (e.y < kRulerPx)
        {
            dragging = Drag::anchor;
        }
        else if (editor.hasSelection() && std::abs(static_cast<float>(e.x) - s0) < 8.0f)
        {
            dragging = Drag::selStart;
        }
        else if (editor.hasSelection() && std::abs(static_cast<float>(e.x) - s1) < 8.0f)
        {
            dragging = Drag::selEnd;
        }
        else if (std::abs(static_cast<float>(e.x) - x0) < 6.0f)
        {
            dragging = Drag::inEdge;
        }
        else if (std::abs(static_cast<float>(e.x) - x1) < 6.0f)
        {
            dragging = Drag::outEdge;
        }
        else
        {
            dragging = Drag::select;
            selectFrom = snapToBeat(ms, e.mods.isAltDown());
        }
        mouseDrag(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const auto* item = editor.item();
        const double total = item != nullptr ? item->durationMs : 1e12;
        const double ms = juce::jlimit(0.0, total, xToMs(static_cast<float>(e.x)));
        const bool free = e.mods.isAltDown();
        auto& d = editor.draft;
        switch (dragging)
        {
        case Drag::anchor:
            d.anchorSec = ms / 1000.0;
            break;
        case Drag::inEdge:
        {
            const double end = d.inMs + d.durationMs;
            d.inMs = juce::jlimit(0.0, end - kMinWindowMs, snapToBeat(ms, free));
            d.durationMs = end - d.inMs;
            break;
        }
        case Drag::outEdge:
            d.durationMs = juce::jlimit(kMinWindowMs, total - d.inMs, snapToBeat(ms, free) - d.inMs);
            break;
        case Drag::select:
        {
            // A drag selects what plays inside the window; it never re-trims the clip.
            if (e.getDistanceFromDragStart() < 3) return;
            const double lo = d.inMs, hi = d.inMs + d.durationMs;
            const double to = juce::jlimit(lo, hi, snapToBeat(ms, free));
            const double from = juce::jlimit(lo, hi, selectFrom);
            editor.selInMs = juce::jmin(from, to);
            editor.selDurMs = juce::jmax(kMinSelectionMs, std::abs(to - from));
            break;
        }
        case Drag::selStart:
        {
            const double end = editor.selInMs + editor.selDurMs;
            editor.selInMs = juce::jlimit(d.inMs, end - kMinSelectionMs, snapToBeat(ms, free));
            editor.selDurMs = end - editor.selInMs;
            break;
        }
        case Drag::selEnd:
            editor.selDurMs = juce::jlimit(kMinSelectionMs, d.inMs + d.durationMs - editor.selInMs, snapToBeat(ms, free) - editor.selInMs);
            break;
        case Drag::volume:
            if (volumeIndex >= 0) moveVolumePoint(e);
            return;
        case Drag::none: return;
        }
        repaint();
        if (onDraftChanged) onDraftChanged();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        // A click (no drag) moves the preview playhead there; outside a selection it also
        // clears it (useClipEditorCanvasInteraction onUp).
        if (dragging == Drag::select && e.getDistanceFromDragStart() < 3)
        {
            const double ms = xToMs(static_cast<float>(e.x));
            if (editor.hasSelection() && (ms < editor.selInMs || ms > editor.selInMs + editor.selDurMs)) editor.clearSelection();
            editor.seekPreview(ms - editor.draft.inMs);
        }
        else if (dragging == Drag::select || dragging == Drag::selStart || dragging == Drag::selEnd)
            editor.applyPreviewLoop();
        else if (dragging == Drag::volume)
            editor.pushPreviewDraft();
        else if (dragging != Drag::none)
            editor.restartPreviewIfPlaying();
        volumeIndex = -1;
        dragging = Drag::none;
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown() || std::abs(w.deltaY) > std::abs(w.deltaX))
            zoom(w.deltaY > 0 ? 1.25 : 0.8, xToMs(static_cast<float>(e.x)));
        else
        {
            const auto* item = editor.item();
            const double total = item != nullptr ? item->durationMs : viewDurationMs;
            viewInMs = juce::jlimit(0.0, juce::jmax(0.0, total - viewDurationMs), viewInMs - w.deltaX * viewDurationMs * 0.5);
            repaint();
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const auto x0 = msToX(editor.draft.inMs);
        const auto x1 = msToX(editor.draft.inMs + editor.draft.durationMs);
        const bool edge = std::abs(static_cast<float>(e.x) - x0) < 6.0f || std::abs(static_cast<float>(e.x) - x1) < 6.0f;
        setMouseCursor(e.y < kRulerPx ? juce::MouseCursor::DraggingHandCursor
                       : edge         ? juce::MouseCursor::LeftRightResizeCursor
                                      : juce::MouseCursor::IBeamCursor);
    }

  private:
    enum class Drag
    {
        none,
        anchor,
        inEdge,
        outEdge,
        select,
        selStart,
        selEnd,
        volume,
    };
    static constexpr double kMinSelectionMs = 50.0;
    static constexpr double kMaxGain = 1.5;

    // Window timeline ms <-> source ms through the draft's stretch (volumeOverlay.ts).
    juce::Point<float> pointXY(const EnvelopePoint& p, juce::Rectangle<float> body) const
    {
        const double ratio = juce::jmax(1e-6, editor.previewRatio());
        const float x = msToX(editor.draft.inMs + p.timeMs * ratio);
        const float y = body.getBottom() - static_cast<float>(juce::jlimit(0.0, kMaxGain, p.gain) / kMaxGain) * body.getHeight();
        return {x, y};
    }

    EnvelopePoint pointAt(const juce::MouseEvent& e, bool snap) const
    {
        const auto body = getLocalBounds().toFloat().withTrimmedTop(static_cast<float>(kRulerPx));
        const double ratio = juce::jmax(1e-6, editor.previewRatio());
        double sourceMs = xToMs(static_cast<float>(e.x));
        if (snap) sourceMs = snapToBeat(sourceMs, false);
        const double t = juce::jlimit(0.0, editor.envelopeDurationMs(), (sourceMs - editor.draft.inMs) / ratio);
        const double gain = juce::jlimit(0.0, kMaxGain, (body.getBottom() - e.position.y) / body.getHeight() * kMaxGain);
        return {t, gain};
    }

    // Click a handle to drag it, empty space to add one (Shift snaps to the beat grid);
    // Alt- or right-click removes a handle. The two ends stay pinned in time.
    void volumeDown(const juce::MouseEvent& e)
    {
        editor.ensureEnvelope();
        auto& env = editor.draft.envelope;
        const auto body = getLocalBounds().toFloat().withTrimmedTop(static_cast<float>(kRulerPx));
        int hit = -1;
        for (int i = 0; i < static_cast<int>(env.size()); ++i)
            if (pointXY(env[static_cast<size_t>(i)], body).getDistanceFrom(e.position) <= 8.0f) hit = i;
        const bool endpoint = hit == 0 || hit == static_cast<int>(env.size()) - 1;
        if (hit >= 0 && (e.mods.isAltDown() || e.mods.isPopupMenu()))
        {
            if (!endpoint) env.erase(env.begin() + hit);
            dragging = Drag::none;
            repaint();
            editor.pushPreviewDraft();
            return;
        }
        if (e.mods.isPopupMenu()) return;
        if (hit < 0)
        {
            const auto p = pointAt(e, e.mods.isShiftDown());
            auto at = std::upper_bound(env.begin(), env.end(), p.timeMs, [](double t, const EnvelopePoint& q) { return t < q.timeMs; });
            if (at == env.begin()) at = env.begin() + 1;
            if (at == env.end()) at = env.end() - 1;
            hit = static_cast<int>(env.insert(at, p) - env.begin());
        }
        volumeIndex = hit;
        dragging = Drag::volume;
        repaint();
    }

    void moveVolumePoint(const juce::MouseEvent& e)
    {
        auto& env = editor.draft.envelope;
        if (volumeIndex < 0 || volumeIndex >= static_cast<int>(env.size())) return;
        auto p = pointAt(e, e.mods.isShiftDown());
        const auto i = static_cast<size_t>(volumeIndex);
        if (i == 0) p.timeMs = 0.0;
        else if (i + 1 == env.size()) p.timeMs = editor.envelopeDurationMs();
        else p.timeMs = juce::jlimit(env[i - 1].timeMs, env[i + 1].timeMs, p.timeMs); // keeps order
        env[i] = p;
        repaint();
    }

    int volumeIndex = -1;

    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

    double anchorIndex(const std::vector<double>& beats) const
    {
        for (size_t i = 0; i < beats.size(); ++i)
            if (std::abs(beats[i] - editor.draft.anchorSec * 1000.0) < 0.5) return static_cast<double>(i);
        return 0.0;
    }

    double snapToBeat(double ms, bool free) const
    {
        if (free) return ms;
        const auto beats = editor.gridBeatsMs();
        double best = ms;
        double bestDist = 8.0 / juce::jmax(1, getWidth()) * viewDurationMs; // 8 px magnet
        for (const double b : beats)
            if (std::abs(b - ms) < bestDist)
            {
                bestDist = std::abs(b - ms);
                best = b;
            }
        return best;
    }

    ClipEditor& editor;
    Drag dragging = Drag::none;
    double selectFrom = 0.0;
};

// ── Editor ─────────────────────────────────────────────────────────────────
ClipEditor::ClipEditor(EngineClient& client, ProjectMirror& projectMirror, TimelineActions& timelineActions,
                       WaveformStore& waveformStore, const juce::String& id, const juce::String& itemId)
    : engine(client), mirror(projectMirror), actions(timelineActions), waveforms(waveformStore), subscriptions(client),
      clipId(id), libraryItemId(itemId)
{
    wave = std::make_unique<WaveView>(*this);
    wave->onDraftChanged = [this] { draftToControls(); };
    addAndMakeVisible(*wave);

    title.setFont(uiFont(15.0f, true));
    title.setColour(juce::Label::textColourId, colours::text);
    info.setFont(uiFont(12.0f));
    info.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(title);
    addAndMakeVisible(info);

    playButton.onClick = [this] { togglePlay(); };
    skipStartButton.setTooltip("Back to the start");
    skipEndButton.setTooltip("To the end");
    skipStartButton.onClick = [this] { seekPreview(0.0); };
    skipEndButton.onClick = [this] { seekPreview(juce::jmax(0.0, draft.durationMs - 1.0)); };
    addAndMakeVisible(skipStartButton);
    addAndMakeVisible(skipEndButton);
    loopButton.setClickingTogglesState(true);
    loopButton.setToggleState(loop, juce::dontSendNotification);
    loopButton.onClick = [this]
    {
        loop = loopButton.getToggleState();
        applyPreviewLoop();
    };
    metronomeButton.setClickingTogglesState(true);
    metronomeButton.onClick = [this]
    {
        metronome = metronomeButton.getToggleState();
        pushPreviewDraft();
    };
    zoomInButton.onClick = [this] { wave->zoom(1.5, draft.inMs + draft.durationMs / 2.0); };
    zoomOutButton.onClick = [this] { wave->zoom(1.0 / 1.5, draft.inMs + draft.durationMs / 2.0); };
    fitButton.onClick = [this] { wave->fit(); };
    for (auto* b : {&playButton, &loopButton, &metronomeButton, &zoomInButton, &zoomOutButton, &fitButton}) addAndMakeVisible(b);

    tempoToggle.onClick = [this] { controlsChanged(); };
    tempoMode.addItem("Follow project tempo", 1);
    tempoMode.addItem("Fixed BPM", 2);
    tempoMode.addItem("Stretch %", 3);
    tempoMode.onChange = [this] { controlsChanged(); };
    for (int i = 0; i < kWarpModes.size(); ++i) warpMode.addItem(kWarpModeLabels[i], i + 1);
    warpMode.onChange = [this] { controlsChanged(); };
    setupSlider(fixedBpm, 20.0, 300.0, 0.01);
    setupSlider(semitones, -12.0, 12.0, 1.0);
    setupSlider(cents, -100.0, 100.0, 1.0);
    setupSlider(gridBpm, 20.0, 300.0, 0.01);
    setupSlider(anchor, 0.0, 60.0, 0.001);
    for (auto* s : {&fixedBpm, &semitones, &cents, &gridBpm, &anchor})
    {
        s->onValueChange = [this] { controlsChanged(); };
        addAndMakeVisible(s);
    }
    halveButton.onClick = [this] { gridBpm.setValue(gridBpm.getValue() / 2.0); };
    doubleButton.onClick = [this] { gridBpm.setValue(gridBpm.getValue() * 2.0); };
    correctTempoButton.setTooltip("Correct this audio's tempo everywhere it is used (re-fits every clip of it)");
    correctTempoButton.onClick = [this]
    {
        if (const auto* i = item())
        {
            engine.send("LIBRARY_ITEM_CORRECT_TEMPO",
                        payload({{"itemId", i->id}, {"bpm", draft.gridBpm}, {"beatAnchorSec", draft.anchorSec}}));
            original.gridBpm = draft.gridBpm;
            original.anchorSec = draft.anchorSec;
        }
    };
    for (auto* c : std::initializer_list<juce::Component*>{&tempoToggle, &tempoMode, &warpMode, &halveButton, &doubleButton,
                                                           &correctTempoButton, &reverseToggle, &brakeToggle, &backspinToggle,
                                                           &sliceDivision, &sliceSamplesButton, &sliceTimelineButton,
                                                           &fullButton, &resetButton, &saveButton, &cancelButton})
        addAndMakeVisible(c);
    for (auto* l : {&fixedBpmLabel, &semitonesLabel, &centsLabel, &gridBpmLabel, &anchorLabel})
    {
        l->setColour(juce::Label::textColourId, colours::textDim);
        l->setFont(uiFont(12.0f));
        addAndMakeVisible(l);
    }
    reverseToggle.onClick = [this] { controlsChanged(); };
    brakeToggle.onClick = [this]
    {
        if (brakeToggle.getToggleState()) backspinToggle.setToggleState(false, juce::dontSendNotification);
        controlsChanged();
    };
    backspinToggle.onClick = [this]
    {
        if (backspinToggle.getToggleState()) brakeToggle.setToggleState(false, juce::dontSendNotification);
        controlsChanged();
    };
    for (size_t i = 0; i < kSlices.size(); ++i) sliceDivision.addItem(kSlices[i].second, static_cast<int>(i) + 1);
    sliceDivision.setSelectedItemIndex(2, juce::dontSendNotification);
    gridMarkersButton.setTooltip("Place slice markers on the chosen beat-grid division");
    gridMarkersButton.onClick = [this]
    {
        sliceMarkers.clear();
        const double beats = kSlices[static_cast<size_t>(juce::jmax(0, sliceDivision.getSelectedItemIndex()))].first;
        const auto grid = gridBeatsMs();
        for (double b = 0.0; b < static_cast<double>(grid.size()); b += beats)
        {
            const double t = grid[static_cast<size_t>(b)];
            if (t > draft.inMs + 1.0 && t < draft.inMs + draft.durationMs - 1.0) sliceMarkers.push_back(t);
        }
        wave->repaint();
        draftToControls();
    };
    sliceModeButton.setClickingTogglesState(true);
    sliceModeButton.setTooltip("Click the waveform to add a slice marker; Alt-click a marker to remove it");
    sliceModeButton.onClick = [this]
    {
        sliceMode = sliceModeButton.getToggleState();
        sliceModeButton.setColour(juce::TextButton::buttonColourId, sliceMode ? colours::emerald500 : colours::zinc800);
    };
    addAndMakeVisible(gridMarkersButton);
    addAndMakeVisible(sliceModeButton);
    sliceSamplesButton.onClick = [this]
    {
        if (sliceMarkers.empty()) gridMarkersButton.onClick();
        if (libraryMode()) return;
        const int n = actions.sliceClipAtMarkers(clipId, sliceMarkers, "simple");
        if (actions.onNotify) actions.onNotify(juce::String(n) + " slices saved to the library", false);
    };
    sliceTimelineButton.setTooltip("Cut the clip into adjacent clips at every marker");
    sliceTimelineButton.onClick = [this]
    {
        const auto* c = clip();
        if (c == nullptr) return;
        if (sliceMarkers.empty()) gridMarkersButton.onClick();
        // Timeline positions of each marker inside the clip (source ms -> timeline ms).
        auto markers = sliceMarkers;
        std::sort(markers.begin(), markers.end());
        std::vector<double> cuts;
        const double ratio = c->effectiveTempoRatio > 0.0 ? c->effectiveTempoRatio : 1.0;
        for (const double src : markers)
            if (src > c->inMs + 1.0 && src < c->inMs + c->durationMs - 1.0) cuts.push_back(c->offsetMs + (src - c->inMs) / ratio);
        engine.inUndoGroup("Slice clip",
                           [&]
                           {
                               auto current = clipId;
                               for (const double at : cuts)
                               {
                                   const auto right = actions.splitClipAt(current, at);
                                   mirror.refreshNow();
                                   if (right.isEmpty()) break;
                                   current = right;
                               }
                           });
        close();
    };
    fullButton.setTooltip("Use the whole audio file");
    fullButton.onClick = [this]
    {
        if (const auto* i = item())
        {
            draft.inMs = 0.0;
            draft.durationMs = i->durationMs;
            wave->fit();
            draftToControls();
        }
    };
    volumeButton.setClickingTogglesState(true);
    volumeButton.setTooltip("Edit the clip's volume shape: click to add a point, drag to move it, Alt-click to remove it");
    volumeButton.onClick = [this]
    {
        volumeMode = volumeButton.getToggleState();
        volumeButton.setColour(juce::TextButton::buttonColourId, volumeMode ? colours::amber500 : colours::zinc800);
        if (volumeMode) ensureEnvelope();
        wave->repaint();
    };
    cropButton.setTooltip("Narrow the clip to the selection");
    cropButton.onClick = [this] { crop(); };
    cropButton.setEnabled(false);
    addAndMakeVisible(volumeButton);
    addAndMakeVisible(cropButton);
    resetButton.onClick = [this]
    {
        selInMs = selDurMs = 0.0;
        draft = original;
        draftToControls();
        wave->fit();
    };
    saveAsNewButton.setTooltip("Save the selection (or the clip window) to the library as a new clip");
    saveAsNewButton.onClick = [this] { saveAsNewClip(); };
    addAndMakeVisible(saveAsNewButton);
    if (libraryMode())
    {
        // A library item has no placement: volume and the tails belong to timeline clips.
        for (juce::Component* c : std::initializer_list<juce::Component*>{&volumeButton, &reverseToggle, &brakeToggle, &backspinToggle,
                                                                          &sliceTimelineButton})
            c->setEnabled(false);
        const auto* i = item();
        saveButton.setVisible(i != nullptr && i->kind == "clip");
    }
    saveButton.setColour(juce::TextButton::buttonColourId, colours::sky600);
    saveButton.onClick = [this] { save(); };
    cancelButton.onClick = [this] { close(); };

    subscriptions.on("PREVIEW_STATE",
                     [this](const juce::var& p)
                     {
                         const bool loaded = static_cast<bool>(p.getProperty("isLoaded", false));
                         const bool playing = static_cast<bool>(p.getProperty("isPlaying", false));
                         if (previewWaiting && loaded && !playing)
                         {
                             previewWaiting = false;
                             previewLoaded = true;
                             applyPreviewLoop();
                             pushPreviewDraft();
                             if (hasSelection()) engine.send("PREVIEW_SEEK", payload({{"positionMs", selInMs - draft.inMs}}));
                             engine.send("PREVIEW_PLAY");
                             return;
                         }
                         if (!previewWaiting)
                         {
                             previewPlaying = playing;
                             previewLoaded = loaded;
                         }
                         playButton.setButtonText(previewPlaying || previewWaiting ? "Pause" : "Play");
                     });
    subscriptions.on("PREVIEW_POSITION",
                     [this](const juce::var& p) { previewPositionMs = static_cast<double>(p.getProperty("positionMs", 0.0)); });
    subscriptions.on("PREVIEW_ENDED",
                     [this](const juce::var&)
                     {
                         previewPlaying = false;
                         playButton.setButtonText("Play");
                     });
    subscriptions.on("TEMPO_CORRECTION_APPLIED",
                     [this](const juce::var& p)
                     {
                         if (!static_cast<bool>(p.getProperty("ok", false)) && actions.onNotify)
                             actions.onNotify(p.getProperty("error", "Tempo correction failed").toString(), true);
                         else if (actions.onNotify)
                             actions.onNotify("Tempo corrected to " + juce::String(static_cast<double>(p.getProperty("appliedBpm", 0.0)), 2) + " BPM", false);
                     });

    mirror.addChangeListener(this);
    readDraft();
    original = draft;
    draftToControls();
    wave->fit();
    startTimerHz(30);
    setWantsKeyboardFocus(true);
    setSize(1100, 640);
}

ClipEditor::~ClipEditor()
{
    stopPreview();
    mirror.removeChangeListener(this);
}

void ClipEditor::show(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, WaveformStore& waveforms,
                      const juce::String& clipId)
{
    if (mirror.model().findClip(clipId) == nullptr) return;
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new ClipEditor(engine, mirror, actions, waveforms, clipId));
    options.dialogTitle = "Clip Editor";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

const ClipModel* ClipEditor::clip() const
{
    return mirror.model().findClip(clipId);
}

const LibraryItemModel* ClipEditor::item() const
{
    if (libraryMode()) return mirror.model().findLibraryItem(libraryItemId);
    const auto* c = clip();
    return c != nullptr ? mirror.model().findLibraryItem(c->libraryItemId) : nullptr;
}

const LibraryItemModel* ClipEditor::sourceItem() const
{
    const auto* i = item();
    if (i != nullptr && i->kind == "clip" && i->sourceItemId.isNotEmpty())
        if (const auto* s = mirror.model().findLibraryItem(i->sourceItemId)) return s;
    return i;
}

void ClipEditor::showLibraryItem(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, WaveformStore& waveforms,
                                 const juce::String& itemId)
{
    if (mirror.model().findLibraryItem(itemId) == nullptr) return;
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new ClipEditor(engine, mirror, actions, waveforms, {}, itemId));
    options.dialogTitle = "Clip Editor";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

double ClipEditor::sourceBpm() const
{
    const auto* i = item();
    return i != nullptr && i->bpm > 0.0 ? i->bpm : 0.0;
}

void ClipEditor::readDraft()
{
    const auto* c = clip();
    if (c == nullptr && libraryMode())
    {
        const auto* i = item();
        if (i == nullptr) return;
        const auto& r = i->raw;
        const bool saved = i->kind == "clip";
        draft.inMs = saved ? static_cast<double>(r.getProperty("sourceInMs", 0.0)) : 0.0;
        const double savedDuration = static_cast<double>(r.getProperty("sourceDurationMs", 0.0));
        draft.durationMs = saved && savedDuration > 0.0 ? savedDuration : i->durationMs;
        const double ratio = static_cast<double>(r.getProperty("tempoRatio", 0.0));
        const bool pinned = ratio > 0.0 && std::abs(ratio - 1.0) > 1e-9;
        draft.tempoEnabled = static_cast<bool>(r.getProperty("warpEnabled", false)) && (pinned || !r.hasProperty("tempoRatio"));
        draft.followProject = !pinned;
        draft.fixedBpm = pinned && sourceBpm() > 0.0 ? sourceBpm() * ratio : (pinned ? ratio * 100.0 : mirror.model().bpm);
        draft.mode = r.getProperty("warpMode", kDefaultWarpMode).toString().isNotEmpty() ? r.getProperty("warpMode", kDefaultWarpMode).toString() : juce::String(kDefaultWarpMode);
        draft.semitones = juce::roundToInt(static_cast<double>(r.getProperty("semitones", 0.0)));
        draft.cents = juce::roundToInt(static_cast<double>(r.getProperty("cents", 0.0)));
        draft.gridBpm = i->bpm > 0.0 ? i->bpm : mirror.model().bpm;
        draft.anchorSec = i->beatAnchorSec;
        draft.envelope.clear();
        return;
    }
    if (c == nullptr) return;
    const auto* i = item();
    draft.inMs = c->inMs;
    draft.durationMs = c->durationMs;
    const bool pinned = c->raw.hasProperty("tempoRatio") && c->tempoRatio > 0.0 && std::abs(c->tempoRatio - 1.0) > 1e-9;
    const bool pitchOnly = c->warpEnabled && !pinned && std::abs(c->tempoRatio - 1.0) < 1e-9 && c->raw.hasProperty("tempoRatio");
    draft.tempoEnabled = c->warpEnabled && !pitchOnly;
    draft.followProject = !pinned;
    draft.fixedBpm = pinned && sourceBpm() > 0.0 ? sourceBpm() * c->tempoRatio : (pinned ? c->tempoRatio * 100.0 : mirror.model().bpm);
    draft.mode = c->warpMode.isNotEmpty() ? c->warpMode : juce::String(kDefaultWarpMode);
    draft.semitones = juce::roundToInt(c->semitones);
    draft.cents = juce::roundToInt(c->cents);
    draft.gridBpm = i != nullptr && i->bpm > 0.0 ? i->bpm : mirror.model().bpm;
    draft.anchorSec = i != nullptr ? i->beatAnchorSec : 0.0;
    draft.reversed = c->reversed;
    draft.brake = c->brake;
    draft.backspin = c->backspin;
    draft.envelope = c->envelope;
}

double ClipEditor::envelopeDurationMs() const
{
    return draft.durationMs / juce::jmax(1e-6, previewRatio());
}

void ClipEditor::ensureEnvelope()
{
    auto& env = draft.envelope;
    const double end = envelopeDurationMs();
    if (env.size() < 2) env = {{0.0, env.empty() ? 1.0 : env.front().gain}, {end, env.empty() ? 1.0 : env.back().gain}};
    env.front().timeMs = 0.0;
    env.back().timeMs = end;
}

void ClipEditor::clearSelection()
{
    selInMs = 0.0;
    selDurMs = 0.0;
    applyPreviewLoop();
    if (wave != nullptr) wave->repaint();
}

void ClipEditor::applyPreviewLoop()
{
    cropButton.setEnabled(hasSelection());
    if (!previewPlaying && !previewWaiting && !previewLoaded) return;
    // The engine owns the wrap: a selection loops itself, otherwise the whole window.
    const double start = hasSelection() ? selInMs - draft.inMs : 0.0;
    const double end = hasSelection() ? start + selDurMs : draft.durationMs;
    engine.send("PREVIEW_SET_LOOP", payload({{"enabled", loop}, {"startMs", start}, {"endMs", end}}));
}

void ClipEditor::crop()
{
    if (!hasSelection()) return;
    // The window narrows to the selection; the volume shape keeps its place in the audio.
    const double ratio = juce::jmax(1e-6, previewRatio());
    const double shift = (selInMs - draft.inMs) / ratio;
    const double length = selDurMs / ratio;
    std::vector<EnvelopePoint> env;
    if (!draft.envelope.empty())
    {
        env.push_back({0.0, envelope::gainAt(draft.envelope, shift)});
        for (const auto& p : draft.envelope)
            if (p.timeMs > shift && p.timeMs < shift + length) env.push_back({p.timeMs - shift, p.gain});
        env.push_back({length, envelope::gainAt(draft.envelope, shift + length)});
    }
    draft.envelope = env;
    draft.inMs = selInMs;
    draft.durationMs = selDurMs;
    selInMs = selDurMs = 0.0;
    wave->fit();
    draftToControls();
    restartPreviewIfPlaying();
}

void ClipEditor::draftToControls()
{
    const auto* i = item();
    title.setText((clip() != nullptr && clip()->name.isNotEmpty() ? clip()->name : i != nullptr ? i->displayName() : juce::String("Clip")),
                  juce::dontSendNotification);
    info.setText("Window " + time(draft.inMs) + " - " + time(draft.inMs + draft.durationMs) + "  (" + time(draft.durationMs) + ")"
                     + (sourceBpm() > 0.0 ? "   Source " + juce::String(sourceBpm(), 2) + " BPM" : juce::String("   No tempo detected"))
                     + (i != nullptr && i->key.isNotEmpty() ? "   " + i->key : juce::String())
                     + (sliceMarkers.empty() ? juce::String() : "   " + juce::String(static_cast<int>(sliceMarkers.size())) + " slice markers"),
                 juce::dontSendNotification);
    tempoToggle.setToggleState(draft.tempoEnabled, juce::dontSendNotification);
    const bool hasSource = sourceBpm() > 0.0;
    tempoMode.setItemEnabled(1, hasSource);
    tempoMode.setItemEnabled(2, hasSource);
    tempoMode.setSelectedId(draft.followProject && hasSource ? 1 : hasSource ? 2 : 3, juce::dontSendNotification);
    const bool stretch = tempoMode.getSelectedId() == 3;
    fixedBpmLabel.setText(stretch ? "Stretch %" : "Clip BPM", juce::dontSendNotification);
    fixedBpm.setRange(stretch ? 25.0 : 20.0, stretch ? 400.0 : 300.0, 0.01);
    fixedBpm.setValue(draft.fixedBpm, juce::dontSendNotification);
    warpMode.setSelectedItemIndex(juce::jmax(0, kWarpModes.indexOf(draft.mode)), juce::dontSendNotification);
    semitones.setValue(draft.semitones, juce::dontSendNotification);
    cents.setValue(draft.cents, juce::dontSendNotification);
    gridBpm.setValue(draft.gridBpm, juce::dontSendNotification);
    anchor.setRange(0.0, juce::jmax(1.0, i != nullptr ? i->durationMs / 1000.0 : 60.0), 0.001);
    anchor.setValue(draft.anchorSec, juce::dontSendNotification);
    reverseToggle.setToggleState(draft.reversed, juce::dontSendNotification);
    brakeToggle.setToggleState(draft.brake, juce::dontSendNotification);
    backspinToggle.setToggleState(draft.backspin, juce::dontSendNotification);
    tempoMode.setEnabled(draft.tempoEnabled);
    fixedBpm.setEnabled(draft.tempoEnabled && !(draft.followProject && hasSource));
    wave->repaint();
}

void ClipEditor::controlsChanged()
{
    draft.tempoEnabled = tempoToggle.getToggleState();
    draft.followProject = tempoMode.getSelectedId() == 1;
    draft.fixedBpm = fixedBpm.getValue();
    draft.mode = kWarpModes[juce::jmax(0, warpMode.getSelectedItemIndex())];
    draft.semitones = juce::roundToInt(semitones.getValue());
    draft.cents = juce::roundToInt(cents.getValue());
    draft.gridBpm = gridBpm.getValue();
    draft.anchorSec = anchor.getValue();
    draft.reversed = reverseToggle.getToggleState();
    draft.brake = brakeToggle.getToggleState();
    draft.backspin = backspinToggle.getToggleState();
    draftToControls();
    pushPreviewDraft();
}

std::vector<double> ClipEditor::gridBeatsMs() const
{
    std::vector<double> out;
    const auto* i = item();
    const double total = i != nullptr ? i->durationMs : draft.inMs + draft.durationMs;
    const double step = 60000.0 / juce::jmax(1.0, draft.gridBpm);
    // Rigid grid through the anchor (the draft); the item's own (possibly variable) grid
    // when the draft still matches it.
    const bool unchanged = i != nullptr && std::abs(draft.gridBpm - i->bpm) < 1e-6
                           && std::abs(draft.anchorSec - i->beatAnchorSec) < 1e-6 && i->beats.size() > 1;
    if (unchanged)
    {
        for (const double b : i->beats) out.push_back(b * 1000.0);
        return out;
    }
    const double anchorMs = draft.anchorSec * 1000.0;
    for (double t = anchorMs - std::floor(anchorMs / step) * step; t <= total; t += step) out.push_back(t);
    return out;
}

juce::var ClipEditor::warpRatioForSave() const
{
    const bool pitchActive = draft.semitones != 0 || draft.cents != 0;
    const bool hasSource = sourceBpm() > 0.0;
    if (draft.tempoEnabled && !(draft.followProject && hasSource))
        return hasSource && tempoMode.getSelectedId() == 2 ? draft.fixedBpm / sourceBpm() : draft.fixedBpm / 100.0;
    if (!draft.tempoEnabled && pitchActive) return 1.0;
    return {};
}

void ClipEditor::saveLibraryClip()
{
    const auto* i = item();
    if (i == nullptr || i->kind != "clip")
    {
        saveAsNewClip();
        return;
    }
    // libraryClipActions.updateLibraryClipEdit: the saved clip and every placement of it.
    const auto window = hasSelection() ? std::pair{selInMs, selDurMs} : std::pair{draft.inMs, draft.durationMs};
    const bool pitchActive = draft.semitones != 0 || draft.cents != 0;
    const bool warpEnabled = draft.tempoEnabled || pitchActive;
    const auto ratio = warpRatioForSave();
    const auto itemId = i->id;
    std::vector<std::pair<juce::String, double>> placements; // clip id, timeline start
    for (const auto& t : mirror.model().tracks)
        for (const auto& c : t.clips)
            if (c.libraryItemId == itemId) placements.emplace_back(c.id, c.offsetMs);
    auto* add = new juce::DynamicObject();
    for (const auto& key : {"filePath", "fileName", "name", "sampleRate", "channelCount", "key", "sourceItemId", "sourceClipId", "mediaId", "collapsed"})
        if (i->raw.hasProperty(key)) add->setProperty(key, i->raw.getProperty(key, {}));
    add->setProperty("itemId", itemId);
    add->setProperty("kind", "clip");
    add->setProperty("durationMs", sourceItem() != nullptr ? sourceItem()->durationMs : i->durationMs);
    add->setProperty("sourceInMs", window.first);
    add->setProperty("sourceDurationMs", window.second);
    add->setProperty("warpEnabled", warpEnabled);
    add->setProperty("warpMode", draft.mode);
    add->setProperty("tempoRatio", ratio);
    add->setProperty("semitones", draft.semitones);
    add->setProperty("cents", draft.cents);
    // The saved clip's key follows its pitch, from the source's own key.
    const auto sourceKey = sourceItem() != nullptr && sourceItem()->key.isNotEmpty() ? sourceItem()->key : i->key;
    if (const auto key = shiftedKey(sourceKey, draft.semitones, draft.cents); key.isNotEmpty()) add->setProperty("key", key);
    const bool gridChanged = std::abs(draft.gridBpm - original.gridBpm) > 1e-6 || std::abs(draft.anchorSec - original.anchorSec) > 1e-6;
    engine.inUndoGroup("Edit clip",
                       [&]
                       {
                           if (gridChanged)
                               engine.send("LIBRARY_ITEM_SET_MANUAL_TEMPO",
                                           payload({{"itemId", itemId}, {"bpm", draft.gridBpm}, {"beatAnchorSec", draft.anchorSec}}));
                           engine.send("LIBRARY_ADD", juce::var(add));
                           for (const auto& [placed, start] : placements)
                           {
                               engine.send("CLIP_TRIM", payload({{"clipId", placed}, {"startMs", start}, {"inMs", window.first}, {"durationMs", window.second}}));
                               engine.send("CLIP_SET_WARP", payload({{"clipId", placed},
                                                                     {"warpEnabled", warpEnabled},
                                                                     {"warpMode", draft.mode},
                                                                     {"tempoRatio", ratio},
                                                                     {"semitones", draft.semitones},
                                                                     {"cents", draft.cents}}));
                           }
                       });
    close();
}

void ClipEditor::saveAsNewClip()
{
    const auto* source = sourceItem();
    if (source == nullptr) return;
    // libraryClipActions.addLibraryClipFromSelection: the selection (or the window) of the file.
    const double inMs = std::floor(hasSelection() ? selInMs : draft.inMs);
    const double durationMs = std::floor(hasSelection() ? selDurMs : draft.durationMs);
    if (durationMs <= 0.0) return;
    for (const auto& existing : mirror.model().library)
        if (existing.kind == "clip" && existing.sourceItemId == source->id
            && std::abs(static_cast<double>(existing.raw.getProperty("sourceInMs", -1.0)) - inMs) < 0.5
            && std::abs(static_cast<double>(existing.raw.getProperty("sourceDurationMs", -1.0)) - durationMs) < 0.5)
        {
            if (actions.onNotify) actions.onNotify("That selection is already saved as \"" + existing.displayName() + "\"", false);
            close();
            return;
        }
    const auto clock = [](double ms)
    {
        const int total = static_cast<int>(ms / 1000.0);
        return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
    };
    int next = 1;
    for (const auto& existing : mirror.model().library)
        if (existing.id.startsWithChar('l') && existing.id.substring(1).containsOnly("0123456789"))
            next = juce::jmax(next, existing.id.substring(1).getIntValue() + 1);
    const auto name = source->displayName() + " (" + clock(inMs) + "-" + clock(inMs + durationMs) + ")";
    engine.inUndoGroup("Save to library",
                       [&]
                       {
                           engine.send("LIBRARY_ADD", payload({{"itemId", "l" + juce::String(next)},
                                                               {"filePath", source->filePath},
                                                               {"fileName", source->fileName},
                                                               {"kind", "clip"},
                                                               {"name", name},
                                                               {"durationMs", source->durationMs},
                                                               {"sampleRate", source->sampleRate},
                                                               {"channelCount", source->channelCount},
                                                               {"key", source->key},
                                                               {"sourceItemId", source->id},
                                                               {"sourceClipId", ""},
                                                               {"sourceInMs", inMs},
                                                               {"sourceDurationMs", durationMs},
                                                               {"mediaId", source->mediaId}}));
                       });
    if (actions.onNotify) actions.onNotify("Saved \"" + name + "\" to the library", false);
    close();
}

void ClipEditor::save()
{
    if (libraryMode())
    {
        saveLibraryClip();
        return;
    }
    const auto* c = clip();
    if (c == nullptr)
    {
        close();
        return;
    }
    const auto* i = item();
    const double projectBpm = mirror.model().bpm;
    const bool hasSource = sourceBpm() > 0.0;
    const bool pitchActive = draft.semitones != 0 || draft.cents != 0;
    const bool warpEnabled = draft.tempoEnabled || pitchActive;
    juce::var ratio; // void = follow the project
    if (draft.tempoEnabled && !(draft.followProject && hasSource))
        ratio = hasSource && tempoMode.getSelectedId() == 2 ? draft.fixedBpm / sourceBpm() : draft.fixedBpm / 100.0;
    else if (!draft.tempoEnabled && pitchActive)
        ratio = 1.0;

    engine.inUndoGroup("Save clip changes",
                       [&]
                       {
                           // Grid first, so the clip's warp below fits the corrected tempo.
                           if (i != nullptr && (std::abs(draft.gridBpm - original.gridBpm) > 1e-6
                                                || std::abs(draft.anchorSec - original.anchorSec) > 1e-6))
                               engine.send("LIBRARY_ITEM_SET_MANUAL_TEMPO",
                                           payload({{"itemId", i->id}, {"bpm", draft.gridBpm}, {"beatAnchorSec", draft.anchorSec}}));
                           mirror.refreshNow();
                           if (const auto* now = clip())
                           {
                               if (std::abs(draft.inMs - now->inMs) > 0.01 || std::abs(draft.durationMs - now->durationMs) > 0.01)
                               {
                                   // Same timeline start, new source window.
                                   const double r = now->effectiveTempoRatio > 0.0 ? now->effectiveTempoRatio : 1.0;
                                   engine.send("CLIP_TRIM", payload({{"clipId", clipId},
                                                                     {"startMs", now->offsetMs},
                                                                     {"inMs", draft.inMs},
                                                                     {"durationMs", draft.durationMs}}));
                                   juce::ignoreUnused(r);
                               }
                           }
                           const bool warpChanged = draft.tempoEnabled != original.tempoEnabled || draft.followProject != original.followProject
                                                    || std::abs(draft.fixedBpm - original.fixedBpm) > 1e-6 || draft.mode != original.mode
                                                    || draft.semitones != original.semitones || draft.cents != original.cents;
                           if (warpChanged)
                               engine.send("CLIP_SET_WARP", payload({{"clipId", clipId},
                                                                     {"warpEnabled", warpEnabled},
                                                                     {"warpMode", draft.mode},
                                                                     {"tempoRatio", ratio},
                                                                     {"semitones", draft.semitones},
                                                                     {"cents", draft.cents}}));
                           const bool envelopeChanged = draft.envelope.size() != original.envelope.size()
                                                        || !std::equal(draft.envelope.begin(), draft.envelope.end(), original.envelope.begin(),
                                                                       [](const EnvelopePoint& a, const EnvelopePoint& b)
                                                                       { return std::abs(a.timeMs - b.timeMs) < 1e-6 && std::abs(a.gain - b.gain) < 1e-6; });
                           if (envelopeChanged)
                               actions.setClipEnvelope(clipId, envelope::isFlatUnity(draft.envelope) ? std::vector<EnvelopePoint>{} : draft.envelope);
                           if (draft.reversed != original.reversed) actions.setClipReversed(clipId, draft.reversed);
                           if (draft.brake != original.brake) actions.setClipBrake(clipId, draft.brake);
                           if (draft.backspin != original.backspin) actions.setClipBackspin(clipId, draft.backspin);
                       });
    juce::ignoreUnused(projectBpm);
    close();
}

void ClipEditor::togglePlay()
{
    if (previewWaiting) return;
    if (previewPlaying)
    {
        engine.send("PREVIEW_PAUSE");
        previewPlaying = false;
        playButton.setButtonText("Play");
        return;
    }
    if (previewLoaded)
    {
        previewPlaying = true;
        pushPreviewDraft();
        const double rel = selInMs - draft.inMs;
        if (hasSelection() && (previewPositionMs < rel - 0.5 || previewPositionMs >= rel + selDurMs - 0.5))
            seekPreview(rel);
        engine.send("PREVIEW_PLAY");
        playButton.setButtonText("Pause");
        return;
    }
    const auto* i = item();
    if (i == nullptr) return;
    const bool pitchActive = draft.semitones != 0 || draft.cents != 0;
    const double ratio = previewRatio();
    previewWaiting = true;
    previewPlaying = true;
    previewPositionMs = 0.0;
    playButton.setButtonText("Pause");
    engine.send("PREVIEW_LOAD", payload({{"libraryItemId", i->id},
                                         {"inMs", draft.inMs},
                                         {"durationMs", draft.durationMs},
                                         {"warpEnabled", draft.tempoEnabled || pitchActive},
                                         {"warpMode", draft.mode},
                                         {"tempoRatio", ratio},
                                         {"semitones", draft.semitones},
                                         {"cents", draft.cents}}));
    engine.send("PREVIEW_SET_REVERSED", payload({{"reversed", draft.reversed}}));
    engine.send("PREVIEW_SET_BRAKE", payload({{"on", draft.brake}}));
    engine.send("PREVIEW_SET_BACKSPIN", payload({{"on", draft.backspin}}));
    engine.send("PREVIEW_SET_METRONOME", payload({{"enabled", metronome}, {"bpm", draft.gridBpm}, {"beatAnchorSec", draft.anchorSec}}));
}

double ClipEditor::previewRatio() const
{
    if (!draft.tempoEnabled) return 1.0;
    const bool hasSource = sourceBpm() > 0.0;
    return draft.followProject && hasSource ? mirror.model().bpm / sourceBpm()
           : hasSource && tempoMode.getSelectedId() == 2 ? draft.fixedBpm / sourceBpm()
                                                          : draft.fixedBpm / 100.0;
}

void ClipEditor::pushPreviewDraft()
{
    if (!previewPlaying && !previewWaiting && !previewLoaded) return;
    const bool pitchActive = draft.semitones != 0 || draft.cents != 0;
    engine.send("PREVIEW_SET_WARP", payload({{"warpEnabled", draft.tempoEnabled || pitchActive},
                                             {"warpMode", draft.mode},
                                             {"tempoRatio", previewRatio()},
                                             {"semitones", draft.semitones},
                                             {"cents", draft.cents}}));
    engine.send("PREVIEW_SET_REVERSED", payload({{"reversed", draft.reversed}}));
    // Brake and backspin are exclusive in the engine; clear one before setting the other.
    engine.send(draft.brake ? "PREVIEW_SET_BACKSPIN" : "PREVIEW_SET_BRAKE", payload({{"on", false}}));
    engine.send(draft.brake ? "PREVIEW_SET_BRAKE" : "PREVIEW_SET_BACKSPIN", payload({{"on", draft.brake || draft.backspin}}));
    {
        juce::Array<juce::var> points;
        for (const auto& p : envelope::sanitize(draft.envelope))
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("timeMs", p.timeMs);
            o->setProperty("gain", p.gain);
            points.add(juce::var(o));
        }
        engine.send("PREVIEW_SET_ENVELOPE", payload({{"points", points}}));
    }
    engine.send("PREVIEW_SET_METRONOME", payload({{"enabled", metronome}, {"bpm", draft.gridBpm}, {"beatAnchorSec", draft.anchorSec}}));
}

void ClipEditor::stopPreview()
{
    if (previewPlaying || previewWaiting || previewLoaded) engine.send("PREVIEW_STOP");
    previewPlaying = false;
    previewWaiting = false;
    previewLoaded = false;
    previewPositionMs = 0.0;
    playButton.setButtonText("Play");
}

void ClipEditor::seekPreview(double windowMs)
{
    const double ms = juce::jlimit(0.0, juce::jmax(0.0, draft.durationMs), windowMs);
    previewPositionMs = ms;
    if (previewLoaded || previewPlaying) engine.send("PREVIEW_SEEK", payload({{"positionMs", ms}}));
    if (wave != nullptr) wave->repaint();
}

void ClipEditor::restartPreviewIfPlaying()
{
    const bool wasPlaying = previewPlaying || previewWaiting;
    if (!wasPlaying && !previewLoaded) return;
    stopPreview(); // the window changed: the voice reloads it
    if (wasPlaying) togglePlay();
}

void ClipEditor::close()
{
    stopPreview();
    if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) window->exitModalState(0);
}

void ClipEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (libraryMode() ? item() == nullptr : clip() == nullptr) close(); // removed (e.g. undone) while open
}

void ClipEditor::timerCallback()
{
    if (previewPlaying) wave->repaint();
}

bool ClipEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        togglePlay();
        return true;
    }
    if (key == juce::KeyPress::returnKey && key.getModifiers().isCommandDown())
    {
        save();
        return true;
    }
    return false;
}

void ClipEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    g.setColour(colours::border);
    g.drawHorizontalLine(getHeight() - 220, 0.0f, static_cast<float>(getWidth()));
}

void ClipEditor::resized()
{
    auto area = getLocalBounds().reduced(12);
    auto header = area.removeFromTop(26);
    title.setBounds(header.removeFromLeft(360));
    info.setBounds(header);
    area.removeFromTop(4);
    auto transport = area.removeFromTop(28);
    skipStartButton.setBounds(transport.removeFromLeft(34));
    transport.removeFromLeft(2);
    playButton.setBounds(transport.removeFromLeft(70));
    transport.removeFromLeft(2);
    skipEndButton.setBounds(transport.removeFromLeft(34));
    transport.removeFromLeft(4);
    loopButton.setBounds(transport.removeFromLeft(60));
    transport.removeFromLeft(4);
    metronomeButton.setBounds(transport.removeFromLeft(60));
    transport.removeFromLeft(12);
    volumeButton.setBounds(transport.removeFromLeft(70));
    transport.removeFromLeft(4);
    cropButton.setBounds(transport.removeFromLeft(60));
    fitButton.setBounds(transport.removeFromRight(44));
    zoomInButton.setBounds(transport.removeFromRight(30));
    zoomOutButton.setBounds(transport.removeFromRight(30));
    area.removeFromTop(6);

    auto controls = area.removeFromBottom(208);
    area.removeFromBottom(8);
    wave->setBounds(area);

    auto buttons = controls.removeFromBottom(30);
    saveButton.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(6);
    cancelButton.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(6);
    resetButton.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(6);
    saveAsNewButton.setBounds(buttons.removeFromRight(130));
    fullButton.setBounds(buttons.removeFromLeft(100));
    controls.removeFromBottom(8);

    const int colWidth = controls.getWidth() / 3;
    auto row = [](juce::Rectangle<int>& col, juce::Component* label, juce::Component& field)
    {
        auto r = col.removeFromTop(26);
        if (label != nullptr) label->setBounds(r.removeFromLeft(96));
        field.setBounds(r.reduced(0, 2));
        col.removeFromTop(4);
    };
    // Tempo and warp.
    auto warpCol = controls.removeFromLeft(colWidth).reduced(4, 0);
    row(warpCol, nullptr, tempoToggle);
    row(warpCol, nullptr, tempoMode);
    row(warpCol, &fixedBpmLabel, fixedBpm);
    row(warpCol, nullptr, warpMode);
    // Pitch and transforms.
    auto pitchCol = controls.removeFromLeft(colWidth).reduced(4, 0);
    row(pitchCol, &semitonesLabel, semitones);
    row(pitchCol, &centsLabel, cents);
    {
        auto r = pitchCol.removeFromTop(26);
        reverseToggle.setBounds(r.removeFromLeft(r.getWidth() / 3));
        brakeToggle.setBounds(r.removeFromLeft(r.getWidth() / 2));
        backspinToggle.setBounds(r);
        pitchCol.removeFromTop(8);
    }
    {
        auto r = pitchCol.removeFromTop(26);
        sliceModeButton.setBounds(r.removeFromLeft(r.getWidth() / 2 - 2));
        r.removeFromLeft(4);
        gridMarkersButton.setBounds(r);
        pitchCol.removeFromTop(4);
    }
    {
        auto r = pitchCol.removeFromTop(26);
        sliceDivision.setBounds(r.removeFromLeft(80));
        r.removeFromLeft(4);
        sliceSamplesButton.setBounds(r.removeFromLeft(r.getWidth() / 2 - 2));
        r.removeFromLeft(4);
        sliceTimelineButton.setBounds(r);
    }
    // Beat grid.
    auto gridCol = controls.reduced(4, 0);
    {
        auto r = gridCol.removeFromTop(26);
        gridBpmLabel.setBounds(r.removeFromLeft(96));
        doubleButton.setBounds(r.removeFromRight(34).reduced(0, 2));
        halveButton.setBounds(r.removeFromRight(34).reduced(0, 2));
        gridBpm.setBounds(r.reduced(0, 2));
        gridCol.removeFromTop(4);
    }
    row(gridCol, &anchorLabel, anchor);
    row(gridCol, nullptr, correctTempoButton);
}

} // namespace stemcel::native
