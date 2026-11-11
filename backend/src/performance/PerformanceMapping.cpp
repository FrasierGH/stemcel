#include "PerformanceMapping.h"

#include "Log.h"
#include "midi/MidiControllerProfiles.h"

#include <map>
#include <mutex>

namespace stemcel::perf
{
namespace
{
struct ControlName
{
    Control control;
    const char* name;
};

constexpr ControlName kControlNames[] = {
    {Control::play, "play"},
    {Control::playShift, "playShift"},
    {Control::cue, "cue"},
    {Control::cueShift, "cueShift"},
    {Control::sync, "sync"},
    {Control::syncLong, "syncLong"},
    {Control::syncShift, "syncShift"},
    {Control::tempo, "tempo"},
    {Control::keyLock, "keyLock"},
    {Control::quantize, "quantize"},
    {Control::loopIn, "loopIn"},
    {Control::loopInLong, "loopInLong"},
    {Control::loopOut, "loopOut"},
    {Control::reloopExit, "reloopExit"},
    {Control::reloopShift, "reloopShift"},
    {Control::callPrev, "callPrev"},
    {Control::callNext, "callNext"},
    {Control::callPrevShift, "callPrevShift"},
    {Control::callNextShift, "callNextShift"},
    {Control::jogTouch, "jogTouch"},
    {Control::jogScratch, "jogScratch"},
    {Control::jogBend, "jogBend"},
    {Control::jogSearch, "jogSearch"},
    {Control::padMode, "padMode"},
    {Control::pad, "pad"},
    {Control::padShift, "padShift"},
    {Control::trim, "trim"},
    {Control::eqHi, "eqHi"},
    {Control::eqMid, "eqMid"},
    {Control::eqLow, "eqLow"},
    {Control::filter, "filter"},
    {Control::fader, "fader"},
    {Control::hpCue, "hpCue"},
    {Control::hpCueShift, "hpCueShift"},
    {Control::faderStartPlay, "faderStartPlay"},
    {Control::faderStartCue, "faderStartCue"},
    {Control::load, "load"},
    {Control::loadShift, "loadShift"},
    {Control::beatJump, "beatJump"},
    {Control::beatLoop, "beatLoop"},
    {Control::keyShift, "keyShift"},
    {Control::hotCue, "hotCue"},
    {Control::hotCueDelete, "hotCueDelete"},
    {Control::seek, "seek"},
    {Control::crossfader, "crossfader"},
    {Control::masterLevel, "masterLevel"},
    {Control::masterCue, "masterCue"},
    {Control::masterCueShift, "masterCueShift"},
    {Control::hpMix, "hpMix"},
    {Control::hpLevel, "hpLevel"},
    {Control::browse, "browse"},
    {Control::browseShift, "browseShift"},
    {Control::browsePress, "browsePress"},
    {Control::browsePressShift, "browsePressShift"},
    {Control::fxBeatPrev, "fxBeatPrev"},
    {Control::fxBeatNext, "fxBeatNext"},
    {Control::fxBpmAuto, "fxBpmAuto"},
    {Control::fxTap, "fxTap"},
    {Control::fxSelect, "fxSelect"},
    {Control::fxSelectPrev, "fxSelectPrev"},
    {Control::fxChannel, "fxChannel"},
    {Control::fxLevel, "fxLevel"},
    {Control::fxOn, "fxOn"},
    {Control::fxRelease, "fxRelease"},
    {Control::colorFxSelect, "colorFxSelect"},
    {Control::releaseFxSelect, "releaseFxSelect"},
    {Control::fxTypeSelect, "fxTypeSelect"},
    {Control::samplerBank, "samplerBank"},
    {Control::samplerVolume, "samplerVolume"},
    {Control::samplerLoad, "samplerLoad"},
};

constexpr const char* kPadModeNames[] = {"hotCue",   "padFx1", "beatJump", "sampler",
                                         "keyboard", "padFx2", "beatLoop", "keyShift"};

int parseHexByte(const juce::String& token)
{
    if (token.length() != 2 || !token.containsOnly("0123456789abcdefABCDEF")) return -1;
    return token.getHexValue32();
}

std::vector<juce::uint8> parseBytes(const juce::String& text)
{
    std::vector<juce::uint8> bytes;
    for (const auto& token : juce::StringArray::fromTokens(text, " ", ""))
    {
        if (token.isEmpty()) continue;
        const int b = parseHexByte(token);
        if (b < 0) return {};
        bytes.push_back(static_cast<juce::uint8>(b));
    }
    return bytes;
}

// "9n 0B" → one (status, data1) per deck it applies to. `deck` pins a deck (0 = every deck,
// or a global address when the status names its channel).
struct Address
{
    int deck = 0;
    int status = 0;
    int data1 = 0;
};

bool parseAddress(const juce::String& text, int pinnedDeck, const std::vector<int>& deckChannels,
                  const std::vector<std::array<int, 2>>& padChannels, std::vector<Address>& out)
{
    const auto tokens = juce::StringArray::fromTokens(text.trim(), " ", "");
    if (tokens.size() != 2) return false;
    const int data1 = parseHexByte(tokens[1]);
    if (data1 < 0 || data1 > 127) return false;
    const auto status = tokens[0];
    const int decks = static_cast<int>(deckChannels.size());
    auto forDecks = [&](auto channelOf)
    {
        for (int d = 1; d <= decks; ++d)
            if (pinnedDeck == 0 || pinnedDeck == d) out.push_back({d, channelOf(d), data1});
    };
    if (status == "P" || status == "S")
    {
        const int which = status == "P" ? 0 : 1;
        if (padChannels.size() < static_cast<size_t>(decks)) return false;
        forDecks([&](int d) { return 0x90 | padChannels[static_cast<size_t>(d - 1)][static_cast<size_t>(which)]; });
        return true;
    }
    if (status.length() == 2 && (status[1] == 'n' || status[1] == 'N'))
    {
        const int high = parseHexByte(status.substring(0, 1) + "0");
        if (high < 0x80) return false;
        forDecks([&](int d) { return high | deckChannels[static_cast<size_t>(d - 1)]; });
        return true;
    }
    const int s = parseHexByte(status);
    if (s < 0x80) return false;
    out.push_back({pinnedDeck, s, data1});
    return true;
}

std::mutex& cacheMutex()
{
    static std::mutex m;
    return m;
}
} // namespace

const char* controlName(Control control) noexcept
{
    for (const auto& entry : kControlNames)
        if (entry.control == control) return entry.name;
    return "none";
}

Control controlFromName(const juce::String& name) noexcept
{
    for (const auto& entry : kControlNames)
        if (name == entry.name) return entry.control;
    return Control::none;
}

const char* padModeName(PadMode mode) noexcept
{
    const auto i = static_cast<size_t>(mode);
    return i < std::size(kPadModeNames) ? kPadModeNames[i] : "hotCue";
}

PadMode padModeFromName(const juce::String& name) noexcept
{
    for (size_t i = 0; i < std::size(kPadModeNames); ++i)
        if (name == kPadModeNames[i]) return static_cast<PadMode>(i);
    return PadMode::count;
}

bool isAppControl(Control control) noexcept
{
    switch (control)
    {
        case Control::load:
        case Control::loadShift:
        case Control::browse:
        case Control::browseShift:
        case Control::browsePress:
        case Control::browsePressShift:
        case Control::masterCueShift:
        case Control::samplerLoad:
            return true;
        default:
            return false;
    }
}

std::shared_ptr<const Mapping> Mapping::fromFile(const juce::File& file, juce::String& error)
{
    const auto json = juce::JSON::parse(file.loadFileAsString());
    if (json.isVoid())
    {
        error = "not JSON";
        return nullptr;
    }
    return fromJson(json, error);
}

std::shared_ptr<const Mapping> Mapping::fromJson(const juce::var& json, juce::String& error)
{
    auto mapping = std::shared_ptr<Mapping>(new Mapping());
    mapping->mappingName = json.getProperty("name", "").toString();
    if (const auto* models = json.getProperty("models", {}).getArray())
        for (const auto& m : *models) mapping->models.push_back(m.toString().trim().toUpperCase());
    if (mapping->mappingName.isEmpty() || mapping->models.empty())
    {
        error = "a mapping needs a name and models";
        return nullptr;
    }
    mapping->ticksPerTurn = static_cast<int>(json.getProperty("jogTicksPerTurn", 720));

    std::vector<int> deckChannels;
    if (const auto* channels = json.getProperty("deckChannels", {}).getArray())
        for (const auto& c : *channels) deckChannels.push_back(juce::jlimit(0, 15, static_cast<int>(c)));
    if (deckChannels.empty() || deckChannels.size() > static_cast<size_t>(kDecks))
    {
        error = "deckChannels must list 1.." + juce::String(kDecks) + " channels";
        return nullptr;
    }
    std::vector<std::array<int, 2>> padChannels;
    if (const auto* pads = json.getProperty("padChannels", {}).getArray())
        for (const auto& p : *pads)
            if (const auto* pair = p.getArray(); pair != nullptr && pair->size() == 2)
                padChannels.push_back({juce::jlimit(0, 15, static_cast<int>((*pair)[0])),
                                       juce::jlimit(0, 15, static_cast<int>((*pair)[1]))});

    if (const auto* init = json.getProperty("init", {}).getArray())
        for (const auto& frame : *init)
        {
            auto bytes = parseBytes(frame.toString());
            if (bytes.empty())
            {
                error = "bad init frame: " + frame.toString();
                return nullptr;
            }
            mapping->init.push_back(std::move(bytes));
        }

    const auto* controls = json.getProperty("controls", {}).getArray();
    if (controls == nullptr)
    {
        error = "no controls";
        return nullptr;
    }
    for (const auto& entry : *controls)
    {
        const auto controlText = entry.getProperty("control", "").toString();
        const auto control = controlFromName(controlText);
        if (control == Control::none)
        {
            error = "unknown control: " + controlText;
            return nullptr;
        }
        Binding binding;
        binding.control = control;
        binding.index = static_cast<int>(entry.getProperty("index", 0));
        binding.pressOnly = static_cast<bool>(entry.getProperty("pressOnly", false));
        if (entry.hasProperty("mode"))
        {
            binding.mode = padModeFromName(entry.getProperty("mode", "").toString());
            if (binding.mode == PadMode::count)
            {
                error = "unknown pad mode in " + controlText;
                return nullptr;
            }
        }
        const auto encodingText = entry.getProperty("encoding", "").toString();
        const bool hasLsb = entry.hasProperty("lsb");
        const int count = juce::jlimit(1, 64, static_cast<int>(entry.getProperty("count", 1)));
        std::vector<Address> addresses;
        if (!parseAddress(entry.getProperty("midi", "").toString(), static_cast<int>(entry.getProperty("deck", 0)),
                          deckChannels, padChannels, addresses))
        {
            error = "bad midi address in " + controlText + ": " + entry.getProperty("midi", "").toString();
            return nullptr;
        }
        const bool isCc = !addresses.empty() && (addresses.front().status & 0xF0) == 0xB0;
        if (encodingText == "rel64") binding.encoding = Encoding::rel64;
        else if (encodingText == "relSigned") binding.encoding = Encoding::relSigned;
        else if (encodingText == "button") binding.encoding = Encoding::button;
        else binding.encoding = isCc ? Encoding::absolute : Encoding::button;
        binding.fourteenBit = hasLsb;
        const int lsb = hasLsb ? parseHexByte(entry.getProperty("lsb", "").toString()) : -1;
        if (hasLsb && (lsb < 0 || lsb > 127))
        {
            error = "bad lsb in " + controlText;
            return nullptr;
        }

        for (const auto& address : addresses)
            for (int i = 0; i < count; ++i)
            {
                const int data1 = address.data1 + i;
                if (data1 > 127) break;
                auto b = binding;
                b.deck = address.deck;
                if (count > 1) b.index = i;
                mapping->table[static_cast<size_t>(address.status)][static_cast<size_t>(data1)].push_back(b);
                if (hasLsb)
                {
                    auto low = b;
                    low.isLsb = true;
                    low.msbData1 = data1;
                    mapping->table[static_cast<size_t>(address.status)][static_cast<size_t>(lsb)].push_back(low);
                }
                // Note Off arrives on its own status (0x8n) for some controllers.
                if ((address.status & 0xF0) == 0x90)
                    mapping->table[static_cast<size_t>(address.status & 0x8F)][static_cast<size_t>(data1)].push_back(b);
            }
    }

    if (const auto* leds = json.getProperty("leds", {}).getArray())
        for (const auto& entry : *leds)
        {
            const auto state = entry.getProperty("state", "").toString();
            const int count = juce::jlimit(1, 64, static_cast<int>(entry.getProperty("count", 1)));
            const int pinned = static_cast<int>(entry.getProperty("deck", 0));
            juce::Array<juce::var> midiList;
            if (const auto* list = entry.getProperty("midi", {}).getArray()) midiList = *list;
            else midiList.add(entry.getProperty("midi", ""));
            // Resolve per deck: one LedBinding per deck the entry covers.
            std::map<int, LedBinding> perDeck;
            for (const auto& midi : midiList)
            {
                std::vector<Address> addresses;
                if (!parseAddress(midi.toString(), pinned, deckChannels, padChannels, addresses))
                {
                    error = "bad led address for " + state + ": " + midi.toString();
                    return nullptr;
                }
                for (const auto& a : addresses)
                {
                    auto& led = perDeck[a.deck];
                    led.state = state;
                    led.deck = a.deck;
                    led.count = count;
                    led.addresses.push_back({a.status, a.data1});
                }
            }
            for (auto& [deck, led] : perDeck) mapping->ledBindings.push_back(std::move(led));
        }
    return mapping;
}

std::vector<Mapping::Led> Mapping::leds(const juce::String& state, int deck, int index) const
{
    std::vector<Led> result;
    for (const auto& binding : ledBindings)
    {
        if (binding.state != state || binding.deck != deck) continue;
        if (index < 0 || index >= binding.count) continue;
        for (auto led : binding.addresses)
        {
            led.data1 += index;
            if (led.data1 <= 127) result.push_back(led);
        }
    }
    return result;
}

std::shared_ptr<const Mapping> Mapping::forDevice(const juce::String& deviceName)
{
    static std::vector<std::shared_ptr<const Mapping>> loaded;
    static bool scanned = false;
    {
        const std::scoped_lock lock(cacheMutex());
        if (!scanned)
        {
            scanned = true;
            juce::Array<juce::File> files;
            midiMappingDirectory().getChildFile("performance").findChildFiles(
                files, juce::File::findFiles, false, "*.json", juce::File::FollowSymlinks::no);
            for (const auto& file : files)
            {
                juce::String error;
                if (auto mapping = fromFile(file, error)) loaded.push_back(std::move(mapping));
                else stemcel::log::error("performance", "invalid performance mapping " + file.getFileName() + ": " + error);
            }
        }
    }
    const auto upper = deviceName.toUpperCase();
    std::shared_ptr<const Mapping> best;
    int bestLength = -1;
    for (const auto& mapping : loaded)
        for (const auto& model : mapping->models)
        {
            int start = upper.indexOf(model);
            while (start >= 0)
            {
                const auto end = start + model.length();
                const bool before = start == 0 || !juce::CharacterFunctions::isLetterOrDigit(upper[start - 1]);
                const bool after = end == upper.length() || !juce::CharacterFunctions::isLetterOrDigit(upper[end]);
                if (before && after && model.length() > bestLength)
                {
                    best = mapping;
                    bestLength = model.length();
                }
                start = upper.indexOf(start + 1, model);
            }
        }
    return best;
}

Mapping::Translator::Translator(std::shared_ptr<const Mapping> m) : mapping(std::move(m))
{
    for (auto& row : msb) row.fill(0);
    for (auto& row : lsb) row.fill(0);
}

int Mapping::Translator::translate(const juce::uint8* bytes, int size, Event* out, int maxEvents)
{
    if (mapping == nullptr || size < 3 || maxEvents <= 0) return 0;
    const int status = bytes[0];
    const int data1 = bytes[1] & 0x7F;
    const int data2 = bytes[2] & 0x7F;
    const auto& bindings = mapping->table[static_cast<size_t>(status)][static_cast<size_t>(data1)];
    int written = 0;
    const bool noteOff = (status & 0xF0) == 0x80;
    for (const auto& b : bindings)
    {
        if (written >= maxEvents) break;
        Event e;
        e.control = b.control;
        e.deck = static_cast<std::int8_t>(b.deck);
        e.index = static_cast<std::int8_t>(b.index);
        e.mode = b.mode;
        switch (b.encoding)
        {
            case Encoding::button:
                e.value = (!noteOff && data2 > 0) ? 1.0F : 0.0F;
                if (b.pressOnly && e.value < 0.5F) continue;
                break;
            case Encoding::rel64:
                e.value = static_cast<float>(data2 - 64);
                if (data2 == 64) continue;
                break;
            case Encoding::relSigned:
                e.value = static_cast<float>(data2 < 64 ? data2 : data2 - 128);
                if (data2 == 0) continue;
                break;
            case Encoding::absolute:
                if (b.fourteenBit)
                {
                    auto& held = msb[static_cast<size_t>(status & 0x0F)];
                    if (!b.isLsb)
                    {
                        // The MSB alone moves the control (with the last LSB), so a controller
                        // that never sends the LSB still works; the LSB then refines it.
                        held[static_cast<size_t>(data1)] = data2;
                        const int low = lsb[static_cast<size_t>(status & 0x0F)][static_cast<size_t>(data1)];
                        e.value = static_cast<float>((data2 << 7) | low) / 16383.0F;
                    }
                    else
                    {
                        const int high = held[static_cast<size_t>(b.msbData1)];
                        lsb[static_cast<size_t>(status & 0x0F)][static_cast<size_t>(b.msbData1)] = data2;
                        e.value = static_cast<float>((high << 7) | data2) / 16383.0F;
                    }
                }
                else
                {
                    e.value = static_cast<float>(data2) / 127.0F;
                }
                break;
        }
        out[written++] = e;
    }
    return written;
}

} // namespace stemcel::perf
