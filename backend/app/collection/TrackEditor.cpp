#include "TrackEditor.h"

#include "AudioImport.h"
#include "Rekordbox.h"
#include "Theme.h"
#include "WaveformStore.h"

#include <algorithm>
#include <cmath>

namespace stemcel::native::dj
{
namespace
{
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [key, value] : fields) o->setProperty(key, value);
    return juce::var(o);
}

juce::String clock(double sec)
{
    if (sec < 0.0) sec = 0.0;
    const int whole = static_cast<int>(sec);
    return juce::String(whole / 60) + ":" + juce::String(whole % 60).paddedLeft('0', 2) + "."
           + juce::String(static_cast<int>((sec - whole) * 10.0));
}

constexpr int kTopRow = 26;
constexpr int kControlRow = 30;
} // namespace

juce::Colour trackColour(int index)
{
    static const juce::Colour palette[] = {juce::Colour(0x00000000), juce::Colour(0xffff3a8a), juce::Colour(0xffff3333),
                                           juce::Colour(0xffff9900), juce::Colour(0xffffee00), juce::Colour(0xff33cc33),
                                           juce::Colour(0xff00ccff), juce::Colour(0xff3366ff), juce::Colour(0xffaa66ff)};
    return index > 0 && index < 9 ? palette[index] : juce::Colour(0x00000000);
}

juce::String trackColourName(int index)
{
    static const char* names[] = {"None", "Pink", "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple"};
    return index >= 0 && index < 9 ? names[index] : "None";
}

juce::Colour hotCueColour(int slot)
{
    // A CDJ's defaults when a hot cue has no colour of its own.
    static const juce::Colour colours[] = {juce::Colour(0xff28e214), juce::Colour(0xff28e214), juce::Colour(0xff28e214),
                                           juce::Colour(0xff28e214), juce::Colour(0xffff8c00), juce::Colour(0xffff8c00),
                                           juce::Colour(0xffff8c00), juce::Colour(0xffff8c00)};
    return slot >= 0 && slot < 8 ? colours[slot] : colours::amber400;
}

TrackEditor::TrackEditor(EngineClient& client, WaveformStore& store) : engine(client), waveforms(store), subscriptions(client)
{
    title.setFont(uiFont(15.0F, true));
    title.setColour(juce::Label::textColourId, colours::text);
    info.setFont(uiFont(12.5F));
    info.setColour(juce::Label::textColourId, colours::textDim);
    info.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(title);
    addAndMakeVisible(info);

    playButton.onClick = [this] { togglePlay(); };
    playButton.setTooltip("Play or pause the track (Space)");
    cueButton.onClick = [this]
    {
        // Back to the first hot cue, else the first beat, else the start.
        const auto* t = track();
        if (t == nullptr) return;
        double to = 0.0;
        bool found = false;
        for (const auto& c : t->cues)
            if (c.isHot() && (!found || c.pos < to))
            {
                to = c.pos;
                found = true;
            }
        if (!found)
            if (const auto beats = t->beatTimes(); !beats.empty()) to = beats.front();
        seek(to);
    };
    cueButton.setTooltip("Back to the first hot cue (or the first beat)");
    for (auto* b : {&playButton, &cueButton}) addAndMakeVisible(*b);

    bpmField.setJustification(juce::Justification::centred);
    bpmField.setInputRestrictions(7, "0123456789.");
    bpmField.setTooltip("The track's tempo: type one to set the grid to it");
    bpmField.onReturnKey = [this] { setTempo(bpmField.getText().getDoubleValue()); };
    bpmField.onFocusLost = [this]
    {
        if (const auto* t = track(); t != nullptr && std::abs(bpmField.getText().getDoubleValue() - t->bpm) > 0.004)
            setTempo(bpmField.getText().getDoubleValue());
    };
    addAndMakeVisible(bpmField);
    halfButton.onClick = [this] { scaleTempo(0.5); };
    halfButton.setTooltip("Half the tempo");
    doubleButton.onClick = [this] { scaleTempo(2.0); };
    doubleButton.setTooltip("Double the tempo");
    earlierButton.onClick = [this] { shiftGrid(juce::ModifierKeys::currentModifiers.isShiftDown() ? -1.0 : -5.0); };
    earlierButton.setTooltip("Move the grid 5 ms earlier (Shift: 1 ms)");
    laterButton.onClick = [this] { shiftGrid(juce::ModifierKeys::currentModifiers.isShiftDown() ? 1.0 : 5.0); };
    laterButton.setTooltip("Move the grid 5 ms later (Shift: 1 ms)");
    downbeatButton.onClick = [this] { downbeatHere(); };
    downbeatButton.setTooltip("Put a downbeat (beat 1 of a bar) at the playhead, keeping the tempo");
    tapButton.onClick = [this] { tap(); };
    tapButton.setTooltip("Tap along to set the tempo");
    quantize.setToggleState(true, juce::dontSendNotification);
    quantize.setTooltip("Quantize: cues snap to the nearest beat");
    for (auto* b : {&halfButton, &doubleButton, &earlierButton, &laterButton, &downbeatButton, &tapButton}) addAndMakeVisible(*b);
    addAndMakeVisible(quantize);

    for (int i = 0; i < 8; ++i)
    {
        auto& b = hotCueButtons[static_cast<size_t>(i)];
        b.setButtonText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)));
        b.setTooltip("Hot cue " + b.getButtonText() + ": set at the playhead, or jump to it. Right-click for more");
        b.onClick = [this, i]
        {
            if (juce::ModifierKeys::currentModifiers.isPopupMenu()) return;
            hotCuePressed(i);
        };
        b.addMouseListener(this, false);
        addAndMakeVisible(b);
    }
    memoryButton.onClick = [this] { addMemoryCue(); };
    memoryButton.setTooltip("Add a memory cue at the playhead (a loop, when the preview loops)");
    addAndMakeVisible(memoryButton);

    for (int i = 0; i <= 5; ++i) ratingBox.addItem(i == 0 ? juce::String("No rating") : juce::String::repeatedString("*", i), i + 1);
    ratingBox.onChange = [this]
    {
        const int rating = ratingBox.getSelectedId() - 1;
        if (id != 0 && rating >= 0) Collection::shared().update(id, [rating](Track& t) { t.rating = rating; });
    };
    for (int i = 0; i <= 8; ++i) colourBox.addItem(trackColourName(i), i + 1);
    colourBox.onChange = [this]
    {
        const int colour = colourBox.getSelectedId() - 1;
        if (id != 0 && colour >= 0) Collection::shared().update(id, [colour](Track& t) { t.colour = colour; });
    };
    addAndMakeVisible(ratingBox);
    addAndMakeVisible(colourBox);

    subscriptions.on("PREVIEW_STATE",
                     [this](const juce::var& p)
                     {
                         const bool loaded = static_cast<bool>(p.getProperty("isLoaded", false));
                         const bool playing = static_cast<bool>(p.getProperty("isPlaying", false));
                         if (playWhenLoaded && loaded && !playing)
                         {
                             playWhenLoaded = false;
                             previewLoaded = true;
                             engine.send("PREVIEW_SEEK", object({{"positionMs", playhead * 1000.0}}));
                             engine.send("PREVIEW_PLAY");
                             return;
                         }
                         if (!playWhenLoaded)
                         {
                             previewLoaded = loaded;
                             previewPlaying = playing;
                         }
                         playButton.setButtonText(previewPlaying || playWhenLoaded ? "Pause" : "Play");
                     });
    subscriptions.on("PREVIEW_POSITION",
                     [this](const juce::var& p)
                     {
                         if (!previewPlaying) return;
                         playhead = static_cast<double>(p.getProperty("positionMs", 0.0)) / 1000.0;
                         repaint();
                     });
    subscriptions.on("PREVIEW_ENDED",
                     [this](const juce::var&)
                     {
                         previewPlaying = false;
                         playButton.setButtonText("Play");
                     });

    Collection::shared().addChangeListener(this);
    waveforms.addChangeListener(this);
    setWantsKeyboardFocus(true);
    refresh();
}

TrackEditor::~TrackEditor()
{
    alive->store(false);
    pool.removeAllJobs(true, 10000);
    stopPreview();
    Collection::shared().removeChangeListener(this);
    waveforms.removeChangeListener(this);
    for (auto& b : hotCueButtons) b.removeMouseListener(this);
}

const Track* TrackEditor::track() const
{
    return id != 0 ? Collection::shared().find(id) : nullptr;
}

void TrackEditor::showTrack(juce::int64 next)
{
    if (next == id) return;
    if (previewPlaying || playWhenLoaded) stopPreview();
    id = next;
    playhead = 0.0;
    taps.clear();
    previewLoaded = false;
    playablePath = {};
    loadedPath = {};
    if (const auto* t = track())
    {
        // The first beat is where the needle starts.
        if (const auto beats = t->beatTimes(); !beats.empty()) playhead = beats.front();
        // A playable path for the preview and the waveform (a transcode for formats JUCE
        // can't read), found off the message thread.
        const juce::File file(t->path);
        pool.addJob(
            [this, file, next, alive = alive]
            {
                const auto path = AudioImporter::playablePathFor(file, sharedRekordbox());
                juce::MessageManager::callAsync(
                    [this, path, next, alive]
                    {
                        if (!alive->load() || next != id) return;
                        playablePath = path;
                        repaint();
                    });
            });
    }
    refresh();
}

void TrackEditor::refresh()
{
    const auto* t = track();
    const bool has = t != nullptr;
    for (auto* c : std::initializer_list<juce::Component*>{&playButton, &cueButton, &bpmField, &halfButton, &doubleButton, &earlierButton,
                                                           &laterButton, &downbeatButton, &tapButton, &quantize, &memoryButton, &ratingBox,
                                                           &colourBox})
        c->setEnabled(has);
    if (!has)
    {
        title.setText("Select a track", juce::dontSendNotification);
        info.setText({}, juce::dontSendNotification);
        for (auto& b : hotCueButtons)
        {
            b.setEnabled(false);
            b.setColour(juce::TextButton::buttonColourId, colours::zinc800);
        }
        repaint();
        return;
    }
    title.setText(t->displayTitle() + (t->artist.isNotEmpty() ? "  -  " + t->artist : juce::String()), juce::dontSendNotification);
    juce::String text;
    if (t->state == Track::State::pending || t->state == Track::State::analysing) text << "Analysing...  ";
    if (t->state == Track::State::failed) text << t->error << "  ";
    if (t->key.isNotEmpty()) text << t->key << "  ";
    if (t->hasLoudness) text << juce::String(t->loudnessLufs, 1) << " LUFS  ";
    text << clock(t->durationSec).upToLastOccurrenceOf(".", false, false);
    info.setText(text, juce::dontSendNotification);
    if (!bpmField.hasKeyboardFocus(false)) bpmField.setText(t->bpm > 0.0 ? juce::String(t->bpm, 2) : juce::String(), false);
    ratingBox.setSelectedId(t->rating + 1, juce::dontSendNotification);
    colourBox.setSelectedId(t->colour + 1, juce::dontSendNotification);
    for (int i = 0; i < 8; ++i)
    {
        auto& b = hotCueButtons[static_cast<size_t>(i)];
        b.setEnabled(true);
        const auto* c = t->hotCue(i);
        b.setColour(juce::TextButton::buttonColourId, c != nullptr ? hotCueColour(i).withAlpha(0.75F) : colours::zinc800);
        b.setColour(juce::TextButton::textColourOffId, c != nullptr ? juce::Colours::black : colours::textDim);
        b.setButtonText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)) + (c != nullptr && c->isLoop() ? " (loop)" : ""));
    }
    repaint();
}

void TrackEditor::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &Collection::shared())
        refresh();
    else
        repaint();
}

void TrackEditor::timerCallback() {}

void TrackEditor::togglePlay()
{
    const auto* t = track();
    if (t == nullptr) return;
    if (previewPlaying || playWhenLoaded)
    {
        playWhenLoaded = false;
        engine.send("PREVIEW_PAUSE");
        previewPlaying = false;
        playButton.setButtonText("Play");
        return;
    }
    const auto path = playablePath.isNotEmpty() ? playablePath : t->path;
    if (!previewLoaded || loadedPath != path)
    {
        loadedPath = path;
        playWhenLoaded = true;
        engine.send("PREVIEW_LOAD", object({{"filePath", path}}));
    }
    else
    {
        engine.send("PREVIEW_SEEK", object({{"positionMs", playhead * 1000.0}}));
        engine.send("PREVIEW_PLAY");
    }
    playButton.setButtonText("Pause");
}

void TrackEditor::stopPreview()
{
    playWhenLoaded = false;
    if (previewLoaded || previewPlaying) engine.send("PREVIEW_STOP");
    previewPlaying = false;
    previewLoaded = false;
    playButton.setButtonText("Play");
}

void TrackEditor::seek(double sec)
{
    const auto* t = track();
    if (t == nullptr) return;
    playhead = juce::jlimit(0.0, std::max(0.0, t->durationSec), sec);
    if (previewLoaded) engine.send("PREVIEW_SEEK", object({{"positionMs", playhead * 1000.0}}));
    repaint();
}

double TrackEditor::snapped(double sec) const
{
    const auto* t = track();
    if (t == nullptr || !quantize.getToggleState()) return sec;
    // The beats and the half beats between them.
    const auto whole = t->beatTimes();
    if (whole.empty()) return sec;
    std::vector<double> beats;
    beats.reserve(whole.size() * 2);
    for (size_t i = 0; i < whole.size(); ++i)
    {
        beats.push_back(whole[i]);
        if (i + 1 < whole.size()) beats.push_back(0.5 * (whole[i] + whole[i + 1]));
    }
    const auto at = std::lower_bound(beats.begin(), beats.end(), sec);
    double best = at != beats.end() ? *at : beats.back();
    if (at != beats.begin() && std::abs(*(at - 1) - sec) < std::abs(best - sec)) best = *(at - 1);
    return best;
}

void TrackEditor::hotCuePressed(int slot)
{
    const auto* t = track();
    if (t == nullptr) return;
    if (const auto* c = t->hotCue(slot))
    {
        seek(c->pos);
        return;
    }
    const double at = snapped(playhead);
    Collection::shared().update(id,
                                [slot, at](Track& track)
                                {
                                    Cue c;
                                    c.slot = slot;
                                    c.pos = at;
                                    track.cues.push_back(c);
                                });
}

void TrackEditor::hotCueMenu(int slot)
{
    const auto* t = track();
    if (t == nullptr) return;
    const auto* c = t->hotCue(slot);
    juce::PopupMenu menu;
    const auto beats = t->beatTimes();
    menu.addItem("Set Here", [this, slot]
                 {
                     const double at = snapped(playhead);
                     Collection::shared().update(id,
                                                 [slot, at](Track& track)
                                                 {
                                                     track.cues.erase(std::remove_if(track.cues.begin(), track.cues.end(),
                                                                                     [slot](const Cue& c) { return c.slot == slot; }),
                                                                      track.cues.end());
                                                     Cue cue;
                                                     cue.slot = slot;
                                                     cue.pos = at;
                                                     track.cues.push_back(cue);
                                                 });
                 });
    if (c != nullptr)
    {
        juce::PopupMenu loops;
        for (const int length : {1, 2, 4, 8, 16, 32})
            loops.addItem(juce::String(length) + (length == 1 ? " beat" : " beats"), t->bpm > 0.0,
                          false, [this, slot, length]
                          {
                              Collection::shared().update(id,
                                                          [slot, length](Track& track)
                                                          {
                                                              for (auto& cue : track.cues)
                                                                  if (cue.slot == slot) cue.end = cue.pos + length * 60.0 / track.bpm;
                                                          });
                          });
        loops.addItem("No Loop", c->isLoop(), false, [this, slot]
                      {
                          Collection::shared().update(id,
                                                      [slot](Track& track)
                                                      {
                                                          for (auto& cue : track.cues)
                                                              if (cue.slot == slot) cue.end = -1.0;
                                                      });
                      });
        menu.addSubMenu("Loop", loops);
        menu.addItem("Name...", [this, slot, name = c->name]
                     {
                         auto* window = new juce::AlertWindow("Hot cue name", "A name the CDJ shows with the cue", juce::MessageBoxIconType::NoIcon);
                         window->addTextEditor("name", name);
                         window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
                         window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                         window->enterModalState(true, juce::ModalCallbackFunction::create(
                                                           [this, window, slot](int result)
                                                           {
                                                               if (result != 1) return;
                                                               const auto text = window->getTextEditorContents("name").trim();
                                                               Collection::shared().update(id,
                                                                                           [slot, text](Track& track)
                                                                                           {
                                                                                               for (auto& cue : track.cues)
                                                                                                   if (cue.slot == slot) cue.name = text;
                                                                                           });
                                                           }),
                                                 true);
                     });
        menu.addSeparator();
        menu.addItem("Delete", [this, slot]
                     {
                         Collection::shared().update(id,
                                                     [slot](Track& track)
                                                     {
                                                         track.cues.erase(std::remove_if(track.cues.begin(), track.cues.end(),
                                                                                         [slot](const Cue& c) { return c.slot == slot; }),
                                                                          track.cues.end());
                                                     });
                     });
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void TrackEditor::addMemoryCue()
{
    const double at = snapped(playhead);
    Collection::shared().update(id,
                                [at](Track& track)
                                {
                                    for (const auto& c : track.cues)
                                        if (!c.isHot() && std::abs(c.pos - at) < 0.01) return;
                                    Cue c;
                                    c.pos = at;
                                    track.cues.push_back(c);
                                });
}

void TrackEditor::cueMenuAt(double sec)
{
    // The memory cue nearest the click, within a few pixels.
    const auto* t = track();
    if (t == nullptr) return;
    const double tolerance = zoomSeconds / std::max(1.0F, detailArea().getWidth()) * 6.0;
    const Cue* nearest = nullptr;
    for (const auto& c : t->cues)
        if (!c.isHot() && std::abs(c.pos - sec) < tolerance && (nearest == nullptr || std::abs(c.pos - sec) < std::abs(nearest->pos - sec)))
            nearest = &c;
    juce::PopupMenu menu;
    menu.addItem("Add Memory Cue Here", [this, sec]
                 {
                     seek(snapped(sec));
                     addMemoryCue();
                 });
    if (nearest != nullptr)
        menu.addItem("Delete Memory Cue at " + clock(nearest->pos), [this, pos = nearest->pos]
                     {
                         Collection::shared().update(id,
                                                     [pos](Track& track)
                                                     {
                                                         track.cues.erase(std::remove_if(track.cues.begin(), track.cues.end(),
                                                                                         [pos](const Cue& c) { return !c.isHot() && std::abs(c.pos - pos) < 1e-6; }),
                                                                          track.cues.end());
                                                     });
                     });
    menu.addItem("Put Beat 1 Here", [this, sec]
                 {
                     seek(sec);
                     downbeatHere();
                 });
    menu.showMenuAsync(juce::PopupMenu::Options());
}

// ── The grid ────────────────────────────────────────────────────────────────

void TrackEditor::shiftGrid(double ms)
{
    Collection::shared().update(id,
                                [ms](Track& t)
                                {
                                    const double by = ms / 1000.0;
                                    t.anchorSec += by;
                                    for (auto& b : t.beats) b += by;
                                    t.gridLocked = true;
                                });
}

void TrackEditor::scaleTempo(double factor)
{
    Collection::shared().update(id,
                                [factor](Track& t)
                                {
                                    if (t.bpm <= 0.0) return;
                                    // A moving grid becomes a steady one at its average tempo.
                                    t.bpm *= factor;
                                    t.beats.clear();
                                    if (factor < 1.0) t.firstBeat = t.firstBeat > 0 ? 1 : 0;
                                    t.gridLocked = true;
                                });
}

void TrackEditor::setTempo(double bpm)
{
    if (!(bpm >= 40.0 && bpm <= 300.0)) return;
    Collection::shared().update(id,
                                [bpm](Track& t)
                                {
                                    if (t.bpm <= 0.0) t.anchorSec = 0.0;
                                    t.bpm = bpm;
                                    t.beats.clear();
                                    t.gridLocked = true;
                                });
}

void TrackEditor::downbeatHere()
{
    const double at = playhead;
    Collection::shared().update(id,
                                [at](Track& t)
                                {
                                    if (t.bpm <= 0.0) return;
                                    // The steady grid through this point, with beat 1 here.
                                    t.beats.clear();
                                    t.anchorSec = at;
                                    t.firstBeat = 1;
                                    t.gridLocked = true;
                                });
}

void TrackEditor::tap()
{
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (!taps.empty() && now - taps.back() > 2.0) taps.clear();
    taps.push_back(now);
    if (taps.size() > 16) taps.erase(taps.begin());
    if (taps.size() >= 4)
    {
        const double interval = (taps.back() - taps.front()) / static_cast<double>(taps.size() - 1);
        if (interval > 0.0) bpmField.setText(juce::String(60.0 / interval, 2), false);
        if (taps.size() >= 8) setTempo(std::round(60.0 / interval * 100.0) / 100.0);
    }
}

// ── Drawing and the mouse ───────────────────────────────────────────────────

juce::Rectangle<float> TrackEditor::overviewArea() const
{
    auto r = getLocalBounds().reduced(8, 4);
    r.removeFromTop(kTopRow + 4);
    return r.removeFromTop(34).toFloat();
}

juce::Rectangle<float> TrackEditor::detailArea() const
{
    auto r = getLocalBounds().reduced(8, 4);
    r.removeFromTop(kTopRow + 4 + 34 + 4);
    r.removeFromBottom(kControlRow * 2 + 8);
    return r.toFloat();
}

double TrackEditor::secAtOverview(float x) const
{
    const auto* t = track();
    const auto area = overviewArea();
    if (t == nullptr || area.getWidth() <= 0.0F) return 0.0;
    return juce::jlimit(0.0, t->durationSec, (x - area.getX()) / area.getWidth() * t->durationSec);
}

double TrackEditor::secAtDetail(float x) const
{
    const auto area = detailArea();
    return playhead + (x - area.getCentreX()) / std::max(1.0F, area.getWidth()) * zoomSeconds;
}

void TrackEditor::paintGrid(juce::Graphics& g, juce::Rectangle<float> area, double startSec, double lengthSec, bool numbers) const
{
    const auto* t = track();
    if (t == nullptr || lengthSec <= 0.0) return;
    const auto beats = t->beatTimes();
    const auto from = std::lower_bound(beats.begin(), beats.end(), startSec);
    const double pxPerBeat = t->bpm > 0.0 ? area.getWidth() / lengthSec * 60.0 / t->bpm : 0.0;
    if (!numbers && pxPerBeat < 3.0)
    {
        // Too dense: bars only.
    }
    g.setFont(uiFont(10.0F, true));
    for (auto it = from; it != beats.end() && *it < startSec + lengthSec; ++it)
    {
        const int index = static_cast<int>(it - beats.begin());
        const int number = t->beatNumberAt(index);
        if (!numbers && number != 1 && pxPerBeat < 6.0) continue;
        const float x = area.getX() + static_cast<float>((*it - startSec) / lengthSec) * area.getWidth();
        const bool bar = number == 1;
        g.setColour(bar ? colours::red500.withAlpha(numbers ? 0.9F : 0.6F) : juce::Colours::white.withAlpha(numbers ? 0.35F : 0.12F));
        g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
        if (numbers && number > 0)
            g.drawText(juce::String(number), juce::Rectangle<float>(x + 2.0F, area.getY(), 14.0F, 12.0F), juce::Justification::topLeft);
        // In the zoomed view, the half beat after it (cues snap to those too).
        if (numbers && it + 1 != beats.end())
        {
            const float hx = area.getX() + static_cast<float>((0.5 * (*it + *(it + 1)) - startSec) / lengthSec) * area.getWidth();
            g.setColour(juce::Colours::white.withAlpha(0.13F));
            g.drawVerticalLine(juce::roundToInt(hx), area.getY() + area.getHeight() * 0.3F, area.getBottom() - area.getHeight() * 0.3F);
        }
    }
}

void TrackEditor::paintCues(juce::Graphics& g, juce::Rectangle<float> area, double startSec, double lengthSec, bool labels) const
{
    const auto* t = track();
    if (t == nullptr || lengthSec <= 0.0) return;
    const auto xAt = [&](double sec) { return area.getX() + static_cast<float>((sec - startSec) / lengthSec) * area.getWidth(); };
    for (const auto& c : t->cues)
    {
        if (c.pos < startSec - lengthSec || c.pos > startSec + lengthSec * 2.0) continue;
        const float x = xAt(c.pos);
        const auto colour = c.isHot() ? (c.colour > 0 ? trackColour(c.colour) : hotCueColour(c.slot)) : colours::red500;
        if (c.isLoop())
        {
            g.setColour(colour.withAlpha(0.18F));
            g.fillRect(juce::Rectangle<float>(x, area.getY(), std::max(1.0F, xAt(c.end) - x), area.getHeight()));
        }
        g.setColour(colour);
        g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
        if (c.isHot())
        {
            const auto tag = juce::Rectangle<float>(x, area.getY(), labels ? 16.0F : 10.0F, labels ? 14.0F : 10.0F);
            g.fillRect(tag);
            if (labels)
            {
                g.setColour(juce::Colours::black);
                g.setFont(uiFont(10.5F, true));
                g.drawText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + c.slot)), tag, juce::Justification::centred);
                if (c.name.isNotEmpty())
                {
                    g.setColour(colour);
                    g.drawText(c.name, juce::Rectangle<float>(x + 18.0F, area.getY(), 120.0F, 14.0F), juce::Justification::centredLeft);
                }
            }
        }
        else
        {
            juce::Path p;
            p.addTriangle(x - 4.0F, area.getBottom(), x + 4.0F, area.getBottom(), x, area.getBottom() - 7.0F);
            g.fillPath(p);
        }
    }
}

void TrackEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::panel);
    g.setColour(colours::border);
    g.drawHorizontalLine(0, 0.0F, static_cast<float>(getWidth()));
    const auto* t = track();
    const auto overview = overviewArea();
    const auto detail = detailArea();
    g.setColour(colours::well);
    g.fillRect(overview);
    g.fillRect(detail);
    if (t == nullptr || t->durationSec <= 0.0)
    {
        g.setColour(colours::textFaint);
        g.setFont(uiFont(13.0F));
        g.drawText(t == nullptr ? juce::String("Pick a track to see its beat grid and cues")
                                : juce::String("Waiting for the analysis..."),
                   detail, juce::Justification::centred);
        return;
    }
    const auto path = playablePath.isNotEmpty() ? playablePath : t->path;
    const double lengthMs = t->durationSec * 1000.0;
    if (auto peaks = waveforms.get(path))
    {
        drawPeaks(g, *peaks, overview.reduced(0.0F, 2.0F), 0.0, lengthMs, false, colours::zinc400);
        const double start = playhead - zoomSeconds / 2.0;
        const double clippedStart = std::max(0.0, start);
        const double clippedEnd = std::min(t->durationSec, start + zoomSeconds);
        if (clippedEnd > clippedStart)
        {
            auto r = detail;
            r.setLeft(detail.getX() + static_cast<float>((clippedStart - start) / zoomSeconds) * detail.getWidth());
            r.setRight(detail.getX() + static_cast<float>((clippedEnd - start) / zoomSeconds) * detail.getWidth());
            drawPeaks(g, *peaks, r.reduced(0.0F, 4.0F), clippedStart * 1000.0, (clippedEnd - clippedStart) * 1000.0, false, colours::accent);
        }
    }
    paintGrid(g, overview, 0.0, t->durationSec, false);
    paintCues(g, overview, 0.0, t->durationSec, false);
    paintGrid(g, detail, playhead - zoomSeconds / 2.0, zoomSeconds, true);
    paintCues(g, detail, playhead - zoomSeconds / 2.0, zoomSeconds, true);

    // The playhead: a line on the overview, the centre of the detail view.
    g.setColour(juce::Colours::white);
    const float px = overview.getX() + static_cast<float>(playhead / t->durationSec) * overview.getWidth();
    g.fillRect(juce::Rectangle<float>(px - 1.0F, overview.getY(), 2.0F, overview.getHeight()));
    g.fillRect(juce::Rectangle<float>(detail.getCentreX() - 1.0F, detail.getY(), 2.0F, detail.getHeight()));
    g.setFont(uiFont(11.0F));
    g.setColour(colours::text);
    g.drawText(clock(playhead), detail.withTrimmedLeft(detail.getWidth() / 2.0F + 6.0F).removeFromBottom(14.0F),
               juce::Justification::centredLeft);
}

void TrackEditor::resized()
{
    auto r = getLocalBounds().reduced(8, 4);
    auto top = r.removeFromTop(kTopRow);
    colourBox.setBounds(top.removeFromRight(96).reduced(0, 2));
    top.removeFromRight(6);
    ratingBox.setBounds(top.removeFromRight(96).reduced(0, 2));
    top.removeFromRight(10);
    info.setBounds(top.removeFromRight(top.getWidth() / 2));
    title.setBounds(top);

    auto controls = r.removeFromBottom(kControlRow * 2 + 6);
    auto row1 = controls.removeFromTop(kControlRow);
    controls.removeFromTop(6);
    auto row2 = controls;
    playButton.setBounds(row1.removeFromLeft(64).reduced(0, 2));
    row1.removeFromLeft(4);
    cueButton.setBounds(row1.removeFromLeft(48).reduced(0, 2));
    row1.removeFromLeft(18);
    auto bpmLabel = row1.removeFromLeft(34);
    juce::ignoreUnused(bpmLabel);
    bpmField.setBounds(row1.removeFromLeft(70).reduced(0, 3));
    row1.removeFromLeft(4);
    for (auto* b : {&halfButton, &doubleButton})
    {
        b->setBounds(row1.removeFromLeft(36).reduced(0, 2));
        row1.removeFromLeft(3);
    }
    row1.removeFromLeft(8);
    for (auto* b : {&earlierButton, &laterButton})
    {
        b->setBounds(row1.removeFromLeft(30).reduced(0, 2));
        row1.removeFromLeft(3);
    }
    row1.removeFromLeft(8);
    downbeatButton.setBounds(row1.removeFromLeft(66).reduced(0, 2));
    row1.removeFromLeft(4);
    tapButton.setBounds(row1.removeFromLeft(48).reduced(0, 2));
    row1.removeFromLeft(10);
    quantize.setBounds(row1.removeFromLeft(44).reduced(0, 2));

    const int cueWidth = std::min(84, (row2.getWidth() - 100) / 8);
    for (auto& b : hotCueButtons)
    {
        b.setBounds(row2.removeFromLeft(cueWidth).reduced(1, 2));
    }
    row2.removeFromLeft(10);
    memoryButton.setBounds(row2.removeFromLeft(90).reduced(0, 2));
}

void TrackEditor::mouseDown(const juce::MouseEvent& e)
{
    // A hot cue button's right-click.
    for (int i = 0; i < 8; ++i)
        if (e.eventComponent == &hotCueButtons[static_cast<size_t>(i)])
        {
            if (e.mods.isPopupMenu()) hotCueMenu(i);
            return;
        }
    if (e.eventComponent != this) return;
    const auto pos = e.position;
    if (overviewArea().contains(pos))
    {
        draggingOverview = true;
        seek(secAtOverview(pos.x));
    }
    else if (detailArea().contains(pos))
    {
        if (e.mods.isPopupMenu())
        {
            cueMenuAt(secAtDetail(pos.x));
            return;
        }
        draggingDetail = true;
        dragStartX = pos.x;
        dragStartPlayhead = playhead;
    }
    grabKeyboardFocus();
}

void TrackEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (e.eventComponent != this) return;
    if (draggingOverview)
        seek(secAtOverview(e.position.x));
    else if (draggingDetail)
        seek(dragStartPlayhead - (e.position.x - dragStartX) / std::max(1.0F, detailArea().getWidth()) * zoomSeconds);
}

void TrackEditor::mouseUp(const juce::MouseEvent&)
{
    draggingOverview = false;
    draggingDetail = false;
}

void TrackEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!detailArea().contains(e.position)) return;
    zoomSeconds = juce::jlimit(1.0, 64.0, zoomSeconds * (wheel.deltaY > 0 ? 0.8 : 1.25));
    repaint();
}

bool TrackEditor::keyPressed(const juce::KeyPress& key)
{
    if (track() == nullptr) return false;
    if (key == juce::KeyPress::spaceKey)
    {
        togglePlay();
        return true;
    }
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
    {
        // A beat back or on (a bar with Shift).
        const auto* t = track();
        const double beat = t->bpm > 0.0 ? 60.0 / t->bpm : 0.5;
        const double by = (key.getModifiers().isShiftDown() ? 4.0 : 1.0) * beat * (key == juce::KeyPress::leftKey ? -1.0 : 1.0);
        seek(playhead + by);
        return true;
    }
    const auto c = key.getTextCharacter();
    if (c >= '1' && c <= '8')
    {
        hotCuePressed(static_cast<int>(c - '1'));
        return true;
    }
    if (c == 'm' || c == 'M')
    {
        addMemoryCue();
        return true;
    }
    return false;
}

} // namespace stemcel::native::dj
