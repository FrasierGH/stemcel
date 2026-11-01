#include "LibraryPanel.h"
#include "AppPaths.h"

#include "MediaStore.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "AutoMix.h"
#include "FilesBrowser.h"
#include "FxPanels.h"
#include "LibraryInfoDialog.h"
#include "ProjectDialogs.h"
#include "RekordboxBrowser.h"
#include "Theme.h"
#include "TransitionsPanel.h"

namespace stemcel::native
{
namespace
{
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        if (!v.isVoid()) o->setProperty(k, v);
    return juce::var(o);
}

juce::String formatDuration(double ms)
{
    const int total = static_cast<int>(ms / 1000.0 + 0.5);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}

void styleTable(juce::TableListBox& table)
{
    table.setColour(juce::ListBox::backgroundColourId, colours::zinc900);
    table.setColour(juce::ListBox::outlineColourId, colours::border);
    table.setRowHeight(24);
    table.getHeader().setColour(juce::TableHeaderComponent::backgroundColourId, colours::zinc800);
    table.getHeader().setColour(juce::TableHeaderComponent::textColourId, colours::textDim);
    table.getHeader().setColour(juce::TableHeaderComponent::outlineColourId, colours::border);
    table.setMultipleSelectionEnabled(true);
}
} // namespace

juce::var importDragDescription(const std::vector<ImportRequest>& requests)
{
    juce::Array<juce::var> list;
    for (const auto& r : requests)
        list.add(object({{"path", r.file.getFullPathName()},
                         {"rekordboxId", r.hints.rekordboxId.isNotEmpty() ? juce::var(r.hints.rekordboxId) : juce::var()},
                         {"title", r.hints.title},
                         {"artist", r.hints.artist},
                         {"key", r.hints.key},
                         {"bpm", r.hints.bpm}}));
    return object({{"import", list}});
}

std::vector<ImportRequest> importRequestsFromDrag(const juce::var& description)
{
    std::vector<ImportRequest> out;
    if (const auto* list = description.getProperty("import", {}).getArray())
        for (const auto& r : *list)
        {
            ImportRequest request;
            request.file = juce::File(r.getProperty("path", {}).toString());
            request.hints.rekordboxId = r.getProperty("rekordboxId", {}).toString();
            request.hints.title = r.getProperty("title", {}).toString();
            request.hints.artist = r.getProperty("artist", {}).toString();
            request.hints.key = r.getProperty("key", {}).toString();
            request.hints.bpm = static_cast<double>(r.getProperty("bpm", 0.0));
            out.push_back(std::move(request));
        }
    return out;
}

// ── Audition ───────────────────────────────────────────────────────────────
Audition::Audition(EngineClient& client, AudioImporter& audioImporter)
    : engine(client), importer(audioImporter), subscriptions(client)
{
    subscriptions.on("PREVIEW_STATE",
                     [this](const juce::var& p)
                     {
                         const bool loaded = static_cast<bool>(p.getProperty("isLoaded", false));
                         const bool isPlaying = static_cast<bool>(p.getProperty("isPlaying", false));
                         if (waitingForLoad && loaded && !isPlaying)
                         {
                             waitingForLoad = false;
                             engine.send("PREVIEW_PLAY");
                             return;
                         }
                         if (!waitingForLoad && playing != isPlaying)
                         {
                             playing = isPlaying;
                             sendChangeMessage();
                         }
                     });
    subscriptions.on("PREVIEW_ENDED",
                     [this](const juce::var&)
                     {
                         playing = false;
                         sendChangeMessage();
                     });
}

Audition::~Audition()
{
    alive->store(false);
    pool.removeAllJobs(true, 10000);
}

void Audition::toggle(const juce::File& target)
{
    if (playing && target == file)
    {
        stop();
        return;
    }
    file = target;
    playing = true;
    sendChangeMessage();
    // Resolving a playable path may transcode (m4a on Windows), so off the message thread.
    pool.addJob(
        [this, target, alive = alive]
        {
            const auto path = importer.playablePath(target);
            juce::MessageManager::callAsync(
                [this, target, path, alive]
                {
                    if (!alive->load() || target != file || !playing) return;
                    if (path.isEmpty())
                    {
                        playing = false;
                        sendChangeMessage();
                        return;
                    }
                    waitingForLoad = true;
                    engine.send("PREVIEW_LOAD", object({{"filePath", path}}));
                });
        });
}

void Audition::stop()
{
    waitingForLoad = false;
    if (playing) engine.send("PREVIEW_STOP");
    playing = false;
    sendChangeMessage();
}

// ── Library items ──────────────────────────────────────────────────────────
// Library tiles (LibraryPanelLibraryView / LibrarySourceGroup / LibraryClipRow): one tile per
// source, stem or sample with its cover art (or a kind icon), title, artist, length, key, BPM
// and use count; its saved clips listed under it behind a disclosure; saved clips whose source
// is gone in a group of their own.
namespace tiles
{
constexpr int kTileWidth = 240;
constexpr int kTileHeight = 75;
constexpr int kGap = 12;
constexpr int kToggleHeight = 22;
constexpr int kRowHeight = 40;

// The renderer's tile icons (Material paths), drawn when there is no cover art.
const char* const kSourceIcon = "M12 3v10.55A4 4 0 1 0 14 17V7h4V3h-6zm0 16a2 2 0 1 1 0-4 2 2 0 0 1 0 4z";
const char* const kStemIcon = "M11.99 18.54l-7.37-5.73L3 14.07l9 7 9-7-1.63-1.27-7.38 5.74zM12 16l7.36-5.73L21 9l-9-7-9 7 1.63 1.27L12 16z";
const char* const kSampleIcon = "M7 18h2V6H7v12zm4 4h2V2h-2v20zm-8-8h2v-4H3v4zm12 4h2V6h-2v12zm4-8v4h2v-4h-2z";
const char* const kScratchIcon = "M12 2C6.477 2 2 6.477 2 12s4.477 10 10 10 10-4.477 10-10S17.523 2 12 2zm0 3.5a6.5 6.5 0 016.5 6.5 1 1 0 11-2 0 "
                                 "4.5 4.5 0 00-4.5-4.5 1 1 0 110-2zM12 10a2 2 0 100 4 2 2 0 000-4z";

// keyBadge.ts: one hue per pitch class, darker for minor keys.
juce::Colour keyHue(const juce::String& key)
{
    static const std::map<juce::String, juce::uint32> hues{
        {"C", 0xffef4444},  {"C#", 0xfff97316}, {"D", 0xfff59e0b}, {"D#", 0xffeab308}, {"E", 0xff84cc16}, {"F", 0xff22c55e},
        {"F#", 0xff10b981}, {"G", 0xff14b8a6},  {"G#", 0xff06b6d4}, {"A", 0xff0ea5e9}, {"A#", 0xff3b82f6}, {"B", 0xff8b5cf6}};
    static const std::map<juce::String, juce::String> aliases{{"B#", "C"}, {"Db", "C#"}, {"Eb", "D#"}, {"E#", "F"},
                                                              {"Gb", "F#"}, {"Ab", "G#"}, {"Bb", "A#"}, {"Cb", "B"}};
    const auto t = key.trim();
    if (t.isEmpty() || !juce::String("ABCDEFGabcdefg").containsChar(t[0])) return colours::zinc500;
    juce::String note = juce::String::charToString(juce::CharacterFunctions::toUpperCase(t[0]));
    if (t.length() > 1 && (t[1] == '#' || t[1] == 'b')) note += juce::String::charToString(t[1]);
    if (const auto a = aliases.find(note); a != aliases.end()) note = a->second;
    const auto h = hues.find(note);
    return h != hues.end() ? juce::Colour(h->second) : colours::zinc500;
}

bool isMinor(const juce::String& key)
{
    // "A minor", "Am", "C#m", "Bbm" (keyBadge.ts); "major" / "maj" never.
    const auto t = key.trim();
    if (t.containsIgnoreCase("minor")) return true;
    if (t.containsIgnoreCase("major")) return false;
    const int at = t.length() > 1 && (t[1] == '#' || t[1] == 'b') ? 2 : 1;
    return t.length() > at && t[at] == 'm' && !t.substring(at).startsWithIgnoreCase("maj");
}

// A small rounded pill; returns its width.
int pill(juce::Graphics& g, int right, int centreY, const juce::String& text, juce::Colour fill, juce::Colour border, juce::Colour ink)
{
    const auto font = uiFont(9.5f);
    const int w = static_cast<int>(juce::GlyphArrangement::getStringWidth(font, text)) + 8;
    const juce::Rectangle<float> r(static_cast<float>(right - w), static_cast<float>(centreY - 7), static_cast<float>(w), 14.0f);
    g.setColour(fill);
    g.fillRoundedRectangle(r, 3.0f);
    g.setColour(border);
    g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
    g.setColour(ink);
    g.setFont(font);
    g.drawText(text, r, juce::Justification::centred, false);
    return w;
}

int keyPill(juce::Graphics& g, int right, int centreY, const juce::String& key)
{
    const auto hue = keyHue(key);
    const bool minor = isMinor(key);
    return pill(g, right, centreY, key, minor ? hue.darker(1.8f).withAlpha(0.7f) : hue.darker(0.6f).withAlpha(0.4f),
                minor ? hue.darker(1.2f).withAlpha(0.7f) : hue.withAlpha(0.6f), minor ? hue.brighter(0.3f) : hue.brighter(1.2f));
}

int usePill(juce::Graphics& g, int right, int centreY, int uses)
{
    return pill(g, right, centreY, juce::String(uses), uses > 0 ? colours::sky600.withAlpha(0.35f) : colours::zinc800,
                uses > 0 ? colours::sky500.withAlpha(0.6f) : colours::zinc700, uses > 0 ? colours::sky400.brighter(0.6f) : colours::zinc400);
}
} // namespace tiles

class LibraryItemsView final : public juce::Component, private juce::ChangeListener
{
  public:
    explicit LibraryItemsView(LibraryContext& context) : ctx(context)
    {
        viewport.setViewedComponent(&canvas, false);
        nameEditor.setFont(uiFont(12.0f, true));
        nameEditor.setColour(juce::TextEditor::outlineColourId, colours::sky500);
        nameEditor.onReturnKey = [this] { commitRename(true); };
        nameEditor.onEscapeKey = [this] { commitRename(false); };
        nameEditor.onFocusLost = [this] { commitRename(true); };
        canvas.addChildComponent(nameEditor);
        viewport.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport);

        filter.setTextToShowWhenEmpty("Filter library", colours::textFaint);
        filter.onTextChange = [this] { rebuild(); };
        addAndMakeVisible(filter);
        // Which kinds to show: everything, the tracks, their stems, or the samples.
        kindFilter.addItemList({"All", "Tracks", "Stems", "Samples"}, 1);
        kindFilter.setSelectedId(1, juce::dontSendNotification);
        kindFilter.onChange = [this] { rebuild(); };
        addAndMakeVisible(kindFilter);

        empty.setJustificationType(juce::Justification::centred);
        empty.setColour(juce::Label::textColourId, colours::textFaint);
        empty.setInterceptsMouseClicks(false, false);
        addChildComponent(empty);

        ctx.mirror.addChangeListener(this);
        ctx.prefs.addChangeListener(this);
        setWantsKeyboardFocus(true);
        rebuild();
    }

    ~LibraryItemsView() override
    {
        ctx.mirror.removeChangeListener(this);
        ctx.prefs.removeChangeListener(this);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop(26).reduced(2);
        kindFilter.setBounds(top.removeFromRight(110));
        top.removeFromRight(4);
        filter.setBounds(top);
        viewport.setBounds(area);
        empty.setBounds(area);
        layout();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            removeSelected();
            return true;
        }
        return false;
    }

    void startRename(const juce::String& id) { rename(id); }

  private:
    struct Hit
    {
        juce::Rectangle<int> bounds;
        juce::String itemId;
        enum class Kind { tile, row, toggle } kind = Kind::tile;
        juce::Rectangle<int> title; // double-click here renames
    };

    // The scrolling content: groups laid out like a wrapping flex row.
    class Canvas final : public juce::Component
    {
      public:
        explicit Canvas(LibraryItemsView& o) : owner(o) {}
        void paint(juce::Graphics& g) override { owner.paintCanvas(g); }
        void mouseDown(const juce::MouseEvent& e) override { owner.canvasDown(e); }
        void mouseDrag(const juce::MouseEvent& e) override { owner.canvasDrag(e); }
        void mouseDoubleClick(const juce::MouseEvent& e) override { owner.canvasDoubleClick(e); }

      private:
        LibraryItemsView& owner;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override { rebuild(); }

    juce::File mediaRoot() const { return media::rootFor(ctx.mirror.model().filePath); }

    juce::String mediaIdFor(const LibraryItemModel& item) const
    {
        juce::String id = item.mediaId;
        for (const auto* cursor = &item; id.isEmpty() && cursor != nullptr && cursor->sourceItemId.isNotEmpty();)
        {
            cursor = ctx.mirror.model().findLibraryItem(cursor->sourceItemId);
            if (cursor != nullptr) id = cursor->mediaId;
        }
        return id;
    }

    // The tags the import stored: title and artist beat the file name (displayTitle / displayArtist).
    juce::var metadataFor(const LibraryItemModel& item)
    {
        const auto id = mediaIdFor(item);
        if (id.isEmpty()) return {};
        if (const auto it = metadataCache.find(id); it != metadataCache.end()) return it->second;
        const auto meta = media::metadata(mediaRoot(), id);
        metadataCache[id] = meta;
        return meta;
    }

    juce::String displayTitle(const LibraryItemModel& item)
    {
        if (item.name.trim().isNotEmpty()) return item.name.trim();
        const auto title = metadataFor(item).getProperty("title", {}).toString().trim();
        return title.isNotEmpty() ? title : item.displayName();
    }

    juce::String displayArtist(const LibraryItemModel& item) { return metadataFor(item).getProperty("artist", {}).toString().trim(); }

    juce::Image coverFor(const LibraryItemModel& item)
    {
        if (static_cast<bool>(item.raw.getProperty("coverArtHidden", false))) return {};
        const auto overrideName = item.raw.getProperty("coverArtOverride", {}).toString();
        const auto file = overrideName.isNotEmpty() ? mediaRoot().getChildFile("covers").getChildFile(juce::File::createLegalFileName(overrideName))
                                                    : media::cover(mediaRoot(), mediaIdFor(item));
        if (!file.existsAsFile()) return {};
        const auto key = file.getFullPathName() + "|" + juce::String(file.getLastModificationTime().toMilliseconds());
        if (const auto it = coverCache.find(key); it != coverCache.end()) return it->second;
        auto image = juce::ImageFileFormat::loadFrom(file);
        if (image.isValid()) image = image.rescaled(tiles::kTileHeight * 2, tiles::kTileHeight * 2, juce::Graphics::highResamplingQuality);
        coverCache[key] = image;
        return image;
    }

    double effectiveClipBpm(const LibraryItemModel& item) const
    {
        // A warped saved clip plays at its pinned tempo, or the project's when it follows it.
        const auto& r = item.raw;
        double bpm = item.bpm;
        if (const auto* source = ctx.mirror.model().findLibraryItem(item.sourceItemId); bpm <= 0.0 && source != nullptr) bpm = source->bpm;
        if (!static_cast<bool>(r.getProperty("warpEnabled", false))) return bpm;
        const double ratio = static_cast<double>(r.getProperty("tempoRatio", 0.0));
        return ratio > 0.0 ? bpm * ratio : ctx.mirror.model().bpm;
    }

    void rebuild()
    {
        if (renamingId.isNotEmpty() && ctx.mirror.model().findLibraryItem(renamingId) == nullptr) commitRename(false);
        const auto query = filter.getText().trim().toLowerCase();
        const auto& m = ctx.mirror.model();
        metadataCache.clear();
        uses.clear();
        for (const auto& t : m.tracks)
            for (const auto& c : t.clips) ++uses[c.libraryItemId];
        const auto kind = kindFilter.getText();
        auto matches = [this, &query, &kind](const LibraryItemModel& item)
        {
            const bool ofKind = kind == "Stems" ? item.kind == "stem"
                              : kind == "Samples" ? item.kind == "sample"
                              : kind == "Tracks" ? (item.kind == "source" || item.kind == "clip")
                                                 : true;
            return ofKind
                   && (query.isEmpty() || displayTitle(item).toLowerCase().contains(query) || displayArtist(item).toLowerCase().contains(query)
                       || item.key.toLowerCase().contains(query) || item.fileName.toLowerCase().contains(query));
        };
        groups.clear();
        orphans.clear();
        ids.clear();
        for (const auto& item : m.library)
        {
            if (item.kind == "clip")
            {
                if (m.findLibraryItem(item.sourceItemId) == nullptr && matches(item)) orphans.add(item.id);
                continue;
            }
            Group g;
            g.sourceId = item.id;
            for (const auto& child : m.library)
                if (child.kind == "clip" && child.sourceItemId == item.id && matches(child)) g.children.add(child.id);
            if (!matches(item) && g.children.isEmpty()) continue;
            groups.push_back(g);
        }
        for (const auto& g : groups)
        {
            ids.add(g.sourceId);
            ids.addArray(g.children);
        }
        ids.addArray(orphans);
        for (auto it = selection.begin(); it != selection.end();)
            it = ids.contains(*it) ? std::next(it) : selection.erase(it);
        empty.setText(m.library.empty() ? juce::String("Drop audio files here, or click Import to add them.")
                      : filter.getText().trim().isEmpty() ? "No " + kindFilter.getText().toLowerCase() + " in the library yet."
                                                          : "No library items match \"" + filter.getText().trim() + "\".",
                      juce::dontSendNotification);
        empty.setVisible(m.library.empty() || ids.isEmpty());
        layout();
    }

    bool expanded(const LibraryItemModel& source) const
    {
        // Filtering shows the matching saved clips whatever the disclosure says.
        return filter.getText().trim().isNotEmpty() || !static_cast<bool>(source.raw.getProperty("collapsed", false));
    }

    void layout()
    {
        hits.clear();
        const int width = juce::jmax(tiles::kTileWidth + 2 * tiles::kGap, viewport.getMaximumVisibleWidth());
        int x = 8, y = 8, rowHeight = 0;
        auto place = [&](int h) -> juce::Point<int>
        {
            if (x + tiles::kTileWidth > width - 8 && x > 8)
            {
                x = 8;
                y += rowHeight + tiles::kGap;
                rowHeight = 0;
            }
            const juce::Point<int> at{x, y};
            x += tiles::kTileWidth + tiles::kGap;
            rowHeight = juce::jmax(rowHeight, h);
            return at;
        };
        const auto& m = ctx.mirror.model();
        groupBounds.clear();
        for (const auto& g : groups)
        {
            const auto* source = m.findLibraryItem(g.sourceId);
            if (source == nullptr) continue;
            const bool open = expanded(*source);
            const int h = tiles::kTileHeight + (g.children.isEmpty() ? 0 : tiles::kToggleHeight + (open ? tiles::kRowHeight * g.children.size() : 0));
            const auto at = place(h);
            groupBounds.push_back({at.x, at.y, tiles::kTileWidth, h});
            const juce::Rectangle<int> tile(at.x, at.y, tiles::kTileWidth, tiles::kTileHeight);
            const int textX = at.x + (showImages() ? tiles::kTileHeight + 8 : 8);
            hits.push_back({tile, g.sourceId, Hit::Kind::tile, {textX, at.y + 4, at.x + tiles::kTileWidth - textX - 6, 16}});
            int cy = at.y + tiles::kTileHeight;
            if (!g.children.isEmpty())
            {
                hits.push_back({{at.x, cy, tiles::kTileWidth, tiles::kToggleHeight}, g.sourceId, Hit::Kind::toggle, {}});
                cy += tiles::kToggleHeight;
                if (open)
                    for (const auto& child : g.children)
                    {
                        hits.push_back({{at.x, cy, tiles::kTileWidth, tiles::kRowHeight}, child, Hit::Kind::row, {at.x + 14, cy + 4, 150, 14}});
                        cy += tiles::kRowHeight;
                    }
            }
        }
        if (!orphans.isEmpty())
        {
            const int h = 20 + tiles::kRowHeight * orphans.size();
            const auto at = place(h);
            groupBounds.push_back({at.x, at.y, tiles::kTileWidth, h});
            int cy = at.y + 20;
            for (const auto& id : orphans)
            {
                hits.push_back({{at.x, cy, tiles::kTileWidth, tiles::kRowHeight}, id, Hit::Kind::row, {at.x + 14, cy + 4, 150, 14}});
                cy += tiles::kRowHeight;
            }
            orphanHeader = {at.x, at.y, tiles::kTileWidth, 20};
        }
        else
            orphanHeader = {};
        canvas.setSize(width, y + rowHeight + 8);
        canvas.repaint();
    }

    bool showImages() const { return static_cast<bool>(ctx.prefs.get("ui", "showLibraryTileImages", true)); }

    void paintCanvas(juce::Graphics& g)
    {
        g.fillAll(colours::zinc900);
        const auto& m = ctx.mirror.model();
        for (const auto& b : groupBounds)
        {
            g.setColour(colours::zinc950.withAlpha(0.5f));
            g.fillRoundedRectangle(b.toFloat(), 6.0f);
        }
        if (!orphanHeader.isEmpty())
        {
            g.setColour(colours::zinc500);
            g.setFont(uiFont(10.0f));
            g.drawText("SAVED CLIPS (SOURCE MISSING)", orphanHeader.reduced(8, 0), juce::Justification::centredLeft, true);
        }
        for (const auto& hit : hits)
        {
            const auto* item = m.findLibraryItem(hit.itemId);
            if (item == nullptr) continue;
            const bool selected = selection.count(hit.itemId) != 0;
            if (hit.kind == Hit::Kind::tile) paintTile(g, hit, *item, selected);
            else if (hit.kind == Hit::Kind::toggle) paintToggle(g, hit, *item);
            else paintRow(g, hit, *item, selected);
        }
        for (const auto& b : groupBounds)
        {
            g.setColour(colours::zinc800);
            g.drawRoundedRectangle(b.toFloat().reduced(0.5f), 6.0f, 1.0f);
        }
    }

    void paintTile(juce::Graphics& g, const Hit& hit, const LibraryItemModel& item, bool selected)
    {
        auto r = hit.bounds;
        g.setColour(selected ? colours::sky600.withAlpha(0.3f) : colours::zinc950.withAlpha(0.6f));
        g.fillRect(r.reduced(1));
        if (showImages())
        {
            const auto art = r.removeFromLeft(tiles::kTileHeight);
            const bool stem = item.kind == "stem";
            const bool scratch = item.kind == "sample" && item.raw.getProperty("scratchPatternId", {}).toString().isNotEmpty();
            const bool sample = item.kind == "sample";
            if (const auto cover = coverFor(item); cover.isValid())
                g.drawImage(cover, art.toFloat(), juce::RectanglePlacement::fillDestination);
            else
            {
                const auto ink = stem ? juce::Colour(0xff2dd4bf) : scratch ? juce::Colour(0xffe879f9) : sample ? juce::Colour(0xff818cf8) : colours::sky400;
                g.setColour(colours::zinc900.overlaidWith(ink.withAlpha(0.08f)));
                g.fillRect(art.reduced(1));
                auto icon = juce::Drawable::parseSVGPath(stem ? tiles::kStemIcon : scratch ? tiles::kScratchIcon : sample ? tiles::kSampleIcon : tiles::kSourceIcon);
                icon.applyTransform(icon.getTransformToScaleToFit(art.toFloat().withSizeKeepingCentre(24.0f, 24.0f), true));
                g.setColour(ink.withAlpha(0.85f));
                g.fillPath(icon);
            }
            // LibraryTypeBadge: what kind of audio this is.
            const auto badge = stem ? "STEM" : scratch ? "SCRATCH" : sample ? "SAMPLE" : "SOURCE";
            const juce::Rectangle<float> b(static_cast<float>(art.getX() + 3), static_cast<float>(art.getBottom() - 15), 50.0f, 12.0f);
            g.setColour(colours::zinc950.withAlpha(0.8f));
            g.fillRoundedRectangle(b.withWidth(juce::GlyphArrangement::getStringWidth(uiFont(8.0f, true), badge) + 6.0f), 2.0f);
            g.setColour(colours::zinc300);
            g.setFont(uiFont(8.0f, true));
            g.drawText(badge, b.translated(3.0f, 0.0f), juce::Justification::centredLeft, false);
            g.setColour(colours::zinc800);
            g.drawVerticalLine(art.getRight(), static_cast<float>(art.getY()), static_cast<float>(art.getBottom()));
        }
        auto text = r.reduced(8, 5);
        g.setColour(item.unresolved ? colours::red500 : colours::zinc100);
        g.setFont(uiFont(12.0f, true));
        g.drawText(displayTitle(item), text.removeFromTop(16), juce::Justification::centredLeft, true);
        if (const auto artist = displayArtist(item); artist.isNotEmpty())
        {
            g.setColour(colours::zinc400);
            g.setFont(uiFont(11.0f));
            g.drawText(artist, text.removeFromTop(14), juce::Justification::centredLeft, true);
        }
        g.setColour(colours::zinc500);
        g.setFont(uiFont(10.0f));
        g.drawText(formatDuration(item.durationMs), text.removeFromTop(13), juce::Justification::centredLeft, true);
        // Pills, right to left: use count, BPM, key (or Simple for a one-shot).
        const int cy = r.getBottom() - 12;
        int right = r.getRight() - 6;
        right -= tiles::usePill(g, right, cy, uses[item.id]) + 4;
        const auto type = item.raw.getProperty("audioType", {}).toString();
        if (type == "simple")
            tiles::pill(g, right, cy, "Simple", juce::Colour(0xff312e81).withAlpha(0.6f), juce::Colour(0xff6366f1).withAlpha(0.6f), juce::Colour(0xffc7d2fe));
        else
        {
            if (item.bpm > 0.0)
            {
                const bool variable = static_cast<bool>(item.raw.getProperty("variableTempo", false));
                right -= tiles::pill(g, right, cy, (variable ? "~" : "") + juce::String(item.bpm, 2) + " BPM", colours::zinc800,
                                     variable ? colours::amber500.withAlpha(0.6f) : colours::zinc700, colours::zinc300)
                         + 4;
            }
            if (item.key.isNotEmpty()) tiles::keyPill(g, right, cy, item.key);
        }
    }

    void paintToggle(juce::Graphics& g, const Hit& hit, const LibraryItemModel& source)
    {
        const auto r = hit.bounds;
        g.setColour(colours::zinc800.withAlpha(0.6f));
        g.drawHorizontalLine(r.getY(), static_cast<float>(r.getX()), static_cast<float>(r.getRight()));
        int count = 0;
        for (const auto& grp : groups)
            if (grp.sourceId == source.id) count = grp.children.size();
        const bool open = expanded(source);
        juce::Path arrow;
        const float ax = static_cast<float>(r.getX() + 12), ay = static_cast<float>(r.getCentreY());
        if (open) arrow.addTriangle(ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
        else arrow.addTriangle(ax - 2.0f, ay - 4.0f, ax - 2.0f, ay + 4.0f, ax + 3.0f, ay);
        g.setColour(colours::zinc400);
        g.fillPath(arrow);
        g.setFont(uiFont(10.5f));
        g.drawText(juce::String(count) + (count == 1 ? " saved clip" : " saved clips"), r.withTrimmedLeft(22), juce::Justification::centredLeft, false);
    }

    void paintRow(juce::Graphics& g, const Hit& hit, const LibraryItemModel& item, bool selected)
    {
        auto r = hit.bounds;
        if (selected)
        {
            g.setColour(colours::sky600.withAlpha(0.3f));
            g.fillRect(r);
        }
        g.setColour(colours::zinc800.withAlpha(0.6f));
        g.drawHorizontalLine(r.getY(), static_cast<float>(r.getX()), static_cast<float>(r.getRight()));
        const bool orphan = ctx.mirror.model().findLibraryItem(item.sourceItemId) == nullptr;
        g.setColour(orphan ? colours::amber500.withAlpha(0.6f) : colours::sky500.withAlpha(0.6f));
        g.fillRoundedRectangle(static_cast<float>(r.getX() + 6), static_cast<float>(r.getCentreY() - 12), 4.0f, 24.0f, 1.5f);
        const int cy = r.getCentreY();
        int right = r.getRight() - 6;
        right -= tiles::usePill(g, right, cy, uses[item.id]) + 4;
        if (const double bpm = effectiveClipBpm(item); bpm > 0.0)
            right -= tiles::pill(g, right, cy, juce::String(bpm, 2) + " BPM", colours::zinc800, colours::zinc700, colours::zinc300) + 4;
        if (item.key.isNotEmpty()) right -= tiles::keyPill(g, right, cy, item.key) + 4;
        const int textWidth = juce::jmax(20, right - (r.getX() + 16) - 2);
        g.setColour(colours::zinc100);
        g.setFont(uiFont(11.0f, true));
        g.drawText(displayTitle(item), r.getX() + 16, r.getY() + 5, textWidth, 14, juce::Justification::centredLeft, true);
        g.setColour(colours::zinc500);
        g.setFont(uiFont(10.0f));
        const double duration = static_cast<double>(item.raw.getProperty("sourceDurationMs", item.durationMs));
        g.drawText(formatDuration(duration), r.getX() + 16, r.getY() + 21, textWidth, 13, juce::Justification::centredLeft, true);
    }

    const Hit* hitAt(juce::Point<int> p) const
    {
        for (const auto& h : hits)
            if (h.bounds.contains(p)) return &h;
        return nullptr;
    }

    void canvasDown(const juce::MouseEvent& e)
    {
        grabKeyboardFocus();
        dragStarted = false;
        const auto* hit = hitAt(e.getPosition());
        if (hit == nullptr)
        {
            selection.clear();
            canvas.repaint();
            return;
        }
        if (hit->kind == Hit::Kind::toggle)
        {
            if (filter.getText().trim().isEmpty()) setCollapsed(hit->itemId, expanded(*ctx.mirror.model().findLibraryItem(hit->itemId)));
            return;
        }
        if (e.mods.isCommandDown())
        {
            if (!selection.erase(hit->itemId)) selection.insert(hit->itemId);
        }
        else if (selection.count(hit->itemId) == 0 || !e.mods.isPopupMenu())
            selection = {hit->itemId};
        canvas.repaint();
        if (e.mods.isPopupMenu()) showMenu(hit->itemId);
    }

    void canvasDrag(const juce::MouseEvent& e)
    {
        if (dragStarted || e.getDistanceFromDragStart() < 5 || e.mods.isPopupMenu()) return;
        const auto* hit = hitAt(e.getMouseDownPosition());
        if (hit == nullptr || hit->kind == Hit::Kind::toggle) return;
        dragStarted = true;
        // Onto a track to place it (the timeline takes one item per drop).
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
            container->startDragging("libraryItem:" + hit->itemId, &canvas,
                                     juce::ScaledImage(canvas.createComponentSnapshot(hit->bounds.withHeight(juce::jmin(hit->bounds.getHeight(), 40)))));
    }

    void canvasDoubleClick(const juce::MouseEvent& e)
    {
        const auto* hit = hitAt(e.getPosition());
        if (hit == nullptr || hit->kind == Hit::Kind::toggle) return;
        // The title renames; anywhere else opens the item in the Clip Editor.
        if (hit->title.contains(e.getPosition())) rename(hit->itemId);
        else if (ctx.openEditor) ctx.openEditor(hit->itemId);
    }

    void setCollapsed(const juce::String& id, bool collapsed)
    {
        const auto* item = ctx.mirror.model().findLibraryItem(id);
        if (item == nullptr) return;
        // The renderer re-sends the item with its disclosure (libraryStore.setItemCollapsed).
        auto* add = new juce::DynamicObject();
        for (const auto& key : {"filePath", "kind", "name", "fileName", "durationMs", "sampleRate", "channelCount", "key", "sourceItemId",
                                "sourceClipId", "sourceInMs", "sourceDurationMs"})
            if (item->raw.hasProperty(key)) add->setProperty(key, item->raw.getProperty(key, {}));
        add->setProperty("itemId", id);
        add->setProperty("collapsed", collapsed ? juce::var(true) : juce::var());
        ctx.engine.send("LIBRARY_ADD", juce::var(add));
    }

    void addToTrack(const juce::String& itemId)
    {
        const auto* item = ctx.mirror.model().findLibraryItem(itemId);
        if (item == nullptr) return;
        auto trackId = ctx.view.selectedTrackId;
        if (ctx.mirror.model().findTrack(trackId) == nullptr)
        {
            trackId = ctx.actions.addTrack();
            ctx.mirror.refreshNow();
            item = ctx.mirror.model().findLibraryItem(itemId);
            if (item == nullptr) return;
        }
        const auto clipId = ctx.actions.addClip(trackId, *item, ctx.actions.snap(ctx.live.positionMs));
        ctx.view.selectedTrackId = trackId;
        ctx.view.selectedClipIds = {clipId};
        ctx.view.changed();
    }

    // ~/Stemcel/Sample Library: stems and samples kept for any project, by type, and shown
    // in the Files tab (added as one of its folders the first time).
    static juce::File sampleLibraryFolder()
    {
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(appFolderName()).getChildFile("Sample Library");
    }

    // The selected files into this computer's rekordbox collection and its "Stemcel"
    // playlist (Rekordbox::addToComputerLibrary: refused while rekordbox runs, and the
    // database is backed up before the session's first change).
    void addToRekordbox()
    {
        const auto lasting = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                 .getChildFile(appFolderName())
                                 .getChildFile("rekordbox");
        juce::Array<juce::File> files;
        for (const auto& id : selection)
        {
            const auto* item = ctx.mirror.model().findLibraryItem(id);
            if (item == nullptr || item->kind == "clip") continue;
            // The file as imported, not a WAV made to play it.
            juce::File file(item->filePath);
            if (!file.existsAsFile()) file = juce::File(item->audioPath());
            if (!file.existsAsFile()) continue;
            // Stems and samples Stemcel made live in its temporary media store, which rekordbox
            // would lose track of: they're copied somewhere lasting first.
            if (file.isAChildOf(systemTempDirectory()))
            {
                lasting.createDirectory();
                const auto base = juce::File::createLegalFileName(displayTitle(*item));
                auto target = lasting.getChildFile(base + file.getFileExtension());
                for (int n = 2; target.exists(); ++n) target = lasting.getChildFile(base + " " + juce::String(n) + file.getFileExtension());
                if (!file.copyFileTo(target))
                {
                    if (ctx.notify) ctx.notify("Couldn't copy " + displayTitle(*item) + " to " + lasting.getFullPathName(), true);
                    continue;
                }
                file = target;
            }
            files.add(file);
        }
        if (files.isEmpty())
        {
            if (ctx.notify) ctx.notify("None of the selected items has an audio file to add to rekordbox", true);
            return;
        }
        if (ctx.notify) ctx.notify("Adding " + juce::String(files.size()) + " file(s) to rekordbox...", false);
        auto notify = ctx.notify;
        auto& rekordbox = ctx.rekordbox;
        juce::Component::SafePointer<juce::Component> self(this);
        juce::Thread::launch(
            [files, notify, self, &rekordbox]
            {
                const auto result = rekordbox.addToComputerLibrary(files, "Stemcel");
                juce::MessageManager::callAsync(
                    [result, notify, self]
                    {
                        if (self == nullptr || !notify) return;
                        if (!static_cast<bool>(result.getProperty("ok", false)))
                        {
                            notify("rekordbox: " + result.getProperty("error", "the change was refused").toString(), true);
                            return;
                        }
                        const int added = result.getProperty("added", 0);
                        const int existing = result.getProperty("existing", 0);
                        const auto* refused = result.getProperty("refused", {}).getArray();
                        juce::String text = "Added " + juce::String(added) + " file(s) to rekordbox's \"Stemcel\" playlist";
                        if (existing > 0) text << " (" << existing << " already in the collection)";
                        if (refused != nullptr && !refused->isEmpty())
                            text << "; skipped " << refused->size() << ": " << (*refused)[0].toString();
                        notify(text, refused != nullptr && !refused->isEmpty());
                    });
            });
    }

    void saveToSampleLibrary(const juce::String& itemId)
    {
        const auto* item = ctx.mirror.model().findLibraryItem(itemId);
        if (item == nullptr) return;
        const juce::File source(item->audioPath());
        if (!source.existsAsFile())
        {
            if (ctx.notify) ctx.notify("The audio file for " + displayTitle(*item) + " is missing", true);
            return;
        }
        // Stems are named "<Type> - <source>"; their type is the folder.
        juce::String type = "Samples";
        if (item->kind == "stem")
            for (const auto* t : {"Vocals", "Drums", "Bass", "Guitar", "Piano", "Other", "Instrumental", "Kick", "Snare", "Toms", "Hi-hat"})
                if (item->name.startsWithIgnoreCase(t)) type = t;
        const auto folder = sampleLibraryFolder().getChildFile(type);
        folder.createDirectory();
        const auto base = juce::File::createLegalFileName(displayTitle(*item));
        auto target = folder.getChildFile(base + source.getFileExtension());
        for (int n = 2; target.exists(); ++n) target = folder.getChildFile(base + " " + juce::String(n) + source.getFileExtension());
        if (!source.copyFileTo(target))
        {
            if (ctx.notify) ctx.notify("Could not copy " + displayTitle(*item) + " into the sample library", true);
            return;
        }
        juce::Array<juce::var> folders;
        bool listed = false;
        if (const auto* list = ctx.prefs.get("ui", "fileBrowserFolders", {}).getArray())
            for (const auto& f : *list)
            {
                folders.add(f);
                listed = listed || juce::File(f.toString()) == sampleLibraryFolder();
            }
        if (!listed)
        {
            folders.add(sampleLibraryFolder().getFullPathName());
            ctx.prefs.set("ui", "fileBrowserFolders", folders);
            ctx.prefs.save();
        }
        if (ctx.notify) ctx.notify("Saved " + displayTitle(*item) + " to the sample library (" + type + ")", false);
    }

    void showMenu(const juce::String& itemId)
    {
        const auto* item = ctx.mirror.model().findLibraryItem(itemId);
        if (item == nullptr) return;
        const auto id = item->id;
        const bool isClip = item->kind == "clip";
        const bool isFile = !isClip;
        const bool inUse = uses[id] > 0;
        // useLibraryItemActions.ts contextMenuItems, in its order and with its labels.
        juce::PopupMenu menu;
        menu.addItem(isClip ? "Open in Editor" : "Preview", ctx.openEditor != nullptr, false,
                     [this, id] { if (ctx.openEditor) ctx.openEditor(id); });
        menu.addItem("Audition", [this, id]
                     {
                         if (const auto* i = ctx.mirror.model().findLibraryItem(id))
                             ctx.audition.toggle(juce::File(i->filePath));
                     });
        menu.addItem("Open in Scratch Editor", ctx.openScratch != nullptr, false, [this, id] { if (ctx.openScratch) ctx.openScratch(id); });
        menu.addItem("Show Information", [this, id] { LibraryInfoDialog::show(ctx.engine, ctx.mirror, id); });
        // A tempo to correct: not a one-shot, and something already detected.
        if (item->raw.getProperty("audioType", {}).toString() != "simple" && item->bpm > 0.0)
            menu.addItem("Edit BPM...", [this, id] { editBpm(id); });
        menu.addSeparator();
        menu.addItem("Rename", [this, id] { rename(id); });
        menu.addItem("Add to selected track at playhead", [this, id] { addToTrack(id); });
        // Two or more tracks selected: mix them (in the library's order, or harmonically).
        if (selection.count(id) != 0 && selection.size() >= 2 && ctx.autoMix != nullptr)
        {
            std::vector<juce::String> ids;
            for (const auto& i : ctx.mirror.model().library)
                if (selection.count(i.id) != 0) ids.push_back(i.id);
            juce::PopupMenu harmonic, matched, asListed;
            for (const auto& style : automix::styles())
            {
                harmonic.addItem(style, [this, ids, style] { ctx.autoMix(ids, true, style, false); });
                matched.addItem(style, [this, ids, style] { ctx.autoMix(ids, true, style, true); });
                asListed.addItem(style, [this, ids, style] { ctx.autoMix(ids, false, style, false); });
            }
            juce::PopupMenu autoMix;
            autoMix.addSubMenu("In Harmonic Order", harmonic);
            autoMix.addSubMenu("In Harmonic Order, Matching Keys", matched);
            autoMix.addSubMenu("In This Order", asListed);
            menu.addSubMenu("Auto Mix " + juce::String(static_cast<int>(ids.size())) + " Tracks", autoMix);
            if (ids.size() == 2 && ctx.mashup != nullptr)
            {
                const auto* a = ctx.mirror.model().findLibraryItem(ids[0]);
                const auto* b = ctx.mirror.model().findLibraryItem(ids[1]);
                if (a != nullptr && b != nullptr)
                {
                    juce::PopupMenu mashup;
                    mashup.addItem("Vocals of " + displayTitle(*a) + " over " + displayTitle(*b), [this, ids] { ctx.mashup(ids[0], ids[1]); });
                    mashup.addItem("Vocals of " + displayTitle(*b) + " over " + displayTitle(*a), [this, ids] { ctx.mashup(ids[1], ids[0]); });
                    menu.addSubMenu("Mashup", mashup);
                }
            }
        }
        if (item->kind == "stem" || item->kind == "sample")
            menu.addItem("Save to Sample Library", [this, id] { saveToSampleLibrary(id); });
        if (isFile) menu.addItem("Reanalyse File", [this, id] { reanalyse(id); });
        if (item->kind == "source" && ctx.analyseSections != nullptr)
            menu.addItem("Analyse Sections", [this, id] { ctx.analyseSections(id); });
        if (isFile && ctx.tempoReport != nullptr) menu.addItem("BPM Variability Check...", [this, id] { ctx.tempoReport(id); });
        if (isFile)
            menu.addItem("Add to rekordbox", ctx.rekordbox.isAvailable(), false, [this] { addToRekordbox(); });
        if (item->kind == "source" || item->kind == "sample")
            menu.addItem("Separate Stems", ctx.separateStems != nullptr, false, [this, id] { if (ctx.separateStems) ctx.separateStems(id); });
        else if (isClip)
        {
            menu.addItem("Save as Sample (Music)", [this, id] { saveAsSample(id, "music"); });
            menu.addItem("Save as Sample (Simple)", [this, id] { saveAsSample(id, "simple"); });
        }
        if (isFile)
        {
            menu.addSeparator();
            const auto type = item->raw.getProperty("audioType", {}).toString();
            const bool lowConfidence = static_cast<bool>(item->raw.getProperty("lowConfidence", false));
            menu.addItem(type.isEmpty() ? juce::String("Auto-classify (currently ") + (lowConfidence ? "music, tempo unverified" : "music") + ")"
                                        : juce::String("Auto-classify"),
                         type.isNotEmpty(), false, [this, id] { setAudioType(id, "auto"); });
            menu.addItem("Treat as Music", type != "music", false, [this, id] { setAudioType(id, "music"); });
            menu.addItem("Treat as Simple", type != "simple", false, [this, id] { setAudioType(id, "simple"); });
            menu.addSeparator();
            menu.addItem("Update Image...", [this, id] { LibraryInfoDialog::show(ctx.engine, ctx.mirror, id); });
            if (static_cast<bool>(item->raw.getProperty("coverArtHidden", false)))
                menu.addItem("Restore Image", [this, id] { setCoverHidden(id, false); });
            else if (item->mediaId.isNotEmpty() || item->raw.getProperty("coverArtOverride", {}).toString().isNotEmpty())
                menu.addItem("Remove Image", [this, id] { setCoverHidden(id, true); });
        }
        if (item->unresolved)
            menu.addItem("Relink missing audio...", [this] { RelinkDialog::showIfNeeded(ctx.engine, ctx.mirror); });
        menu.addItem("Show file", [this, id]
                     {
                         if (const auto* i = ctx.mirror.model().findLibraryItem(id))
                             juce::File(i->filePath).revealToUser();
                     });
        menu.addSeparator();
        // Saved clips can always go (their timeline clips are unlinked and keep their audio);
        // a source, sample or stem in use would orphan the sound those clips play.
        menu.addItem("Remove", !inUse || isClip, false, [this] { removeSelected(); });
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }

    void setAudioType(const juce::String& id, const juce::String& type)
    {
        ctx.engine.send("LIBRARY_ITEM_SET_AUDIO_TYPE", object({{"itemId", id}, {"audioType", type}}));
    }

    void setCoverHidden(const juce::String& id, bool hidden)
    {
        ctx.engine.send("LIBRARY_ITEM_SET_COVER_HIDDEN", object({{"itemId", id}, {"hidden", hidden}}));
    }

    void saveAsSample(const juce::String& id, const juce::String& audioType)
    {
        const auto* item = ctx.mirror.model().findLibraryItem(id);
        if (item == nullptr || item->kind != "clip") return;
        ctx.engine.send("LIBRARY_ITEM_SAVE_AS_SAMPLE", object({{"libraryItemId", id},
                                                              {"itemId", "sample-" + juce::Uuid().toDashedString()},
                                                              {"sampleName", item->displayName()},
                                                              {"audioType", audioType}}));
        if (ctx.notify) ctx.notify("Saved \"" + item->displayName() + "\" as a sample", false);
    }

    // Rename in place on the tile, as the renderer's library does (an inline field).
    void rename(const juce::String& id)
    {
        const auto* item = ctx.mirror.model().findLibraryItem(id);
        if (item == nullptr) return;
        const Hit* hit = nullptr;
        for (const auto& h : hits)
            if (h.itemId == id && h.kind != Hit::Kind::toggle) hit = &h;
        if (hit == nullptr) return;
        viewport.setViewPosition(viewport.getViewPositionX(),
                                 juce::jlimit(0, juce::jmax(0, canvas.getHeight() - viewport.getHeight()), hit->bounds.getY() - 8));
        renamingId = id;
        nameEditor.setBounds(hit->title.expanded(2, 2));
        nameEditor.setText(displayTitle(*item), false);
        nameEditor.setVisible(true);
        nameEditor.grabKeyboardFocus();
        nameEditor.selectAll();
    }

    void commitRename(bool keep)
    {
        if (renamingId.isEmpty()) return;
        const auto id = std::exchange(renamingId, {});
        nameEditor.setVisible(false);
        const auto name = nameEditor.getText().trim();
        const auto* i = ctx.mirror.model().findLibraryItem(id);
        if (!keep || i == nullptr || name.isEmpty() || name == displayTitle(*i)) return;
        // LIBRARY_ADD upserts the whole item, so everything it is derived from goes with it
        // (libraryStore.renameItem); omitting playbackFilePath keeps the decoded-cache path.
        auto* add = new juce::DynamicObject();
        for (const auto& key : {"filePath", "fileName", "kind", "durationMs", "sampleRate", "channelCount", "key", "mediaId",
                                "sourceItemId", "sourceClipId", "sourceInMs", "sourceDurationMs", "collapsed"})
            if (i->raw.hasProperty(key)) add->setProperty(key, i->raw.getProperty(key, {}));
        add->setProperty("itemId", id);
        add->setProperty("name", name);
        ctx.engine.send("LIBRARY_ADD", juce::var(add));
    }

    void editBpm(const juce::String& id)
    {
        const auto* item = ctx.mirror.model().findLibraryItem(id);
        if (item == nullptr) return;
        auto* window = new juce::AlertWindow("Edit BPM", "The tempo of \"" + item->displayName() + "\" (20-300).",
                                             juce::MessageBoxIconType::NoIcon);
        window->addTextEditor("bpm", item->bpm > 0.0 ? juce::String(item->bpm, 2) : juce::String());
        window->addTextEditor("anchor", juce::String(item->beatAnchorSec, 3), "First beat (seconds)");
        window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        window->enterModalState(true, juce::ModalCallbackFunction::create(
                                          [this, id, window](int result)
                                          {
                                              if (result != 1) return;
                                              const double bpm = window->getTextEditorContents("bpm").getDoubleValue();
                                              const double anchor = window->getTextEditorContents("anchor").getDoubleValue();
                                              if (bpm < 20.0 || bpm > 300.0)
                                              {
                                                  ctx.notify("BPM must be between 20 and 300.", true);
                                                  return;
                                              }
                                              ctx.engine.send("LIBRARY_ITEM_SET_MANUAL_TEMPO",
                                                              object({{"itemId", id}, {"bpm", bpm}, {"beatAnchorSec", anchor}}));
                                          }),
                               true);
    }

    void reanalyse(const juce::String& id)
    {
        const auto* i = ctx.mirror.model().findLibraryItem(id);
        if (i == nullptr) return;
        ctx.engine.send("LIBRARY_REANALYSE", object({{"itemId", id},
                                                     {"filePath", i->filePath},
                                                     {"fileName", i->fileName},
                                                     {"durationMs", i->durationMs},
                                                     {"sampleRate", i->sampleRate},
                                                     {"channelCount", i->channelCount},
                                                     {"playbackFilePath", i->playbackFilePath},
                                                     {"key", i->key}}));
        ctx.notify("Re-analysing \"" + i->displayName() + "\"", false);
    }

    void removeSelected()
    {
        // libraryStore.ts removeItem: a source, sample or stem in use stays (removing it would
        // orphan its clips' audio); a saved clip's timeline clips are rebound to its source.
        const auto& model = ctx.mirror.model();
        juce::StringArray selected, refused;
        for (const auto& id : selection)
        {
            const auto* item = model.findLibraryItem(id);
            if (item == nullptr) continue;
            if (item->kind != "clip" && uses[id] > 0) refused.add(item->displayName());
            else selected.add(id);
        }
        if (!refused.isEmpty() && ctx.notify)
            ctx.notify(refused.joinIntoString(", ") + (refused.size() == 1 ? " is" : " are")
                           + " used on the timeline; remove those clips first.",
                       true);
        if (selected.isEmpty()) return;

        const bool cleanup = static_cast<bool>(ctx.prefs.get("ui", "cleanupProjectFiles", false));
        std::vector<std::pair<juce::String, juce::String>> rebinds; // timeline clip -> source item
        juce::StringArray removing; // the selection plus saved clips cut from it
        juce::StringArray artifacts, mediaIds, irreversible;
        for (const auto& id : selected)
        {
            const auto* item = model.findLibraryItem(id);
            if (item == nullptr) continue;
            removing.addIfNotAlreadyThere(id);
            if (item->kind == "clip")
            {
                for (const auto& t : model.tracks)
                    for (const auto& c : t.clips)
                        if (c.libraryItemId == id && item->sourceItemId.isNotEmpty()) rebinds.emplace_back(c.id, item->sourceItemId);
            }
            else
            {
                // Saved clips replay their source's file, so they cannot outlive it.
                for (const auto& child : model.library)
                    if (child.kind == "clip" && child.sourceItemId == id) removing.addIfNotAlreadyThere(child.id);
            }
        }
        for (const auto& id : removing)
            if (const auto* item = model.findLibraryItem(id))
            {
                // Only generated files (a stem, a sample) are ever deleted; imported audio never.
                if (cleanup && (item->kind == "stem" || item->kind == "sample"))
                {
                    artifacts.add(item->audioPath());
                    irreversible.add(id);
                }
                if (cleanup && item->mediaId.isNotEmpty()) mediaIds.addIfNotAlreadyThere(item->mediaId);
            }

        ctx.engine.inUndoGroup("Remove from library",
                               [&]
                               {
                                   for (const auto& [clipId, sourceId] : rebinds)
                                       ctx.engine.send("CLIP_REBIND", object({{"clipId", clipId}, {"libraryItemId", sourceId}}));
                                   for (const auto& id : removing)
                                       if (!irreversible.contains(id)) ctx.engine.send("LIBRARY_REMOVE", object({{"itemId", id}}));
                               });
        // A file-deleting removal cannot be undone, so it is neither an undo step nor dirty.
        for (const auto& id : irreversible) ctx.engine.send("LIBRARY_REMOVE", object({{"itemId", id}, {"cleanup", true}}));
        if (!artifacts.isEmpty())
        {
            juce::Array<juce::var> paths;
            for (const auto& a : artifacts) paths.add(a);
            ctx.engine.send("LIBRARY_DELETE_ARTIFACTS", object({{"paths", paths}}));
        }
        // Cover art and tag sidecars go once nothing left in the library uses them.
        if (!mediaIds.isEmpty())
        {
            ctx.mirror.refreshNow();
            const auto root = media::rootFor(ctx.mirror.model().filePath);
            for (const auto& mediaId : mediaIds)
            {
                const bool stillUsed = std::any_of(ctx.mirror.model().library.begin(), ctx.mirror.model().library.end(),
                                                   [&mediaId](const LibraryItemModel& i) { return i.mediaId == mediaId; });
                if (stillUsed || !media::isSafeId(mediaId)) continue;
                root.getChildFile("metadata").getChildFile(mediaId + ".json").deleteFile();
                for (const auto& f : root.getChildFile("covers").findChildFiles(juce::File::findFiles, false, mediaId + ".*")) f.deleteFile();
            }
        }
    }

    struct Group
    {
        juce::String sourceId;
        juce::StringArray children; // its saved clips
    };

    LibraryContext& ctx;
    Canvas canvas{*this};
    juce::Viewport viewport;
    juce::TextEditor filter;
    juce::Label empty;
    juce::StringArray ids; // every visible item, in order
    std::vector<Group> groups;
    juce::StringArray orphans; // saved clips whose source is gone
    std::vector<Hit> hits;
    std::vector<juce::Rectangle<int>> groupBounds;
    juce::Rectangle<int> orphanHeader;
    std::set<juce::String> selection;
    std::map<juce::String, int> uses;
    std::map<juce::String, juce::var> metadataCache;
    std::map<juce::String, juce::Image> coverCache;
    bool dragStarted = false;
    juce::TextEditor nameEditor;
    juce::String renamingId;
    juce::ComboBox kindFilter;
};

// ── Panel ──────────────────────────────────────────────────────────────────
LibraryPanel::LibraryPanel(LibraryContext context) : ctx(std::move(context))
{
    library = std::make_unique<LibraryItemsView>(ctx);
    files = std::make_unique<FilesBrowser>(ctx);
    rekordbox = std::make_unique<RekordboxBrowser>(ctx);
    transitions = std::make_unique<TransitionsPanel>(ctx);
    trackFx = std::make_unique<TrackFxView>(ctx);
    projectFx = std::make_unique<ProjectFxView>(ctx);
    plugins = std::make_unique<PluginsView>(ctx);
    views = {library.get(), files.get(), rekordbox.get(), transitions.get(), trackFx.get(), projectFx.get(), plugins.get()};
    for (auto* v : views) addChildComponent(v);

    const juce::StringArray names{"Library", "Files", "rekordbox", "Transitions", "Track FX", "Project FX", "Plugins"};
    for (int index = 0; index < names.size(); ++index)
    {
        auto* tab = tabs.add(new juce::TextButton(names[index]));
        tab->setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        tab->onClick = [this, index] { showTab(index); };
        addAndMakeVisible(tab);
    }
    importButton.setTooltip("Import audio files into the library (Ctrl/Cmd+I)");
    importButton.onClick = [this] { importWithDialog(); };
    addAndMakeVisible(importButton);

    status.setColour(juce::Label::textColourId, colours::textFaint);
    status.setFont(uiFont(12.0f));
    addAndMakeVisible(status);
    ctx.importer.onProgress = [this]
    {
        const int n = ctx.importer.pendingCount();
        status.setText(n > 0 ? "Importing " + juce::String(n) + " file" + (n == 1 ? "" : "s") + "..." : activity,
                       juce::dontSendNotification);
    };

    showTab(juce::jlimit(0, kTabCount - 1, static_cast<int>(ctx.prefs.get("ui", "nativeLibraryTab", 0))));
}

LibraryPanel::~LibraryPanel()
{
    ctx.importer.onProgress = nullptr;
}

void LibraryPanel::renameItem(const juce::String& itemId)
{
    showTab(0);
    library->startRename(itemId);
}

void LibraryPanel::showTab(int index)
{
    activeTab = juce::jlimit(0, kTabCount - 1, index);
    for (int i = 0; i < kTabCount; ++i)
    {
        views[static_cast<size_t>(i)]->setVisible(i == activeTab);
        tabs[i]->setColour(juce::TextButton::buttonColourId, i == activeTab ? colours::sky600 : colours::zinc800);
    }
    importButton.setVisible(activeTab <= 2);
    ctx.prefs.set("ui", "nativeLibraryTab", index);
    ctx.prefs.save();
}

void LibraryPanel::setActivity(const juce::String& text)
{
    activity = text;
    if (ctx.importer.pendingCount() == 0) status.setText(activity, juce::dontSendNotification);
}

void LibraryPanel::importWithDialog()
{
    chooser = std::make_unique<juce::FileChooser>("Import audio", juce::File(), AudioImporter::audioFileWildcard());
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this](const juce::FileChooser& fc)
                         {
                             if (!fc.getResults().isEmpty()) ctx.importer.import(fc.getResults());
                         });
}

void LibraryPanel::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    g.setColour(dragOver ? colours::accent : colours::border);
    g.drawRect(getLocalBounds(), dragOver ? 2 : 1);
}

void LibraryPanel::resized()
{
    auto area = getLocalBounds().reduced(1);
    auto header = area.removeFromTop(30).reduced(4, 3);
    for (auto* tab : tabs) tab->setBounds(header.removeFromLeft(juce::jmax(64, tab->getBestWidthForHeight(24) + 8)));
    importButton.setBounds(header.removeFromRight(90));
    header.removeFromRight(8);
    status.setBounds(header.reduced(8, 0));
    for (auto* c : views) c->setBounds(area.reduced(4, 2));
}

bool LibraryPanel::isInterestedInFileDrag(const juce::StringArray& paths)
{
    for (const auto& p : paths)
        if (AudioImporter::isAudioFile(juce::File(p)) || juce::File(p).isDirectory()) return true;
    return false;
}

void LibraryPanel::filesDropped(const juce::StringArray& paths, int, int)
{
    dragOver = false;
    repaint();
    juce::Array<juce::File> audio;
    for (const auto& p : paths)
    {
        const juce::File f(p);
        if (f.isDirectory())
        {
            for (const auto& entry : juce::RangedDirectoryIterator(f, true, "*", juce::File::findFiles))
                if (AudioImporter::isAudioFile(entry.getFile())) audio.add(entry.getFile());
        }
        else if (AudioImporter::isAudioFile(f))
        {
            audio.add(f);
        }
    }
    ctx.importer.import(audio);
    showTab(0);
}

} // namespace stemcel::native
