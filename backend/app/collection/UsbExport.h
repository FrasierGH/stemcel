#pragma once

// Export to USB: playlists of Stemcel's collection written to a stick as a rekordbox
// export a CDJ or XDJ reads (both libraries, the audio, the analysis, the cues), with no
// rekordbox involved. Sync makes the stick hold exactly what is chosen, as rekordbox's
// SYNC does; Add puts the choice beside what the stick already has.

#include "Collection.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>

namespace stemcel::native
{
class Rekordbox;
}

namespace stemcel::native::dj
{

class UsbExport final : public juce::Component, private juce::ListBoxModel
{
  public:
    UsbExport(Rekordbox& rekordbox, std::vector<juce::int64> preselected, std::function<void(const juce::String&, bool)> notify);
    ~UsbExport() override;

    static void show(Rekordbox& rekordbox, std::vector<juce::int64> preselected, std::function<void(const juce::String&, bool)> notify);
    // Runs one export without the window (the smoke test, to a folder standing in for a stick).
    static juce::var runForTest(Rekordbox& rekordbox, const std::vector<juce::int64>& lists, const juce::File& stick, bool sync);

    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;

    void updateSummary();
    void refreshDrives();
    void start();
    void finished(const juce::var& report);
    void close();

    Rekordbox& rekordbox;
    std::function<void(const juce::String&, bool)> notify;
    struct Entry
    {
        juce::int64 id = 0;
        juce::String name;
        int depth = 0;
        bool folder = false;
        bool chosen = false;
        int tracks = 0;
    };
    std::vector<Entry> entries;
    juce::Array<juce::var> drives;
    juce::ComboBox driveBox, modeBox;
    juce::TextButton refreshButton{"Refresh"}, exportButton{"Export"}, cancelButton{"Close"};
    juce::ToggleButton ejectToggle{"Eject when done"};
    juce::ListBox list;
    juce::Label summary, progressText;
    double progress = 0.0;
    juce::ProgressBar progressBar{progress};
    bool running = false;
    std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    juce::ThreadPool pool{1};
};

} // namespace stemcel::native::dj
