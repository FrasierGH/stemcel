#include "TrackHeaders.h"

#include "Db.h"
#include "Theme.h"
#include "TimelineView.h"

#include <algorithm>

namespace stemcel::native
{
namespace
{
using VS = TimelineViewState;
constexpr int kMeterWidth = 6;
} // namespace

double gainToSlider(double gain)
{
    return db::linearToPosition(gain, db::kMaxTrackDb);
}

double sliderToGain(double position)
{
    return db::positionToLinear(position, db::kMaxTrackDb);
}

// ── row ─────────────────────────────────────────────────────────────────────
TrackHeaderRow::TrackHeaderRow(TimelineActions& timelineActions, TimelineViewState& viewState, LiveState& liveState)
    : actions(timelineActions), view(viewState), live(liveState)
{
    name.setFont(uiFont(13.0f, true));
    name.setColour(juce::Label::textColourId, colours::zinc100);
    // The row takes the clicks; the label's in-place editor (rename) takes its own.
    name.setInterceptsMouseClicks(false, true);
    name.setColour(juce::Label::backgroundWhenEditingColourId, colours::zinc950);
    name.setColour(juce::Label::textWhenEditingColourId, colours::zinc100);
    name.setColour(juce::Label::outlineWhenEditingColourId, colours::sky500);
    name.onTextChange = [this]
    {
        const auto text = name.getText().trim();
        if (text.isNotEmpty() && text != storedName) actions.renameTrack(trackId, text);
        else name.setText(shownName, juce::dontSendNotification);
    };
    addAndMakeVisible(name);

    for (auto* b : {&automationButton, &mute, &solo})
    {
        b->setClickingTogglesState(false);
        addAndMakeVisible(b);
    }
    automationButton.setColour(juce::TextButton::buttonOnColourId, colours::sky600);
    automationButton.setTooltip("Automation: show or hide this track's lanes (A)");
    automationButton.onClick = [this] { actions.toggleAutomation(trackId); };
    for (auto* l : {&gainCaption, &panCaption})
    {
        l->setFont(uiFont(10.0f));
        l->setColour(juce::Label::textColourId, colours::textFaint);
        l->setJustificationType(juce::Justification::centredLeft);
        l->setInterceptsMouseClicks(false, false);
        addAndMakeVisible(l);
    }
    mute.setColour(juce::TextButton::buttonOnColourId, colours::amber500);
    solo.setColour(juce::TextButton::buttonOnColourId, colours::sky600);
    mute.onClick = [this] { actions.setTrackMuted(trackId, !mute.getToggleState()); };
    solo.onClick = [this] { actions.setTrackSoloed(trackId, !solo.getToggleState()); };

    volume.setRange(0.0, 1.0);
    volume.setDoubleClickReturnValue(true, gainToSlider(1.0));
    // Sent on every change; the engine folds a drag's worth into one undo step.
    volume.onValueChange = [this]
    {
        const auto text = db::format(db::linearToDb(sliderToGain(volume.getValue())));
        volume.setTooltip("Gain " + text + " (double-click for 0 dB)");
        if (volume.isMouseButtonDown()) gainCaption.setText(text, juce::dontSendNotification);
        actions.setTrackGain(trackId, sliderToGain(volume.getValue()));
    };
    volume.onDragEnd = [this] { gainCaption.setText("Gain", juce::dontSendNotification); };
    addAndMakeVisible(volume);

    pan.setRange(-1.0, 1.0);
    pan.setDoubleClickReturnValue(true, 0.0);
    pan.onValueChange = [this]
    {
        const double v = pan.getValue();
        const auto text = std::abs(v) < 0.005 ? juce::String("C") : juce::String(std::abs(v) * 100.0, 0) + (v < 0 ? " L" : " R");
        pan.setTooltip("Pan " + text + " (double-click to centre)");
        if (pan.isMouseButtonDown()) panCaption.setText(text, juce::dontSendNotification);
        actions.setTrackPan(trackId, v);
    };
    pan.onDragEnd = [this] { panCaption.setText("Pan", juce::dontSendNotification); };
    addAndMakeVisible(pan);
}

void TrackHeaderRow::update(const TrackModel& track, int displayIndex, bool silenced, int clipHeight)
{
    trackId = track.id;
    clipLaneHeight = clipHeight;
    lanes = automation::lanesOf(track);
    plugins = track.raw.getProperty("plugins", {});
    for (auto* c : std::initializer_list<juce::Component*>{&mute, &solo, &volume, &pan, &gainCaption, &panCaption})
        c->setVisible(!isMaster());
    automated = actions.automatedParams(track.id);
    automationButton.setToggleState(!lanes.empty(), juce::dontSendNotification);
    automationButton.setTooltip(lanes.empty() ? "Show automation (A)" : "Hide automation (A)");
    this->displayIndex = displayIndex;
    colourIndex = track.colorIndex >= 0 ? track.colorIndex : displayIndex;
    isSilenced = silenced;
    storedName = track.name;
    shownName = track.name.isNotEmpty() ? track.name : "Track " + juce::String(displayIndex + 1);
    if (!name.isBeingEdited()) name.setText(shownName, juce::dontSendNotification);
    name.setTooltip(shownName + " - click to rename");
    mute.setToggleState(track.muted, juce::dontSendNotification);
    solo.setToggleState(track.soloed, juce::dontSendNotification);
    mute.setTooltip(track.muted ? "Unmute" : "Mute");
    solo.setTooltip(track.soloed ? "Unsolo" : "Solo");
    if (!volume.isMouseButtonDown()) volume.setValue(gainToSlider(track.gain), juce::dontSendNotification);
    if (!pan.isMouseButtonDown()) pan.setValue(track.pan, juce::dontSendNotification);
    volume.setTooltip("Gain " + db::format(db::linearToDb(track.gain)) + " (double-click for 0 dB)");
    repaint();
}

void TrackHeaderRow::paint(juce::Graphics& g)
{
    const bool selected = view.selectedTrackId == trackId;
    g.setColour(selected ? colours::zinc800 : colours::zinc900);
    g.fillRect(getLocalBounds());
    g.setColour(colours::border);
    g.drawRect(getLocalBounds());
    g.setColour(paletteSwatch(colourIndex).withMultipliedAlpha(isSilenced ? 0.4f : 1.0f));
    g.fillRect(swatchArea);
    if (isSilenced)
    {
        g.setColour(colours::zinc950.withAlpha(0.35f));
        g.fillRect(getLocalBounds().withTrimmedLeft(swatchArea.getRight()));
    }
    paintMeter(g);

    // Pan's centre, under the slider.
    if (pan.isVisible())
    {
        const auto b = pan.getBounds();
        g.setColour(colours::textFaint);
        g.fillRect(b.getCentreX(), b.getBottom() - 3, 1, 3);
    }

    // Each automation lane's header, beside the lane in the timeline: its parameter (a
    // dropdown), + for another lane, x to close it.
    laneHits.clear();
    int y = clipLaneHeight;
    for (const auto& lane : lanes)
    {
        auto r = juce::Rectangle<int>(0, y, getWidth(), lane.heightPx);
        g.setColour(colours::zinc950);
        g.fillRect(r.withTrimmedLeft(swatchArea.getRight()));
        g.setColour(colours::border);
        g.drawHorizontalLine(y, 0.0f, static_cast<float>(getWidth()));
        auto top = r.reduced(10, 6).removeFromTop(18);
        LaneHit hit{lane.paramId, {}, {}, {}};
        hit.close = top.removeFromRight(18);
        top.removeFromRight(4);
        hit.add = top.removeFromRight(18);
        top.removeFromRight(6);
        hit.caption = top;
        g.setColour(colours::zinc800);
        g.fillRoundedRectangle(hit.caption.toFloat(), 3.0f);
        g.setColour(colours::amber400);
        g.setFont(uiFont(12.0f));
        g.drawText(laneCaption(lane.paramId), hit.caption.reduced(6, 0).withTrimmedRight(12), juce::Justification::centredLeft, true);
        {
            const auto c = hit.caption.toFloat().withLeft(static_cast<float>(hit.caption.getRight() - 14)).getCentre();
            juce::Path arrow;
            arrow.addTriangle(c.x - 3.5f, c.y - 1.5f, c.x + 3.5f, c.y - 1.5f, c.x, c.y + 2.5f);
            g.fillPath(arrow);
        }
        for (const auto& [box, glyph] : {std::pair{hit.add, "+"}, std::pair{hit.close, "x"}})
        {
            g.setColour(colours::zinc800);
            g.fillRoundedRectangle(box.toFloat(), 3.0f);
            g.setColour(colours::textDim);
            g.setFont(uiFont(12.0f, true));
            g.drawText(glyph, box, juce::Justification::centred);
        }
        laneHits.push_back(hit);
        y += lane.heightPx;
    }
}

void TrackHeaderRow::paintMeter(juce::Graphics& g)
{
    g.setColour(colours::zinc950);
    g.fillRect(meterArea);
    std::pair<float, float> peaks{live.masterPeakL, live.masterPeakR};
    if (!isMaster())
    {
        const auto it = live.trackPeaks.find(trackId);
        if (it == live.trackPeaks.end()) return;
        peaks = it->second;
    }
    for (int ch = 0; ch < 2; ++ch)
    {
        const float peak = ch == 0 ? peaks.first : peaks.second;
        const double pos = db::dbToPosition(db::linearToDb(peak), db::kMaxTrackDb);
        auto bar = meterArea.withWidth(meterArea.getWidth() / 2).withX(meterArea.getX() + ch * meterArea.getWidth() / 2);
        const int h = static_cast<int>(bar.getHeight() * pos);
        g.setColour(peak >= 1.0f ? colours::red500 : peak > 0.7f ? colours::amber400 : colours::emerald500);
        g.fillRect(bar.removeFromBottom(h).reduced(1, 0));
    }
}

void TrackHeaderRow::resized()
{
    const auto clipArea = getLocalBounds().withHeight(clipLaneHeight > 0 ? clipLaneHeight : getHeight());
    auto area = clipArea.reduced(6, 6);
    swatchArea = getLocalBounds().removeFromLeft(4);
    meterArea = area.removeFromRight(kMeterWidth * 2);
    area.removeFromRight(4);
    area.removeFromLeft(2);
    auto top = area.removeFromTop(22);
    if (!isMaster())
    {
        solo.setBounds(top.removeFromRight(24));
        top.removeFromRight(4);
        mute.setBounds(top.removeFromRight(24));
        top.removeFromRight(4);
    }
    automationButton.setBounds(top.removeFromRight(24));
    top.removeFromRight(4);
    name.setBounds(top);
    area.removeFromTop(8);
    auto gainRow = area.removeFromTop(18);
    gainCaption.setBounds(gainRow.removeFromLeft(34));
    volume.setBounds(gainRow);
    area.removeFromTop(4);
    auto panRow = area.removeFromTop(18);
    panCaption.setBounds(panRow.removeFromLeft(34));
    pan.setBounds(panRow);
}

bool resizeZone(int y, int clipHeight)
{
    return clipHeight > 0 && std::abs(y - clipHeight) <= 4;
}

void TrackHeaderRow::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(resizeZone(e.y, clipLaneHeight) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void TrackHeaderRow::mouseDrag(const juce::MouseEvent& e)
{
    if (!resizing) return;
    const int next = juce::jlimit(TimelineViewState::kMinTrackHeight, TimelineViewState::kMaxTrackHeight,
                                  resizeStartHeight + e.getDistanceFromDragStartY());
    if (next != clipLaneHeight) actions.setTrackHeight(trackId, next);
}

void TrackHeaderRow::mouseUp(const juce::MouseEvent&)
{
    resizing = false;
}

void TrackHeaderRow::mouseDown(const juce::MouseEvent& e)
{
    if (resizeZone(e.y, clipLaneHeight) && !e.mods.isPopupMenu() && !isMaster())
    {
        resizing = true;
        resizeStartHeight = clipLaneHeight;
        return;
    }
    view.selectedTrackId = trackId;
    view.changed();
    for (const auto& hit : laneHits)
    {
        if (hit.caption.contains(e.getPosition()))
        {
            showLaneParamMenu(hit.paramId, false);
            return;
        }
        if (hit.add.contains(e.getPosition()))
        {
            showLaneParamMenu(hit.paramId, true);
            return;
        }
        if (hit.close.contains(e.getPosition()))
        {
            toggleLane(hit.paramId);
            return;
        }
    }
    if (e.mods.isPopupMenu())
    {
        showMenu();
        return;
    }
    if (name.getBounds().contains(e.getPosition()) && !isMaster()) rename();
}

void TrackHeaderRow::toggleLane(const juce::String& paramId)
{
    auto next = lanes;
    const auto it = std::find_if(next.begin(), next.end(), [&](const auto& l) { return l.paramId == paramId; });
    if (it != next.end()) next.erase(it);
    else next.push_back({paramId, automation::kDefaultLaneHeight});
    actions.setAutomationLanes(trackId, next);
}

juce::String TrackHeaderRow::laneCaption(const juce::String& paramId) const
{
    juce::String slotId;
    int index = 0;
    if (!automation::parsePluginParamId(paramId, slotId, index))
        if (const auto* param = automation::find(paramId)) return param->label;
    // A plugin parameter: the plugin's name and the parameter's, from the live instance.
    juce::String pluginName = "Plugin";
    if (const auto* list = plugins.getArray())
        for (const auto& s : *list)
            if (s.getProperty("slotId", {}).toString() == slotId) pluginName = s.getProperty("name", pluginName).toString();
    const auto names = actions.engineClient().host().engine().getTrackPluginParameterNames(trackId, slotId);
    const auto caption = pluginName + ": " + (index < names.size() ? names[index] : "parameter " + juce::String(index + 1));
    automation::namePluginParam(paramId, caption);
    return caption;
}

void TrackHeaderRow::addPluginLaneMenus(juce::PopupMenu& menu)
{
    // Each insert's parameters, in blocks of 32 (some plugins have hundreds).
    const auto* list = plugins.getArray();
    if (list == nullptr || list->isEmpty()) return;
    menu.addSeparator();
    for (const auto& s : *list)
    {
        const auto slotId = s.getProperty("slotId", {}).toString();
        const auto pluginName = s.getProperty("name", "Plugin").toString();
        const auto names = actions.engineClient().host().engine().getTrackPluginParameterNames(trackId, slotId);
        juce::PopupMenu sub;
        constexpr int kBlock = 32;
        for (int start = 0; start < names.size(); start += kBlock)
        {
            juce::PopupMenu block;
            for (int i = start; i < juce::jmin(names.size(), start + kBlock); ++i)
            {
                const auto id = automation::pluginParamId(slotId, i);
                bool shown = false;
                for (const auto& l : lanes) shown = shown || l.paramId == id;
                block.addItem(names[i], true, shown, [this, id, caption = pluginName + ": " + names[i]]
                              {
                                  automation::namePluginParam(id, caption);
                                  toggleLane(id);
                              });
            }
            if (names.size() <= kBlock) sub = block;
            else sub.addSubMenu(juce::String(start + 1) + "-" + juce::String(juce::jmin(names.size(), start + kBlock)), block);
        }
        if (names.isEmpty()) sub.addItem(s.getProperty("unresolved", false) ? "Not installed" : "No parameters", false, false, [] {});
        menu.addSubMenu(pluginName, sub);
    }
}

void TrackHeaderRow::addParamItems(juce::PopupMenu& menu, const juce::String& current,
                                   std::function<void(const juce::String&)> chosen)
{
    // Those with points first: what a transition or an earlier edit wrote.
    if (!automated.isEmpty())
    {
        menu.addSectionHeader("Automated");
        for (const auto& id : automated)
            menu.addItem(laneCaption(id), true, id == current, [chosen, id] { chosen(id); });
        menu.addSeparator();
    }
    for (const auto& p : isMaster() ? automation::masterParams() : automation::params())
        if (!automated.contains(p.id))
            menu.addItem(p.label, true, current == p.id, [chosen, id = juce::String(p.id)] { chosen(id); });
    const auto* list = plugins.getArray();
    if (list == nullptr || list->isEmpty()) return;
    menu.addSeparator();
    for (const auto& s : *list)
    {
        const auto slotId = s.getProperty("slotId", {}).toString();
        const auto pluginName = s.getProperty("name", "Plugin").toString();
        const auto names = actions.engineClient().host().engine().getTrackPluginParameterNames(trackId, slotId);
        juce::PopupMenu sub;
        constexpr int kBlock = 32;
        for (int start = 0; start < names.size(); start += kBlock)
        {
            juce::PopupMenu block;
            for (int i = start; i < juce::jmin(names.size(), start + kBlock); ++i)
            {
                const auto id = automation::pluginParamId(slotId, i);
                block.addItem(names[i], true, id == current, [chosen, id, caption = pluginName + ": " + names[i]]
                              {
                                  automation::namePluginParam(id, caption);
                                  chosen(id);
                              });
            }
            if (names.size() <= kBlock) sub = block;
            else sub.addSubMenu(juce::String(start + 1) + "-" + juce::String(juce::jmin(names.size(), start + kBlock)), block);
        }
        if (names.isEmpty()) sub.addItem(s.getProperty("unresolved", false) ? "Not installed" : "No parameters", false, false, [] {});
        menu.addSubMenu(pluginName, sub);
    }
}

void TrackHeaderRow::showLaneParamMenu(const juce::String& laneParam, bool adding)
{
    juce::PopupMenu menu;
    const auto id = trackId;
    auto current = lanes;
    addParamItems(menu, adding ? juce::String() : laneParam,
                  [this, id, laneParam, adding, current](const juce::String& picked)
                  {
                      auto next = current;
                      const auto at = std::find_if(next.begin(), next.end(), [&](const auto& l) { return l.paramId == laneParam; });
                      const bool shown = std::any_of(next.begin(), next.end(), [&](const auto& l) { return l.paramId == picked; });
                      if (adding)
                      {
                          // A new lane below this one (or nothing new, when it is already shown).
                          if (!shown) next.insert(at == next.end() ? next.end() : at + 1, {picked, automation::kDefaultLaneHeight});
                      }
                      else if (at != next.end() && picked != laneParam)
                      {
                          // This lane shows the picked parameter; a lane already showing it goes.
                          if (shown)
                              next.erase(std::remove_if(next.begin(), next.end(), [&](const auto& l) { return l.paramId == picked; }), next.end());
                          const auto here = std::find_if(next.begin(), next.end(), [&](const auto& l) { return l.paramId == laneParam; });
                          if (here != next.end()) here->paramId = picked;
                      }
                      actions.setAutomationLanes(id, next);
                  });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

// In place, as the renderer's track header does: Return or clicking away commits, Escape cancels.
void TrackHeaderRow::rename()
{
    name.showEditor();
    if (auto* editor = name.getCurrentTextEditor()) editor->selectAll();
}

void TrackHeaderRow::showMenu()
{
    juce::PopupMenu colourMenu;
    for (int i = 0; i < kPaletteSize; ++i)
        colourMenu.addColouredItem(100 + i, "Colour " + juce::String(i + 1), paletteSwatch(i), true, i == colourIndex);
    juce::PopupMenu automationMenu;
    int index = 0;
    for (const auto& p : isMaster() ? automation::masterParams() : automation::params())
    {
        bool shown = false;
        for (const auto& l : lanes) shown = shown || l.paramId == p.id;
        automationMenu.addItem(300 + index++, p.label, true, shown);
    }
    addPluginLaneMenus(automationMenu);
    if (isMaster())
    {
        automationMenu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                                     [this](int result)
                                     {
                                         if (result >= 300) toggleLane(automation::masterParams()[static_cast<size_t>(result - 300)].id);
                                     });
        return;
    }
    juce::PopupMenu menu;
    menu.addItem(1, "Rename");
    menu.addItem(3, "Move Up", displayIndex > 0);
    menu.addItem(4, "Move Down");
    menu.addSubMenu("Colour", colourMenu);
    menu.addSubMenu("Automation", automationMenu);
    menu.addSeparator();
    menu.addItem(2, "Delete Track");
    const auto id = trackId;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                       [this, id](int result)
                       {
                           if (result == 1) rename();
                           else if (result == 3) actions.moveTrack(id, displayIndex - 1);
                           else if (result == 4) actions.moveTrack(id, displayIndex + 1);
                           else if (result == 2) actions.removeTrack(id);
                           else if (result >= 300)
                           {
                               // Toggle the lane for that parameter.
                               toggleLane(automation::params()[static_cast<size_t>(result - 300)].id);
                           }
                           else if (result >= 100) actions.setTrackColor(id, result - 100);
                       });
}

// ── column ──────────────────────────────────────────────────────────────────
TrackHeaders::TrackHeaders(ProjectMirror& projectMirror, TimelineViewState& viewState, LiveState& liveState,
                           TimelineActions& timelineActions)
    : mirror(projectMirror), view(viewState), live(liveState), actions(timelineActions)
{
    addTrackButton.setTooltip("Add a new track");
    addTrackButton.onClick = [this]
    {
        view.selectedTrackId = actions.addTrack();
        view.changed();
    };
    addAndMakeVisible(addTrackButton);
    tempoButton.setTooltip("Show the tempo lane: draw tempo changes and ramps the warped clips follow");
    tempoButton.setClickingTogglesState(true);
    tempoButton.setColour(juce::TextButton::buttonOnColourId, colours::sky600);
    tempoButton.onClick = [this]
    {
        view.tempoLaneVisible = tempoButton.getToggleState();
        view.changed();
    };
    addAndMakeVisible(tempoButton);
    cover.painter = [this](juce::Graphics& g) { paintHeader(g); };
    cover.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(cover);
    mirror.addChangeListener(this);
    view.addChangeListener(this);
    rebuild();
    startTimerHz(30);
}

TrackHeaders::~TrackHeaders()
{
    mirror.removeChangeListener(this);
    view.removeChangeListener(this);
}

void TrackHeaders::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &mirror)
    {
        // A project with a tempo map opens with its lane showing (and so does a first point).
        const bool hasTempoMap = !mirror.model().tempoMap.empty();
        if (hasTempoMap && !hadTempoMap && !view.tempoLaneVisible)
        {
            view.tempoLaneVisible = true;
            view.changed();
        }
        hadTempoMap = hasTempoMap;
        rebuild();
    }
    tempoButton.setToggleState(view.tempoLaneVisible, juce::dontSendNotification);
    resized();
    for (auto& [id, row] : rowComponents) row->repaint();
}

void TrackHeaders::timerCallback()
{
    for (auto& [id, row] : rowComponents) row->repaint(row->getLocalBounds().removeFromRight(30));
}

void TrackHeaders::rebuild()
{
    const auto& model = mirror.model();
    bool anySolo = false;
    for (const auto& t : model.tracks) anySolo = anySolo || t.soloed;

    const auto layout = layoutRows(model);
    std::map<juce::String, std::unique_ptr<TrackHeaderRow>> next;
    for (size_t i = 0; i < model.tracks.size(); ++i)
    {
        const auto& track = model.tracks[i];
        auto it = rowComponents.find(track.id);
        std::unique_ptr<TrackHeaderRow> row;
        if (it != rowComponents.end()) row = std::move(it->second);
        else
        {
            row = std::make_unique<TrackHeaderRow>(actions, view, live);
            addAndMakeVisible(*row);
        }
        int clipHeight = 0;
        for (const auto& r : layout)
            if (r.trackId == track.id) clipHeight = r.clipHeight;
        row->update(track, static_cast<int>(i), track.muted || (anySolo && !track.soloed), clipHeight);
        next[track.id] = std::move(row);
    }
    // The master's row, under the tracks.
    if (model.master.id.isNotEmpty())
    {
        auto it = rowComponents.find(model.master.id);
        std::unique_ptr<TrackHeaderRow> row;
        if (it != rowComponents.end()) row = std::move(it->second);
        else
        {
            row = std::make_unique<TrackHeaderRow>(actions, view, live);
            addAndMakeVisible(*row);
        }
        row->update(model.master, 0, false, kMasterRowHeight);
        next[model.master.id] = std::move(row);
    }
    rowComponents = std::move(next);
    resized();
}

void TrackHeaders::paint(juce::Graphics& g)
{
    g.fillAll(colours::zinc900);
}

void TrackHeaders::paintHeader(juce::Graphics& g)
{
    g.fillAll(colours::zinc900);
    if (view.tempoLaneVisible)
    {
        const juce::Rectangle<int> lane(0, VS::kRulerHeight, getWidth(), VS::kTempoLaneHeight);
        g.setColour(colours::zinc950);
        g.fillRect(lane);
        auto text = lane.reduced(8, 10);
        g.setColour(colours::zinc400);
        g.setFont(uiFont(11.0f, true));
        g.drawText("TEMPO", text.removeFromTop(16), juce::Justification::centredLeft);
        g.setColour(colours::textFaint);
        g.setFont(uiFont(10.0f));
        g.drawText("BPM map", text.removeFromTop(14), juce::Justification::centredLeft);
    }
}

void TrackHeaders::resized()
{
    auto top = getLocalBounds().removeFromTop(VS::kRulerHeight).reduced(4, 3);
    tempoButton.setBounds(top.removeFromRight(60));
    top.removeFromRight(4);
    addTrackButton.setBounds(top);
    const auto rows = layoutRows(mirror.model());
    for (const auto& r : rows)
    {
        if (auto it = rowComponents.find(r.trackId); it != rowComponents.end())
        {
            const int y = r.top + view.headerHeight() - static_cast<int>(view.scrollY);
            it->second->setBounds(0, y, getWidth(), r.height);
            it->second->setVisible(y + r.height > view.headerHeight() && y < getHeight());
        }
    }
    cover.setBounds(0, 0, getWidth(), view.headerHeight());
    cover.toFront(false);
    cover.repaint();
    addTrackButton.toFront(false);
    tempoButton.toFront(false);
}

void TrackHeaders::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    view.scrollY = juce::jmax(0.0, view.scrollY - w.deltaY * 200.0);
    view.changed();
}

} // namespace stemcel::native
