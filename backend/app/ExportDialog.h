#pragma once

#include "AppPreferences.h"
#include "EngineClient.h"
#include "ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace stemcel::native
{

// Export Mixdown, as the renderer's ExportMixdownDialog / useExportMixdownForm: output path,
// format, rate, depth or bitrate, dither, start bar, tail, length, loudness preset and tags,
// then MIXDOWN_START with a progress view and cancel. The settings persist with the project
// (PROJECT_SET_EXPORT_SETTINGS, the renderer's versioned JSON) and the start bar as its own
// project property, so either app reopens the dialog the way it was left.
class ExportDialog final : public juce::Component, private juce::Timer
{
  public:
    ExportDialog(EngineClient& engine, AppPreferences& prefs, const ProjectModel& project,
                 std::function<void(const juce::String&, bool)> notify);
    ~ExportDialog() override;

    static void show(EngineClient& engine, AppPreferences& prefs, const ProjectModel& project,
                     std::function<void(const juce::String&, bool)> notify);

    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    void timerCallback() override;
    void applyPersisted(const juce::String& json);
    juce::var snapshot() const;
    void browse();
    void submit();
    void start(const juce::File& output);
    void finished(bool ok, const juce::String& message, const juce::File& file);
    void updateEnablement();
    void close();
    juce::String format() const;
    int bitDepth() const;
    double startBarMs() const;
    std::optional<double> durationMs() const;
    std::optional<double> tailSeconds() const;
    double leadInMs() const;
    void refreshTracklist();

    EngineClient& engine;
    AppPreferences& prefs;
    juce::String projectName;
    juce::String projectFile;
    double bpm = 120.0;
    int barCounterStart = 1;
    int restoredDepth = 16; // from the project's saved settings
    double lastClipEndMs = 0.0;
    std::optional<std::pair<double, double>> selection;
    ProjectModel model; // the project as the dialog opened, for the tracklist
    std::function<void(const juce::String&, bool)> notify;
    Subscriptions subscriptions;

    juce::Label outputLabel{{}, "Output"}, formatLabel{{}, "Format"}, rateLabel{{}, "Sample rate"},
        depthLabel{{}, "Bit depth"}, bitrateLabel{{}, "MP3 bitrate"}, startLabel{{}, "Start from bar"},
        startHint, tailLabel{{}, "Tail (s)"}, lengthLabel{{}, "Length"}, loudnessLabel{{}, "Loudness"},
        targetLabel{{}, "Target LUFS"}, ceilingLabel{{}, "Ceiling dBTP"}, titleLabel{{}, "Title"},
        artistLabel{{}, "Artist"}, albumLabel{{}, "Album"}, yearLabel{{}, "Year"}, genreLabel{{}, "Genre"},
        commentLabel{{}, "Comment"}, leadInLabel{{}, "Lead-in"}, tracklistLabel{{}, "Tracklist"};
    juce::TextEditor output, startBar, tail, duration, target, ceiling, title, artist, album, year, genre, comment;
    juce::TextButton browseButton{"Browse..."};
    juce::ComboBox formatBox, rate, depth, bitrate, length, loudness, leadIn;
    juce::ToggleButton dither{"Dither (TPDF)"};
    // The tracklist written beside the file: generated, and editable before the export.
    juce::ToggleButton writeTracklist{"Write tracklist (.txt) beside the file"};
    juce::TextEditor tracklistText;
    bool tracklistEdited = false;
    juce::TextButton exportButton{"Export"}, cancelButton{"Cancel"};

    // Progress view.
    double progress = 0.0;
    juce::String stage;
    juce::ProgressBar progressBar{progress};
    bool rendering = false;
    std::unique_ptr<juce::FileChooser> chooser;
};

} // namespace stemcel::native
