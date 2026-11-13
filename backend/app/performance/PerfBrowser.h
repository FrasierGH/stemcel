#pragma once

// The performance view's browser, as rekordbox's on the DDJ-400: a tree (the library, its
// tracks, stems and samples, and the rekordbox playlists) beside a track list. The controller's
// BROWSE knob moves the cursor; pressing it moves between the tree and the list (or opens a
// folder), SHIFT + press closes it; LOAD loads the track under the cursor.

#include "ProjectModel.h"
#include "Rekordbox.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <set>
#include <vector>

namespace stemcel::native::perf
{

struct BrowserRow
{
    juce::String itemId;      // a library item, or
    juce::String rekordboxId; // a rekordbox track (imported on load), or
    juce::int64 collectionId = 0; // a DJ collection track (the same)
    juce::String title, artist, key, path;
    double bpm = 0.0, durationSec = 0.0;
    // A stretch of the timeline (rendered onto the deck when loaded): one track, or "" the mix.
    bool isRegion = false;
    juce::String regionTrackId;
    double regionStartMs = 0.0, regionEndMs = 0.0;
};

class PerfBrowser final : public juce::Component
{
  public:
    PerfBrowser(ProjectMirror& mirror, std::function<const RekordboxSnapshot*()> rekordbox);

    void refresh();
    void browse(int steps);      // the knob
    void press(bool shifted);    // the knob's press
    const BrowserRow* selectedRow() const;
    std::function<void(const BrowserRow&, int deck)> onLoad; // a row's Load 1 / Load 2 (mouse)

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

  private:
    struct Node
    {
        juce::String label, kind, id; // kind: all, tracks, stems, samples, rekordbox, rbAll, rbList
        int depth = 0;
        bool folder = false;
    };
    void buildTree();
    void buildRows();
    void ensureVisible();
    int rowHeight() const { return 22; }
    juce::Rectangle<int> treeArea() const;
    juce::Rectangle<int> listArea() const;

    ProjectMirror& mirror;
    std::function<const RekordboxSnapshot*()> rekordbox;
    std::vector<Node> nodes;
    std::set<juce::String> expanded{"collection", "rekordbox", "djlib"};
    std::vector<BrowserRow> rows;
    int treeCursor = 1, listCursor = 0;
    int treeScroll = 0, listScroll = 0;
    bool listFocused = false;
    juce::String lastLibraryKey;
};

} // namespace stemcel::native::perf
