#include "MixerPanel.h"

#include "Db.h"
#include "Theme.h"

#include <map>

namespace stemcel::native
{
namespace
{
constexpr int kStripWidth = 104;
constexpr int kInsertRows = 5;
constexpr int kInsertRowHeight = 18;
const juce::String kMasterId{"__master__"}; // plugins::kMasterChainId

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props) o->setProperty(k, v);
    return juce::var(o);
}
} // namespace

// ── One channel strip ────────────────────────────────────────────────────────
class MixerPanel::Strip final : public juce::Component
{
  public:
    Strip(MixerPanel& owner, juce::String id) : panel(owner), ownerId(std::move(id))
    {
        const bool isMaster = ownerId == kMasterId;
        name.setFont(uiFont(11.0f, true));
        name.setJustificationType(juce::Justification::centred);
        name.setColour(juce::Label::textColourId, colours::zinc100);
        addAndMakeVisible(name);

        fader.setSliderStyle(juce::Slider::LinearVertical);
        fader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange(0.0, 1.0);
        fader.setDoubleClickReturnValue(true, db::linearToPosition(1.0, isMaster ? db::kMaxMasterDb : db::kMaxTrackDb));
        fader.onValueChange = [this, isMaster]
        {
            if (!fader.isMouseButtonDown() && !fader.isMouseOverOrDragging()) return;
            const double gain = db::positionToLinear(fader.getValue(), isMaster ? db::kMaxMasterDb : db::kMaxTrackDb);
            if (isMaster) panel.actions.setMasterVolume(gain);
            else panel.actions.setTrackGain(ownerId, gain);
            readout.setText(db::format(db::linearToDb(gain)), juce::dontSendNotification);
        };
        addAndMakeVisible(fader);
        readout.setFont(uiFont(10.0f));
        readout.setJustificationType(juce::Justification::centred);
        readout.setColour(juce::Label::textColourId, colours::textDim);
        addAndMakeVisible(readout);

        if (!isMaster)
        {
            pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            pan.setRange(-1.0, 1.0, 0.01);
            pan.setDoubleClickReturnValue(true, 0.0);
            pan.onValueChange = [this]
            {
                if (pan.isMouseButtonDown()) panel.actions.setTrackPan(ownerId, pan.getValue());
            };
            addAndMakeVisible(pan);
            for (auto* b : {&mute, &solo})
            {
                b->setClickingTogglesState(false);
                addAndMakeVisible(b);
            }
            mute.onClick = [this] { panel.actions.setTrackMuted(ownerId, !mute.getToggleState()); };
            solo.onClick = [this] { panel.actions.setTrackSoloed(ownerId, !solo.getToggleState()); };
        }
        sync();
    }

    void sync()
    {
        const auto& model = panel.mirror.model();
        slots.clear();
        juce::var list;
        if (ownerId == kMasterId)
        {
            name.setText("MASTER", juce::dontSendNotification);
            colour = colours::sky500;
            if (!fader.isMouseButtonDown()) fader.setValue(db::linearToPosition(model.masterVolume, db::kMaxMasterDb), juce::dontSendNotification);
            readout.setText(db::format(db::linearToDb(model.masterVolume)), juce::dontSendNotification);
            list = model.raw.getProperty("masterPlugins", {});
        }
        else if (const auto* track = model.findTrack(ownerId))
        {
            name.setText(track->name, juce::dontSendNotification);
            int index = 0;
            for (size_t i = 0; i < model.tracks.size(); ++i)
                if (model.tracks[i].id == ownerId) index = static_cast<int>(i);
            colour = paletteSwatch(track->colorIndex >= 0 ? track->colorIndex : index);
            if (!fader.isMouseButtonDown()) fader.setValue(db::linearToPosition(track->gain, db::kMaxTrackDb), juce::dontSendNotification);
            readout.setText(db::format(db::linearToDb(track->gain)), juce::dontSendNotification);
            if (!pan.isMouseButtonDown()) pan.setValue(track->pan, juce::dontSendNotification);
            pan.setTooltip(std::abs(track->pan) < 0.01 ? juce::String("Pan: centre")
                                                      : "Pan: " + juce::String(juce::roundToInt(std::abs(track->pan) * 100.0)) + (track->pan < 0 ? "% left" : "% right"));
            mute.setToggleState(track->muted, juce::dontSendNotification);
            solo.setToggleState(track->soloed, juce::dontSendNotification);
            mute.setColour(juce::TextButton::buttonColourId, track->muted ? colours::amber500 : colours::zinc800);
            solo.setColour(juce::TextButton::buttonColourId, track->soloed ? colours::sky600 : colours::zinc800);
            list = track->raw.getProperty("plugins", {});
        }
        if (list.isArray())
            for (const auto& s : *list.getArray())
                slots.push_back({s.getProperty("slotId", {}).toString(), s.getProperty("name", {}).toString(),
                                 static_cast<bool>(s.getProperty("bypassed", false)), static_cast<bool>(s.getProperty("unresolved", false))});
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(ownerId == kMasterId ? colours::zinc900 : colours::zinc950);
        g.setColour(colours::zinc700);
        g.drawRect(getLocalBounds());
        g.setColour(colour);
        g.fillRect(getLocalBounds().removeFromTop(3));

        // Insert slots.
        g.setFont(uiFont(10.0f));
        for (int i = 0; i < kInsertRows; ++i)
        {
            const auto r = insertRow(i);
            g.setColour(colours::zinc900);
            g.fillRect(r);
            g.setColour(colours::zinc700);
            g.drawRect(r);
            if (i < static_cast<int>(slots.size()))
            {
                const auto& s = slots[static_cast<size_t>(i)];
                g.setColour(s.unresolved ? colours::red500 : s.bypassed ? colours::textFaint : colours::zinc100);
                g.drawText(s.name, r.reduced(4, 0), juce::Justification::centredLeft, true);
            }
            else if (i == static_cast<int>(slots.size()))
            {
                g.setColour(colours::textDim);
                g.drawText("+ insert", r.reduced(4, 0), juce::Justification::centredLeft, true);
            }
        }

        // The meter beside the fader: L and R from -60 dB, a mark at 0.
        const auto area = meterArea();
        g.setColour(colours::zinc900);
        g.fillRect(area);
        const auto& live = panel.live;
        float l = 0.0f, r = 0.0f;
        if (ownerId == kMasterId)
        {
            l = live.masterPeakL;
            r = live.masterPeakR;
        }
        else if (const auto it = live.trackPeaks.find(ownerId); it != live.trackPeaks.end())
        {
            l = it->second.first;
            r = it->second.second;
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            const float peak = ch == 0 ? l : r;
            const float dbValue = static_cast<float>(db::linearToDb(peak));
            const float fraction = juce::jlimit(0.0f, 1.0f, (dbValue + 60.0f) / 60.0f);
            auto bar = area.toFloat().withWidth(area.getWidth() / 2.0f - 1.0f).withX(area.getX() + ch * (area.getWidth() / 2.0f + 1.0f));
            auto lit = bar.removeFromBottom(bar.getHeight() * fraction);
            g.setColour(dbValue > -3.0f ? colours::red500 : dbValue > -12.0f ? colours::amber400 : colours::emerald500);
            g.fillRect(lit);
        }
        g.setColour(colours::zinc500);
        g.drawHorizontalLine(area.getY(), static_cast<float>(area.getX()), static_cast<float>(area.getRight()));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);
        area.removeFromTop(4);
        name.setBounds(area.removeFromTop(18));
        area.removeFromTop(kInsertRows * kInsertRowHeight + 6);
        if (ownerId != kMasterId)
        {
            pan.setBounds(area.removeFromTop(40).withSizeKeepingCentre(40, 40));
            auto buttons = area.removeFromTop(22);
            mute.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2 - 1));
            buttons.removeFromLeft(2);
            solo.setBounds(buttons);
            area.removeFromTop(4);
        }
        readout.setBounds(area.removeFromBottom(16));
        auto faderArea = area;
        meter = faderArea.removeFromRight(16).reduced(0, 6);
        fader.setBounds(faderArea);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        for (int i = 0; i < kInsertRows; ++i)
            if (insertRow(i).contains(e.getPosition()))
            {
                if (i < static_cast<int>(slots.size())) slotMenu(i);
                else if (i == static_cast<int>(slots.size())) addMenu();
                return;
            }
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        for (int i = 0; i < static_cast<int>(slots.size()) && i < kInsertRows; ++i)
            if (insertRow(i).contains(e.getPosition()))
                panel.engine.send("TRACK_OPEN_PLUGIN_EDITOR", ref(slots[static_cast<size_t>(i)]));
    }

    const juce::String& id() const { return ownerId; }
    void repaintMeter() { repaint(meter.expanded(1)); }

  private:
    struct Slot
    {
        juce::String slotId, name;
        bool bypassed = false, unresolved = false;
    };

    juce::Rectangle<int> insertRow(int i) const
    {
        return {4, 30 + i * kInsertRowHeight, getWidth() - 8, kInsertRowHeight - 1};
    }

    juce::Rectangle<int> meterArea() const { return meter; }

    juce::var ref(const Slot& s) const { return object({{"trackId", ownerId}, {"slotId", s.slotId}}); }

    void slotMenu(int row)
    {
        const auto s = slots[static_cast<size_t>(row)];
        juce::PopupMenu menu;
        menu.addItem("Open editor", !s.unresolved, false, [this, s] { panel.engine.send("TRACK_OPEN_PLUGIN_EDITOR", ref(s)); });
        menu.addItem(s.bypassed ? "Enable" : "Bypass", [this, s]
                     {
                         auto p = ref(s);
                         p.getDynamicObject()->setProperty("bypassed", !s.bypassed);
                         panel.engine.send("TRACK_SET_PLUGIN_BYPASS", p);
                     });
        menu.addItem("Move up", row > 0, false, [this, s, row]
                     {
                         auto p = ref(s);
                         p.getDynamicObject()->setProperty("index", row - 1);
                         panel.engine.send("TRACK_REORDER_PLUGIN", p);
                     });
        menu.addItem("Move down", row + 1 < static_cast<int>(slots.size()), false, [this, s, row]
                     {
                         auto p = ref(s);
                         p.getDynamicObject()->setProperty("index", row + 1);
                         panel.engine.send("TRACK_REORDER_PLUGIN", p);
                     });
        menu.addSeparator();
        menu.addItem("Remove", [this, s] { panel.engine.send("TRACK_REMOVE_PLUGIN", ref(s)); });
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }

    void addMenu()
    {
        const auto catalogue = panel.engine.latest("PLUGIN_LIST").getProperty("plugins", {});
        if (!catalogue.isArray() || catalogue.size() == 0)
        {
            panel.engine.send("PLUGIN_LIST_REQUEST");
            if (panel.notify) panel.notify("No plugins found yet: scan for them in the Plugins tab.", false);
            return;
        }
        // By format, then maker: VST3, VST, AU, as this build hosts them.
        std::map<juce::String, std::map<juce::String, juce::PopupMenu>> byFormat;
        for (const auto& p : *catalogue.getArray())
        {
            if (static_cast<bool>(p.getProperty("isInstrument", false))) continue;
            const auto identifier = p.getProperty("identifier", {}).toString();
            auto maker = p.getProperty("manufacturer", {}).toString();
            if (maker.isEmpty()) maker = "Other";
            byFormat[p.getProperty("format", "VST3").toString()][maker].addItem(
                p.getProperty("name", {}).toString(),
                [this, identifier] { panel.engine.send("TRACK_ADD_PLUGIN", object({{"trackId", ownerId}, {"identifier", identifier}})); });
        }
        juce::PopupMenu menu;
        for (auto& [format, makers] : byFormat)
        {
            juce::PopupMenu sub;
            for (auto& [maker, items] : makers) sub.addSubMenu(maker, items);
            menu.addSubMenu(format, sub);
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }

    MixerPanel& panel;
    juce::String ownerId;
    juce::Colour colour = colours::zinc500;
    juce::Label name, readout;
    juce::Slider fader, pan;
    juce::TextButton mute{"M"}, solo{"S"};
    juce::Rectangle<int> meter;
    std::vector<Slot> slots;
};

// ── The panel ───────────────────────────────────────────────────────────────
MixerPanel::MixerPanel(EngineClient& client, ProjectMirror& projectMirror, TimelineActions& timelineActions, TimelineViewState& viewState,
                       LiveState& liveState, std::function<void(const juce::String&, bool)> onNotify)
    : engine(client), mirror(projectMirror), actions(timelineActions), view(viewState), live(liveState), notify(std::move(onNotify))
{
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(false, true);
    addAndMakeVisible(viewport);
    master = std::make_unique<Strip>(*this, kMasterId);
    addAndMakeVisible(*master);
    mirror.addChangeListener(this);
    if (engine.latest("PLUGIN_LIST").isVoid()) engine.send("PLUGIN_LIST_REQUEST");
    rebuild();
    startTimerHz(30);
}

MixerPanel::~MixerPanel()
{
    mirror.removeChangeListener(this);
}

void MixerPanel::changeListenerCallback(juce::ChangeBroadcaster*)
{
    juce::StringArray ids;
    for (const auto& t : mirror.model().tracks) ids.add(t.id);
    if (ids != shownTrackIds) rebuild();
    else
    {
        for (auto* s : strips) s->sync();
        master->sync();
    }
}

void MixerPanel::rebuild()
{
    strips.clear();
    shownTrackIds.clear();
    for (const auto& t : mirror.model().tracks)
    {
        shownTrackIds.add(t.id);
        content.addAndMakeVisible(strips.add(new Strip(*this, t.id)));
    }
    master->sync();
    resized();
}

void MixerPanel::timerCallback()
{
    for (auto* s : strips) s->repaintMeter();
    master->repaintMeter();
}

void MixerPanel::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    g.setColour(colours::border);
    g.drawRect(getLocalBounds());
}

void MixerPanel::resized()
{
    auto area = getLocalBounds().reduced(4);
    master->setBounds(area.removeFromRight(kStripWidth + 10));
    area.removeFromRight(8);
    viewport.setBounds(area);
    const int height = juce::jmax(0, area.getHeight() - viewport.getScrollBarThickness());
    content.setSize(juce::jmax(area.getWidth(), strips.size() * (kStripWidth + 4)), height);
    for (int i = 0; i < strips.size(); ++i) strips[i]->setBounds(i * (kStripWidth + 4), 0, kStripWidth, height);
}

} // namespace stemcel::native
