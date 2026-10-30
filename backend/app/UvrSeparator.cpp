#include "UvrSeparator.h"
#include "AppPaths.h"

#include "Log.h"

namespace stemcel::native::uvr
{

const std::vector<Recipe>& recipes()
{
    // From the guide's current recommendations, limited to models anyone can download.
    static const std::vector<Recipe> list{
        {"best", "6 stems, best of both: Mel-Band RoFormer vocals + BS-Roformer SW",
         "Each stem from the stronger model: Kim's Mel-Band RoFormer for the vocals (the highest-scoring vocal "
         "model), BS-Roformer SW for the rest. Two runs.",
         "BS-Roformer-SW.ckpt", {}, {"vocals", "drums", "bass", "guitar", "piano", "other"}, "vocals_mel_band_roformer.ckpt"},
        {"six", "6 stems: vocals, drums, bass, guitar, piano, other",
         "BS-Roformer SW, the guide's six-stem model, in one run.",
         "BS-Roformer-SW.ckpt", {}, {"vocals", "drums", "bass", "guitar", "piano", "other"}},
        {"vocalsTop", "Vocals and the rest: highest-scoring vocals",
         "Kim's Mel-Band RoFormer (12.6 dB vocals on the model list's own test): the vocals, and everything else.",
         "vocals_mel_band_roformer.ckpt", {}, {"vocals", "other"}},
        {"vocals", "Vocals and instrumental: balanced",
         "unwa's Resurrection and Big Beta 6X, tuned for full-sounding vocals: they keep more bleed, so they "
         "score below Kim's Mel-Band RoFormer.", {}, "vocal_balanced",
         {"vocals", "instrumental"}},
        {"vocalsClean", "Vocals and instrumental: cleanest vocals",
         "Ensemble tuned for the least instrument bleed in the vocals (Revive 2 and others).", {}, "vocal_clean",
         {"vocals", "instrumental"}},
        {"instrumental", "Instrumental: fullest",
         "Ensemble of unwa v1e+ and becruily: keeps the most of the instruments.", {}, "instrumental_full",
         {"instrumental", "vocals"}},
        {"karaoke", "Lead vocals out (karaoke)",
         "Three-model karaoke ensemble: lead vocals on one stem, everything else (backing vocals too) on the other.",
         {}, "karaoke", {"vocals", "instrumental"}},
        {"drumsep", "Drum kit pieces: kick, snare, toms, hi-hat, ride, crash",
         "MDX23C DrumSep by aufr33 and jarredou. Use it on a drums stem.", "MDX23C-DrumSep-aufr33-jarredou.ckpt", {},
         {"kick", "snare", "toms", "hh", "ride", "crash"}},
        {"dereverb", "Remove reverb", "Mel-Roformer de-reverb by anvuew. Best on vocals.",
         "dereverb_mel_band_roformer_anvuew_sdr_19.1729.ckpt", {}, {"noreverb", "reverb"}},
        {"custom", "Any model (by file name)", "Any model audio-separator lists (audio-separator --list_models).", {}, {}, {}},
    };
    return list;
}

const Recipe* findRecipe(const juce::String& id)
{
    for (const auto& r : recipes())
        if (r.id == id) return &r;
    return nullptr;
}

juce::File findExecutable(AppPreferences& prefs)
{
    const auto chosen = prefs.get("stems", "audioSeparatorPath", "").toString();
    if (chosen.isNotEmpty() && juce::File(chosen).existsAsFile()) return juce::File(chosen);
#if JUCE_WINDOWS
    const juce::String name = "audio-separator.exe";
    const juce::String separator = ";";
#else
    const juce::String name = "audio-separator";
    const juce::String separator = ":";
#endif
    for (const auto& dir : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("PATH", {}), separator, ""))
        if (const auto f = juce::File::isAbsolutePath(dir.trim()) ? juce::File(dir.trim()).getChildFile(name) : juce::File(); f.existsAsFile())
            return f;
    // Where pip --user and pipx put scripts when that is not on PATH.
    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    juce::Array<juce::File> candidates{home.getChildFile(".local/bin").getChildFile(name)};
#if JUCE_WINDOWS
    for (const auto* base : {"APPDATA", "LOCALAPPDATA"})
    {
        const juce::File root(juce::SystemStats::getEnvironmentVariable(base, {}));
        for (const auto& python : {root.getChildFile("Python"), root.getChildFile("Programs").getChildFile("Python")})
            for (const auto& version : python.findChildFiles(juce::File::findDirectories, false, "Python3*"))
                candidates.add(version.getChildFile("Scripts").getChildFile(name));
    }
#elif JUCE_MAC
    candidates.add(juce::File("/opt/homebrew/bin").getChildFile(name));
    candidates.add(juce::File("/usr/local/bin").getChildFile(name));
    for (const auto& version : home.getChildFile("Library/Python").findChildFiles(juce::File::findDirectories, false, "3.*"))
        candidates.add(version.getChildFile("bin").getChildFile(name));
#endif
    for (const auto& c : candidates)
        if (c.existsAsFile()) return c;
    return {};
}

juce::File modelDir()
{
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(appFolderName()).getChildFile("Models").getChildFile("uvr");
}

juce::String installCommand()
{
    // pipx first, as most machines lack it; audioread because Python 3.13 installs leave it out.
#if JUCE_WINDOWS
    // For PowerShell 5.1 (Windows' default shell), which has no &&. `python -m pipx` works
    // before pip's user Scripts folder is on PATH.
    return "python -m pip install --user pipx; python -m pipx install \"audio-separator[cpu]\"; "
           "python -m pipx inject audio-separator audioread";
#elif JUCE_MAC
    // Homebrew's Python refuses pip --user installs (PEP 668), so pipx comes from Homebrew.
    return "brew install pipx && pipx install \"audio-separator[cpu]\" && pipx inject audio-separator audioread";
#else
    return "python3 -m pip install --user pipx && python3 -m pipx install \"audio-separator[cpu]\" && "
           "python3 -m pipx inject audio-separator audioread";
#endif
}

Acceleration accelerationFrom(const juce::String& toolOutput)
{
    if (toolOutput.contains("CUDA is available in Torch") || toolOutput.contains("setting Torch device to MPS")) return Acceleration::gpu;
    if (toolOutput.contains("running in CPU mode") || toolOutput.contains("+cpu")) return Acceleration::cpu;
    return Acceleration::unknown;
}

void checkAcceleration(const juce::File& executable, std::function<void(Acceleration)> done)
{
    static std::map<juce::String, Acceleration> known;
    const auto key = executable.getFullPathName();
    if (const auto it = known.find(key); it != known.end())
    {
        done(it->second);
        return;
    }
    juce::Thread::launch(
        [executable, key, done = std::move(done)]
        {
            juce::ChildProcess process;
            auto result = Acceleration::unknown;
            if (process.start(juce::StringArray{executable.getFullPathName(), "--env_info"},
                              juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
                result = accelerationFrom(process.readAllProcessOutput());
            juce::MessageManager::callAsync(
                [key, result, done]
                {
                    known[key] = result;
                    done(result);
                });
        });
}

juce::String gpuInstallCommand()
{
#if JUCE_MAC
    return {};
#else
  #if JUCE_WINDOWS
    // nvidia-smi comes with the NVIDIA driver.
    const bool nvidia = juce::File::getSpecialLocation(juce::File::windowsSystemDirectory).getChildFile("nvidia-smi.exe").existsAsFile();
    const juce::String python = "python";
  #else
    const bool nvidia = juce::File("/usr/bin/nvidia-smi").existsAsFile();
    const juce::String python = "python3";
  #endif
    if (!nvidia) return {};
    return python + " -m pipx runpip audio-separator install --force-reinstall --no-deps torch torchvision "
                    "--index-url https://download.pytorch.org/whl/cu130";
#endif
}

juce::File lastRunLog()
{
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(appFolderName()).getChildFile("Logs").getChildFile("uvr-last-run.log");
}

juce::String stemFromFileName(const juce::String& fileName)
{
    // audio-separator names outputs "<input>_(<Stem>)_<model>.<ext>".
    const int close = fileName.lastIndexOfChar(')');
    const int open = close > 0 ? fileName.substring(0, close).lastIndexOfChar('(') : -1;
    if (open < 0) return {};
    return fileName.substring(open + 1, close).trim().toLowerCase();
}

juce::String stemLabel(const juce::String& key)
{
    static const std::map<juce::String, juce::String> labels{
        {"vocals", "Vocals"}, {"instrumental", "Instrumental"}, {"drums", "Drums"}, {"bass", "Bass"}, {"guitar", "Guitar"},
        {"piano", "Piano"}, {"other", "Other"}, {"kick", "Kick"}, {"snare", "Snare"}, {"toms", "Toms"}, {"hh", "Hi-hat"},
        {"ride", "Ride"}, {"crash", "Crash"}, {"noreverb", "Dry"}, {"no reverb", "Dry"}, {"dry", "Dry"}, {"reverb", "Reverb"},
        {"no dry", "Reverb"}, {"noise", "Noise"}, {"no noise", "Denoised"}};
    if (const auto it = labels.find(key); it != labels.end()) return it->second;
    return key.isEmpty() ? juce::String("Stem") : key.substring(0, 1).toUpperCase() + key.substring(1);
}

double progressFrom(const juce::String& text)
{
    // tqdm bars: " 42%|####      | 21/50 [...]"
    double last = -1.0;
    for (int at = text.indexOfChar('%'); at > 0; at = text.indexOfChar(at + 1, '%'))
    {
        int start = at;
        while (start > 0 && juce::CharacterFunctions::isDigit(text[start - 1])) --start;
        if (start < at && at - start <= 3) last = juce::jlimit(0.0, 100.0, text.substring(start, at).getDoubleValue());
    }
    return last;
}

Job::Job(juce::File exe, juce::File in, juce::File out, Recipe r)
    : juce::Thread("UVR separation"), executable(std::move(exe)), input(std::move(in)), outputDir(std::move(out)), recipe(std::move(r))
{
}

Job::~Job()
{
    alive->store(false);
    cancel();
    stopThread(15000);
}

void Job::cancel()
{
    cancelled = true;
    const juce::ScopedLock lock(processLock);
    process.kill();
}

void Job::readLine(const juce::String& line)
{
    if (const auto a = accelerationFrom(line); a != Acceleration::unknown) acceleration = a;
    // An ensemble lists its models on the line after "Multiple models specified for ensembling:".
    if (line.trimStart().startsWithChar('[') && (line.contains(".ckpt") || line.contains(".onnx") || line.contains(".pth") || line.contains(".yaml")))
        models = juce::jmax(1, line.retainCharacters(",").length() + 1);
    if (line.contains("Processing with model:") || (models == 1 && line.contains("Loading model")))
    {
        model = juce::jmin(models, model + 1);
        downloading = false;
    }
    if (line.containsIgnoreCase("download")) downloading = true;
    else if (line.contains("it/s") || line.contains("s/it") || line.containsIgnoreCase("separation")) downloading = false;
    double p = progressFrom(line);
    if (p < 0.0) return;
    juce::String stage = "download";
    if (!downloading)
    {
        const int at = juce::jmax(1, model);
        p = ((at - 1) + p / 100.0) / models * 100.0;
        stage = juce::String("separate")
                + (acceleration == Acceleration::gpu ? " (GPU)" : acceleration == Acceleration::cpu ? " (CPU, slow)" : "")
                + (models > 1 ? " " + juce::String(at) + "/" + juce::String(models) : juce::String());
    }
    juce::MessageManager::callAsync([alive = alive, cb = onProgress, p, stage] { if (alive->load() && cb) cb(p, stage); });
}

void Job::finish(bool ok, const juce::String& error, std::map<juce::String, juce::File> stems)
{
    juce::MessageManager::callAsync(
        [alive = alive, cb = onDone, ok, error, stems = std::move(stems)]() mutable
        {
            if (alive->load() && cb) cb(ok, error, std::move(stems));
        });
}

void Job::run()
{
    outputDir.createDirectory();
    modelDir().createDirectory();
    juce::StringArray args{executable.getFullPathName(), input.getFullPathName()};
    if (recipe.preset.isNotEmpty()) args.addArray({"--ensemble_preset", recipe.preset});
    else args.addArray({"-m", recipe.model});
    args.addArray({"--model_file_dir", modelDir().getFullPathName(), "--output_dir", outputDir.getFullPathName(),
                   "--output_format", "WAV", "--log_level", "info"});
    log::info("uvr", "run " + args.joinIntoString(" "));

    {
        const juce::ScopedLock lock(processLock);
        if (cancelled || !process.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            finish(false, cancelled ? juce::String("cancelled") : "Could not start " + executable.getFullPathName(), {});
            return;
        }
    }
    lastRunLog().getParentDirectory().createDirectory();
    lastRunLog().deleteFile();
    juce::FileOutputStream logFile(lastRunLog());
    if (logFile.openedOk()) logFile << args.joinIntoString(" ") << "\n\n";
    juce::String tail;    // the end of the output, for the error message
    juce::String partial; // a line still being written
    // Small reads: the pipe read blocks until it has them all, and a tqdm update is ~100 bytes.
    char buffer[128];
    while (!threadShouldExit() && !cancelled)
    {
        const int n = process.readProcessOutput(buffer, sizeof(buffer));
        if (n <= 0)
        {
            if (!process.isRunning()) break;
            wait(50);
            continue;
        }
        const auto chunk = juce::String::fromUTF8(buffer, n);
        if (logFile.openedOk()) logFile << chunk;
        tail = (tail + chunk).getLastCharacters(4000);
        partial += chunk;
        for (int end = partial.indexOfAnyOf("\r\n"); end >= 0; end = partial.indexOfAnyOf("\r\n"))
        {
            readLine(partial.substring(0, end));
            partial = partial.substring(end + 1);
        }
    }
    if (logFile.openedOk()) logFile.flush();
    if (cancelled || threadShouldExit())
    {
        process.kill();
        if (cancelled) finish(false, "cancelled", {});
        return;
    }
    const auto exitCode = process.getExitCode();
    if (exitCode != 0)
    {
        // The last error-looking line says the most.
        juce::String reason = "audio-separator exited with code " + juce::String(exitCode);
        for (const auto& line : juce::StringArray::fromLines(tail))
            if (line.containsIgnoreCase("error") || line.containsIgnoreCase("exception")) reason = line.trim();
        log::warn("uvr", "failed: " + tail.getLastCharacters(800));
        finish(false, reason + " (the full output is in " + lastRunLog().getFullPathName() + ")", {});
        return;
    }
    // Name the outputs by their stem, so the folder reads like the built-in engine's.
    std::map<juce::String, juce::File> stems;
    for (const auto& f : outputDir.findChildFiles(juce::File::findFiles, false, "*.wav"))
    {
        const auto key = stemFromFileName(f.getFileName());
        if (key.isEmpty() || stems.count(key) != 0) continue;
        auto target = outputDir.getChildFile(juce::File::createLegalFileName(stemLabel(key)) + ".wav");
        if (target != f && (target.existsAsFile() || !f.moveFileTo(target))) target = f;
        stems[key] = target;
    }
    log::info("uvr", "done: " + juce::String(static_cast<int>(stems.size())) + " stems in " + outputDir.getFullPathName()
                         + (acceleration == Acceleration::cpu ? " (ran on the CPU)" : ""));
    if (stems.empty()) finish(false, "audio-separator wrote no stems (its output is in " + lastRunLog().getFullPathName() + ")", {});
    else finish(true, {}, std::move(stems));
}

} // namespace stemcel::native::uvr
