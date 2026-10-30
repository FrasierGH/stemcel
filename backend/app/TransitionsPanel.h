#pragma once

#include "LibraryPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// The Transitions tab, in three columns: the Outs (how the outgoing track leaves), the Ins
// (how the incoming one arrives) and the full transitions, to drag onto a clip on the timeline
// (or double-click for the selected clip). An Out goes on the join where the clip ends under
// another track's clip, an In where it starts over one; the join's other half stays as it was.
class TransitionsPanel final : public juce::Component
{
  public:
    explicit TransitionsPanel(LibraryContext& context);
    ~TransitionsPanel() override;

    void resized() override;

    struct Row
    {
        juce::String heading; // a section heading when set
        juce::String drag;    // "transition:out:<kind>" and the like
        juce::String name, description, tag;
    };
    class Column;

  private:
    LibraryContext& ctx;
    juce::Label hint;
    std::unique_ptr<Column> outs, ins, full;
};

} // namespace stemcel::native
