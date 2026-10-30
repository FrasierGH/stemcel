#include "LibraryInfoDialog.h"

#include "MediaStore.h"
#include "Theme.h"

namespace stemcel::native
{
namespace
{
juce::var payload(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props) o->setProperty(k, v);
    return juce::var(o);
}

juce::String duration(double ms)
{
    const int total = static_cast<int>(ms / 1000.0 + 0.5);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}
} // namespace

LibraryInfoDialog::LibraryInfoDialog(EngineClient& client, ProjectMirror& projectMirror, const juce::String& id)
    : engine(client), mirror(projectMirror), itemId(id)
{
    audioType.addItem("Detect automatically", 1);
    audioType.addItem("Music (tempo, beats, warping)", 2);
    audioType.addItem("One-shot sample (no tempo)", 3);
    audioType.onChange = [this]
    {
        const juce::StringArray values{"auto", "music", "simple"};
        engine.send("LIBRARY_ITEM_SET_AUDIO_TYPE", payload({{"itemId", itemId}, {"audioType", values[audioType.getSelectedId() - 1]}}));
    };
    hideCover.onClick = [this] { engine.send("LIBRARY_ITEM_SET_COVER_HIDDEN", payload({{"itemId", itemId}, {"hidden", hideCover.getToggleState()}})); };
    chooseCoverButton.onClick = [this] { chooseCover(); };
    showFile.onClick = [this]
    {
        if (const auto* item = mirror.model().findLibraryItem(itemId)) juce::File(item->filePath).revealToUser();
    };
    closeButton.onClick = [this]
    {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
    };
    for (auto* c : std::initializer_list<juce::Component*>{&audioType, &hideCover, &chooseCoverButton, &showFile, &closeButton}) addAndMakeVisible(c);
    mirror.addChangeListener(this);
    refresh();
    setSize(620, 440);
}

LibraryInfoDialog::~LibraryInfoDialog()
{
    mirror.removeChangeListener(this);
}

void LibraryInfoDialog::show(EngineClient& engine, ProjectMirror& mirror, const juce::String& itemId)
{
    if (mirror.model().findLibraryItem(itemId) == nullptr) return;
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new LibraryInfoDialog(engine, mirror, itemId));
    options.dialogTitle = "Audio Info";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

juce::File LibraryInfoDialog::mediaRoot() const
{
    return media::rootFor(mirror.model().filePath);
}

void LibraryInfoDialog::changeListenerCallback(juce::ChangeBroadcaster*)
{
    refresh();
}

void LibraryInfoDialog::refresh()
{
    const auto* item = mirror.model().findLibraryItem(itemId);
    if (item == nullptr) return;
    // Derived items (stems, samples, clips) share their source's media entry.
    juce::String mediaId = item->mediaId;
    for (const auto* cursor = item; mediaId.isEmpty() && cursor != nullptr && cursor->sourceItemId.isNotEmpty();)
    {
        cursor = mirror.model().findLibraryItem(cursor->sourceItemId);
        if (cursor != nullptr) mediaId = cursor->mediaId;
    }
    const auto meta = media::metadata(mediaRoot(), mediaId);
    const bool hidden = static_cast<bool>(item->raw.getProperty("coverArtHidden", false));
    const auto overrideName = item->raw.getProperty("coverArtOverride", {}).toString();
    juce::File coverFile = overrideName.isNotEmpty() ? mediaRoot().getChildFile("covers").getChildFile(juce::File::createLegalFileName(overrideName))
                                                     : media::cover(mediaRoot(), mediaId);
    cover = !hidden && coverFile.existsAsFile() ? juce::ImageFileFormat::loadFrom(coverFile) : juce::Image();

    rows.clear();
    auto add = [this](const juce::String& k, const juce::String& v)
    {
        if (v.isNotEmpty()) rows.set(k, v);
    };
    add("Name", item->displayName());
    add("Title", meta.getProperty("title", {}).toString());
    add("Artist", meta.getProperty("artist", {}).toString());
    add("Album", meta.getProperty("album", {}).toString());
    if (const auto* genres = meta.getProperty("genre", {}).getArray())
    {
        juce::StringArray g;
        for (const auto& v : *genres) g.add(v.toString());
        add("Genre", g.joinIntoString(", "));
    }
    add("Year", meta.getProperty("year", {}).toString());
    add("Kind", item->kind);
    add("Tempo", item->bpm > 0.0 ? juce::String(item->bpm, 2) + " BPM" : juce::String());
    add("Key", item->key);
    add("Length", duration(item->durationMs));
    add("Format", juce::String(item->sampleRate / 1000.0, 1) + " kHz, " + (item->channelCount == 1 ? "mono" : item->channelCount == 2 ? "stereo" : juce::String(item->channelCount) + " ch")
                      + (meta.getProperty("container", {}).toString().isNotEmpty() ? ", " + meta.getProperty("container", {}).toString() : juce::String()));
    add("File", item->filePath);
    if (item->playbackFilePath.isNotEmpty() && item->playbackFilePath != item->filePath) add("Plays from", item->playbackFilePath);

    const auto type = item->raw.getProperty("audioType", {}).toString();
    audioType.setSelectedId(type == "music" ? 2 : type == "simple" ? 3 : 1, juce::dontSendNotification);
    hideCover.setToggleState(hidden, juce::dontSendNotification);
    repaint();
}

void LibraryInfoDialog::chooseCover()
{
    chooser = std::make_unique<juce::FileChooser>("Choose Cover Image", juce::File(), "*.jpg;*.jpeg;*.png;*.webp;*.gif;*.bmp");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto picked = fc.getResult();
                             if (!picked.existsAsFile()) return;
                             // Same naming as the Electron app: covers/override-<itemId>.<ext>.
                             auto ext = picked.getFileExtension().trimCharactersAtStart(".").toLowerCase();
                             if (ext == "jpeg") ext = "jpg";
                             const auto name = "override-" + juce::File::createLegalFileName(itemId) + "." + ext;
                             const auto covers = mediaRoot().getChildFile("covers");
                             covers.createDirectory();
                             if (!picked.copyFileTo(covers.getChildFile(name))) return;
                             engine.inUndoGroup("Change cover art",
                                                [&]
                                                {
                                                    engine.send("LIBRARY_ITEM_SET_COVER_OVERRIDE", payload({{"itemId", itemId}, {"coverFile", name}}));
                                                    engine.send("LIBRARY_ITEM_SET_COVER_HIDDEN", payload({{"itemId", itemId}, {"hidden", false}}));
                                                });
                         });
}

void LibraryInfoDialog::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    const auto art = juce::Rectangle<int>(16, 16, 180, 180);
    g.setColour(colours::zinc800);
    g.fillRoundedRectangle(art.toFloat(), 6.0f);
    if (cover.isValid()) g.drawImage(cover, art.toFloat(), juce::RectanglePlacement::centred);
    else
    {
        g.setColour(colours::textFaint);
        g.setFont(uiFont(12.0f));
        g.drawText("No cover art", art, juce::Justification::centred);
    }
    auto area = juce::Rectangle<int>(214, 16, getWidth() - 230, getHeight() - 100);
    for (const auto& key : rows.getAllKeys())
    {
        auto r = area.removeFromTop(22);
        g.setColour(colours::textDim);
        g.setFont(uiFont(12.0f));
        g.drawText(key, r.removeFromLeft(80), juce::Justification::centredLeft);
        g.setColour(colours::text);
        g.setFont(uiFont(13.0f));
        g.drawText(rows[key], r, juce::Justification::centredLeft, true);
    }
}

void LibraryInfoDialog::resized()
{
    auto bottom = getLocalBounds().reduced(16).removeFromBottom(30);
    closeButton.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(8);
    showFile.setBounds(bottom.removeFromRight(100));
    auto controls = juce::Rectangle<int>(16, 206, 180, 100);
    chooseCoverButton.setBounds(controls.removeFromTop(28));
    controls.removeFromTop(4);
    hideCover.setBounds(controls.removeFromTop(24));
    audioType.setBounds(16, getHeight() - 90, 280, 26);
}

} // namespace stemcel::native
