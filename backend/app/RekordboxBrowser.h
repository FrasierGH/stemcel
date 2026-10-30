#pragma once

#include "LibraryPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace stemcel::native
{

// The rekordbox tab: a rekordbox library read live through the Rust reader — this
// computer's, one kept on a drive, or the Device Library or OneLibrary of a connected
// USB/SD export, picked at the top — with its playlist tree on the left and tracks on the
// right. Importing a track brings rekordbox's beat grid (BPM + first downbeat) and key with
// it. Rows drag onto the timeline, double-click imports, Space auditions.
//
// Playlists can be made, renamed and deleted, and tracks added to and taken out of them,
// from the right-click menus; this computer's library also takes new audio files. Every
// change waits for rekordbox to quit, and the first change to a library in a session
// copies its database files to ~/Stemcel/Backups/rekordbox first (Rekordbox::write).
class RekordboxBrowser final : public juce::Component,
                               private juce::TableListBoxModel,
                               private juce::ChangeListener,
                               private juce::Timer
{
  public:
    explicit RekordboxBrowser(LibraryContext& context);
    ~RekordboxBrowser() override;

    void resized() override;
    void paint(juce::Graphics&) override;
    void visibilityChanged() override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseUp(const juce::MouseEvent&) override;

    void load();
    void showPlaylist(const juce::String& playlistId); // empty = all tracks
    // Lists this computer's library and every connected drive with a rekordbox export.
    void refreshSources();
    void chooseSource(const RekordboxSource& source);
    // Smoke-test support: applies a write as the menus do and calls `done` with its result
    // once the library has been read again.
    void writeForTest(const juce::var& request, std::function<void(const juce::var&)> done);
    const RekordboxSnapshot* currentSnapshot() const { return snapshot.get(); }

  private:
    class PlaylistItem;

    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override;
    void cellDoubleClicked(int row, int column, const juce::MouseEvent&) override;
    void cellClicked(int row, int column, const juce::MouseEvent&) override;
    void returnKeyPressed(int lastRowSelected) override;
    void sortOrderChanged(int newSortColumnId, bool isForwards) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void applySnapshot(std::shared_ptr<const RekordboxSnapshot> snapshot);
    void rebuildRows();
    std::vector<ImportRequest> selectedRequests() const;
    juce::StringArray selectedTrackIds() const;
    const RekordboxTrack* trackAt(int row) const;

    // Writes: run off the message thread; the library is read again when they land.
    void runWrite(const juce::var& request, const juce::String& done, std::function<void(const juce::var&)> then = {});
    void showPlaylistMenu(const RekordboxPlaylist* playlist);
    void showTrackMenu(int row);
    void addPlaylistTargets(juce::PopupMenu& menu, const juce::String& parentId);
    void askForName(const juce::String& title, const juce::String& initial, std::function<void(const juce::String&)> then);
    void createPlaylist(const juce::String& parentId, bool folder);
    void renamePlaylist(const RekordboxPlaylist& playlist);
    void deletePlaylist(const RekordboxPlaylist& playlist);
    void addFilesToPlaylist(const juce::String& playlistId);

    LibraryContext& ctx;
    std::shared_ptr<const RekordboxSnapshot> snapshot;
    std::vector<RekordboxSource> sources;
    juce::ComboBox sourceBox;
    juce::TreeView tree;
    std::unique_ptr<juce::TreeViewItem> treeRoot;
    juce::TableListBox table;
    juce::TextEditor filter;
    juce::TextButton reloadButton{"Reload"}, importButton{"Import"};
    juce::Label message;
    juce::String playlistId;
    std::vector<const RekordboxTrack*> rows;
    std::function<void(const juce::var&)> afterReload;
    juce::var lastWrite;
    std::unique_ptr<juce::FileChooser> chooser;
    int sortColumn = 0;
    bool sortForwards = true;
    bool loading = false;
    juce::ThreadPool pool{1};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace stemcel::native
