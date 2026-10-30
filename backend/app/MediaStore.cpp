#include "MediaStore.h"

#include "AppPaths.h"

namespace stemcel::native::media
{
namespace
{
juce::String extensionForMime(const juce::String& mime)
{
    if (mime == "image/png") return "png";
    if (mime == "image/webp") return "webp";
    if (mime == "image/gif") return "gif";
    return "jpg";
}

juce::String mimeForImage(const juce::File& file)
{
    juce::FileInputStream in(file);
    if (!in.openedOk()) return "image/jpeg";
    unsigned char head[4] = {};
    in.read(head, 4);
    if (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') return "image/png";
    if (head[0] == 'G' && head[1] == 'I' && head[2] == 'F') return "image/gif";
    if (head[0] == 'R' && head[1] == 'I' && head[2] == 'F' && head[3] == 'F') return "image/webp";
    return "image/jpeg";
}
} // namespace

juce::File rootFor(const juce::String& projectFile)
{
    if (projectFile.isNotEmpty()) return juce::File(projectFile).getParentDirectory();
    // The Electron app's unsaved workspace: <temp>/Stemcel.
    return systemTempDirectory().getChildFile(appFolderName());
}

bool isSafeId(const juce::String& mediaId)
{
    return mediaId.isNotEmpty() && mediaId.length() <= 128
           && mediaId.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
}

bool save(const juce::File& root, const juce::String& mediaId, const juce::var& metadata, const juce::File& cover)
{
    if (!isSafeId(mediaId)) return false;
    const auto metadataDir = root.getChildFile("metadata");
    if (!metadataDir.createDirectory()) return false;
    auto* record = new juce::DynamicObject();
    record->setProperty("version", 1);
    record->setProperty("metadata", metadata);
    if (cover.existsAsFile())
    {
        const auto mime = mimeForImage(cover);
        const auto coversDir = root.getChildFile("covers");
        coversDir.createDirectory();
        const auto name = mediaId + "." + extensionForMime(mime);
        if (cover.copyFileTo(coversDir.getChildFile(name)))
        {
            auto* c = new juce::DynamicObject();
            c->setProperty("file", name);
            c->setProperty("mimeType", mime);
            record->setProperty("cover", juce::var(c));
        }
    }
    return metadataDir.getChildFile(mediaId + ".json").replaceWithText(juce::JSON::toString(juce::var(record)));
}

juce::var metadata(const juce::File& root, const juce::String& mediaId)
{
    if (!isSafeId(mediaId)) return {};
    const auto file = root.getChildFile("metadata").getChildFile(mediaId + ".json");
    if (!file.existsAsFile()) return {};
    return juce::JSON::parse(file).getProperty("metadata", {});
}

juce::File cover(const juce::File& root, const juce::String& mediaId)
{
    if (!isSafeId(mediaId)) return {};
    const auto file = root.getChildFile("metadata").getChildFile(mediaId + ".json");
    if (!file.existsAsFile()) return {};
    const auto name = juce::JSON::parse(file).getProperty("cover", {}).getProperty("file", {}).toString();
    if (name.isEmpty()) return {};
    return root.getChildFile("covers").getChildFile(juce::File::createLegalFileName(name));
}

void migrate(const juce::File& from, const juce::File& to)
{
    if (from == to) return;
    for (const auto* sub : {"metadata", "covers"})
    {
        const auto src = from.getChildFile(sub);
        if (!src.isDirectory()) continue;
        const auto dst = to.getChildFile(sub);
        dst.createDirectory();
        for (const auto& entry : juce::RangedDirectoryIterator(src, false, "*", juce::File::findFiles))
        {
            const auto target = dst.getChildFile(entry.getFile().getFileName());
            if (!target.existsAsFile()) entry.getFile().copyFileTo(target);
        }
    }
}

} // namespace stemcel::native::media
