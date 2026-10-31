#include "Tracklist.h"

#include "MediaStore.h"

#include <algorithm>
#include <map>
#include <regex>

namespace stemcel::native::tracklist
{
namespace
{
std::string utf8(const juce::String& s)
{
    return s.toStdString();
}

juce::String fromUtf8(const std::string& s)
{
    return juce::String::fromUTF8(s.c_str());
}

juce::String replace(const juce::String& text, const std::regex& pattern, const char* with = "")
{
    return fromUtf8(std::regex_replace(utf8(text), pattern, with));
}

juce::String tidy(juce::String s)
{
    static const std::regex spaces(R"(\s{2,})");
    s = replace(s.trim(), spaces, " ");
    // Separators left dangling at either end ("Title -", "- Title").
    while (s.startsWithChar('-') || s.endsWithChar('-') || s.startsWithChar(',') || s.endsWithChar(','))
        s = s.trimCharactersAtStart("-,").trimCharactersAtEnd("-,").trim();
    return s;
}

// A bracket's contents that only say how the file was got: video and lyric tags, quality,
// downloads, sites and labels' catalogue numbers.
bool isNoise(const juce::String& inner)
{
    static const std::regex noise(
        R"(^\s*((official\s*)?(music\s*|lyrics?\s*|audio\s*|hd\s*|hq\s*|4k\s*)*(video|audio|visuali[sz]er|lyrics?)(\s*video)?|official|lyrics?|hq|hd|4k|uhd|\d{2,4}\s*k(bps)?|\d+\s*kbps|free\s*(download|dl)|out\s*now|premiere|explicit|clean|full\s*song|mp3|flac|wav|m4a|aac|youtube|soundcloud|bandcamp|\d{4})\s*$)",
        std::regex::icase);
    static const std::regex site(R"((www\.|\.(com|net|org|co|io|fm|ru|uk|de)\b|https?:))", std::regex::icase);
    // ABC123, CAT-001, [BEAT042]: letters and digits only, both present.
    static const std::regex catalogue(R"(^\s*[A-Z]{2,8}[- ]?\d{2,5}[A-Z]?\s*$)");
    const auto s = utf8(inner);
    return std::regex_search(s, noise) || std::regex_search(s, site) || std::regex_search(s, catalogue);
}

bool isFeature(const juce::String& inner)
{
    static const std::regex feat(R"(^\s*(feat\.?|ft\.?|featuring)\s+)", std::regex::icase);
    return std::regex_search(utf8(inner), feat);
}

bool hasMixName(const juce::String& s)
{
    static const std::regex mix(R"(\b(mix|remix|edit|vip|dub|bootleg|rework|flip|version|instrumental|acapella|remaster(ed)?)\b)",
                                std::regex::icase);
    return std::regex_search(utf8(s), mix);
}

// "feat. X" written outside brackets: "Title feat. X" or "Artist ft X".
std::pair<juce::String, juce::String> splitFeature(const juce::String& s)
{
    static const std::regex feat(R"(^(.*?)\s+((?:feat\.?|ft\.?|featuring)\s+.*)$)", std::regex::icase);
    std::smatch m;
    const auto text = utf8(s);
    if (std::regex_match(text, m, feat)) return {fromUtf8(m[1].str()), fromUtf8(m[2].str())};
    return {s, {}};
}
} // namespace

namespace
{
// Pronouns and possessives end titles ("Stand by Me"); they are never an artist's name.
bool soundsLikeArtist(const juce::String& s)
{
    static const juce::StringArray notArtists{"me", "you", "him", "her", "us", "them", "it", "my", "your", "his", "our", "their"};
    const auto first = s.upToFirstOccurrenceOf(" ", false, false).toLowerCase();
    return s.isNotEmpty() && !juce::CharacterFunctions::isLowerCase(s[0]) && !notArtists.contains(first);
}

Guess parse(juce::String s, bool hasExtension)
{
    s = s.trim();
    // The extension, when it is one (".mp3", not "Vol. 2").
    static const std::regex extension(R"(\.[A-Za-z0-9]{2,5}$)");
    if (hasExtension && std::regex_search(utf8(s), extension))
        s = s.upToLastOccurrenceOf(".", false, false);
    s = s.replaceCharacter('_', ' ');

    // Brackets: noise goes, a featured artist moves to the artist, the rest (mix names) stays.
    juce::StringArray features;
    {
        static const std::regex bracket(R"(\s*[\(\[\{]([^\(\)\[\]\{\}]*)[\)\]\}])");
        juce::String kept;
        auto text = utf8(s);
        std::smatch m;
        while (std::regex_search(text, m, bracket))
        {
            kept << fromUtf8(m.prefix().str());
            const auto inner = fromUtf8(m[1].str()).trim();
            if (isFeature(inner)) features.add(inner);
            else if (!inner.isEmpty() && !isNoise(inner)) kept << " (" << inner << ")";
            text = m.suffix().str();
        }
        kept << fromUtf8(text);
        s = kept;
    }
    // Sites written plainly ("www.example.com", "example.net").
    static const std::regex site(R"(\s*\b(www\.)?[\w-]+\.(com|net|org|ru)\b)", std::regex::icase);
    s = replace(s, site);
    s = tidy(s);

    // Leading track numbers: "01 ", "01.", "1 - ", "12)", and vinyl sides "A1 ", "B2.".
    static const std::regex trackNumber(R"(^(0\d{1,2}\s*[.\-)]?\s+|\d{1,3}\s*[.)]\s*|\d{1,3}\s+-\s+|[A-Da-d][1-9]\s*[.\-)]?\s+)(?=\S))");
    s = tidy(replace(s, trackNumber));

    Guess guess;
    // " - ", " -- ", and the en and em dashes (as UTF-8 bytes: std::regex matches bytes).
    static const std::regex dashUtf8("\\s+(?:-{1,2}|\xE2\x80\x93|\xE2\x80\x94)\\s+");
    std::smatch m;
    const auto text = utf8(s);
    if (std::regex_search(text, m, dashUtf8))
    {
        guess.artist = tidy(fromUtf8(m.prefix().str()));
        guess.title = tidy(fromUtf8(m.suffix().str()));
        // "Title (Extended Mix) - Artist": the mix name says which side is the title.
        if (hasMixName(guess.artist) && !hasMixName(guess.title)) std::swap(guess.artist, guess.title);
    }
    else
    {
        // "Title by Artist", but not "Stand by Me": two words or more of title, then an artist.
        static const std::regex by(R"(^(\S+\s+.+)\s+by\s+(.+)$)", std::regex::icase);
        if (std::regex_match(text, m, by) && soundsLikeArtist(fromUtf8(m[2].str())))
        {
            guess.title = tidy(fromUtf8(m[1].str()));
            guess.artist = tidy(fromUtf8(m[2].str()));
        }
        else
        {
            guess.title = s;
        }
    }

    // A featured artist written in the title goes with the artist.
    if (guess.artist.isNotEmpty())
    {
        auto [title, feature] = splitFeature(guess.title);
        // Unless a mix name follows it ("Title ft X (Y Remix)"): that part stays in the title.
        if (feature.isNotEmpty() && !hasMixName(feature))
        {
            guess.title = tidy(title);
            features.add(feature);
        }
        for (const auto& f : features) guess.artist << " " << f;
        guess.artist = tidy(guess.artist);
    }
    else if (!features.isEmpty())
    {
        guess.title << " (" << features.joinIntoString(", ") << ")";
    }
    if (guess.title.isEmpty()) std::swap(guess.title, guess.artist);
    return guess;
}
} // namespace

Guess fromFileName(const juce::String& fileName)
{
    return parse(fileName, true);
}

juce::String Entry::line() const
{
    const auto total = static_cast<juce::int64>(juce::jmax(0.0, std::floor(atMs / 1000.0)));
    const auto h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    const auto two = [](juce::int64 v) { return juce::String(v).paddedLeft('0', 2); };
    const auto time = h > 0 ? juce::String(h) + ":" + two(m) + ":" + two(s) : two(m) + ":" + two(s);
    return time + "  " + (artist.isNotEmpty() ? artist + " - " + title : title);
}

std::vector<Entry> fromProject(const ProjectModel& project, double fromMs, double toMs, double leadInMs)
{
    const bool anySolo = std::any_of(project.tracks.begin(), project.tracks.end(), [](const TrackModel& t) { return t.soloed; });
    // The first moment each source is heard.
    std::map<juce::String, double> firstMs;
    for (const auto& track : project.tracks)
    {
        if (track.muted || (anySolo && !track.soloed)) continue;
        for (const auto& clip : track.clips)
        {
            const auto* item = project.findLibraryItem(clip.libraryItemId);
            if (item == nullptr) continue;
            const double end = clip.offsetMs + clip.effectiveDurationMs;
            if (end <= fromMs || clip.offsetMs >= toMs) continue;
            auto key = item->id;
            if (item->kind == "stem" && project.findLibraryItem(item->sourceItemId) != nullptr) key = item->sourceItemId;
            const double at = juce::jmax(0.0, clip.offsetMs - fromMs);
            if (const auto it = firstMs.find(key); it == firstMs.end() || at < it->second) firstMs[key] = at;
        }
    }

    const auto root = media::rootFor(project.filePath);
    std::vector<Entry> entries;
    for (const auto& [itemId, at] : firstMs)
    {
        const auto* item = project.findLibraryItem(itemId);
        Entry e;
        e.atMs = at + leadInMs;
        const auto meta = item->mediaId.isNotEmpty() ? media::metadata(root, item->mediaId) : juce::var();
        const auto tagTitle = meta.getProperty("title", {}).toString().trim();
        const auto tagArtist = meta.getProperty("artist", {}).toString().trim();
        if (tagTitle.isNotEmpty() && tagArtist.isNotEmpty())
        {
            e.artist = tagArtist;
            e.title = tagTitle;
        }
        else
        {
            // A title tag alone is often "Artist - Title" itself; otherwise the file name.
            const auto fileName = item->fileName.isNotEmpty() ? item->fileName : juce::File(item->filePath).getFileName();
            auto guess = tagTitle.isNotEmpty() ? parse(tagTitle, false) : fromFileName(fileName);
            if (guess.title.isEmpty()) guess = parse(item->displayName(), false);
            e.artist = tagArtist.isNotEmpty() ? tagArtist : guess.artist;
            e.title = guess.title;
        }
        entries.push_back(e);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.atMs < b.atMs; });
    return entries;
}

juce::String toText(const std::vector<Entry>& entries)
{
    juce::StringArray lines;
    for (const auto& e : entries) lines.add(e.line());
    return lines.joinIntoString("\n");
}

} // namespace stemcel::native::tracklist
