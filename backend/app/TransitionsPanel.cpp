#include "TransitionsPanel.h"

#include "MixTransitions.h"
#include "Theme.h"

namespace stemcel::native
{

// One column: a heading and its draggable rows.
class TransitionsPanel::Column final : public juce::Component, private juce::ListBoxModel
{
  public:
    Column(LibraryContext& context, const juce::String& heading, juce::Colour tint, std::vector<Row> items)
        : ctx(context), colour(tint), rows(std::move(items))
    {
        title.setText(heading, juce::dontSendNotification);
        title.setFont(uiFont(11.0f, true));
        title.setColour(juce::Label::textColourId, colour);
        addAndMakeVisible(title);
        list.setRowHeight(34);
        list.setColour(juce::ListBox::backgroundColourId, colours::zinc950);
        list.setColour(juce::ListBox::outlineColourId, colours::zinc700);
        list.setOutlineThickness(1);
        addAndMakeVisible(list);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        title.setBounds(area.removeFromTop(20));
        list.setBounds(area);
    }

  private:
    int getNumRows() override { return static_cast<int>(rows.size()); }

    void paintListBoxItem(int index, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (index < 0 || index >= static_cast<int>(rows.size())) return;
        const auto& row = rows[static_cast<size_t>(index)];
        if (row.heading.isNotEmpty())
        {
            g.setColour(colours::zinc900);
            g.fillRect(0, 0, width, height);
            g.setColour(colour);
            g.setFont(uiFont(10.0f, true));
            g.drawText(row.heading, 8, height / 2 - 2, width - 16, height / 2, juce::Justification::centredLeft);
            return;
        }
        if (selected)
        {
            g.setColour(colours::zinc800);
            g.fillRect(0, 0, width, height);
        }
        g.setColour(colour);
        g.fillRect(0, 4, 3, height - 8);
        g.setColour(colours::zinc100);
        g.setFont(uiFont(12.0f, true));
        g.drawText(row.name, 10, 2, width - 14, height / 2, juce::Justification::centredLeft, true);
        g.setColour(colours::textDim);
        g.setFont(uiFont(10.5f));
        g.drawText(row.description, 10, height / 2, width - 14, height / 2 - 2, juce::Justification::centredLeft, true);
    }

    juce::var getDragSourceDescription(const juce::SparseSet<int>& selected) override
    {
        if (selected.isEmpty()) return {};
        const int index = selected[0];
        if (index < 0 || index >= static_cast<int>(rows.size())) return {};
        return rows[static_cast<size_t>(index)].drag;
    }

    juce::String getTooltipForRow(int index) override
    {
        if (index < 0 || index >= static_cast<int>(rows.size())) return {};
        return rows[static_cast<size_t>(index)].description;
    }

    void listBoxItemDoubleClicked(int index, const juce::MouseEvent&) override
    {
        if (index < 0 || index >= static_cast<int>(rows.size()) || rows[static_cast<size_t>(index)].drag.isEmpty()) return;
        // Onto the selected clip, at its join nearest its end for an Out and its start for an In.
        const auto& what = rows[static_cast<size_t>(index)].drag;
        for (const auto& clipId : ctx.view.selectedClipIds)
            if (const auto* clip = ctx.mirror.model().findClip(clipId))
            {
                const double at = what.startsWith("transition:in:") ? clip->offsetMs : clip->endMs();
                const auto why = mix::drop(ctx.engine, ctx.actions, ctx.mirror.model(), clipId, at, what);
                if (why.isNotEmpty() && ctx.notify) ctx.notify(why, true);
                return;
            }
        if (ctx.notify) ctx.notify("Select a clip on the timeline first, or drag the transition onto one.", true);
    }

    LibraryContext& ctx;
    juce::Colour colour;
    std::vector<Row> rows;
    juce::Label title;
    juce::ListBox list{"transitions", this};
};

TransitionsPanel::TransitionsPanel(LibraryContext& context) : ctx(context)
{
    std::vector<Row> outRows, inRows, fullRows;
    for (const auto& k : mix::halfKinds())
        (k.outgoing ? outRows : inRows).push_back({{}, (k.outgoing ? "transition:out:" : "transition:in:") + k.id, k.name, k.description, {}});
    for (const auto& category : mix::categories())
    {
        fullRows.push_back({category.toUpperCase(), {}, {}, {}, {}});
        for (const auto& p : mix::presets())
            if (p.category == category) fullRows.push_back({{}, "transition:preset:" + p.id, p.name, p.description, {}});
    }
    outs = std::make_unique<Column>(ctx, "OUTS: how the outgoing track leaves", colours::amber400, std::move(outRows));
    ins = std::make_unique<Column>(ctx, "INS: how the incoming track arrives", colours::sky400, std::move(inRows));
    full = std::make_unique<Column>(ctx, "FULL TRANSITIONS: an out and an in together", colours::violet500, std::move(fullRows));
    for (auto* c : {outs.get(), ins.get(), full.get()}) addAndMakeVisible(c);

    hint.setText("Drag onto a clip that overlaps a clip on another track: an Out where it ends, an In where it starts. Double-click "
                 "a transition on the timeline to tweak its span, curves and EQ.",
                 juce::dontSendNotification);
    hint.setFont(uiFont(11.0f));
    hint.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(hint);
}

TransitionsPanel::~TransitionsPanel() = default;

void TransitionsPanel::resized()
{
    auto area = getLocalBounds();
    hint.setBounds(area.removeFromTop(20));
    area.removeFromTop(2);
    const int w = (area.getWidth() - 16) / 3;
    outs->setBounds(area.removeFromLeft(w));
    area.removeFromLeft(8);
    ins->setBounds(area.removeFromLeft(w));
    area.removeFromLeft(8);
    full->setBounds(area);
}

} // namespace stemcel::native
