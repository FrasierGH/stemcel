#include "BuiltInFx.h"

#include "DjFx.h"

namespace stemcel::plugins
{
namespace
{
constexpr const char* kBeatPrefix = "djfx:beat:";
constexpr const char* kColorPrefix = "djfx:color:";

// "LOW CUT ECHO" -> "Low Cut Echo"; "DUB ECHO" -> "Dub Echo".
juce::String titleCase(const juce::String& upper)
{
    juce::String out;
    bool start = true;
    for (const auto c : upper)
    {
        out << (start ? juce::String::charToString(c) : juce::String::charToString(juce::CharacterFunctions::toLowerCase(c)));
        start = c == ' ' || c == '/' || c == '(';
    }
    return out;
}

juce::PluginDescription describe(bool beat, int type)
{
    juce::PluginDescription d;
    d.name = titleCase(beat ? djfx::beatFxName(type) : djfx::colorFxName(type));
    d.descriptiveName = d.name + (beat ? " (Beat FX)" : " (Sound Color FX)");
    d.pluginFormatName = kBuiltInFormatName;
    d.category = beat ? "Beat FX" : "Sound Color FX";
    d.manufacturerName = "Stemcel";
    d.version = "1";
    d.fileOrIdentifier = juce::String(beat ? kBeatPrefix : kColorPrefix) + juce::String(type);
    d.uniqueId = d.deprecatedUid = (beat ? 0x5354000 : 0x5354100) + type;
    d.isInstrument = false;
    d.numInputChannels = 2;
    d.numOutputChannels = 2;
    return d;
}

// A parameter, its value normalised 0..1. `labels` name its steps (Beats); `steps` 2 is a
// switch (On).
class Param final : public juce::AudioPluginInstance::HostedParameter
{
  public:
    Param(juce::String idIn, juce::String nameIn, float defaultIn, int stepsIn = 0, juce::StringArray labelsIn = {},
          std::function<juce::String(float)> textIn = {})
        : id(std::move(idIn)), name(std::move(nameIn)), def(defaultIn), steps(stepsIn), labels(std::move(labelsIn)),
          text(std::move(textIn)), value(defaultIn)
    {
    }
    float getValue() const override { return value.load(std::memory_order_relaxed); }
    void setValue(float v) override { value.store(juce::jlimit(0.0F, 1.0F, v), std::memory_order_relaxed); }
    float getDefaultValue() const override { return def; }
    juce::String getName(int maximumLength) const override { return name.substring(0, maximumLength); }
    juce::String getLabel() const override { return {}; }
    int getNumSteps() const override { return steps > 0 ? steps : juce::AudioProcessor::getDefaultNumParameterSteps(); }
    bool isDiscrete() const override { return steps > 0; }
    bool isBoolean() const override { return steps == 2; }
    juce::String getParameterID() const override { return id; }
    juce::String getText(float v, int) const override
    {
        if (!labels.isEmpty()) return labels[index(v)];
        if (steps == 2) return v >= 0.5F ? "On" : "Off";
        return text ? text(v) : juce::String(juce::roundToInt(v * 100.0F)) + "%";
    }
    float getValueForText(const juce::String& s) const override
    {
        if (!labels.isEmpty()) return labels.size() > 1 ? static_cast<float>(juce::jmax(0, labels.indexOf(s))) / static_cast<float>(labels.size() - 1) : 0.0F;
        return s.getFloatValue() / 100.0F;
    }
    int index(float v) const { return juce::jlimit(0, juce::jmax(0, labels.size() - 1), juce::roundToInt(v * static_cast<float>(juce::jmax(1, labels.size() - 1)))); }

  private:
    juce::String id, name;
    float def;
    int steps;
    juce::StringArray labels;
    std::function<juce::String(float)> text;
    std::atomic<float> value;
};

// One effect. Its parameters are what the slot's knobs and automation lanes drive.
class DjFxPlugin final : public juce::AudioPluginInstance
{
  public:
    DjFxPlugin(juce::PluginDescription desc, bool beatFx, int fxType)
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          description(std::move(desc)), beat(beatFx), type(fxType)
    {
        auto add = [this](std::unique_ptr<Param> p)
        {
            auto* raw = p.get();
            addHostedParameter(std::move(p));
            return raw;
        };
        if (beat)
        {
            strength = add(std::make_unique<Param>("strength", "Strength", 0.5F));
            juce::StringArray steps;
            for (int i = 0; i < static_cast<int>(djfx::kBeatSteps.size()); ++i) steps.add(djfx::beatStepLabel(i));
            beats = add(std::make_unique<Param>("beats", "Beats",
                                                static_cast<float>(djfx::kDefaultBeatStep) / static_cast<float>(steps.size() - 1),
                                                steps.size(), steps));
            on = add(std::make_unique<Param>("on", "On", 1.0F, 2));
        }
        else
        {
            // Bipolar, as the FILTER knob: centre off, one character each way.
            strength = add(std::make_unique<Param>("strength", "Strength", 0.75F, 0, juce::StringArray{},
                                                   [](float v)
                                                   {
                                                       const float b = v * 2.0F - 1.0F;
                                                       return std::abs(b) < 0.01F ? juce::String("Off")
                                                                                  : (b > 0 ? "+" : "-") + juce::String(juce::roundToInt(std::abs(b) * 100.0F)) + "%";
                                                   }));
        }
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override { d = description; }
    const juce::String getName() const override { return description.name; }

    void prepareToPlay(double sampleRate, int) override
    {
        if (beat)
        {
            beatUnit.allocate(sampleRate);
            beatUnit.setType(type);
        }
        else
        {
            colorUnit.allocate(sampleRate);
            colorUnit.setType(type);
        }
    }
    void releaseResources() override {}
    void reset() override
    {
        if (beat) beatUnit.reset();
        else colorUnit.reset();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const juce::ScopedNoDenormals noDenormals;
        if (buffer.getNumChannels() < 2) return;
        double bpm = 120.0;
        if (auto* head = getPlayHead())
            if (const auto position = head->getPosition())
                if (const auto b = position->getBpm(); b.hasValue() && *b > 20.0) bpm = *b;
        auto* l = buffer.getWritePointer(0);
        auto* r = buffer.getWritePointer(1);
        const int n = buffer.getNumSamples();
        if (beat)
        {
            if (!beatUnit.allocated()) return;
            beatUnit.setBpm(bpm);
            beatUnit.setLevel(strength->getValue());
            beatUnit.setBeatStep(beats->index(beats->getValue()));
            beatUnit.setOn(on->getValue() >= 0.5F);
            beatUnit.process(l, r, n);
        }
        else
        {
            if (!colorUnit.allocated()) return;
            colorUnit.setBpm(bpm);
            colorUnit.setAmount(strength->getValue() * 2.0F - 1.0F);
            colorUnit.process(l, r, n);
        }
    }

    double getTailLengthSeconds() const override { return beat ? 4.0 : 0.5; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
               && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } // Track FX's knobs edit it
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    // The parameters' values, in their order.
    void getStateInformation(juce::MemoryBlock& data) override
    {
        juce::MemoryOutputStream out(data, false);
        out.writeInt(1); // version
        out.writeInt(getParameters().size());
        for (auto* p : getParameters()) out.writeFloat(p->getValue());
    }
    void setStateInformation(const void* data, int size) override
    {
        juce::MemoryInputStream in(data, static_cast<size_t>(size), false);
        if (in.readInt() != 1) return;
        const int count = in.readInt();
        auto params = getParameters();
        for (int i = 0; i < count && i < params.size(); ++i) params[i]->setValueNotifyingHost(in.readFloat());
    }

  private:
    juce::PluginDescription description;
    const bool beat;
    const int type;
    Param* strength = nullptr;
    Param* beats = nullptr;
    Param* on = nullptr;
    djfx::BeatFxUnit beatUnit;
    djfx::ColorFxUnit colorUnit;
};

bool parse(const juce::String& identifier, bool& beat, int& type)
{
    if (identifier.startsWith(kBeatPrefix)) beat = true;
    else if (identifier.startsWith(kColorPrefix)) beat = false;
    else return false;
    const auto number = identifier.fromLastOccurrenceOf(":", false, false);
    if (number.isEmpty() || !number.containsOnly("0123456789")) return false;
    type = number.getIntValue();
    return beat ? type < static_cast<int>(djfx::BeatFx::count) && type != static_cast<int>(djfx::BeatFx::spinBack)
                : type < static_cast<int>(djfx::ColorFx::count);
}
} // namespace

juce::Array<juce::PluginDescription> builtInDescriptions()
{
    juce::Array<juce::PluginDescription> out;
    for (const auto type : djfx::kBeatFxOrder) out.add(describe(true, static_cast<int>(type)));
    for (int type = 0; type < static_cast<int>(djfx::ColorFx::count); ++type) out.add(describe(false, type));
    return out;
}

bool isBuiltIn(const juce::PluginDescription& description)
{
    return description.pluginFormatName == kBuiltInFormatName;
}

bool isBuiltInIdentifier(const juce::String& identifier)
{
    bool beat = false;
    int type = 0;
    return parse(identifier, beat, type);
}

std::unique_ptr<juce::AudioPluginInstance> createBuiltIn(const juce::PluginDescription& description, double sampleRate,
                                                         int blockSize)
{
    bool beat = false;
    int type = 0;
    if (!parse(description.fileOrIdentifier, beat, type)) return nullptr;
    auto plugin = std::make_unique<DjFxPlugin>(describe(beat, type), beat, type);
    plugin->setRateAndBufferSizeDetails(sampleRate, blockSize);
    return plugin;
}

} // namespace stemcel::plugins
