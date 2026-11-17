// Stem Mix in performance mode: a deck's track's stems (vocals, the instrumental, bass and
// drums) taken in and out of its mix on the pads. Double-press SAMPLER (or the deck's STEMS
// button) for the stem pads. The stems come from the library: separated already, they are read
// for the deck as it loads; otherwise STEMS separates them (UVR's models, as the library's
// Separate Stems does) and the deck takes them when the separation ends. The instrumental is
// what is left of the track without the other three; it is written once as a "Melody" stem
// of its own, so a take made with stems out can put each stem on the timeline.

#include "PerformanceView.h"

#include "Log.h"
#include "Theme.h"

namespace stemcel::native::perf
{

namespace
{
// The library names of the stems, in pad order. With all four the instrumental is "Melody":
// everything but the vocals, drums and bass. A two-stem split (vocals and the rest) has an
// "Instrumental" (everything but the vocals) and no bass or drums.
const juce::StringArray kStemLibraryLabels{"Vocals", "Melody", "Bass", "Drums"};
const juce::String kTwoStemInstrumental{"Instrumental"};
} // namespace

std::array<juce::String, kStems> PerformanceView::stemItemsFor(const juce::String& itemId) const
{
    std::array<juce::String, kStems> out;
    if (itemId.isEmpty()) return out;
    for (int s = 0; s < kStems; ++s) out[static_cast<size_t>(s)] = ctx.stems.findStem(itemId, kStemLibraryLabels[s]);
    // No bass and drums: a two-stem split, its instrumental the other half.
    if (out[2].isEmpty() || out[3].isEmpty())
    {
        out[1] = ctx.stems.findStem(itemId, kTwoStemInstrumental);
        out[2] = out[3] = {};
    }
    return out;
}

std::optional<double> PerformanceView::danceBeats() const
{
    int lead = state.masterDeck;
    const auto playing = [&](int deck)
    {
        const auto& s = state.decks[static_cast<size_t>(deck - 1)];
        return s.loaded && (s.playing || s.previewing);
    };
    if (lead < 1 || lead > kDecks || !playing(lead))
    {
        lead = 0;
        for (int deck = 1; deck <= kDecks && lead == 0; ++deck)
            if (playing(deck)) lead = deck;
    }
    if (lead == 0) return std::nullopt;
    const auto& info = infos[static_cast<size_t>(lead - 1)];
    if (info.bpm <= 0.0) return std::nullopt;
    return (decks[static_cast<size_t>(lead - 1)].shownPos - info.anchorSec) * info.bpm / 60.0;
}

void PerformanceView::stemProgress(double percent, const juce::String& stage)
{
    stemPercent = percent;
    stemStage = stage;
}

void PerformanceView::stemsButtonPressed(int deck)
{
    const auto& s = state.decks[static_cast<size_t>(deck - 1)];
    if (!s.loaded) return;
    if (s.hasStems)
    {
        // The stem pads, or back to hot cues.
        sendControl(ctx.engine, "padMode", deck, 1.0, 0, s.padMode == "stems" ? "hotCue" : "stems");
        return;
    }
    // No stems yet: separate, as two stems (fast) or four (the ensemble).
    juce::PopupMenu menu;
    const bool fourLast = ctx.prefs.get("performance", "stemSplit", "four").toString() == "four";
    menu.addSectionHeader("Separate the stems of " + infos[static_cast<size_t>(deck - 1)].title);
    menu.addItem(2, "Vocals and instrumental (2 stems, faster)", true, !fourLast);
    menu.addItem(4, "Ensemble: vocals, instrumental, bass, drums (4 stems)", true, fourLast);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(decks[static_cast<size_t>(deck - 1)].stemsButton.get()),
                       [safe = juce::Component::SafePointer<PerformanceView>(this), deck](int chosen)
                       {
                           if (safe == nullptr || chosen == 0) return;
                           safe->ctx.prefs.set("performance", "stemSplit", chosen == 2 ? "two" : "four");
                           safe->ctx.prefs.save();
                           safe->prepareStems(deck, true, chosen == 2);
                           sendControl(safe->ctx.engine, "padMode", deck, 1.0, 0, "stems");
                       });
}

void PerformanceView::prepareStems(int deck, bool separateIfMissing)
{
    // Stem Mix chosen on the controller: the split chosen last on screen.
    prepareStems(deck, separateIfMissing, ctx.prefs.get("performance", "stemSplit", "four").toString() == "two");
}

void PerformanceView::prepareStems(int deck, bool separateIfMissing, bool twoStems)
{
    auto& ds = deckStems[static_cast<size_t>(deck - 1)];
    const auto& info = infos[static_cast<size_t>(deck - 1)];
    if (info.itemId.isEmpty())
    {
        if (separateIfMissing && ctx.toast) ctx.toast("Stems: the track is still joining the library; try again in a moment", false);
        return;
    }
    if (ds.serial != info.serial)
    {
        ds = {};
        ds.serial = info.serial;
    }
    if (ds.loading || ds.separating) return;
    const auto items = stemItemsFor(info.itemId);
    const auto& model = ctx.mirror.model();
    const auto pathOf = [&](int stem) -> juce::String
    {
        const auto* item = model.findLibraryItem(items[static_cast<size_t>(stem)]);
        return item != nullptr ? item->audioPath() : juce::String();
    };
    const auto vocals = pathOf(0), bass = pathOf(2), drums = pathOf(3);
    // What the library has: all four (bass and drums too), or two (the vocals; the rest is the
    // track less them). Two will do unless four were asked for.
    const bool haveFour = vocals.isNotEmpty() && bass.isNotEmpty() && drums.isNotEmpty();
    const bool haveTwo = vocals.isNotEmpty() && !haveFour;
    if (haveFour || (haveTwo && (twoStems || !separateIfMissing)))
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("deck", deck);
        o->setProperty("serial", info.serial);
        o->setProperty("vocals", vocals);
        if (haveFour)
        {
            o->setProperty("drums", drums);
            o->setProperty("bass", bass);
        }
        // The instrumental written once, beside the vocals.
        if (items[1].isEmpty())
            o->setProperty("instPath", juce::File(vocals).getParentDirectory()
                                           .getChildFile(info.itemId + (haveFour ? "-melody.wav" : "-instrumental.wav"))
                                           .getFullPathName());
        ds.loading = true;
        ds.twoStems = !haveFour;
        ctx.engine.send("PERF_STEMS", juce::var(o));
        return;
    }
    if (!separateIfMissing) return;
    if (ctx.stems.busy())
    {
        if (ctx.toast) ctx.toast("Stems: another separation is running; STEMS again once it ends", false);
        return;
    }
    // Four: the library's best recipe, Mel-Band RoFormer for the vocals and BS-Roformer SW for
    // the drums and bass (still the best open models for each, by the separation guides).
    // Two: Mel-Band RoFormer's vocals alone (one model, so a good deal faster).
    ds.separating = true;
    ds.twoStems = twoStems;
    // (Vocals already split off: only the drums and bass, from BS-Roformer SW.)
    if (twoStems) ctx.stems.startUvr(info.itemId, {}, "vocalsTop", {}, {"vocals"});
    else if (vocals.isNotEmpty()) ctx.stems.startUvr(info.itemId, {}, "six", {}, {"drums", "bass"});
    else ctx.stems.startUvr(info.itemId, {}, "best", {}, {"vocals", "drums", "bass"});
    if (ctx.toast)
        ctx.toast("Separating " + juce::String(twoStems ? "the vocals" : "the stems") + " of " + info.title + " for deck " + juce::String(deck) + "...", false);
}

void PerformanceView::onStemsReady(const juce::var& p)
{
    const int deck = static_cast<int>(p.getProperty("deck", 0));
    if (deck < 1 || deck > kDecks) return;
    auto& ds = deckStems[static_cast<size_t>(deck - 1)];
    const auto& info = infos[static_cast<size_t>(deck - 1)];
    if (static_cast<int>(p.getProperty("serial", -1)) != ds.serial) return;
    ds.loading = false;
    if (!static_cast<bool>(p.getProperty("ok", false)))
    {
        ds.failed = true;
        log::warn("performance", "stems for deck " + juce::String(deck) + ": " + p.getProperty("error", "").toString());
        if (ctx.toast) ctx.toast("Stems: " + p.getProperty("error", "couldn't read them").toString(), true);
        return;
    }
    const auto instPath = p.getProperty("instPath", "").toString();
    const auto instLabel = ds.twoStems ? kTwoStemInstrumental : kStemLibraryLabels[1];
    if (instPath.isNotEmpty() && info.itemId.isNotEmpty() && ctx.stems.findStem(info.itemId, instLabel).isEmpty())
        ctx.stems.addLibraryStem(info.itemId, instLabel, juce::File(instPath));
}

void PerformanceView::stemTick()
{
    for (int deck = 1; deck <= kDecks; ++deck)
    {
        auto& ds = deckStems[static_cast<size_t>(deck - 1)];
        const auto& s = state.decks[static_cast<size_t>(deck - 1)];
        const auto& info = infos[static_cast<size_t>(deck - 1)];
        if (!s.loaded || info.serial == 0) continue;
        if (ds.serial != info.serial)
        {
            // A new track: its stems are read now if the library has them.
            ds = {};
            ds.serial = info.serial;
            prepareStems(deck, false);
        }
        // A separation for it ended: the deck takes the stems.
        if (ds.separating && !ctx.stems.busy())
        {
            ds.separating = false;
            if (stemItemsFor(info.itemId)[0].isEmpty())
            {
                ds.failed = true;
                if (ctx.toast) ctx.toast("Stems: the separation didn't finish for " + info.title, true);
            }
            else prepareStems(deck, false);
        }
        // Stem Mix chosen on the controller for a track without stems: separate them (not in
        // the smoke test, which has no models to wait for).
        static const bool smokeTest = juce::JUCEApplicationBase::getCommandLineParameters().contains("--smoke-test");
        if (s.padMode == "stems" && !s.hasStems && !ds.loading && !ds.separating && !ds.failed && !ds.asked && !smokeTest)
        {
            ds.asked = true;
            prepareStems(deck, true);
        }
    }
}

juce::String PerformanceView::stemsButtonText(int deck) const
{
    const auto& ds = deckStems[static_cast<size_t>(deck - 1)];
    if (ds.separating) return stemPercent >= 0.0 ? "SEP " + juce::String(juce::roundToInt(stemPercent)) + "%" : juce::String("SEP...");
    if (ds.loading) return "STEMS...";
    return "STEMS";
}

void PerformanceView::paintStemLegend(juce::Graphics& g, int deck) const
{
    // The key: which pad is which stem (the controller's pads are one colour), and whether each
    // is in: 1–4 in or out, 5–8 alone, SHIFT + a pad all back.
    const auto area = legendAreas[static_cast<size_t>(deck - 1)];
    if (area.isEmpty()) return;
    const auto& s = state.decks[static_cast<size_t>(deck - 1)];
    auto r = area.toFloat();
    g.setFont(uiFont(10.0F, true));
    const float chipW = juce::jmin(78.0F, (r.getWidth() - 120.0F) / static_cast<float>(kStems));
    for (int stem = 0; stem < kStems; ++stem)
    {
        auto chip = r.removeFromLeft(chipW).reduced(2.0F, 1.0F);
        if (s.hasStems && (s.stemsPresent & (1 << stem)) == 0) continue; // a two-stem split's bass and drums
        const bool in = (s.stemsOff & (1 << stem)) == 0;
        const auto colour = stemColour(stem);
        g.setColour(colour.withAlpha(in ? 0.9F : 0.18F));
        g.fillRoundedRectangle(chip.removeFromLeft(chip.getHeight()).reduced(2.0F), 2.0F);
        g.setColour(in ? colours::text : colours::textFaint);
        g.drawText(juce::String(stem + 1) + " " + kStemNames[stem], chip.withTrimmedLeft(3.0F), juce::Justification::centredLeft, false);
    }
    g.setColour(colours::textFaint);
    g.setFont(uiFont(10.0F));
    const juce::String hint = s.hasStems ? "5-8 solo · SHIFT+pad all in" : (deckStems[static_cast<size_t>(deck - 1)].separating ? "separating..." : "no stems yet");
    g.drawText(hint, r, juce::Justification::centredRight, true);
}

} // namespace stemcel::native::perf
