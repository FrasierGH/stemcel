#pragma once

#include "LibraryPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace stemcel::native
{

// The rekordbox tab: the installed rekordbox library read live (read-only) through the
// Rust reader — playlist tree on the left, tracks on the right. Importing a track brings
// rekordbox's beat grid (BPM + first downbeat) and key with it. Rows drag onto the
// timeline, double-click imports, Space auditions.
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

    void load();
    void showPlaylist(const juce::String& playlistId); // empty = all tracks

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
    const RekordboxTrack* trackAt(int row) const;

    LibraryContext& ctx;
    std::shared_ptr<const RekordboxSnapshot> snapshot;
    juce::TreeView tree;
    std::unique_ptr<juce::TreeViewItem> treeRoot;
    juce::TableListBox table;
    juce::TextEditor filter;
    juce::TextButton reloadButton{"Reload"}, importButton{"Import"};
    juce::Label message;
    juce::String playlistId;
    std::vector<const RekordboxTrack*> rows;
    int sortColumn = 0;
    bool sortForwards = true;
    bool loading = false;
    juce::ThreadPool pool{1};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace stemcel::native
