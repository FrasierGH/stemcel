#pragma once

#include "LibraryPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>

namespace stemcel::native
{

// The Files tab: folders the user added (shared with the Electron app as
// `ui.fileBrowserFolders`), a folder tree, and the audio files of the selected folder
// with their tags. Filtering searches the whole selected folder tree by name. Rows drag
// onto the timeline, double-click imports, Space auditions.
class FilesBrowser final : public juce::Component,
                           private juce::TableListBoxModel,
                           private juce::ChangeListener,
                           private juce::Timer
{
  public:
    explicit FilesBrowser(LibraryContext& context);
    ~FilesBrowser() override;

    void resized() override;
    void paint(juce::Graphics&) override;
    bool keyPressed(const juce::KeyPress&) override;

    void showFolder(const juce::File& folder);

  private:
    struct Row
    {
        juce::File file;
        juce::String title, artist, key;
        double bpm = 0.0;
        double durationMs = 0.0;
        bool tagsLoaded = false;
    };
    class FolderItem;

    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override;
    void cellDoubleClicked(int row, int column, const juce::MouseEvent&) override;
    void cellClicked(int row, int column, const juce::MouseEvent&) override;
    void returnKeyPressed(int lastRowSelected) override;
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { table.repaint(); }

    void rebuildTree();
    void addFolder();
    void removeSelectedFolder();
    void refreshRows();
    void loadTagsFor(size_t from);
    std::vector<ImportRequest> selectedRequests() const;
    juce::StringArray roots() const;

    LibraryContext& ctx;
    juce::TreeView tree;
    std::unique_ptr<juce::TreeViewItem> treeRoot;
    juce::TableListBox table;
    juce::TextEditor filter;
    juce::TextButton addButton{"Add folder..."}, removeButton{"Remove"};
    juce::Label hint;
    juce::File folder;
    std::vector<Row> rows;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::ThreadPool tagPool{1};
    std::shared_ptr<std::atomic<int>> generation = std::make_shared<std::atomic<int>>(0);
};

} // namespace stemcel::native
