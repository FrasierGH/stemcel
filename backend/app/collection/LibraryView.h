#pragma once

// Library mode: Stemcel's DJ collection on its own screen, in place of the timeline, as
// rekordbox's Export mode is. Playlists and folders on the left (and play histories read
// back from sticks), the tracks on the right, and the track editor beneath: the beat grid,
// cues, rating and colour. From here tracks come in (files, folders, this computer's
// rekordbox library), go onto USB sticks for CDJs, and into the project.

#include "Collection.h"
#include "EngineClient.h"
#include "TrackEditor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace stemcel::native
{
class WaveformStore;
class Rekordbox;
class AudioImporter;
} // namespace stemcel::native

namespace stemcel::native::dj
{

class LibraryView final : public juce::Component,
                          public juce::FileDragAndDropTarget,
                          public juce::DragAndDropContainer,
                          public juce::DragAndDropTarget,
                          private juce::TableListBoxModel,
                          private juce::ChangeListener,
                          private juce::Timer
{
  public:
    struct Context
    {
        EngineClient& engine;
        WaveformStore& waveforms;
        AudioImporter& importer;
        Rekordbox& rekordbox;
        std::function<void(const juce::String& message, bool isError)> notify;
        std::function<void()> close;
        std::function<void()> performance; // into performance mode
    };

    explicit LibraryView(Context context);
    ~LibraryView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

    // What the table shows: the whole collection (0), a playlist, a folder or a history.
    void showList(juce::int64 playlist);
    juce::int64 shownList() const { return listId; }
    std::vector<juce::int64> selectedTracks() const;
    void selectTrack(juce::int64 id);
    TrackEditor& editor() { return trackEditor; }
    int rowCount() const { return static_cast<int>(rows.size()); }
    // A new playlist named `name` at the top, for the smoke test and the menus.
    juce::int64 newPlaylist(const juce::String& name, juce::int64 parent, bool folder);
    void exportToStick();
    void readHistories();
    void importFromRekordbox();
    // Into the project library, with the collection's grid, key and cues.
    void addToProject(const std::vector<juce::int64>& ids);

  private:
    class ListItem;

    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void cellDoubleClicked(int row, int column, const juce::MouseEvent&) override;
    void cellClicked(int row, int column, const juce::MouseEvent&) override;
    void deleteKeyPressed(int lastRowSelected) override;
    void returnKeyPressed(int lastRowSelected) override;
    void sortOrderChanged(int newSortColumnId, bool isForwards) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void rebuildTree();
    void rebuildRows();
    const Track* trackAt(int row) const;
    void showTrackMenu(int row);
    void showListMenu(const Playlist* playlist, bool histories);
    void addPlaylistTargets(juce::PopupMenu& menu, juce::int64 parent, const std::vector<juce::int64>& ids);
    void askForName(const juce::String& title, const juce::String& initial, std::function<void(const juce::String&)> then);
    void addFiles();
    void addFolder();
    void editTags(const std::vector<juce::int64>& ids);
    void removeSelected();
    void updateStatus();

    Context ctx;
    Collection& collection;
    juce::TreeView tree;
    std::unique_ptr<juce::TreeViewItem> treeRoot;
    juce::TableListBox table;
    TrackEditor trackEditor;
    juce::TextEditor search;
    juce::TextButton addFilesButton{"Add Files..."}, addFolderButton{"Add Folder..."}, rekordboxButton{"From rekordbox..."},
        exportButton{"Export to USB..."}, historyButton{"Histories from USB"}, closeButton{"Back to Timeline"}, linkButton{"LINK"},
        performButton{"Performance Mode"};
    void toggleLink();
    void startLink(const juce::String& interfaceName);
    void showLinkStatus();
    bool linkStarting = false;
    juce::String linkText;
    int ticks = 0;
    juce::Label status;
    juce::int64 listId = 0;
    std::vector<juce::int64> rows;
    int sortColumn = 0;
    bool sortForwards = true;
    bool importing = false;
    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace stemcel::native::dj
