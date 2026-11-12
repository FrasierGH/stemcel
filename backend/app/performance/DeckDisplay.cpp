#include "DeckDisplay.h"

#include "Theme.h"

namespace stemcel::native::perf
{
namespace
{
juce::String clock(double seconds)
{
    seconds = juce::jmax(0.0, seconds);
    const int m = static_cast<int>(seconds / 60.0);
    const double s = seconds - m * 60.0;
    return juce::String(m) + ":" + juce::String(s, 1).paddedLeft('0', 4);
}

const juce::Colour kHotCueColours[8] = {juce::Colour(0xff28e214), juce::Colour(0xff30d2ff), juce::Colour(0xffff8c00),
                                        juce::Colour(0xffe6e600), juce::Colour(0xffff3c78), juce::Colour(0xff9e7bff),
                                        juce::Colour(0xff00e0b0), juce::Colour(0xffff5050)};
} // namespace

void DeckDisplay::update(const DeckState& s, const DeckTrackInfo& i, double posSec, bool master)
{
    state = s;
    info = i;
    pos = posSec;
    isMaster = master;
    repaint();
}

void DeckDisplay::showTrack(const juce::String& name, std::vector<TrackClip> clips, double playheadMs, double lengthMs, bool playing)
{
    trackMode = true;
    trackName = name;
    trackClips = std::move(clips);
    trackPlayheadMs = playheadMs;
    trackLengthMs = std::max(lengthMs, playheadMs + 1000.0);
    trackPlaying = playing;
    repaint();
}

void DeckDisplay::drawClips(juce::Graphics& g, juce::Rectangle<float> area, double fromMs, double toMs)
{
    const double span = toMs - fromMs;
    if (span <= 0.0) return;
    auto xAt = [&](double ms) { return area.getX() + static_cast<float>((ms - fromMs) / span) * area.getWidth(); };
    for (const auto& c : trackClips)
    {
        const double a = std::max(c.startMs, fromMs), b = std::min(c.endMs, toMs);
        if (b <= a) continue;
        auto r = juce::Rectangle<float>(xAt(a), area.getY(), std::max(1.0F, xAt(b) - xAt(a)), area.getHeight());
        g.setColour(colours::accent.withAlpha(0.12F));
        g.fillRect(r);
        if (c.audioPath.isEmpty()) continue;
        if (auto peaks = waveforms.get(c.audioPath))
            drawPeaks(g, *peaks, r.reduced(0.0F, 2.0F), c.inMs + (a - c.startMs) * c.ratio, (b - a) * c.ratio, false, colours::accent);
    }
}

void DeckDisplay::paintTrack(juce::Graphics& g)
{
    auto header = getLocalBounds().reduced(8, 6).removeFromTop(38);
    g.setColour(colours::amber400);
    g.setFont(uiFont(22.0F, true));
    g.drawText(juce::String(deck), header.removeFromLeft(24), juce::Justification::centredLeft);
    auto right = header.removeFromRight(160);
    g.setColour(colours::text);
    g.setFont(uiFont(15.0F, true));
    g.drawText("Timeline track: " + trackName, header.removeFromTop(20), juce::Justification::centredLeft, true);
    g.setColour(colours::textDim);
    g.setFont(uiFont(12.0F));
    g.drawText("The knobs and FX act on this track; with REC on, moves are written as it plays", header,
               juce::Justification::centredLeft, true);
    const double sec = trackPlayheadMs / 1000.0;
    const int m = static_cast<int>(sec / 60.0);
    g.setColour(trackPlaying ? colours::emerald500 : colours::text);
    g.setFont(uiFont(16.0F, true));
    g.drawText(juce::String(m) + ":" + juce::String(sec - m * 60.0, 1).paddedLeft('0', 4), right.removeFromTop(20),
               juce::Justification::centredRight);

    auto overview = overviewArea().toFloat();
    drawWell(g, overview);
    drawClips(g, overview, 0.0, trackLengthMs);
    g.setColour(colours::text);
    g.fillRect(overview.getX() + static_cast<float>(trackPlayheadMs / trackLengthMs) * overview.getWidth() - 1.0F, overview.getY(), 2.0F,
               overview.getHeight());
    auto close = closeUpArea().toFloat();
    drawWell(g, close);
    g.saveState();
    g.reduceClipRegion(close.toNearestInt());
    drawClips(g, close, trackPlayheadMs - halfWindow * 1000.0, trackPlayheadMs + halfWindow * 1000.0);
    g.restoreState();
    g.setColour(colours::red500);
    g.fillRect(close.getCentreX() - 1.0F, close.getY(), 2.0F, close.getHeight());
}

juce::Rectangle<int> DeckDisplay::overviewArea() const
{
    auto r = getLocalBounds().reduced(6);
    r.removeFromTop(42);
    return r.removeFromTop(34);
}

juce::Rectangle<int> DeckDisplay::closeUpArea() const
{
    auto r = getLocalBounds().reduced(6);
    r.removeFromTop(42 + 34 + 6);
    return r;
}

void DeckDisplay::paint(juce::Graphics& g)
{
    g.setColour(colours::panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0F);
    if (trackMode)
    {
        paintTrack(g);
        return;
    }
    auto header = getLocalBounds().reduced(8, 6).removeFromTop(38);

    // Deck number, title and artist.
    g.setColour(isMaster ? colours::amber400 : colours::accent);
    g.setFont(uiFont(22.0F, true));
    g.drawText(juce::String(deck), header.removeFromLeft(24), juce::Justification::centredLeft);
    auto right = header.removeFromRight(240);
    g.setColour(colours::text);
    g.setFont(uiFont(15.0F, true));
    auto titleRow = header.removeFromTop(20);
    g.drawText(state.loaded ? info.title : "Load a track (browse, then LOAD)", titleRow, juce::Justification::centredLeft, true);
    g.setColour(colours::textDim);
    g.setFont(uiFont(12.0F));
    g.drawText(info.artist, header, juce::Justification::centredLeft, true);

    // Tempo, key and time on the right.
    if (state.loaded)
    {
        auto bpmBox = right.removeFromRight(86);
        g.setColour(state.sync ? colours::accent : colours::text);
        g.setFont(uiFont(20.0F, true));
        g.drawText(state.effectiveBpm > 0.0 ? juce::String(state.effectiveBpm, 2) : "--.--", bpmBox.removeFromTop(22),
                   juce::Justification::centredRight);
        g.setFont(uiFont(11.0F));
        g.setColour(colours::textDim);
        const double pct = (state.tempo - 1.0) * 100.0;
        const juce::String range = state.range >= 0.99 ? "WIDE" : "±" + juce::String(juce::roundToInt(state.range * 100.0)) + "%";
        g.drawText((pct >= 0 ? "+" : "") + juce::String(pct, 2) + "%  " + range, bpmBox, juce::Justification::centredRight);

        auto keyBox = right.removeFromRight(70);
        g.setFont(uiFont(13.0F, true));
        g.setColour(state.keyLock ? colours::accent : colours::textDim);
        juce::String key = info.key.isNotEmpty() ? info.key : "-";
        if (state.semitones != 0) key << " " << (state.semitones > 0 ? "+" : "") << state.semitones;
        g.drawText(key, keyBox.removeFromTop(20), juce::Justification::centredRight);
        g.setFont(uiFont(11.0F));
        juce::String flags;
        if (state.keyLock) flags << "MT ";
        if (state.quantize) flags << "Q ";
        if (state.sync) flags << (state.master ? "MASTER" : "SYNC");
        else if (state.master) flags << "MASTER";
        g.drawText(flags.trim(), keyBox, juce::Justification::centredRight);

        g.setFont(uiFont(16.0F, true));
        g.setColour(colours::text);
        g.drawText("-" + clock(state.lengthSec - pos), right.removeFromTop(20), juce::Justification::centredRight);
        g.setFont(uiFont(11.0F));
        g.setColour(colours::textDim);
        g.drawText(clock(pos), right, juce::Justification::centredRight);
    }

    paintOverview(g, overviewArea().toFloat());
    paintCloseUp(g, closeUpArea().toFloat());
}

void DeckDisplay::paintOverview(juce::Graphics& g, juce::Rectangle<float> area)
{
    drawWell(g, area);
    if (!state.loaded || state.lengthSec <= 0.0) return;
    const double lengthMs = state.lengthSec * 1000.0;
    if (info.audioPath.isNotEmpty())
        if (auto peaks = waveforms.get(info.audioPath))
            drawPeaks(g, *peaks, area.reduced(1.0F), 0.0, lengthMs, false, colours::zinc400);
    auto xAt = [&](double sec) { return area.getX() + static_cast<float>(sec / state.lengthSec) * area.getWidth(); };
    if (state.loopActive)
    {
        g.setColour(colours::amber400.withAlpha(0.35F));
        g.fillRect(juce::Rectangle<float>(xAt(state.loopInSec), area.getY(), juce::jmax(2.0F, xAt(state.loopOutSec) - xAt(state.loopInSec)),
                                          area.getHeight()));
    }
    for (int i = 0; i < 8; ++i)
        if (state.hotCues[static_cast<size_t>(i)].set())
        {
            g.setColour(kHotCueColours[i]);
            g.fillRect(xAt(state.hotCues[static_cast<size_t>(i)].pos) - 1.0F, area.getY(), 2.0F, 6.0F);
        }
    for (const auto& m : state.memoryCues)
    {
        g.setColour(colours::red500);
        g.fillRect(xAt(m.pos) - 1.0F, area.getBottom() - 5.0F, 2.0F, 5.0F);
    }
    g.setColour(colours::amber400);
    g.fillRect(xAt(state.cueSec) - 1.0F, area.getY(), 2.0F, area.getHeight());
    // What has played is dimmed, as on a CDJ.
    g.setColour(juce::Colours::black.withAlpha(0.35F));
    g.fillRect(area.withRight(xAt(pos)));
    g.setColour(colours::text);
    g.fillRect(xAt(pos) - 1.0F, area.getY(), 2.0F, area.getHeight());
}

void DeckDisplay::paintCloseUp(juce::Graphics& g, juce::Rectangle<float> area)
{
    drawWell(g, area);
    if (!state.loaded) return;
    const double from = pos - halfWindow, span = halfWindow * 2.0;
    auto xAt = [&](double sec) { return area.getX() + static_cast<float>((sec - from) / span) * area.getWidth(); };
    g.saveState();
    g.reduceClipRegion(area.toNearestInt());
    if (state.loopActive)
    {
        g.setColour(colours::amber400.withAlpha(0.18F));
        g.fillRect(juce::Rectangle<float>(xAt(state.loopInSec), area.getY(), xAt(state.loopOutSec) - xAt(state.loopInSec), area.getHeight()));
    }
    // The beat grid: the analysed beats (bars brighter), carried on at the tempo past them.
    if (info.beats.size() >= 2)
    {
        const auto& b = info.beats;
        const double period = info.bpm > 0.0 ? 60.0 / info.bpm : b[1] - b[0];
        // Bars count from the downbeat (the anchor).
        long long downbeat = 0;
        for (size_t i = 0; i < b.size(); ++i)
            if (std::abs(b[i] - info.anchorSec) < std::abs(b[static_cast<size_t>(downbeat)] - info.anchorSec)) downbeat = static_cast<long long>(i);
        auto drawBeat = [&](double t, long long k)
        {
            if (t < from || t > from + span || t < 0.0 || t > state.lengthSec) return;
            const bool bar = (((k - downbeat) % 4) + 4) % 4 == 0;
            g.setColour(bar ? colours::text.withAlpha(0.55F) : colours::textDim.withAlpha(0.3F));
            g.fillRect(xAt(t), area.getY(), bar ? 1.5F : 1.0F, area.getHeight());
        };
        for (size_t i = 0; i < b.size(); ++i) drawBeat(b[i], static_cast<long long>(i));
        const auto n = static_cast<long long>(b.size());
        for (long long k = n; b.back() + static_cast<double>(k - n + 1) * period <= from + span; ++k)
            drawBeat(b.back() + static_cast<double>(k - n + 1) * period, k);
        for (long long k = -1; b.front() + static_cast<double>(k) * period >= from - period; --k)
            drawBeat(b.front() + static_cast<double>(k) * period, k);
    }
    else if (info.bpm > 0.0)
    {
        const double beat = 60.0 / info.bpm;
        const auto first = static_cast<long long>(std::floor((from - info.anchorSec) / beat));
        for (long long k = first; k <= first + static_cast<long long>(span / beat) + 2; ++k)
        {
            const double t = info.anchorSec + static_cast<double>(k) * beat;
            if (t < 0.0 || t > state.lengthSec) continue;
            const bool bar = ((k % 4) + 4) % 4 == 0;
            g.setColour(bar ? colours::text.withAlpha(0.55F) : colours::textDim.withAlpha(0.3F));
            g.fillRect(xAt(t), area.getY(), bar ? 1.5F : 1.0F, area.getHeight());
        }
    }
    if (info.audioPath.isNotEmpty())
        if (auto peaks = waveforms.get(info.audioPath))
        {
            const double startMs = juce::jmax(0.0, from) * 1000.0;
            const double endMs = juce::jmin(state.lengthSec, from + span) * 1000.0;
            if (endMs > startMs)
            {
                auto r = area.reduced(0.0F, 3.0F);
                r.setLeft(xAt(startMs / 1000.0));
                r.setRight(xAt(endMs / 1000.0));
                drawPeaks(g, *peaks, r, startMs, endMs - startMs, false, isMaster ? colours::amber400 : colours::accent);
            }
        }
    for (int i = 0; i < 8; ++i)
        if (const auto& c = state.hotCues[static_cast<size_t>(i)]; c.set())
        {
            g.setColour(kHotCueColours[i]);
            g.fillRect(xAt(c.pos) - 1.0F, area.getY(), 2.0F, area.getHeight());
            g.setFont(uiFont(10.0F, true));
            g.drawText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)),
                       juce::Rectangle<float>(xAt(c.pos) + 2.0F, area.getY() + 1.0F, 12.0F, 12.0F), juce::Justification::centredLeft);
        }
    g.setColour(colours::amber400);
    g.fillRect(xAt(state.cueSec) - 1.0F, area.getY(), 2.0F, area.getHeight());
    g.restoreState();
    // The playhead, centre.
    g.setColour(colours::red500);
    g.fillRect(area.getCentreX() - 1.0F, area.getY(), 2.0F, area.getHeight());
}

void DeckDisplay::mouseDown(const juce::MouseEvent& e)
{
    const auto overview = overviewArea();
    if (overview.contains(e.getPosition()) && state.loaded && onSeek)
        onSeek((e.position.x - static_cast<float>(overview.getX())) / static_cast<float>(overview.getWidth()) * state.lengthSec);
}

} // namespace stemcel::native::perf
