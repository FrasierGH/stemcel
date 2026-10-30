#include "RekordboxBrowser.h"

#include "KeyDetection.h"
#include "Theme.h"

#include <algorithm>

namespace stemcel::native
{
namespace
{
// One library read per app run, shared by every workspace; Reload replaces it.
std::shared_ptr<const RekordboxSnapshot> cachedSnapshot;

// rekordbox's track colours (djmdColor order).
juce::Colour trackColour(int index)
{
    static const juce::Colour palette[] = {juce::Colour(0x00000000), juce::Colour(0xffff3a8a), juce::Colour(0xffff3333),
                                           juce::Colour(0xffff9900), juce::Colour(0xffffee00), juce::Colour(0xff33cc33),
                                           juce::Colour(0xff00ccff), juce::Colour(0xff3366ff), juce::Colour(0xffaa66ff)};
    return index > 0 && index < 9 ? palette[index] : juce::Colour(0x00000000);
}

juce::String formatDuration(double ms)
{
    if (ms <= 0.0) return {};
    const int total = static_cast<int>(ms / 1000.0 + 0.5);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}

enum Column
{
    play = 1,
    title,
    artist,
    bpm,
    key,
    length,
    genre,
    rating,
    added,
};
} // namespace

class RekordboxBrowser::PlaylistItem final : public juce::TreeViewItem
{
  public:
    PlaylistItem(RekordboxBrowser& owner, const RekordboxPlaylist* list) : browser(owner), playlist(list)
    {
        if (playlist != nullptr && playlist->isFolder())
            for (const auto* child : browser.snapshot->childrenOf(playlist->id))
                addSubItem(new PlaylistItem(browser, child));
    }

    bool mightContainSubItems() override { return getNumSubItems() > 0; }
    juce::String getUniqueName() const override { return playlist == nullptr ? "all" : playlist->id; }
    int getItemHeight() const override { return 22; }
    bool canBeSelected() const override { return playlist == nullptr || !playlist->isFolder(); }

    void paintItem(juce::Graphics& g, int width, int height) override
    {
        if (isSelected()) g.fillAll(colours::sky600.withAlpha(0.35f));
        juce::String name = playlist == nullptr ? "All tracks" : playlist->name;
        if (playlist != nullptr && !playlist->isFolder() && playlist->kind != "smart")
            name << "  (" << playlist->trackIds.size() << ")";
        g.setColour(playlist != nullptr && playlist->kind == "smart" ? colours::textFaint
                    : playlist != nullptr && playlist->isFolder()   ? colours::textDim
                                                                    : colours::text);
        g.setFont(uiFont(13.0f, playlist == nullptr));
        g.drawText(name, 4, 0, width - 4, height, juce::Justification::centredLeft, true);
    }

    void itemSelectionChanged(bool isNowSelected) override
    {
        if (isNowSelected) browser.showPlaylist(playlist == nullptr ? juce::String() : playlist->id);
    }

  private:
    RekordboxBrowser& browser;
    const RekordboxPlaylist* playlist;
};

RekordboxBrowser::RekordboxBrowser(LibraryContext& context) : ctx(context)
{
    tree.setRootItemVisible(false);
    tree.setColour(juce::TreeView::backgroundColourId, colours::zinc950);
    tree.setIndentSize(14);
    addAndMakeVisible(tree);

    auto& header = table.getHeader();
    header.addColumn("", play, 26, 26, 26, juce::TableHeaderComponent::notResizableOrSortable);
    header.addColumn("Title", title, 240, 80, -1, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Artist", artist, 170, 60, -1, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("BPM", bpm, 60, 40, 90, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Key", key, 70, 40, 120, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Time", length, 56, 40, 90, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Genre", genre, 110, 50, -1, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Rating", rating, 60, 40, 90, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Added", added, 90, 50, 140, juce::TableHeaderComponent::defaultFlags);
    table.setColour(juce::ListBox::backgroundColourId, colours::zinc900);
    table.getHeader().setColour(juce::TableHeaderComponent::backgroundColourId, colours::zinc800);
    table.getHeader().setColour(juce::TableHeaderComponent::textColourId, colours::textDim);
    table.setRowHeight(24);
    table.setMultipleSelectionEnabled(true);
    table.setModel(this);
    addAndMakeVisible(table);

    filter.setTextToShowWhenEmpty("Search title, artist, genre, key", colours::textFaint);
    filter.onTextChange = [this] { startTimer(150); };
    addAndMakeVisible(filter);

    reloadButton.setTooltip("Read the rekordbox library again");
    reloadButton.onClick = [this]
    {
        cachedSnapshot.reset();
        load();
    };
    importButton.setTooltip("Import the selected tracks with their beat grid and key");
    importButton.onClick = [this] { ctx.importer.import(selectedRequests()); };
    addAndMakeVisible(reloadButton);
    addAndMakeVisible(importButton);

    message.setJustificationType(juce::Justification::centred);
    message.setColour(juce::Label::textColourId, colours::textFaint);
    addChildComponent(message);

    ctx.audition.addChangeListener(this);
    setWantsKeyboardFocus(true);
    if (cachedSnapshot != nullptr) applySnapshot(cachedSnapshot);
}

RekordboxBrowser::~RekordboxBrowser()
{
    alive->store(false);
    pool.removeAllJobs(true, 60000);
    ctx.audition.removeChangeListener(this);
    tree.setRootItem(nullptr);
}

void RekordboxBrowser::visibilityChanged()
{
    if (isVisible() && snapshot == nullptr && !loading) load();
}

void RekordboxBrowser::load()
{
    if (!ctx.rekordbox.isAvailable())
    {
        message.setText(ctx.rekordbox.unavailableReason(), juce::dontSendNotification);
        message.setVisible(true);
        return;
    }
    if (cachedSnapshot != nullptr)
    {
        applySnapshot(cachedSnapshot);
        return;
    }
    loading = true;
    message.setText("Reading your rekordbox library...", juce::dontSendNotification);
    message.setVisible(true);
    message.toFront(false);
    pool.addJob(
        [this, alive = alive]
        {
            auto result = std::make_shared<RekordboxSnapshot>(ctx.rekordbox.open());
            juce::MessageManager::callAsync(
                [this, alive, result]
                {
                    if (!alive->load()) return;
                    loading = false;
                    if (!result->ok)
                    {
                        message.setText("Couldn't read the rekordbox library: " + result->error, juce::dontSendNotification);
                        message.setVisible(true);
                        return;
                    }
                    cachedSnapshot = result;
                    applySnapshot(result);
                });
        });
}

void RekordboxBrowser::applySnapshot(std::shared_ptr<const RekordboxSnapshot> next)
{
    tree.setRootItem(nullptr);
    snapshot = std::move(next);
    message.setVisible(false);
    // "All tracks" sits above the playlist tree.
    struct Holder final : public juce::TreeViewItem
    {
        bool mightContainSubItems() override { return true; }
        void paintItem(juce::Graphics&, int, int) override {}
    };
    auto holder = std::make_unique<Holder>();
    auto* all = new PlaylistItem(*this, nullptr);
    holder->addSubItem(all);
    for (const auto* child : snapshot->childrenOf({})) holder->addSubItem(new PlaylistItem(*this, child));
    treeRoot = std::move(holder);
    tree.setRootItem(treeRoot.get());
    treeRoot->setOpen(true);
    all->setSelected(true, true);
}

void RekordboxBrowser::showPlaylist(const juce::String& id)
{
    playlistId = id;
    rebuildRows();
}

void RekordboxBrowser::timerCallback()
{
    stopTimer();
    rebuildRows();
}

void RekordboxBrowser::rebuildRows()
{
    rows.clear();
    if (snapshot == nullptr)
    {
        table.updateContent();
        return;
    }
    const auto words = juce::StringArray::fromTokens(filter.getText().toLowerCase(), true);
    auto matches = [&words](const RekordboxTrack& t)
    {
        if (words.isEmpty()) return true;
        const auto haystack = (t.title + " " + t.artist + " " + t.album + " " + t.genre + " " + t.key + " " + t.label).toLowerCase();
        for (const auto& w : words)
            if (!haystack.contains(w)) return false;
        return true;
    };
    if (playlistId.isEmpty())
    {
        for (const auto& t : snapshot->tracks)
            if (matches(t)) rows.push_back(&t);
    }
    else if (const auto* list = snapshot->findPlaylist(playlistId))
    {
        for (const auto& id : list->trackIds)
            if (const auto* t = snapshot->findTrack(id); t != nullptr && matches(*t)) rows.push_back(t);
    }

    if (sortColumn != 0)
    {
        auto less = [this](const RekordboxTrack* a, const RekordboxTrack* b)
        {
            switch (sortColumn)
            {
            case title: return a->title.compareNatural(b->title) < 0;
            case artist: return a->artist.compareNatural(b->artist) < 0;
            case bpm: return a->bpm < b->bpm;
            case key: return a->key.compareNatural(b->key) < 0;
            case length: return a->durationMs < b->durationMs;
            case genre: return a->genre.compareNatural(b->genre) < 0;
            case rating: return a->rating < b->rating;
            case added: return a->dateAdded < b->dateAdded;
            default: return false;
            }
        };
        std::stable_sort(rows.begin(), rows.end(), [&](auto* a, auto* b) { return sortForwards ? less(a, b) : less(b, a); });
    }
    table.updateContent();
    table.repaint();
}

void RekordboxBrowser::sortOrderChanged(int newSortColumnId, bool isForwards)
{
    sortColumn = newSortColumnId;
    sortForwards = isForwards;
    rebuildRows();
}

const RekordboxTrack* RekordboxBrowser::trackAt(int row) const
{
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[static_cast<size_t>(row)] : nullptr;
}

int RekordboxBrowser::getNumRows()
{
    return static_cast<int>(rows.size());
}

void RekordboxBrowser::paintRowBackground(juce::Graphics& g, int row, int, int height, bool selected)
{
    g.fillAll(selected ? colours::sky600.withAlpha(0.35f) : (row % 2 ? colours::zinc900 : colours::zinc950));
    if (const auto* t = trackAt(row); t != nullptr && t->colour > 0)
    {
        g.setColour(trackColour(t->colour));
        g.fillRect(0, 2, 3, height - 4);
    }
}

void RekordboxBrowser::paintCell(juce::Graphics& g, int row, int column, int width, int height, bool)
{
    const auto* t = trackAt(row);
    if (t == nullptr) return;
    if (column == play)
    {
        const bool playing = ctx.audition.current() == juce::File(t->path);
        g.setColour(playing ? colours::emerald500 : colours::textFaint);
        juce::Path icon;
        const float cx = width / 2.0f, cy = height / 2.0f;
        if (playing) icon.addRectangle(cx - 4, cy - 4, 8, 8);
        else icon.addTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 5, cy);
        g.fillPath(icon);
        return;
    }
    juce::String text;
    switch (column)
    {
    case title: text = t->title; break;
    case artist: text = t->artist; break;
    case bpm: text = t->bpm > 0.0 ? juce::String(t->bpm, 2) : juce::String(); break;
    case key: text = t->key; break;
    case length: text = formatDuration(t->durationMs); break;
    case genre: text = t->genre; break;
    case rating: text = juce::String::repeatedString(juce::CharPointer_UTF8("\xe2\x98\x85"), juce::jlimit(0, 5, t->rating)); break;
    case added: text = t->dateAdded; break;
    default: break;
    }
    const bool missing = column == title && !juce::File(t->path).existsAsFile();
    g.setColour(missing ? colours::red500 : column == title ? colours::text : colours::textDim);
    g.setFont(uiFont(13.0f));
    g.drawText(text, 6, 0, width - 8, height, juce::Justification::centredLeft, true);
}

std::vector<ImportRequest> RekordboxBrowser::selectedRequests() const
{
    std::vector<ImportRequest> out;
    for (int i = 0; i < table.getNumSelectedRows(); ++i)
        if (const auto* t = trackAt(table.getSelectedRow(i)))
        {
            ImportRequest request;
            request.file = juce::File(t->path);
            request.hints.rekordboxId = t->id;
            request.hints.title = t->title;
            request.hints.artist = t->artist;
            request.hints.key = t->key;
            request.hints.bpm = t->bpm;
            out.push_back(std::move(request));
        }
    return out;
}

juce::var RekordboxBrowser::getDragSourceDescription(const juce::SparseSet<int>&)
{
    return importDragDescription(selectedRequests());
}

void RekordboxBrowser::cellClicked(int row, int column, const juce::MouseEvent& e)
{
    const auto* t = trackAt(row);
    if (t == nullptr) return;
    if (column == play)
    {
        ctx.audition.toggle(juce::File(t->path));
        return;
    }
    if (e.mods.isPopupMenu())
    {
        if (!table.isRowSelected(row)) table.selectRow(row);
        const auto path = t->path;
        juce::PopupMenu menu;
        menu.addItem("Import to library", [this] { ctx.importer.import(selectedRequests()); });
        menu.addItem("Audition", [this, path] { ctx.audition.toggle(juce::File(path)); });
        menu.addItem("Show file", [path] { juce::File(path).revealToUser(); });
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }
}

void RekordboxBrowser::cellDoubleClicked(int, int, const juce::MouseEvent&)
{
    ctx.importer.import(selectedRequests());
}

void RekordboxBrowser::returnKeyPressed(int)
{
    ctx.importer.import(selectedRequests());
}

bool RekordboxBrowser::keyPressed(const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey && table.getNumSelectedRows() > 0)
    {
        if (const auto* t = trackAt(table.getSelectedRow(0))) ctx.audition.toggle(juce::File(t->path));
        return true;
    }
    return false;
}

void RekordboxBrowser::changeListenerCallback(juce::ChangeBroadcaster*)
{
    table.repaint();
}

void RekordboxBrowser::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void RekordboxBrowser::resized()
{
    auto area = getLocalBounds();
    auto left = area.removeFromLeft(juce::jmin(260, area.getWidth() / 3));
    tree.setBounds(left);
    area.removeFromLeft(4);
    auto top = area.removeFromTop(26).reduced(0, 2);
    importButton.setBounds(top.removeFromRight(70));
    top.removeFromRight(4);
    reloadButton.setBounds(top.removeFromRight(70));
    top.removeFromRight(6);
    filter.setBounds(top);
    table.setBounds(area);
    message.setBounds(getLocalBounds());
}

} // namespace stemcel::native
