#include "UsbExport.h"

#include "Rekordbox.h"
#include "Theme.h"

#include <cmath>
#include <set>

namespace stemcel::native::dj
{
namespace
{
juce::String size(double bytes)
{
    if (bytes >= 1e9) return juce::String(bytes / 1e9, 1) + " GB";
    return juce::String(bytes / 1e6, 0) + " MB";
}

juce::String stageName(const juce::String& stage)
{
    if (stage == "analysing") return "Preparing analysis";
    if (stage == "checking") return "Checking the stick";
    if (stage == "copying") return "Copying audio";
    if (stage == "database") return "Writing the databases";
    if (stage == "verifying") return "Verifying";
    if (stage == "publishing") return "Finishing";
    return stage;
}
} // namespace

UsbExport::UsbExport(Rekordbox& rb, std::vector<juce::int64> preselected, std::function<void(const juce::String&, bool)> tell)
    : rekordbox(rb), notify(std::move(tell))
{
    auto& collection = Collection::shared();
    // The playlist tree, flattened with its depth.
    std::function<void(juce::int64, int)> walk = [&](juce::int64 parent, int depth)
    {
        for (const auto* p : collection.childrenOf(parent, false))
        {
            Entry e;
            e.id = p->id;
            e.name = p->name;
            e.depth = depth;
            e.folder = p->isFolder();
            e.tracks = static_cast<int>(collection.tracksIn(p->id).size());
            e.chosen = std::find(preselected.begin(), preselected.end(), p->id) != preselected.end();
            entries.push_back(e);
            if (p->isFolder()) walk(p->id, depth + 1);
        }
    };
    walk(0, 0);

    driveBox.setTextWhenNothingSelected("Looking for drives...");
    addAndMakeVisible(driveBox);
    refreshButton.onClick = [this] { refreshDrives(); };
    addAndMakeVisible(refreshButton);
    modeBox.addItem("Sync: the stick holds exactly these playlists", 1);
    modeBox.addItem("Add: put these beside what the stick has", 2);
    modeBox.setSelectedId(2, juce::dontSendNotification);
    addAndMakeVisible(modeBox);
    ejectToggle.setToggleState(true, juce::dontSendNotification);
    addAndMakeVisible(ejectToggle);

    list.setModel(this);
    list.setRowHeight(22);
    list.setColour(juce::ListBox::backgroundColourId, colours::zinc900);
    addAndMakeVisible(list);

    summary.setColour(juce::Label::textColourId, colours::textDim);
    progressText.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(summary);
    addAndMakeVisible(progressText);
    addChildComponent(progressBar);

    exportButton.onClick = [this] { start(); };
    cancelButton.onClick = [this]
    {
        if (running)
            stop->store(true);
        else
            close();
    };
    addAndMakeVisible(exportButton);
    addAndMakeVisible(cancelButton);
    setSize(560, 520);
    refreshDrives();
    updateSummary();
}

UsbExport::~UsbExport()
{
    alive->store(false);
    stop->store(true);
    pool.removeAllJobs(true, 600000);
}

void UsbExport::show(Rekordbox& rb, std::vector<juce::int64> preselected, std::function<void(const juce::String&, bool)> tell)
{
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Export to USB";
    options.content.setOwned(new UsbExport(rb, std::move(preselected), std::move(tell)));
    options.dialogBackgroundColour = colours::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.resizable = false;
    options.launchAsync();
}

void UsbExport::refreshDrives()
{
    driveBox.clear(juce::dontSendNotification);
    const auto result = rekordbox.volumes();
    drives.clear();
    if (const auto* list = result.getProperty("volumes", {}).getArray())
        for (const auto& v : *list)
        {
            // Sticks and cards; a fixed drive only when it already holds an export.
            if (!static_cast<bool>(v.getProperty("removable", false)) && !static_cast<bool>(v.getProperty("hasExport", false))) continue;
            drives.add(v);
        }
    for (int i = 0; i < drives.size(); ++i)
    {
        const auto& v = drives.getReference(i);
        juce::String text = v.getProperty("name", "").toString();
        if (text.isEmpty()) text = "Drive";
        text << " (" << v.getProperty("root", "").toString() << ")  " << size(static_cast<double>(v.getProperty("freeBytes", 0.0)))
             << " free, " << v.getProperty("fileSystem", "").toString();
        if (static_cast<bool>(v.getProperty("hasExport", false)))
            text << ", has " << static_cast<int>(v.getProperty("exportTracks", 0)) << " tracks";
        driveBox.addItem(text, i + 1);
    }
    if (drives.isEmpty())
        driveBox.setTextWhenNothingSelected("No USB stick or SD card found: plug one in and Refresh");
    else
        driveBox.setSelectedItemIndex(0, juce::dontSendNotification);
}

int UsbExport::getNumRows()
{
    return static_cast<int>(entries.size());
}

void UsbExport::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool)
{
    if (row < 0 || row >= getNumRows()) return;
    const auto& e = entries[static_cast<size_t>(row)];
    const int x = 8 + e.depth * 16;
    const auto box = juce::Rectangle<int>(x, (height - 14) / 2, 14, 14);
    g.setColour(colours::border);
    g.drawRect(box);
    if (e.chosen)
    {
        g.setColour(colours::accent);
        g.fillRect(box.reduced(3));
    }
    g.setColour(e.folder ? colours::textDim : colours::text);
    g.setFont(uiFont(13.0F, e.folder));
    g.drawText(e.name + "  (" + juce::String(e.tracks) + ")", x + 22, 0, width - x - 26, height, juce::Justification::centredLeft, true);
}

void UsbExport::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < getNumRows() && !running)
    {
        auto& e = entries[static_cast<size_t>(row)];
        e.chosen = !e.chosen;
        // A folder takes its contents with it.
        if (e.folder)
            for (size_t i = static_cast<size_t>(row) + 1; i < entries.size() && entries[i].depth > e.depth; ++i) entries[i].chosen = e.chosen;
        list.repaint();
    }
    updateSummary();
}

void UsbExport::updateSummary()
{
    std::vector<juce::int64> chosen;
    for (const auto& e : entries)
        if (e.chosen) chosen.push_back(e.id);
    std::set<juce::int64> tracks;
    for (const auto id : chosen)
        for (const auto t : Collection::shared().tracksIn(id)) tracks.insert(t);
    double bytes = 0.0;
    for (const auto t : tracks)
        if (const auto* track = Collection::shared().find(t)) bytes += static_cast<double>(track->fileSize);
    summary.setText(juce::String(static_cast<int>(tracks.size())) + " tracks, about " + size(bytes), juce::dontSendNotification);
    exportButton.setEnabled(!tracks.empty() && !running);
}

void UsbExport::start()
{
    const int drive = driveBox.getSelectedItemIndex();
    if (drive < 0 || drive >= drives.size() || running) return;
    std::vector<juce::int64> chosen;
    for (const auto& e : entries)
        if (e.chosen) chosen.push_back(e.id);
    if (chosen.empty()) return;
    const juce::File root(drives[drive].getProperty("root", "").toString());
    const bool sync = modeBox.getSelectedId() == 1;
    const bool eject = ejectToggle.getToggleState();
    const auto request = Collection::shared().exportRequest(chosen, root, sync);
    // Analysis waits while the stick is written (both read every track).
    Collection::shared().setPaused(true);
    running = true;
    stop->store(false);
    exportButton.setEnabled(false);
    cancelButton.setButtonText("Stop");
    progressBar.setVisible(true);
    progressText.setText("Starting...", juce::dontSendNotification);
    pool.addJob(
        [this, request, root, eject, alive = alive, stopFlag = stop]
        {
            auto report = rekordbox.exportToStick(
                request,
                [this, alive](const juce::var& p)
                {
                    const double total = static_cast<double>(p.getProperty("total", 0));
                    const double done = static_cast<double>(p.getProperty("done", 0));
                    const auto stage = p.getProperty("stage", "").toString();
                    const auto title = p.getProperty("title", "").toString();
                    juce::MessageManager::callAsync(
                        [this, alive, total, done, stage, title]
                        {
                            if (!alive->load()) return;
                            // Preparing analysis is the first half, the export proper the second.
                            const double fraction = total > 0.0 ? done / total : 0.0;
                            progress = stage == "analysing" ? fraction * 0.5 : 0.5 + fraction * 0.5;
                            progressText.setText(stageName(stage) + (title.isNotEmpty() ? ": " + title : juce::String()),
                                                 juce::dontSendNotification);
                        });
                },
                [stopFlag] { return stopFlag->load(); });
            if (eject && static_cast<bool>(report.getProperty("ok", false)))
                if (auto* o = report.getDynamicObject()) o->setProperty("ejected", rekordbox.eject(root).getProperty("ok", false));
            juce::MessageManager::callAsync(
                [this, report, alive]
                {
                    if (alive->load()) finished(report);
                });
        });
}

void UsbExport::finished(const juce::var& report)
{
    running = false;
    Collection::shared().setPaused(false);
    cancelButton.setButtonText("Close");
    progressBar.setVisible(false);
    if (!static_cast<bool>(report.getProperty("ok", false)))
    {
        const auto error = report.getProperty("error", "the export failed").toString();
        progressText.setText(error, juce::dontSendNotification);
        if (notify) notify("USB export: " + error, true);
        exportButton.setEnabled(true);
        return;
    }
    juce::String text = "Wrote " + juce::String(static_cast<int>(report.getProperty("tracks", 0))) + " tracks and "
                        + juce::String(static_cast<int>(report.getProperty("playlists", 0))) + " playlists";
    if (const int added = report.getProperty("added", 0); added > 0) text << " (" << added << " new)";
    if (const int removed = report.getProperty("removed", 0); removed > 0) text << ", took off " << removed;
    const auto* failed = report.getProperty("failed", {}).getArray();
    if (failed != nullptr && !failed->isEmpty()) text << "; left out " << failed->size() << ": " << (*failed)[0].toString();
    if (static_cast<bool>(report.getProperty("verified", false))) text << ". Verified";
    if (report.hasProperty("ejected")) text << (static_cast<bool>(report.getProperty("ejected", false)) ? ", ejected" : ", couldn't eject");
    progressText.setText(text, juce::dontSendNotification);
    if (notify) notify(text, failed != nullptr && !failed->isEmpty());
}

void UsbExport::close()
{
    if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) window->exitModalState(0);
}

juce::var UsbExport::runForTest(Rekordbox& rb, const std::vector<juce::int64>& lists, const juce::File& stick, bool sync)
{
    return rb.exportToStick(Collection::shared().exportRequest(lists, stick, sync), {}, {});
}

void UsbExport::paint(juce::Graphics& g)
{
    g.fillAll(colours::panel);
    g.setColour(colours::textDim);
    g.setFont(uiFont(12.5F));
    auto r = getLocalBounds().reduced(14);
    g.drawText("Drive", r.removeFromTop(18), juce::Justification::centredLeft);
    r.removeFromTop(30 + 8);
    g.drawText("Playlists to put on it", r.removeFromTop(18), juce::Justification::centredLeft);
}

void UsbExport::resized()
{
    auto r = getLocalBounds().reduced(14);
    r.removeFromTop(18);
    auto drive = r.removeFromTop(30);
    refreshButton.setBounds(drive.removeFromRight(80).reduced(0, 2));
    drive.removeFromRight(6);
    driveBox.setBounds(drive.reduced(0, 2));
    r.removeFromTop(8);
    r.removeFromTop(18);
    auto buttons = r.removeFromBottom(32);
    cancelButton.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(8);
    exportButton.setBounds(buttons.removeFromRight(100));
    r.removeFromBottom(8);
    progressText.setBounds(r.removeFromBottom(22));
    progressBar.setBounds(r.removeFromBottom(18));
    r.removeFromBottom(6);
    ejectToggle.setBounds(r.removeFromBottom(24));
    modeBox.setBounds(r.removeFromBottom(28).reduced(0, 2));
    summary.setBounds(r.removeFromBottom(22));
    r.removeFromBottom(4);
    list.setBounds(r);
}

} // namespace stemcel::native::dj
