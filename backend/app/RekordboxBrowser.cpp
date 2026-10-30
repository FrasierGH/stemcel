#include "RekordboxBrowser.h"

#include "KeyDetection.h"
#include "Theme.h"

#include <algorithm>

namespace stemcel::native
{
namespace
{
// The library on show and its last read, shared by every workspace; Reload or a change
// to it replaces the read.
RekordboxSource currentSource;
std::shared_ptr<const RekordboxSnapshot> cachedSnapshot;

juce::var request(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [key, value] : fields) o->setProperty(key, value);
    return juce::var(o);
}

juce::var idArray(const juce::StringArray& ids)
{
    juce::Array<juce::var> out;
    for (const auto& id : ids) out.add(id);
    return out;
}

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

    void itemClicked(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) browser.showPlaylistMenu(playlist);
    }

  private:
    RekordboxBrowser& browser;
    const RekordboxPlaylist* playlist;
};

RekordboxBrowser::RekordboxBrowser(LibraryContext& context) : ctx(context)
{
    sourceBox.setTooltip("Which rekordbox library to show: this computer's, or one on a connected drive");
    sourceBox.onChange = [this]
    {
        const int index = sourceBox.getSelectedItemIndex();
        if (index >= 0 && index < static_cast<int>(sources.size())) chooseSource(sources[static_cast<size_t>(index)]);
    };
    addAndMakeVisible(sourceBox);

    tree.setRootItemVisible(false);
    tree.setColour(juce::TreeView::backgroundColourId, colours::zinc950);
    tree.setIndentSize(14);
    tree.addMouseListener(this, true);
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

    reloadButton.setTooltip("Read the rekordbox library again, and look for connected drives");
    reloadButton.onClick = [this]
    {
        cachedSnapshot.reset();
        refreshSources();
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
    ctx.rekordbox.changed.addChangeListener(this);
    setWantsKeyboardFocus(true);
    sources = {currentSource};
    sourceBox.addItem(currentSource.label(), 1);
    sourceBox.setSelectedItemIndex(0, juce::dontSendNotification);
    if (cachedSnapshot != nullptr) applySnapshot(cachedSnapshot);
}

RekordboxBrowser::~RekordboxBrowser()
{
    alive->store(false);
    pool.removeAllJobs(true, 60000);
    ctx.audition.removeChangeListener(this);
    ctx.rekordbox.changed.removeChangeListener(this);
    tree.setRootItem(nullptr);
}

void RekordboxBrowser::visibilityChanged()
{
    if (!isVisible()) return;
    if (snapshot == nullptr && !loading) load();
    if (sources.size() <= 1) refreshSources();
}

void RekordboxBrowser::refreshSources()
{
    if (!ctx.rekordbox.isAvailable()) return;
    pool.addJob(
        [this, alive = alive]
        {
            auto found = std::make_shared<std::vector<RekordboxSource>>(ctx.rekordbox.listSources());
            juce::MessageManager::callAsync(
                [this, alive, found]
                {
                    if (!alive->load()) return;
                    sources = *found;
                    // The library on show stays listed even when its drive has gone, until
                    // another is picked.
                    const auto current = currentSource.key();
                    auto at = std::find_if(sources.begin(), sources.end(), [&](const auto& s) { return s.key() == current; });
                    if (at == sources.end())
                    {
                        sources.push_back(currentSource);
                        at = sources.end() - 1;
                    }
                    sourceBox.clear(juce::dontSendNotification);
                    for (size_t i = 0; i < sources.size(); ++i) sourceBox.addItem(sources[i].label(), static_cast<int>(i) + 1);
                    sourceBox.setSelectedItemIndex(static_cast<int>(at - sources.begin()), juce::dontSendNotification);
                });
        });
}

void RekordboxBrowser::chooseSource(const RekordboxSource& source)
{
    if (source.key() == currentSource.key() && cachedSnapshot != nullptr) return;
    currentSource = source;
    cachedSnapshot.reset();
    playlistId = {};
    load();
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
    message.setText(currentSource.kind == RekordboxSource::Kind::computer ? juce::String("Reading your rekordbox library...")
                                                                          : "Reading " + currentSource.label() + "...",
                    juce::dontSendNotification);
    message.setVisible(true);
    message.toFront(false);
    pool.addJob(
        [this, alive = alive]
        {
            auto result = std::make_shared<RekordboxSnapshot>(ctx.rekordbox.open(currentSource));
            juce::MessageManager::callAsync(
                [this, alive, result]
                {
                    if (!alive->load()) return;
                    loading = false;
                    if (result->source.key() != currentSource.key()) return; // another library was picked meanwhile
                    if (!result->ok)
                    {
                        tree.setRootItem(nullptr);
                        snapshot.reset();
                        rebuildRows();
                        message.setText("Couldn't read " + currentSource.label() + ": " + result->error, juce::dontSendNotification);
                        message.setVisible(true);
                        if (auto then = std::exchange(afterReload, {})) then(lastWrite);
                        return;
                    }
                    cachedSnapshot = result;
                    applySnapshot(result);
                    if (auto then = std::exchange(afterReload, {})) then(lastWrite);
                });
        });
}

void RekordboxBrowser::applySnapshot(std::shared_ptr<const RekordboxSnapshot> next)
{
    // A reread of the same library keeps the folders open and the playlist on show.
    const bool same = snapshot != nullptr && next != nullptr && snapshot->source.key() == next->source.key();
    const auto openness = same ? tree.getOpennessState(true) : nullptr;
    const auto keep = same ? playlistId : juce::String();
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
    if (openness != nullptr) tree.restoreOpennessState(*openness, true);
    juce::TreeViewItem* select = all;
    if (keep.isNotEmpty() && snapshot->findPlaylist(keep) != nullptr)
    {
        std::function<juce::TreeViewItem*(juce::TreeViewItem*)> find = [&](juce::TreeViewItem* item) -> juce::TreeViewItem*
        {
            for (int i = 0; i < item->getNumSubItems(); ++i)
            {
                auto* child = item->getSubItem(i);
                if (child->getUniqueName() == keep) return child;
                if (auto* found = find(child)) return found;
            }
            return nullptr;
        };
        if (auto* found = find(treeRoot.get())) select = found;
    }
    select->setSelected(true, true);
    if (select != all) tree.scrollToKeepItemVisible(select);
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
    if (e.mods.isPopupMenu()) showTrackMenu(row);
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

void RekordboxBrowser::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &ctx.rekordbox.changed)
    {
        // A write to the library on show (from here, another workspace or the Library
        // panel's "Add to rekordbox"): read it again.
        if (ctx.rekordbox.lastChangedKey() == currentSource.key() && !loading)
        {
            cachedSnapshot.reset();
            load();
        }
        return;
    }
    table.repaint();
}

juce::StringArray RekordboxBrowser::selectedTrackIds() const
{
    juce::StringArray ids;
    for (int i = 0; i < table.getNumSelectedRows(); ++i)
        if (const auto* t = trackAt(table.getSelectedRow(i))) ids.add(t->id);
    return ids;
}

void RekordboxBrowser::runWrite(const juce::var& change, const juce::String& done, std::function<void(const juce::var&)> then)
{
    pool.addJob(
        [this, alive = alive, change, done, then]
        {
            const auto result = ctx.rekordbox.write(change);
            juce::MessageManager::callAsync(
                [this, alive, result, done, then]
                {
                    if (!alive->load()) return;
                    if (!static_cast<bool>(result.getProperty("ok", false)))
                    {
                        if (ctx.notify) ctx.notify(result.getProperty("error", "rekordbox refused the change").toString(), true);
                        if (then) then(result);
                        return;
                    }
                    if (ctx.notify && done.isNotEmpty()) ctx.notify(done, false);
                    // Rekordbox::changed has the library read again (or this does, if it
                    // hasn't yet); `then` runs once it has been.
                    lastWrite = result;
                    afterReload = then;
                    if (!loading)
                    {
                        cachedSnapshot.reset();
                        load();
                    }
                });
        });
}

void RekordboxBrowser::writeForTest(const juce::var& change, std::function<void(const juce::var&)> done)
{
    runWrite(change, {}, std::move(done));
}

void RekordboxBrowser::askForName(const juce::String& title, const juce::String& initial, std::function<void(const juce::String&)> then)
{
    auto* window = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("name", initial);
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create(
                                      [window, then](int result)
                                      {
                                          const auto name = window->getTextEditorContents("name").trim();
                                          if (result == 1 && name.isNotEmpty()) then(name);
                                      }),
                            true);
}

void RekordboxBrowser::createPlaylist(const juce::String& parentId, bool folder)
{
    askForName(folder ? "New folder" : "New playlist", folder ? "New Folder" : "New Playlist",
               [this, parentId, folder](const juce::String& name)
               {
                   runWrite(request({{"action", "createPlaylist"},
                                     {"name", name},
                                     {"parentId", parentId.isEmpty() ? juce::var() : juce::var(parentId)},
                                     {"folder", folder}}),
                            "Made " + juce::String(folder ? "folder" : "playlist") + " \"" + name + "\" in " + currentSource.label());
               });
}

void RekordboxBrowser::renamePlaylist(const RekordboxPlaylist& playlist)
{
    const auto id = playlist.id;
    askForName("Rename", playlist.name,
               [this, id](const juce::String& name)
               { runWrite(request({{"action", "rename"}, {"id", id}, {"name", name}}), "Renamed to \"" + name + "\""); });
}

void RekordboxBrowser::deletePlaylist(const RekordboxPlaylist& playlist)
{
    const auto id = playlist.id;
    const auto name = playlist.name;
    const auto what = playlist.isFolder() ? juce::String("the folder \"") + name + "\" and everything in it"
                                          : juce::String("the playlist \"") + name + "\"";
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::WarningIcon)
                                     .withTitle("Delete from " + currentSource.label())
                                     .withMessage("Delete " + what + "? The tracks stay in the library.")
                                     .withButton("Delete")
                                     .withButton("Cancel")
                                     .withAssociatedComponent(this),
                                 [this, id, name, alive = alive](int result)
                                 {
                                     if (result == 1 && alive->load())
                                         runWrite(request({{"action", "delete"}, {"id", id}}), "Deleted \"" + name + "\"");
                                 });
}

void RekordboxBrowser::addFilesToPlaylist(const juce::String& targetId)
{
    chooser = std::make_unique<juce::FileChooser>("Add audio files to " + currentSource.label(), juce::File(),
                                                  "*.wav;*.aif;*.aiff;*.mp3;*.m4a;*.aac;*.flac;*.alac;*.ogg");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this, targetId](const juce::FileChooser& fc)
                         {
                             juce::StringArray paths;
                             for (const auto& f : fc.getResults()) paths.add(f.getFullPathName());
                             if (paths.isEmpty()) return;
                             runWrite(request({{"action", "importFiles"}, {"paths", idArray(paths)}, {"playlistId", targetId}}),
                                      {},
                                      [this](const juce::var& result)
                                      {
                                          if (!static_cast<bool>(result.getProperty("ok", false)) || !ctx.notify) return;
                                          const int added = result.getProperty("added", 0);
                                          const int existing = result.getProperty("existing", 0);
                                          const auto* refused = result.getProperty("refused", {}).getArray();
                                          juce::String text = "Added " + juce::String(added) + " file(s) to " + currentSource.label();
                                          if (existing > 0) text << " (" << existing << " already there)";
                                          if (refused != nullptr && !refused->isEmpty())
                                              text << "; skipped " << refused->size() << ": " << (*refused)[0].toString();
                                          ctx.notify(text, refused != nullptr && !refused->isEmpty());
                                      });
                         });
}

void RekordboxBrowser::showPlaylistMenu(const RekordboxPlaylist* playlist)
{
    if (snapshot == nullptr) return;
    const bool device = currentSource.isDevice();
    juce::PopupMenu menu;
    // New lists go inside a folder, or beside a playlist (rekordbox's own menus).
    const auto parentId = playlist == nullptr ? juce::String() : playlist->isFolder() ? playlist->id : playlist->parentId;
    menu.addItem("New playlist", [this, parentId] { createPlaylist(parentId, false); });
    menu.addItem("New folder", [this, parentId] { createPlaylist(parentId, true); });
    if (playlist != nullptr)
    {
        const auto list = *playlist;
        menu.addSeparator();
        if (!device && list.kind == "playlist")
            menu.addItem("Add audio files...", [this, id = list.id] { addFilesToPlaylist(id); });
        menu.addItem("Rename...", [this, list] { renamePlaylist(list); });
        menu.addItem("Delete...", [this, list] { deletePlaylist(list); });
    }
    menu.addSeparator();
    menu.addItem("Show backups", [] { Rekordbox::backupFolder().createDirectory(); Rekordbox::backupFolder().revealToUser(); });
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void RekordboxBrowser::addPlaylistTargets(juce::PopupMenu& menu, const juce::String& parentId)
{
    for (const auto* list : snapshot->childrenOf(parentId))
    {
        if (list->isFolder())
        {
            juce::PopupMenu sub;
            addPlaylistTargets(sub, list->id);
            if (sub.getNumItems() > 0) menu.addSubMenu(list->name, sub);
        }
        else if (list->kind == "playlist")
        {
            const auto id = list->id;
            const auto name = list->name;
            menu.addItem(name,
                         [this, id, name]
                         {
                             const auto ids = selectedTrackIds();
                             runWrite(request({{"action", "addTracks"}, {"playlistId", id}, {"trackIds", idArray(ids)}}),
                                      "Added " + juce::String(ids.size()) + " track(s) to \"" + name + "\"");
                         });
        }
    }
}

void RekordboxBrowser::showTrackMenu(int row)
{
    const auto* t = trackAt(row);
    if (t == nullptr) return;
    if (!table.isRowSelected(row)) table.selectRow(row);
    const auto path = t->path;
    juce::PopupMenu menu;
    menu.addItem("Import to library", [this] { ctx.importer.import(selectedRequests()); });
    menu.addItem("Audition", [this, path] { ctx.audition.toggle(juce::File(path)); });
    menu.addItem("Show file", [path] { juce::File(path).revealToUser(); });
    if (snapshot != nullptr)
    {
        menu.addSeparator();
        juce::PopupMenu targets;
        addPlaylistTargets(targets, {});
        menu.addSubMenu("Add to playlist", targets, targets.getNumItems() > 0);
        if (const auto* list = snapshot->findPlaylist(playlistId); list != nullptr && list->kind == "playlist")
        {
            const auto id = list->id;
            const auto name = list->name;
            menu.addItem("Remove from \"" + name + "\"",
                         [this, id, name]
                         {
                             const auto ids = selectedTrackIds();
                             runWrite(request({{"action", "removeTracks"}, {"playlistId", id}, {"trackIds", idArray(ids)}}),
                                      "Took " + juce::String(ids.size()) + " track(s) out of \"" + name + "\"");
                         });
        }
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void RekordboxBrowser::mouseUp(const juce::MouseEvent& e)
{
    // Right-click in the tree's empty space: new lists at the top level.
    if (!e.mods.isPopupMenu() || snapshot == nullptr || e.eventComponent == nullptr) return;
    if (e.eventComponent != &tree && !tree.isParentOf(e.eventComponent)) return;
    if (tree.getItemAt(e.getEventRelativeTo(&tree).getPosition().y) == nullptr) showPlaylistMenu(nullptr);
}

void RekordboxBrowser::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void RekordboxBrowser::resized()
{
    auto area = getLocalBounds();
    auto left = area.removeFromLeft(juce::jmin(260, area.getWidth() / 3));
    sourceBox.setBounds(left.removeFromTop(26).reduced(0, 2));
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
