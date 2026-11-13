#include "LibraryView.h"

#include "AudioImport.h"
#include "KeyDetection.h"
#include "Rekordbox.h"
#include "Theme.h"
#include "UsbExport.h"
#include "WaveformStore.h"

#include <algorithm>
#include <set>

namespace stemcel::native::dj
{
namespace
{
enum Column
{
    colour = 1,
    number,
    title,
    artist,
    album,
    genre,
    bpm,
    key,
    length,
    rating,
    tags,
    added,
    comment,
    state,
};

constexpr juce::int64 kHistories = -1; // the Histories folder in the tree
const char* kDragPrefix = "stemcel-collection:";

juce::String formatTime(double sec)
{
    if (sec <= 0.0) return {};
    const int total = static_cast<int>(sec + 0.5);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}

std::vector<juce::int64> idsFromDrag(const juce::var& description)
{
    std::vector<juce::int64> ids;
    const auto text = description.toString();
    if (!text.startsWith(kDragPrefix)) return ids;
    for (const auto& token : juce::StringArray::fromTokens(text.fromFirstOccurrenceOf(kDragPrefix, false, false), ",", ""))
        if (token.getLargeIntValue() != 0) ids.push_back(token.getLargeIntValue());
    return ids;
}
} // namespace

// ── The tree ────────────────────────────────────────────────────────────────

class LibraryView::ListItem final : public juce::TreeViewItem
{
  public:
    // id: 0 the collection, kHistories the Histories folder, else a playlist.
    ListItem(LibraryView& owner, juce::int64 listId) : view(owner), id(listId)
    {
        auto& c = view.collection;
        if (id == kHistories)
        {
            for (const auto* p : c.childrenOf(0, true)) addSubItem(new ListItem(view, p->id));
            return;
        }
        if (const auto* p = c.findPlaylist(id); p != nullptr && p->isFolder())
            for (const auto* child : c.childrenOf(id, false)) addSubItem(new ListItem(view, child->id));
    }

    bool mightContainSubItems() override { return getNumSubItems() > 0; }
    juce::String getUniqueName() const override { return juce::String(id); }
    int getItemHeight() const override { return 22; }

    void paintItem(juce::Graphics& g, int width, int height) override
    {
        if (isSelected()) g.fillAll(colours::sky600.withAlpha(0.35F));
        auto& c = view.collection;
        juce::String name;
        auto colour = colours::text;
        bool bold = false;
        if (id == 0)
        {
            name = "Collection  (" + juce::String(static_cast<int>(c.tracks().size())) + ")";
            bold = true;
        }
        else if (id == kHistories)
        {
            name = "Histories";
            colour = colours::textDim;
            bold = true;
        }
        else if (const auto* p = c.findPlaylist(id))
        {
            name = p->name;
            if (p->isFolder())
                colour = colours::textDim;
            else
                name << "  (" << static_cast<int>(p->tracks.size()) << ")";
        }
        g.setColour(colour);
        g.setFont(uiFont(13.0F, bold));
        g.drawText(name, 4, 0, width - 4, height, juce::Justification::centredLeft, true);
    }

    void itemSelectionChanged(bool isNowSelected) override
    {
        if (isNowSelected && id != kHistories) view.showList(id);
    }

    void itemClicked(const juce::MouseEvent& e) override
    {
        if (!e.mods.isPopupMenu()) return;
        view.showListMenu(id > 0 ? view.collection.findPlaylist(id) : nullptr, id == kHistories);
    }

    bool isInterestedInDragSource(const juce::DragAndDropTarget::SourceDetails& details) override
    {
        const auto* p = view.collection.findPlaylist(id);
        if (p == nullptr || p->kind == Playlist::Kind::history) return false;
        if (details.description.toString().startsWith("stemcel-playlist:")) return true; // moved into a folder
        return !p->isFolder() && !idsFromDrag(details.description).empty();
    }

    void itemDropped(const juce::DragAndDropTarget::SourceDetails& details, int) override
    {
        const auto text = details.description.toString();
        if (text.startsWith("stemcel-playlist:"))
        {
            const auto moving = text.fromFirstOccurrenceOf(":", false, false).getLargeIntValue();
            const auto* p = view.collection.findPlaylist(id);
            view.collection.movePlaylist(moving, p != nullptr && p->isFolder() ? id : (p != nullptr ? p->parent : 0));
            return;
        }
        view.collection.addToPlaylist(id, idsFromDrag(details.description));
    }

    juce::var getDragSourceDescription() override
    {
        return id > 0 ? juce::var("stemcel-playlist:" + juce::String(id)) : juce::var();
    }

    const juce::int64 id;

  private:
    LibraryView& view;
};

// ── The view ────────────────────────────────────────────────────────────────

LibraryView::LibraryView(Context context)
    : ctx(std::move(context)), collection(Collection::shared()), trackEditor(ctx.engine, ctx.waveforms)
{
    collection.load();
    tree.setRootItemVisible(false);
    tree.setColour(juce::TreeView::backgroundColourId, colours::zinc950);
    tree.setIndentSize(14);
    tree.addMouseListener(this, true);
    addAndMakeVisible(tree);

    auto& header = table.getHeader();
    const auto flags = juce::TableHeaderComponent::defaultFlags;
    header.addColumn("", colour, 10, 10, 10, juce::TableHeaderComponent::notResizableOrSortable);
    header.addColumn("#", number, 36, 30, 60, flags);
    header.addColumn("Title", title, 240, 80, -1, flags);
    header.addColumn("Artist", artist, 160, 60, -1, flags);
    header.addColumn("Album", album, 130, 50, -1, flags);
    header.addColumn("Genre", genre, 100, 50, -1, flags);
    header.addColumn("BPM", bpm, 60, 40, 90, flags);
    header.addColumn("Key", key, 70, 40, 120, flags);
    header.addColumn("Time", length, 54, 40, 90, flags);
    header.addColumn("Rating", rating, 64, 40, 90, flags);
    header.addColumn("My Tags", tags, 120, 50, -1, flags);
    header.addColumn("Added", added, 86, 50, 140, flags);
    header.addColumn("Comment", comment, 140, 50, -1, flags);
    header.addColumn("", state, 22, 22, 22, juce::TableHeaderComponent::notResizableOrSortable);
    header.setColour(juce::TableHeaderComponent::backgroundColourId, colours::zinc800);
    header.setColour(juce::TableHeaderComponent::textColourId, colours::textDim);
    table.setColour(juce::ListBox::backgroundColourId, colours::zinc900);
    table.setRowHeight(24);
    table.setMultipleSelectionEnabled(true);
    table.setModel(this);
    addAndMakeVisible(table);
    addAndMakeVisible(trackEditor);

    search.setTextToShowWhenEmpty("Search title, artist, album, genre, key, tags", colours::textFaint);
    search.onTextChange = [this] { search.getProperties().set("dirty", true); };
    addAndMakeVisible(search);

    addFilesButton.onClick = [this] { addFiles(); };
    addFilesButton.setTooltip("Add audio files to the collection (and the playlist on show)");
    addFolderButton.onClick = [this] { addFolder(); };
    addFolderButton.setTooltip("Add every audio file in a folder and its folders");
    rekordboxButton.onClick = [this] { importFromRekordbox(); };
    rekordboxButton.setTooltip("Bring this computer's rekordbox library in: tracks, grids, cues and playlists");
    rekordboxButton.setEnabled(ctx.rekordbox.isAvailable());
    exportButton.onClick = [this] { exportToStick(); };
    exportButton.setTooltip("Put playlists on a USB stick or SD card for CDJs and XDJs, without rekordbox");
    exportButton.setEnabled(ctx.rekordbox.isAvailable());
    historyButton.onClick = [this] { readHistories(); };
    performButton.setTooltip("Performance mode, with this collection in its browser (Ctrl/Cmd+Shift+L)");
    performButton.onClick = [this]
    {
        if (ctx.performance) ctx.performance();
    };
    addAndMakeVisible(performButton);
    linkButton.setTooltip("Link Export: players on the same network browse and load the collection, with no USB stick (as rekordbox's LINK)");
    linkButton.setClickingTogglesState(false);
    linkButton.onClick = [this] { toggleLink(); };
    linkButton.setEnabled(ctx.rekordbox.isAvailable());
    addAndMakeVisible(linkButton);
    historyButton.setTooltip("Read the play histories a player kept on a USB stick");
    closeButton.onClick = [this]
    {
        if (ctx.close) ctx.close();
    };
    for (auto* b : {&addFilesButton, &addFolderButton, &rekordboxButton, &exportButton, &historyButton, &closeButton}) addAndMakeVisible(*b);
    status.setColour(juce::Label::textColourId, colours::textFaint);
    status.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(status);

    collection.addChangeListener(this);
    setWantsKeyboardFocus(true);
    rebuildTree();
    showList(0);
    showLinkStatus();
    startTimer(250);
}

LibraryView::~LibraryView()
{
    alive->store(false);
    collection.removeChangeListener(this);
    tree.removeMouseListener(this);
    tree.setRootItem(nullptr);
}

void LibraryView::rebuildTree()
{
    const auto openness = tree.getOpennessState(true);
    tree.setRootItem(nullptr);
    struct Holder final : public juce::TreeViewItem
    {
        bool mightContainSubItems() override { return true; }
        void paintItem(juce::Graphics&, int, int) override {}
    };
    auto holder = std::make_unique<Holder>();
    holder->addSubItem(new ListItem(*this, 0));
    for (const auto* p : collection.childrenOf(0, false)) holder->addSubItem(new ListItem(*this, p->id));
    if (!collection.childrenOf(0, true).empty()) holder->addSubItem(new ListItem(*this, kHistories));
    treeRoot = std::move(holder);
    tree.setRootItem(treeRoot.get());
    treeRoot->setOpen(true);
    if (openness != nullptr) tree.restoreOpennessState(*openness, true);
    // The list on show stays selected.
    std::function<juce::TreeViewItem*(juce::TreeViewItem*)> find = [&](juce::TreeViewItem* item) -> juce::TreeViewItem*
    {
        for (int i = 0; i < item->getNumSubItems(); ++i)
        {
            auto* child = item->getSubItem(i);
            if (child->getUniqueName() == juce::String(listId)) return child;
            if (auto* found = find(child)) return found;
        }
        return nullptr;
    };
    if (auto* found = find(treeRoot.get())) found->setSelected(true, true, juce::dontSendNotification);
}

void LibraryView::showList(juce::int64 playlist)
{
    if (playlist != 0 && collection.findPlaylist(playlist) == nullptr) playlist = 0;
    listId = playlist;
    rebuildRows();
    table.deselectAllRows();
}

void LibraryView::timerCallback()
{
    // The search box's debounce, and Link's status every second.
    if (search.getProperties().getWithDefault("dirty", false))
    {
        search.getProperties().set("dirty", false);
        rebuildRows();
    }
    if (++ticks % 4 == 0) showLinkStatus();
}

void LibraryView::showLinkStatus()
{
    if (linkStarting || !ctx.rekordbox.isAvailable()) return;
    const auto s = ctx.rekordbox.linkStatus();
    const bool running = static_cast<bool>(s.getProperty("running", false));
    linkButton.setToggleState(running, juce::dontSendNotification);
    linkButton.setColour(juce::TextButton::buttonOnColourId, colours::accent.withAlpha(0.7F));
    if (!running)
    {
        if (linkText.isNotEmpty())
        {
            linkText = {};
            updateStatus();
        }
        return;
    }
    juce::StringArray players;
    if (const auto* list = s.getProperty("players", {}).getArray())
        for (const auto& p : *list)
            players.add(p.getProperty("name", "Player").toString() + " " + p.getProperty("number", 0).toString()
                        + (static_cast<bool>(p.getProperty("playing", false)) ? " (playing)" : ""));
    linkText = "LINK on " + s.getProperty("address", "").toString() + ", " + s.getProperty("state", "").toString()
               + (players.isEmpty() ? juce::String() : ": " + players.joinIntoString(", "));
    updateStatus();
}

void LibraryView::toggleLink()
{
    if (linkStarting) return;
    if (linkButton.getToggleState())
    {
        ctx.rekordbox.linkStop();
        showLinkStatus();
        if (ctx.notify) ctx.notify("Link Export stopped", false);
        return;
    }
    const auto found = ctx.rekordbox.linkInterfaces();
    const auto* list = found.getProperty("interfaces", {}).getArray();
    if (list == nullptr || list->isEmpty())
    {
        if (ctx.notify) ctx.notify("No network to run Link on: connect the computer to the players' network (Ethernet or a LINK hub)", true);
        return;
    }
    if (list->size() == 1)
    {
        startLink((*list)[0].getProperty("name", "").toString());
        return;
    }
    juce::PopupMenu menu;
    menu.addSectionHeader("Run Link on");
    for (const auto& i : *list)
        menu.addItem(i.getProperty("name", "").toString() + "  (" + i.getProperty("address", "").toString() + ")",
                     [this, name = i.getProperty("name", "").toString()] { startLink(name); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(linkButton));
}

void LibraryView::startLink(const juce::String& interfaceName)
{
    linkStarting = true;
    linkText = "LINK: preparing the library...";
    updateStatus();
    collection.setPaused(true);
    const auto request = collection.linkRequest(interfaceName);
    auto& rekordbox = ctx.rekordbox;
    juce::Thread::launch(
        [this, request, &rekordbox, alive = alive]
        {
            const auto result = rekordbox.linkStart(request,
                                                    [this, alive](const juce::var& p)
                                                    {
                                                        const auto text = "LINK: preparing " + p.getProperty("done", 0).toString() + " of "
                                                                          + p.getProperty("total", 0).toString();
                                                        juce::MessageManager::callAsync(
                                                            [this, alive, text]
                                                            {
                                                                if (!alive->load()) return;
                                                                linkText = text;
                                                                updateStatus();
                                                            });
                                                    });
            juce::MessageManager::callAsync(
                [this, alive, result]
                {
                    Collection::shared().setPaused(false);
                    if (!alive->load()) return;
                    linkStarting = false;
                    if (!static_cast<bool>(result.getProperty("ok", false)))
                    {
                        linkText = {};
                        if (ctx.notify) ctx.notify("Link Export: " + result.getProperty("error", "couldn't start").toString(), true);
                    }
                    else if (ctx.notify)
                    {
                        ctx.notify("Link Export on " + result.getProperty("interface", "").toString() + ": "
                                       + result.getProperty("tracks", 0).toString() + " tracks for the players",
                                   false);
                    }
                    showLinkStatus();
                });
        });
}

void LibraryView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // A change to the playlists rebuilds the tree; any change, the rows.
    rebuildTree();
    const auto selected = selectedTracks();
    rebuildRows();
    for (const auto id : selected)
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i] == id) table.selectRow(static_cast<int>(i), false, false);
    updateStatus();
}

void LibraryView::updateStatus()
{
    juce::String text = juce::String(static_cast<int>(rows.size())) + " tracks";
    if (const int pending = collection.analysisPending(); pending > 0) text << ", analysing " << pending << "...";
    if (importing) text << ", reading rekordbox...";
    if (linkText.isNotEmpty()) text = linkText + "   |   " + text;
    status.setText(text, juce::dontSendNotification);
}

void LibraryView::rebuildRows()
{
    rows.clear();
    std::vector<juce::int64> source;
    if (listId == 0)
        for (const auto& t : collection.tracks()) source.push_back(t.id);
    else if (const auto* p = collection.findPlaylist(listId); p != nullptr && !p->isFolder())
        source = p->tracks;
    else
        source = collection.tracksIn(listId);
    const auto words = juce::StringArray::fromTokens(search.getText().toLowerCase(), true);
    for (const auto id : source)
    {
        const auto* t = collection.find(id);
        if (t == nullptr) continue;
        if (!words.isEmpty())
        {
            const auto text = (t->displayTitle() + " " + t->artist + " " + t->album + " " + t->genre + " " + t->key + " " + t->label + " "
                               + t->comment + " " + t->tags.joinIntoString(" ") + " " + juce::String(juce::roundToInt(t->bpm)))
                                  .toLowerCase();
            bool all = true;
            for (const auto& w : words) all = all && text.contains(w);
            if (!all) continue;
        }
        rows.push_back(id);
    }
    if (sortColumn != 0 && sortColumn != number)
    {
        const auto keyOf = [this](juce::int64 id) -> juce::var
        {
            const auto* t = collection.find(id);
            switch (sortColumn)
            {
            case title: return t->displayTitle().toLowerCase();
            case artist: return t->artist.toLowerCase();
            case album: return t->album.toLowerCase();
            case genre: return t->genre.toLowerCase();
            case bpm: return t->bpm;
            case key: return t->key;
            case length: return t->durationSec;
            case rating: return t->rating;
            case tags: return t->tags.joinIntoString(", ").toLowerCase();
            case added: return t->dateAdded;
            case comment: return t->comment.toLowerCase();
            default: return {};
            }
        };
        std::stable_sort(rows.begin(), rows.end(),
                         [&](juce::int64 a, juce::int64 b)
                         {
                             const auto ka = keyOf(a), kb = keyOf(b);
                             const bool less = ka.isString() ? ka.toString() < kb.toString() : static_cast<double>(ka) < static_cast<double>(kb);
                             const bool more = ka.isString() ? kb.toString() < ka.toString() : static_cast<double>(kb) < static_cast<double>(ka);
                             return sortForwards ? less : more;
                         });
    }
    else if (sortColumn == number && !sortForwards)
    {
        std::reverse(rows.begin(), rows.end());
    }
    table.updateContent();
    table.repaint();
    updateStatus();
}

const Track* LibraryView::trackAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(rows.size())) return nullptr;
    return collection.find(rows[static_cast<size_t>(row)]);
}

std::vector<juce::int64> LibraryView::selectedTracks() const
{
    std::vector<juce::int64> ids;
    const auto selected = table.getSelectedRows();
    for (int i = 0; i < selected.size(); ++i)
        if (const auto* t = trackAt(selected[i])) ids.push_back(t->id);
    return ids;
}

void LibraryView::selectTrack(juce::int64 id)
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i] == id)
        {
            table.selectRow(static_cast<int>(i));
            return;
        }
}

int LibraryView::getNumRows()
{
    return static_cast<int>(rows.size());
}

void LibraryView::paintRowBackground(juce::Graphics& g, int row, int width, int height, bool selected)
{
    if (selected)
        g.fillAll(colours::sky600.withAlpha(0.35F));
    else if (row % 2 == 1)
        g.fillAll(colours::zinc800.withAlpha(0.4F));
    if (const auto* t = trackAt(row); t != nullptr && trackEditor.trackId() == t->id)
    {
        g.setColour(colours::accent.withAlpha(0.6F));
        g.drawRect(0, 0, width, height);
    }
}

void LibraryView::paintCell(juce::Graphics& g, int row, int column, int width, int height, bool)
{
    const auto* t = trackAt(row);
    if (t == nullptr) return;
    const bool missing = !juce::File(t->path).existsAsFile();
    g.setColour(missing ? colours::red500 : column == title ? colours::text : colours::textDim);
    g.setFont(uiFont(13.0F));
    juce::String text;
    switch (column)
    {
    case colour:
        g.setColour(trackColour(t->colour));
        g.fillRect(2, 3, width - 4, height - 6);
        return;
    case number: text = juce::String(row + 1); break;
    case title: text = t->displayTitle(); break;
    case artist: text = t->artist; break;
    case album: text = t->album; break;
    case genre: text = t->genre; break;
    case bpm: text = t->bpm > 0.0 ? juce::String(t->bpm, 2) : juce::String(); break;
    case key: text = t->key; break;
    case length: text = formatTime(t->durationSec); break;
    case rating: text = juce::String::repeatedString("*", t->rating); break;
    case tags: text = t->tags.joinIntoString(", "); break;
    case added: text = t->dateAdded; break;
    case comment: text = t->comment; break;
    case state:
        if (missing)
            text = "!";
        else if (t->state == Track::State::failed)
            text = "x";
        else if (t->state != Track::State::ready)
            text = "~";
        break;
    default: break;
    }
    g.drawText(text, 4, 0, width - 6, height, column == bpm || column == length ? juce::Justification::centredRight : juce::Justification::centredLeft,
               true);
}

void LibraryView::selectedRowsChanged(int lastRowSelected)
{
    if (const auto* t = trackAt(lastRowSelected)) trackEditor.showTrack(t->id);
    table.repaint();
}

void LibraryView::cellClicked(int row, int, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) showTrackMenu(row);
}

void LibraryView::cellDoubleClicked(int row, int, const juce::MouseEvent&)
{
    returnKeyPressed(row);
}

void LibraryView::returnKeyPressed(int row)
{
    if (const auto* t = trackAt(row))
    {
        trackEditor.showTrack(t->id);
        trackEditor.togglePlay();
    }
}

void LibraryView::deleteKeyPressed(int)
{
    removeSelected();
}

void LibraryView::sortOrderChanged(int newSortColumnId, bool isForwards)
{
    sortColumn = newSortColumnId;
    sortForwards = isForwards;
    rebuildRows();
}

juce::var LibraryView::getDragSourceDescription(const juce::SparseSet<int>& selected)
{
    juce::StringArray ids;
    for (int i = 0; i < selected.size(); ++i)
        if (const auto* t = trackAt(selected[i])) ids.add(juce::String(t->id));
    return ids.isEmpty() ? juce::var() : juce::var(kDragPrefix + ids.joinIntoString(","));
}

bool LibraryView::isInterestedInDragSource(const SourceDetails& details)
{
    // Rows dragged within a playlist on show (in its own order) move.
    const auto* p = collection.findPlaylist(listId);
    return p != nullptr && !p->isFolder() && (sortColumn == 0 || sortColumn == number) && !idsFromDrag(details.description).empty();
}

void LibraryView::itemDropped(const SourceDetails& details)
{
    const auto* p = collection.findPlaylist(listId);
    if (p == nullptr) return;
    const auto point = table.getLocalPoint(this, details.localPosition);
    int insertAt = table.getInsertionIndexForPosition(point.x, point.y);
    if (insertAt < 0) insertAt = static_cast<int>(rows.size());
    const auto moving = idsFromDrag(details.description);
    std::vector<int> positions;
    for (int i = 0; i < static_cast<int>(p->tracks.size()); ++i)
        if (std::find(moving.begin(), moving.end(), p->tracks[static_cast<size_t>(i)]) != moving.end()) positions.push_back(i);
    if (positions.empty())
        collection.addToPlaylist(listId, moving, insertAt);
    else
        collection.reorderPlaylist(listId, positions, insertAt);
}

bool LibraryView::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (juce::File(f).isDirectory() || AudioImporter::isAudioFile(juce::File(f))) return true;
    return false;
}

void LibraryView::filesDropped(const juce::StringArray& files, int, int)
{
    const auto* p = collection.findPlaylist(listId);
    const juce::int64 target = p != nullptr && !p->isFolder() && p->kind == Playlist::Kind::playlist ? listId : 0;
    juce::Array<juce::File> audio;
    for (const auto& f : files)
    {
        const juce::File file(f);
        if (file.isDirectory())
            collection.addFolder(file, target);
        else
            audio.add(file);
    }
    if (!audio.isEmpty()) collection.addFiles(audio, target);
}

bool LibraryView::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey || key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
        return trackEditor.keyPressed(key);
    if (key == juce::KeyPress('f', juce::ModifierKeys::commandModifier, 0))
    {
        search.grabKeyboardFocus();
        return true;
    }
    if (key == juce::KeyPress::escapeKey && ctx.close)
    {
        ctx.close();
        return true;
    }
    return false;
}

// ── Menus ───────────────────────────────────────────────────────────────────

void LibraryView::askForName(const juce::String& heading, const juce::String& initial, std::function<void(const juce::String&)> then)
{
    auto* window = new juce::AlertWindow(heading, {}, juce::MessageBoxIconType::NoIcon);
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

juce::int64 LibraryView::newPlaylist(const juce::String& name, juce::int64 parent, bool folder)
{
    return collection.createPlaylist(name, parent, folder ? Playlist::Kind::folder : Playlist::Kind::playlist);
}

void LibraryView::addPlaylistTargets(juce::PopupMenu& menu, juce::int64 parent, const std::vector<juce::int64>& ids)
{
    for (const auto* p : collection.childrenOf(parent, false))
    {
        if (p->isFolder())
        {
            juce::PopupMenu sub;
            addPlaylistTargets(sub, p->id, ids);
            menu.addSubMenu(p->name, sub);
        }
        else
        {
            menu.addItem(p->name, [this, id = p->id, ids] { collection.addToPlaylist(id, ids); });
        }
    }
    menu.addItem("New Playlist...", [this, parent, ids]
                 {
                     askForName("New playlist", "Untitled playlist",
                                [this, parent, ids](const juce::String& name)
                                {
                                    const auto id = newPlaylist(name, parent, false);
                                    collection.addToPlaylist(id, ids);
                                });
                 });
}

void LibraryView::showTrackMenu(int row)
{
    if (!table.isRowSelected(row)) table.selectRow(row);
    const auto ids = selectedTracks();
    if (ids.empty()) return;
    juce::PopupMenu menu;
    juce::PopupMenu to;
    addPlaylistTargets(to, 0, ids);
    menu.addSubMenu("Add to Playlist", to);
    menu.addItem("Add to Project", [this, ids] { addToProject(ids); });
    menu.addSeparator();
    juce::PopupMenu colourMenu;
    for (int i = 0; i <= 8; ++i)
        colourMenu.addItem(trackColourName(i), [this, ids, i]
                           {
                               for (const auto id : ids) collection.update(id, [i](Track& t) { t.colour = i; });
                           });
    menu.addSubMenu("Colour", colourMenu);
    juce::PopupMenu ratingMenu;
    for (int i = 0; i <= 5; ++i)
        ratingMenu.addItem(i == 0 ? juce::String("None") : juce::String::repeatedString("*", i), [this, ids, i]
                           {
                               for (const auto id : ids) collection.update(id, [i](Track& t) { t.rating = i; });
                           });
    menu.addSubMenu("Rating", ratingMenu);
    menu.addItem("My Tags...", [this, ids] { editTags(ids); });
    menu.addItem("Analyse Again", [this, ids] { collection.reanalyse(ids); });
    if (ids.size() == 1)
    {
        const auto* t = collection.find(ids.front());
        menu.addItem("Edit Title and Artist...", [this, id = t->id, current = t->displayTitle() + " | " + t->artist]
                     {
                         askForName("Title | Artist", current,
                                    [this, id](const juce::String& text)
                                    {
                                        collection.update(id,
                                                          [&text](Track& t)
                                                          {
                                                              t.title = text.upToFirstOccurrenceOf("|", false, false).trim();
                                                              if (text.contains("|")) t.artist = text.fromFirstOccurrenceOf("|", false, false).trim();
                                                          });
                                    });
                     });
        menu.addItem("Relocate...", [this, id = t->id]
                     {
                         chooser = std::make_unique<juce::FileChooser>("Where is this track now?", juce::File(collection.find(id)->path).getParentDirectory(),
                                                                       AudioImporter::audioFileWildcard());
                         chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                              [this, id](const juce::FileChooser& fc)
                                              {
                                                  if (fc.getResult().existsAsFile()) collection.relocate(id, fc.getResult());
                                              });
                     });
        menu.addItem("Show in Folder", [path = t->path] { juce::File(path).revealToUser(); });
    }
    menu.addSeparator();
    const auto* p = collection.findPlaylist(listId);
    if (p != nullptr && !p->isFolder())
        menu.addItem("Remove from Playlist", [this] { removeSelected(); });
    else if (listId == 0)
        menu.addItem("Remove from Collection...", [this] { removeSelected(); });
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void LibraryView::showListMenu(const Playlist* playlist, bool histories)
{
    juce::PopupMenu menu;
    const juce::int64 parent = playlist != nullptr && playlist->isFolder() ? playlist->id : 0;
    if (!histories)
    {
        menu.addItem("New Playlist...", [this, parent]
                     {
                         askForName("New playlist", "Untitled playlist", [this, parent](const juce::String& name) { showList(newPlaylist(name, parent, false)); });
                     });
        menu.addItem("New Folder...", [this, parent]
                     { askForName("New folder", "Untitled folder", [this, parent](const juce::String& name) { newPlaylist(name, parent, true); }); });
    }
    if (playlist != nullptr)
    {
        menu.addSeparator();
        menu.addItem("Rename...", [this, id = playlist->id, name = playlist->name]
                     { askForName("Rename", name, [this, id](const juce::String& n) { collection.renamePlaylist(id, n); }); });
        if (playlist->kind != Playlist::Kind::history)
            menu.addItem("Export to USB...", [this, id = playlist->id] { UsbExport::show(ctx.rekordbox, {id}, ctx.notify); });
        if (playlist->kind == Playlist::Kind::history)
            menu.addItem("Copy to a Playlist", [this, history = playlist->id, name = playlist->name]
                         {
                             const auto id = newPlaylist(name, 0, false);
                             collection.addToPlaylist(id, collection.findPlaylist(history)->tracks);
                         });
        menu.addItem("Delete...", [this, id = playlist->id, name = playlist->name, folder = playlist->isFolder()]
                     {
                         juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Delete " + name + "?",
                                                            folder ? juce::String("The folder and the playlists in it go. The tracks stay in the collection.")
                                                                   : juce::String("The playlist goes. Its tracks stay in the collection."),
                                                            "Delete", "Cancel", nullptr,
                                                            juce::ModalCallbackFunction::create(
                                                                [this, id](int result)
                                                                {
                                                                    if (result == 1)
                                                                    {
                                                                        if (listId == id) showList(0);
                                                                        collection.deletePlaylist(id);
                                                                    }
                                                                }));
                     });
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void LibraryView::editTags(const std::vector<juce::int64>& ids)
{
    // The tags all the tracks share, to edit; the change applies to each.
    juce::StringArray shared;
    if (const auto* first = collection.find(ids.front())) shared = first->tags;
    for (const auto id : ids)
        if (const auto* t = collection.find(id))
            for (int i = shared.size(); --i >= 0;)
                if (!t->tags.contains(shared[i])) shared.remove(i);
    askForName("My Tags (separated by commas)", shared.joinIntoString(", "),
               [this, ids, shared](const juce::String& text)
               {
                   auto next = juce::StringArray::fromTokens(text, ",", "");
                   next.trim();
                   next.removeEmptyStrings();
                   next.removeDuplicates(true);
                   for (const auto id : ids)
                       collection.update(id,
                                         [&](Track& t)
                                         {
                                             for (const auto& gone : shared)
                                                 if (!next.contains(gone)) t.tags.removeString(gone);
                                             for (const auto& tag : next) t.tags.addIfNotAlreadyThere(tag);
                                         });
               });
}

void LibraryView::addToProject(const std::vector<juce::int64>& ids)
{
    std::vector<ImportRequest> requests;
    for (const auto id : ids)
        if (const auto* t = collection.find(id))
        {
            ImportRequest r;
            r.file = juce::File(t->path);
            r.hints.title = t->title;
            r.hints.artist = t->artist;
            r.hints.key = t->key;
            r.hints.bpm = t->bpm;
            if (t->bpm > 0.0)
            {
                const auto beats = t->beatTimes();
                // The first downbeat, as the item's grid anchor.
                double anchor = beats.empty() ? t->anchorSec : beats.front();
                for (size_t i = 0; i < beats.size() && i < 4; ++i)
                    if (t->beatNumberAt(static_cast<int>(i)) == 1)
                    {
                        anchor = beats[i];
                        break;
                    }
                r.hints.beatAnchorSec = anchor;
            }
            r.hints.perfCues = toPerfCues(*t);
            requests.push_back(r);
        }
    if (requests.empty()) return;
    ctx.importer.import(std::move(requests));
    if (ctx.notify) ctx.notify("Adding " + juce::String(static_cast<int>(ids.size())) + " track(s) to the project library", false);
}

void LibraryView::removeSelected()
{
    const auto ids = selectedTracks();
    if (ids.empty()) return;
    const auto* p = collection.findPlaylist(listId);
    if (p != nullptr && !p->isFolder())
    {
        std::vector<int> positions;
        const auto selected = table.getSelectedRows();
        // Positions in the playlist (the rows may be sorted or searched).
        std::set<juce::int64> gone(ids.begin(), ids.end());
        for (int i = 0; i < static_cast<int>(p->tracks.size()); ++i)
            if (gone.count(p->tracks[static_cast<size_t>(i)]) > 0) positions.push_back(i);
        collection.removeFromPlaylist(listId, positions);
        return;
    }
    if (listId != 0) return;
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
                                       "Remove " + juce::String(static_cast<int>(ids.size())) + " track(s) from the collection?",
                                       "They come out of every playlist too. The audio files are not deleted.", "Remove", "Cancel", nullptr,
                                       juce::ModalCallbackFunction::create(
                                           [this, ids](int result)
                                           {
                                               if (result != 1) return;
                                               if (std::find(ids.begin(), ids.end(), trackEditor.trackId()) != ids.end()) trackEditor.showTrack(0);
                                               collection.removeTracks(ids);
                                           }));
}

// ── Toolbar actions ─────────────────────────────────────────────────────────

void LibraryView::addFiles()
{
    chooser = std::make_unique<juce::FileChooser>("Add audio files to the collection", juce::File(), AudioImporter::audioFileWildcard());
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto* p = collection.findPlaylist(listId);
                             const juce::int64 target = p != nullptr && p->kind == Playlist::Kind::playlist ? listId : 0;
                             if (!fc.getResults().isEmpty()) collection.addFiles(fc.getResults(), target);
                         });
}

void LibraryView::addFolder()
{
    chooser = std::make_unique<juce::FileChooser>("Add a folder of music to the collection");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this](const juce::FileChooser& fc)
                         {
                             if (!fc.getResult().isDirectory()) return;
                             collection.addFolder(fc.getResult(), 0,
                                                  [notify = ctx.notify](int added)
                                                  {
                                                      if (notify) notify("Added " + juce::String(added) + " track(s) to the collection", false);
                                                  });
                         });
}

void LibraryView::importFromRekordbox()
{
    if (importing) return;
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon, "Bring in your rekordbox library?",
        "Every track in this computer's rekordbox collection comes into Stemcel's, with its beat grid, key, hot and memory cues, rating, "
        "colour and comment, and every playlist goes in a \"rekordbox\" folder. Tracks already in the collection keep their own grid and "
        "cues. rekordbox's library is only read, never changed.",
        "Bring In", "Cancel", nullptr,
        juce::ModalCallbackFunction::create(
            [this](int result)
            {
                if (result != 1) return;
                importing = true;
                updateStatus();
                collection.importRekordbox(
                    ctx.rekordbox,
                    [this, alive = alive](int done, int total)
                    {
                        if (!alive->load()) return;
                        status.setText("Reading rekordbox: " + juce::String(done) + " of " + juce::String(total), juce::dontSendNotification);
                    },
                    [this, alive = alive](const juce::String& summary, bool ok)
                    {
                        if (!alive->load()) return;
                        importing = false;
                        updateStatus();
                        if (ctx.notify) ctx.notify(summary, !ok);
                    });
            }));
}

void LibraryView::exportToStick()
{
    std::vector<juce::int64> chosen;
    if (const auto* p = collection.findPlaylist(listId); p != nullptr && p->kind != Playlist::Kind::history) chosen.push_back(listId);
    UsbExport::show(ctx.rekordbox, chosen, ctx.notify);
}

void LibraryView::readHistories()
{
    const auto result = ctx.rekordbox.volumes();
    juce::Array<juce::var> sticks;
    if (const auto* list = result.getProperty("volumes", {}).getArray())
        for (const auto& v : *list)
            if (static_cast<bool>(v.getProperty("hasExport", false))) sticks.add(v);
    if (sticks.isEmpty())
    {
        if (ctx.notify) ctx.notify("No USB stick with a rekordbox export is plugged in", true);
        return;
    }
    const auto readFrom = [this](const juce::var& stick)
    {
        const juce::File root(stick.getProperty("root", "").toString());
        auto name = stick.getProperty("name", "").toString();
        if (name.isEmpty()) name = root.getFullPathName();
        const auto read = ctx.rekordbox.stickHistories(root);
        if (!static_cast<bool>(read.getProperty("ok", false)))
        {
            if (ctx.notify) ctx.notify("Couldn't read the histories on " + name + ": " + read.getProperty("error", "").toString(), true);
            return;
        }
        int lists = 0, unknown = 0;
        if (const auto* histories = read.getProperty("histories", {}).getArray())
            for (const auto& h : *histories)
            {
                std::vector<juce::int64> ids;
                if (const auto* tracks = h.getProperty("tracks", {}).getArray())
                    for (const auto& t : *tracks)
                    {
                        // The track it was exported from, else the same title and artist.
                        const auto* found = collection.find(static_cast<juce::int64>(t.getProperty("collectionId", 0)));
                        if (found == nullptr)
                        {
                            const auto title = t.getProperty("title", "").toString();
                            const auto artist = t.getProperty("artist", "").toString();
                            for (const auto& c : collection.tracks())
                                if (c.displayTitle().equalsIgnoreCase(title) && c.artist.equalsIgnoreCase(artist))
                                {
                                    found = &c;
                                    break;
                                }
                        }
                        if (found != nullptr)
                            ids.push_back(found->id);
                        else
                            ++unknown;
                    }
                const auto historyName = h.getProperty("name", "History").toString();
                collection.addHistory(historyName + " (" + name + ")", juce::Time::getCurrentTime().formatted("%Y-%m-%d"), name, ids);
                ++lists;
            }
        if (ctx.notify)
            ctx.notify(lists == 0 ? "No play histories on " + name
                                  : "Read " + juce::String(lists) + " play histor" + (lists == 1 ? "y" : "ies") + " from " + name
                                        + (unknown > 0 ? " (" + juce::String(unknown) + " track(s) not in the collection left out)" : juce::String()),
                       false);
    };
    if (sticks.size() == 1)
    {
        readFrom(sticks[0]);
        return;
    }
    juce::PopupMenu menu;
    for (const auto& stick : sticks)
        menu.addItem(stick.getProperty("name", "").toString() + " (" + stick.getProperty("root", "").toString() + ")",
                     [readFrom, stick] { readFrom(stick); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(historyButton));
}

// ── Layout ──────────────────────────────────────────────────────────────────

void LibraryView::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    g.setColour(colours::border);
    g.drawHorizontalLine(40, 0.0F, static_cast<float>(getWidth()));
}

void LibraryView::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop(40).reduced(8, 6);
    closeButton.setBounds(bar.removeFromLeft(130));
    bar.removeFromLeft(6);
    performButton.setBounds(bar.removeFromLeft(130));
    bar.removeFromLeft(10);
    search.setBounds(bar.removeFromLeft(std::min(320, bar.getWidth() / 3)));
    bar.removeFromLeft(10);
    for (auto* b : {&addFilesButton, &addFolderButton, &rekordboxButton, &exportButton, &historyButton, &linkButton})
    {
        b->setBounds(bar.removeFromLeft(b->getBestWidthForHeight(bar.getHeight()) + 14));
        bar.removeFromLeft(6);
    }
    status.setBounds(bar);
    r.removeFromTop(1);
    trackEditor.setBounds(r.removeFromBottom(std::min(260, r.getHeight() / 2)));
    tree.setBounds(r.removeFromLeft(220));
    table.setBounds(r);
}

} // namespace stemcel::native::dj
