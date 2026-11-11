#include "PerfBrowser.h"

#include "Theme.h"

namespace stemcel::native::perf
{
namespace
{
juce::String minutes(double seconds)
{
    const int s = juce::roundToInt(juce::jmax(0.0, seconds));
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}
} // namespace

PerfBrowser::PerfBrowser(ProjectMirror& m, std::function<const RekordboxSnapshot*()> rb) : mirror(m), rekordbox(std::move(rb))
{
    setWantsKeyboardFocus(false);
    refresh();
}

void PerfBrowser::buildTree()
{
    const auto selected = treeCursor >= 0 && treeCursor < static_cast<int>(nodes.size()) ? nodes[static_cast<size_t>(treeCursor)].kind + "/" + nodes[static_cast<size_t>(treeCursor)].id : juce::String();
    nodes.clear();
    nodes.push_back({"Collection", "folder", "collection", 0, true});
    if (expanded.count("collection") != 0)
    {
        nodes.push_back({"All", "all", {}, 1, false});
        nodes.push_back({"Tracks", "tracks", {}, 1, false});
        nodes.push_back({"Stems", "stems", {}, 1, false});
        nodes.push_back({"Samples & clips", "samples", {}, 1, false});
    }
    const auto* snapshot = rekordbox ? rekordbox() : nullptr;
    nodes.push_back({snapshot != nullptr && snapshot->ok ? "rekordbox" : "rekordbox (open its tab to connect)", "folder", "rekordbox", 0, true});
    if (snapshot != nullptr && snapshot->ok && expanded.count("rekordbox") != 0)
    {
        nodes.push_back({"All tracks", "rbAll", {}, 1, false});
        std::function<void(const juce::String&, int)> add = [&](const juce::String& parent, int depth)
        {
            for (const auto* p : snapshot->childrenOf(parent))
            {
                nodes.push_back({p->name, p->isFolder() ? "folder" : "rbList", p->id, depth, p->isFolder()});
                if (p->isFolder() && expanded.count(p->id) != 0) add(p->id, depth + 1);
            }
        };
        add({}, 1);
    }
    treeCursor = juce::jlimit(0, static_cast<int>(nodes.size()) - 1, treeCursor);
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        if (nodes[static_cast<size_t>(i)].kind + "/" + nodes[static_cast<size_t>(i)].id == selected) treeCursor = i;
}

void PerfBrowser::buildRows()
{
    rows.clear();
    if (treeCursor < 0 || treeCursor >= static_cast<int>(nodes.size())) return;
    const auto& node = nodes[static_cast<size_t>(treeCursor)];
    if (node.kind == "all" || node.kind == "tracks" || node.kind == "stems" || node.kind == "samples")
    {
        for (const auto& item : mirror.model().library)
        {
            const bool fits = node.kind == "all" || (node.kind == "tracks" && item.kind == "source") ||
                              (node.kind == "stems" && item.kind == "stem") ||
                              (node.kind == "samples" && (item.kind == "sample" || item.kind == "clip"));
            if (!fits) continue;
            BrowserRow row;
            row.itemId = item.id;
            row.title = item.displayName();
            row.artist = item.raw.getProperty("artist", "").toString();
            row.key = item.key;
            row.bpm = item.bpm;
            row.durationSec = item.durationMs / 1000.0;
            row.path = item.filePath;
            rows.push_back(std::move(row));
        }
        return;
    }
    const auto* snapshot = rekordbox ? rekordbox() : nullptr;
    if (snapshot == nullptr || !snapshot->ok) return;
    auto addTrack = [&](const RekordboxTrack& t)
    {
        BrowserRow row;
        row.rekordboxId = t.id;
        row.title = t.title;
        row.artist = t.artist;
        row.key = t.key;
        row.bpm = t.bpm;
        row.durationSec = t.durationMs / 1000.0;
        row.path = t.path;
        rows.push_back(std::move(row));
    };
    if (node.kind == "rbAll")
        for (const auto& t : snapshot->tracks) addTrack(t);
    else if (node.kind == "rbList")
        if (const auto* list = snapshot->findPlaylist(node.id))
            for (const auto& id : list->trackIds)
                if (const auto* t = snapshot->findTrack(id)) addTrack(*t);
}

void PerfBrowser::refresh()
{
    // Rebuilt when the library changes (cheap enough on a mirror refresh).
    buildTree();
    buildRows();
    listCursor = juce::jlimit(0, juce::jmax(0, static_cast<int>(rows.size()) - 1), listCursor);
    repaint();
}

const BrowserRow* PerfBrowser::selectedRow() const
{
    return listCursor >= 0 && listCursor < static_cast<int>(rows.size()) ? &rows[static_cast<size_t>(listCursor)] : nullptr;
}

void PerfBrowser::browse(int steps)
{
    if (listFocused)
        listCursor = juce::jlimit(0, juce::jmax(0, static_cast<int>(rows.size()) - 1), listCursor + steps);
    else
    {
        treeCursor = juce::jlimit(0, juce::jmax(0, static_cast<int>(nodes.size()) - 1), treeCursor + steps);
        buildRows();
        listCursor = 0;
        listScroll = 0;
    }
    ensureVisible();
    repaint();
}

void PerfBrowser::press(bool shifted)
{
    if (nodes.empty()) return;
    auto& node = nodes[static_cast<size_t>(treeCursor)];
    if (shifted)
    {
        // Close the folder the cursor is in (or on), back in the tree.
        listFocused = false;
        if (node.folder) expanded.erase(node.id);
        else
            for (int i = treeCursor; i >= 0; --i)
                if (nodes[static_cast<size_t>(i)].folder && nodes[static_cast<size_t>(i)].depth < node.depth)
                {
                    expanded.erase(nodes[static_cast<size_t>(i)].id);
                    treeCursor = i;
                    break;
                }
        buildTree();
        buildRows();
    }
    else if (listFocused)
        listFocused = false;
    else if (node.folder)
    {
        if (expanded.count(node.id) != 0) expanded.erase(node.id);
        else expanded.insert(node.id);
        buildTree();
    }
    else
        listFocused = !rows.empty();
    ensureVisible();
    repaint();
}

juce::Rectangle<int> PerfBrowser::treeArea() const
{
    auto r = getLocalBounds().reduced(4);
    return r.removeFromLeft(juce::jmin(260, r.getWidth() / 3));
}

juce::Rectangle<int> PerfBrowser::listArea() const
{
    auto r = getLocalBounds().reduced(4);
    r.removeFromLeft(juce::jmin(260, r.getWidth() / 3) + 6);
    return r;
}

void PerfBrowser::ensureVisible()
{
    const int treeRows = juce::jmax(1, treeArea().getHeight() / rowHeight());
    if (treeCursor < treeScroll) treeScroll = treeCursor;
    if (treeCursor >= treeScroll + treeRows) treeScroll = treeCursor - treeRows + 1;
    const int listRows = juce::jmax(1, (listArea().getHeight() - rowHeight()) / rowHeight());
    if (listCursor < listScroll) listScroll = listCursor;
    if (listCursor >= listScroll + listRows) listScroll = listCursor - listRows + 1;
}

void PerfBrowser::resized() { ensureVisible(); }

void PerfBrowser::paint(juce::Graphics& g)
{
    g.setColour(colours::panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0F);
    const int h = rowHeight();

    auto tree = treeArea();
    drawWell(g, tree.toFloat());
    for (int i = treeScroll, y = tree.getY(); i < static_cast<int>(nodes.size()) && y + h <= tree.getBottom(); ++i, y += h)
    {
        const auto& n = nodes[static_cast<size_t>(i)];
        auto row = juce::Rectangle<int>(tree.getX(), y, tree.getWidth(), h);
        if (i == treeCursor)
        {
            g.setColour((listFocused ? colours::zinc700 : colours::accent).withAlpha(listFocused ? 1.0F : 0.85F));
            g.fillRect(row);
        }
        g.setColour(i == treeCursor && !listFocused ? colours::onAccent : (n.folder ? colours::text : colours::textDim));
        g.setFont(uiFont(13.0F, n.folder));
        const juce::String marker = n.folder ? (expanded.count(n.id) != 0 ? "v " : "> ") : "";
        g.drawText(marker + n.label, row.withTrimmedLeft(6 + n.depth * 14), juce::Justification::centredLeft, true);
    }

    auto list = listArea();
    drawWell(g, list.toFloat());
    auto header = list.removeFromTop(h);
    const int keyW = 56, bpmW = 64, timeW = 52, artistW = juce::jmax(80, list.getWidth() / 4);
    auto columns = [&](juce::Rectangle<int> r)
    {
        struct C { juce::Rectangle<int> title, artist, bpm, key, time; } c;
        c.time = r.removeFromRight(timeW);
        c.key = r.removeFromRight(keyW);
        c.bpm = r.removeFromRight(bpmW);
        c.artist = r.removeFromRight(artistW);
        c.title = r;
        return c;
    };
    {
        auto c = columns(header);
        g.setColour(colours::textFaint);
        g.setFont(uiFont(11.0F, true));
        g.drawText("TITLE", c.title.withTrimmedLeft(6), juce::Justification::centredLeft);
        g.drawText("ARTIST", c.artist, juce::Justification::centredLeft);
        g.drawText("BPM", c.bpm, juce::Justification::centredRight);
        g.drawText("KEY", c.key.withTrimmedLeft(10), juce::Justification::centredLeft);
        g.drawText("TIME", c.time.withTrimmedRight(6), juce::Justification::centredRight);
    }
    if (rows.empty())
    {
        g.setColour(colours::textFaint);
        g.setFont(uiFont(13.0F));
        g.drawText("Nothing here", list, juce::Justification::centred);
    }
    for (int i = listScroll, y = list.getY(); i < static_cast<int>(rows.size()) && y + h <= list.getBottom(); ++i, y += h)
    {
        const auto& r = rows[static_cast<size_t>(i)];
        auto row = juce::Rectangle<int>(list.getX(), y, list.getWidth(), h);
        const bool sel = i == listCursor;
        if (sel)
        {
            g.setColour(listFocused ? colours::accent.withAlpha(0.85F) : colours::zinc700);
            g.fillRect(row);
        }
        auto c = columns(row);
        g.setColour(sel && listFocused ? colours::onAccent : colours::text);
        g.setFont(uiFont(13.0F));
        g.drawText(r.title, c.title.withTrimmedLeft(6), juce::Justification::centredLeft, true);
        g.setColour(sel && listFocused ? colours::onAccent : colours::textDim);
        g.drawText(r.artist, c.artist, juce::Justification::centredLeft, true);
        g.drawText(r.bpm > 0.0 ? juce::String(r.bpm, 2) : "", c.bpm, juce::Justification::centredRight);
        g.drawText(r.key, c.key.withTrimmedLeft(10), juce::Justification::centredLeft);
        g.drawText(minutes(r.durationSec), c.time.withTrimmedRight(6), juce::Justification::centredRight);
    }
}

void PerfBrowser::mouseDown(const juce::MouseEvent& e)
{
    const int h = rowHeight();
    if (treeArea().contains(e.getPosition()))
    {
        const int i = treeScroll + (e.getPosition().y - treeArea().getY()) / h;
        if (i >= 0 && i < static_cast<int>(nodes.size()))
        {
            treeCursor = i;
            listFocused = false;
            if (nodes[static_cast<size_t>(i)].folder) press(false);
            else
            {
                buildRows();
                listCursor = 0;
                listScroll = 0;
            }
        }
    }
    else if (listArea().contains(e.getPosition()))
    {
        const int i = listScroll + (e.getPosition().y - listArea().getY() - h) / h;
        if (i >= 0 && i < static_cast<int>(rows.size()))
        {
            listCursor = i;
            listFocused = true;
            if (e.mods.isPopupMenu() && onLoad)
            {
                juce::PopupMenu menu;
                menu.addItem("Load to deck 1", [this, row = rows[static_cast<size_t>(i)]] { onLoad(row, 1); });
                menu.addItem("Load to deck 2", [this, row = rows[static_cast<size_t>(i)]] { onLoad(row, 2); });
                menu.showMenuAsync(juce::PopupMenu::Options());
            }
        }
    }
    repaint();
}

void PerfBrowser::mouseDoubleClick(const juce::MouseEvent& e)
{
    // A double-click loads to the deck on that side of the screen... the left half → 1.
    if (listArea().contains(e.getPosition()) && onLoad)
        if (const auto* row = selectedRow()) onLoad(*row, e.getPosition().x < getWidth() / 2 ? 1 : 2);
}

void PerfBrowser::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int steps = wheel.deltaY > 0 ? -3 : 3;
    if (treeArea().contains(e.getPosition()))
        treeScroll = juce::jlimit(0, juce::jmax(0, static_cast<int>(nodes.size()) - 1), treeScroll + steps);
    else
        listScroll = juce::jlimit(0, juce::jmax(0, static_cast<int>(rows.size()) - 1), listScroll + steps);
    repaint();
}

} // namespace stemcel::native::perf
