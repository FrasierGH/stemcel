#pragma once

#include "LibraryPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace stemcel::native
{

// A labelled rotary control whose drag is one undo step: every value during a drag carries
// the same `gestureId`, and the release sends `gestureEnd` (the renderer's FxRangeControl).
class FxKnob final : public juce::Component
{
  public:
    using Send = std::function<void(double value, const juce::String& gestureId, bool gestureEnd)>;

    FxKnob(const juce::String& label, double min, double max, double step, double defaultValue, Send send,
           std::function<juce::String(double)> format = {});

    void setValue(double value);
    void resized() override;

  private:
    juce::Slider slider;
    juce::Label label;
    Send send;
    juce::String gestureId;
};

// A titled group of knobs (one FX module).
class FxModule final : public juce::Component
{
  public:
    explicit FxModule(const juce::String& title);
    FxKnob& add(std::unique_ptr<FxKnob> knob);
    void addExtra(juce::Component& extra, int width);
    void paint(juce::Graphics&) override;
    void resized() override;
    int preferredWidth() const;

  private:
    juce::String title;
    std::vector<std::unique_ptr<FxKnob>> knobs;
    std::vector<std::pair<juce::Component*, int>> extras;
};

// Track FX for the selected track: Tone (bass/mid/treble), Filter, Sends (room/echo),
// Leveler, Punch, Saturation, Bit crusher.
class TrackFxView final : public juce::Component, private juce::ChangeListener
{
  public:
    explicit TrackFxView(LibraryContext& context);
    ~TrackFxView() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    // Each knob shown (by project-state key) with the automation lane that drives it (empty
    // when none does).
    std::vector<std::pair<juce::String, juce::String>> knobAutomation() const;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void sync();
    void sendTrack(const juce::String& type, std::initializer_list<std::pair<const char*, juce::var>> props,
                   const juce::String& gestureId, bool gestureEnd);

    LibraryContext& ctx;
    juce::Label heading;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<FxModule>> modules;
    std::map<juce::String, FxKnob*> knobs; // by project-state key
};

// Project FX: Room (reverb), Echo (delay, with note value), Glue compressor, the safety
// limiter and the metronome.
class ProjectFxView final : public juce::Component, private juce::ChangeListener
{
  public:
    explicit ProjectFxView(LibraryContext& context);
    ~ProjectFxView() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    // Each knob shown with the master automation lane that drives it (empty when none does).
    std::vector<std::pair<juce::String, juce::String>> knobAutomation() const;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void sync();

    LibraryContext& ctx;
    std::vector<std::unique_ptr<FxModule>> modules;
    std::map<juce::String, FxKnob*> knobs;
    juce::OwnedArray<juce::TextButton> noteButtons;
    std::unique_ptr<juce::Component> columnOwner; // holds the note buttons beside the echo knobs
    juce::ToggleButton limiter{"Safety limiter"}, metronome{"Metronome"};
};

// VST3/AU plugins on the selected track: the chain (bypass, reorder, open editor, remove)
// and the scanned catalogue to add from.
class PluginsView final : public juce::Component, private juce::ChangeListener, private juce::ListBoxModel
{
  public:
    explicit PluginsView(LibraryContext& context);
    ~PluginsView() override;
    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    struct Slot
    {
        juce::String slotId, name, manufacturer;
        bool bypassed = false;
        bool unresolved = false;
    };
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void sync();
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void showAddMenu();
    void slotMenu(int row);
    juce::var slotRef(const Slot& slot) const;

    LibraryContext& ctx;
    Subscriptions subscriptions;
    juce::Label heading;
    juce::ListBox chain;
    juce::TextButton addButton{"Add plugin..."}, scanButton{"Scan for plugins"};
    // The master bus's inserts instead of the selected track's.
    juce::ToggleButton masterTarget{"Master bus"};
    juce::String targetId() const;
    juce::Label scanStatus;
    std::vector<Slot> slots;
    juce::var catalogue;
};

} // namespace stemcel::native
