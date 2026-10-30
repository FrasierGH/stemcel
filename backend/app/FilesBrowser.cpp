#include "FilesBrowser.h"

#include "KeyDetection.h"
#include "Theme.h"

namespace stemcel::native
{
namespace
{
constexpr int kMaxSearchResults = 2000;

juce::String formatDuration(double ms)
{
    if (ms <= 0.0) return {};
    const int total = static_cast<int>(ms / 1000.0 + 0.5);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}
} // namespace

// One folder in the tree; children are listed when first opened.
class FilesBrowser::FolderItem final : public juce::TreeViewItem
{
  public:
    FolderItem(FilesBrowser& owner, juce::File dir, bool isRoot) : browser(owner), folder(std::move(dir)), root(isRoot) {}

    bool mightContainSubItems() override { return true; }
    juce::String getUniqueName() const override { return folder.getFullPathName(); }
    int getItemHeight() const override { return 22; }

    void paintItem(juce::Graphics& g, int width, int height) override
    {
        if (isSelected()) g.fillAll(colours::sky600.withAlpha(0.35f));
        g.setColour(root ? colours::text : colours::textDim);
        g.setFont(uiFont(13.0f, root));
        g.drawText(root ? folder.getFileName() + "  (" + folder.getParentDirectory().getFullPathName() + ")"
                        : folder.getFileName(),
                   4, 0, width - 4, height, juce::Justification::centredLeft, true);
    }

    void itemOpennessChanged(bool isNowOpen) override
    {
        if (!isNowOpen || listed) return;
        listed = true;
        juce::Array<juce::File> dirs;
        for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findDirectories))
            if (!entry.isHidden()) dirs.add(entry.getFile());
        dirs.sort();
        for (const auto& d : dirs) addSubItem(new FolderItem(browser, d, false));
    }

    void itemSelectionChanged(bool isNowSelected) override
    {
        if (isNowSelected) browser.showFolder(folder);
    }

    const juce::File& file() const { return folder; }
    bool isRootFolder() const { return root; }

  private:
    FilesBrowser& browser;
    juce::File folder;
    bool root;
    bool listed = false;
};

FilesBrowser::FilesBrowser(LibraryContext& context) : ctx(context)
{
    tree.setRootItemVisible(false);
    tree.setDefaultOpenness(false);
    tree.setColour(juce::TreeView::backgroundColourId, colours::zinc950);
    tree.setIndentSize(14);
    addAndMakeVisible(tree);

    auto& header = table.getHeader();
    header.addColumn("", 1, 26, 26, 26, juce::TableHeaderComponent::notResizableOrSortable);
    header.addColumn("File", 2, 260, 80, -1, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Title", 3, 180, 60, -1, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Artist", 4, 150, 60, -1, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("BPM", 5, 60, 40, 90, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Key", 6, 80, 40, 120, juce::TableHeaderComponent::defaultFlags);
    header.addColumn("Length", 7, 60, 40, 90, juce::TableHeaderComponent::defaultFlags);
    table.setColour(juce::ListBox::backgroundColourId, colours::zinc900);
    table.getHeader().setColour(juce::TableHeaderComponent::backgroundColourId, colours::zinc800);
    table.getHeader().setColour(juce::TableHeaderComponent::textColourId, colours::textDim);
    table.setRowHeight(24);
    table.setMultipleSelectionEnabled(true);
    table.setModel(this);
    addAndMakeVisible(table);

    filter.setTextToShowWhenEmpty("Search this folder", colours::textFaint);
    filter.onTextChange = [this] { startTimer(250); };
    addAndMakeVisible(filter);

    addButton.onClick = [this] { addFolder(); };
    removeButton.onClick = [this] { removeSelectedFolder(); };
    addAndMakeVisible(addButton);
    addAndMakeVisible(removeButton);

    hint.setText("Add a folder of samples or music to browse it here.", juce::dontSendNotification);
    hint.setJustificationType(juce::Justification::centred);
    hint.setColour(juce::Label::textColourId, colours::textFaint);
    addChildComponent(hint);

    ctx.audition.addChangeListener(this);
    rebuildTree();
    setWantsKeyboardFocus(true);
}

FilesBrowser::~FilesBrowser()
{
    ++*generation;
    tagPool.removeAllJobs(true, 5000);
    ctx.audition.removeChangeListener(this);
    tree.setRootItem(nullptr);
}

juce::StringArray FilesBrowser::roots() const
{
    juce::StringArray out;
    if (const auto* list = ctx.prefs.get("ui", "fileBrowserFolders", {}).getArray())
        for (const auto& v : *list) out.add(v.toString());
    return out;
}

void FilesBrowser::rebuildTree()
{
    tree.setRootItem(nullptr);
    treeRoot = std::make_unique<FolderItem>(*this, juce::File(), true);
    for (const auto& r : roots())
        if (juce::File(r).isDirectory()) treeRoot->addSubItem(new FolderItem(*this, juce::File(r), true));
    tree.setRootItem(treeRoot.get());
    hint.setVisible(treeRoot->getNumSubItems() == 0);
    if (treeRoot->getNumSubItems() > 0 && tree.getNumSelectedItems() == 0) treeRoot->getSubItem(0)->setSelected(true, true);
}

void FilesBrowser::addFolder()
{
    chooser = std::make_unique<juce::FileChooser>("Add a folder to browse");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto dir = fc.getResult();
                             if (!dir.isDirectory()) return;
                             auto list = roots();
                             if (list.contains(dir.getFullPathName())) return;
                             list.add(dir.getFullPathName());
                             juce::Array<juce::var> values;
                             for (const auto& s : list) values.add(s);
                             ctx.prefs.set("ui", "fileBrowserFolders", values);
                             ctx.prefs.save();
                             rebuildTree();
                         });
}

void FilesBrowser::removeSelectedFolder()
{
    auto* item = dynamic_cast<FolderItem*>(tree.getSelectedItem(0));
    while (item != nullptr && !item->isRootFolder()) item = dynamic_cast<FolderItem*>(item->getParentItem());
    if (item == nullptr || item->file() == juce::File()) return;
    auto list = roots();
    list.removeString(item->file().getFullPathName());
    juce::Array<juce::var> values;
    for (const auto& s : list) values.add(s);
    ctx.prefs.set("ui", "fileBrowserFolders", values);
    ctx.prefs.save();
    folder = juce::File();
    rows.clear();
    table.updateContent();
    rebuildTree();
}

void FilesBrowser::showFolder(const juce::File& dir)
{
    folder = dir;
    refreshRows();
}

void FilesBrowser::timerCallback()
{
    stopTimer();
    refreshRows();
}

void FilesBrowser::refreshRows()
{
    ++*generation;
    rows.clear();
    const auto query = filter.getText().trim().toLowerCase();
    if (folder.isDirectory())
    {
        // A search walks the whole tree under the folder; otherwise just its files.
        const bool recursive = query.isNotEmpty();
        for (const auto& entry : juce::RangedDirectoryIterator(folder, recursive, "*", juce::File::findFiles))
        {
            const auto f = entry.getFile();
            if (entry.isHidden() || !AudioImporter::isAudioFile(f)) continue;
            if (recursive && !f.getFileName().toLowerCase().contains(query)) continue;
            rows.push_back({f});
            if (static_cast<int>(rows.size()) >= kMaxSearchResults) break;
        }
        std::sort(rows.begin(), rows.end(),
                  [](const Row& a, const Row& b) { return a.file.getFileName().compareNatural(b.file.getFileName()) < 0; });
    }
    table.updateContent();
    table.repaint();
    loadTagsFor(0);
}

void FilesBrowser::loadTagsFor(size_t from)
{
    // Tags load in the background, a page at a time, and land if the listing is unchanged.
    const int gen = *generation;
    std::vector<juce::File> files;
    for (size_t i = from; i < rows.size() && i < from + 64; ++i) files.push_back(rows[i].file);
    if (files.empty()) return;
    tagPool.addJob(
        [this, files, from, gen, generationRef = generation]
        {
            struct Tag
            {
                juce::String title, artist, key;
                double bpm, durationMs;
            };
            std::vector<Tag> tags;
            for (const auto& f : files)
            {
                if (*generationRef != gen) return;
                const auto t = ctx.rekordbox.readTags(f);
                tags.push_back({t.getProperty("title", {}).toString(), t.getProperty("artist", {}).toString(),
                                normaliseKeyName(t.getProperty("key", {}).toString()),
                                static_cast<double>(t.getProperty("bpm", 0.0)),
                                static_cast<double>(t.getProperty("durationMs", 0.0))});
            }
            juce::MessageManager::callAsync(
                [this, tags, from, gen, generationRef]
                {
                    if (*generationRef != gen) return;
                    for (size_t i = 0; i < tags.size() && from + i < rows.size(); ++i)
                    {
                        auto& row = rows[from + i];
                        row.title = tags[i].title;
                        row.artist = tags[i].artist;
                        row.key = tags[i].key;
                        row.bpm = tags[i].bpm;
                        row.durationMs = tags[i].durationMs;
                        row.tagsLoaded = true;
                    }
                    table.repaint();
                    loadTagsFor(from + tags.size());
                });
        });
}

int FilesBrowser::getNumRows()
{
    return static_cast<int>(rows.size());
}

void FilesBrowser::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll(selected ? colours::sky600.withAlpha(0.35f) : (row % 2 ? colours::zinc900 : colours::zinc950));
}

void FilesBrowser::paintCell(juce::Graphics& g, int row, int column, int width, int height, bool)
{
    if (row < 0 || row >= static_cast<int>(rows.size())) return;
    const auto& r = rows[static_cast<size_t>(row)];
    if (column == 1)
    {
        const bool playing = ctx.audition.current() == r.file;
        g.setColour(playing ? colours::emerald500 : colours::textFaint);
        juce::Path icon;
        const float cx = width / 2.0f, cy = height / 2.0f;
        if (playing) icon.addRectangle(cx - 4, cy - 4, 8, 8);
        else icon.addTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 5, cy);
        g.fillPath(icon);
        return;
    }
    juce::String text;
    switch (column)
    {
    case 2: text = r.file.getParentDirectory() == folder ? r.file.getFileName() : r.file.getRelativePathFrom(folder); break;
    case 3: text = r.title; break;
    case 4: text = r.artist; break;
    case 5: text = r.bpm > 0.0 ? juce::String(r.bpm, 1) : juce::String(); break;
    case 6: text = r.key; break;
    case 7: text = formatDuration(r.durationMs); break;
    default: break;
    }
    g.setColour(column == 2 ? colours::text : colours::textDim);
    g.setFont(uiFont(13.0f));
    g.drawText(text, 6, 0, width - 8, height, juce::Justification::centredLeft, true);
}

std::vector<ImportRequest> FilesBrowser::selectedRequests() const
{
    std::vector<ImportRequest> out;
    for (int i = 0; i < table.getNumSelectedRows(); ++i)
    {
        const int row = table.getSelectedRow(i);
        if (row < 0 || row >= static_cast<int>(rows.size())) continue;
        const auto& r = rows[static_cast<size_t>(row)];
        ImportRequest request{r.file, {}};
        request.hints.bpm = r.bpm;
        out.push_back(std::move(request));
    }
    return out;
}

juce::var FilesBrowser::getDragSourceDescription(const juce::SparseSet<int>&)
{
    return importDragDescription(selectedRequests());
}

void FilesBrowser::cellClicked(int row, int column, const juce::MouseEvent& e)
{
    if (row < 0 || row >= static_cast<int>(rows.size())) return;
    if (column == 1)
    {
        ctx.audition.toggle(rows[static_cast<size_t>(row)].file);
        return;
    }
    if (e.mods.isPopupMenu())
    {
        if (!table.isRowSelected(row)) table.selectRow(row);
        juce::PopupMenu menu;
        menu.addItem("Import to library", [this] { ctx.importer.import(selectedRequests()); });
        menu.addItem("Audition", [this, row] { ctx.audition.toggle(rows[static_cast<size_t>(row)].file); });
        menu.addItem("Show file", [this, row] { rows[static_cast<size_t>(row)].file.revealToUser(); });
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }
}

void FilesBrowser::cellDoubleClicked(int, int, const juce::MouseEvent&)
{
    ctx.importer.import(selectedRequests());
}

void FilesBrowser::returnKeyPressed(int)
{
    ctx.importer.import(selectedRequests());
}

bool FilesBrowser::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey && table.getNumSelectedRows() > 0)
    {
        const int row = table.getSelectedRow(0);
        if (row >= 0 && row < static_cast<int>(rows.size())) ctx.audition.toggle(rows[static_cast<size_t>(row)].file);
        return true;
    }
    return false;
}

void FilesBrowser::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void FilesBrowser::resized()
{
    auto area = getLocalBounds();
    auto left = area.removeFromLeft(juce::jmin(300, area.getWidth() / 3));
    auto buttons = left.removeFromBottom(28).reduced(2);
    addButton.setBounds(buttons.removeFromLeft(buttons.getWidth() * 2 / 3).reduced(1, 0));
    removeButton.setBounds(buttons.reduced(1, 0));
    tree.setBounds(left);
    hint.setBounds(left);
    area.removeFromLeft(4);
    filter.setBounds(area.removeFromTop(26).reduced(0, 2));
    table.setBounds(area);
}

} // namespace stemcel::native
