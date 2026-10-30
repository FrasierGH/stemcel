# Native app (JUCE, no Electron)

`StemcelNative` is Stemcel as one native process: a JUCE user interface on top of the
same engine the Electron app drives over its WebSocket bridge. It builds on macOS and
Windows from `backend/`, reads and writes the same `.stemcel` projects, and keeps the
Electron app's preferences, autosave buckets, media store, transcode cache and stem
models, so existing projects and settings carry over.

## Building

```bash
cmake -S backend -B backend/build -DSTEMCEL_BUILD_TESTS=ON
cmake --build backend/build --config Debug --target StemcelNative
```

The app lands in `backend/build/StemcelNative_artefacts/<Config>/` (`Stemcel.exe`, or
`Stemcel.app` on macOS) with its runtime files beside the executable.

The rekordbox reader (`backend/rekordbox_ffi`, Rust) is built by CMake through cargo when
`cargo` is on the path (`-DSTEMCEL_BUILD_REKORDBOX=OFF` skips it; the app then says
rekordbox is unavailable). Its vendored OpenSSL needs a native Perl on Windows: pass
`-DSTEMCEL_PERL=<perl.exe>` when the first `perl` on `PATH` is Git Bash's.

### Smoke test

```bash
Stemcel --smoke-test <work dir>
```

drives the real app through a scripted journey (new project, import, edit, rekordbox
fixture import, FX, automation, crossfade, channel split, recording and scratch sessions,
clipboard, autosave and recovery, mixdown, clip editor) and exits 0 when every step
passes. Results, an arrange-view snapshot and a clip-editor snapshot are written to the
work dir. CI runs it on every platform; the packaging scripts run it on the packaged app.

## Architecture

```
StemcelNative (one process)
├── backend/app/                UI: JUCE components, models mirrored from ProjectState
│     └── EngineClient          in-process commands and events (no WebSocket, no JSON)
├── stemcel_backend_core      the engine and every bridge command handler (shared)
└── stemcel_rekordbox (Rust)  rbxport's rbl-db / rbl-index / rbl-anlz, loaded at runtime
```

- **Same command layer.** UI actions send the bridge command types the renderer sends,
  through `dispatchBridgeMessage`, so undo transactions, drag coalescing (`gestureId` /
  `gestureEnd`), transition reconciliation and every engine behaviour are shared code.
  `EngineHost` owns the engine wiring for both the console backend and the native app.
- **Event ordering.** Broadcasts reach the UI synchronously on the message thread; a
  command sent from inside a broadcast handler is queued until the current command
  finishes, which is the ordering the WebSocket gave the renderer.
- **State.** `ProjectMirror` rebuilds a read-only `ProjectModel` from the engine's
  `PROJECT_STATE` snapshot whenever the project's ValueTree changes; there is no
  optimistic UI state to reconcile.

## rekordbox

The rekordbox tab reads the installed library live and read-only: the SQLCipher
`master.db` (passphrase unwrapped from the rekordbox agent's `options.json`), the
playlist tree, and per-track ANLZ analysis. Importing a track brings rekordbox's beat
grid (tempo and first downbeat, applied as a manual tempo so the engine's own detection
never overrides it) and key. `cargo run --release --example probe` in
`backend/rekordbox_ffi` prints counts for the installed library without any track names.

## Stem separation

Two engines. The built-in one runs ONNX models in-process: a Mel-Band RoFormer vocal pack,
a BS-RoFormer 4-stem pack and htdemucs_ft as the backup, downloaded and hash-checked on
first use. The UVR engine (`app/UvrSeparator.*`) runs
[python-audio-separator](https://github.com/nomadkaraoke/python-audio-separator), the
engine behind Ultimate Vocal Remover, as a child process: Stemcel passes the source (a
timeline clip's window is written to a temporary WAV first), a model or ensemble preset,
`--model_file_dir ~/Stemcel/Models/uvr` and the project's stems folder, follows its
progress bars, and imports what it writes (`<input>_(<Stem>)_<model>.wav`, renamed to
`<Stem>.wav`) as ordinary stems. Stemcel does not ship the tool or its models; the user
installs it once (INSTALL.md). The recipes in the dialog follow deton24's separation guide,
limited to models anyone can download: BS-Roformer SW for 6 stems (alone, or with the
built-in vocal model's vocals), the vocal / instrumental / karaoke ensemble presets, MDX23C
DrumSep, anvuew's de-reverb, or any model by file name. `STEMCEL_SMOKE_UVR=1` with the
tool on PATH adds real separations (SW, and best of both) to the smoke journey.

The tool runs its models on the GPU only if its PyTorch can: pip's default PyTorch on Windows
is CPU-only, which takes minutes per model on a full song and looks stuck. The dialog asks the
tool (`--env_info`) and says which it will use; on the CPU with an NVIDIA GPU present it shows
(and copies) the command that puts the CUDA build into the tool's pipx environment. Progress
covers a whole ensemble ("separate (GPU) 2/3"), downloads show as such, Cancel stops the tool
at once, and each run's full output is kept in `~/Stemcel/Logs/uvr-last-run.log`.

### Measured quality

Global SDR (dB, higher is better; the MDX challenge's measure) on 12 MUSDB18 test songs
(the freely downloadable 7-second excerpts, joined into one 82-second file so every model
sees full-length context, then scored song by song), on a 16-thread CPU:

| Engine | Vocals | Drums | Bass | Other | Mean | Time for 82 s |
|---|---|---|---|---|---|---|
| Built-in, Best (vocal and rhythm packs) | **12.46** | 10.31 | 6.91 | 5.83 | 8.88 | 302 s |
| UVR, 6 stems (BS-Roformer SW; Other = guitar + piano + other) | 11.69 | **10.55** | **8.20** | **6.51** | **9.24** | 212 s |
| UVR, vocals balanced (ensemble) | 8.86 | | | | | 717 s |
| **UVR, 6 stems best of both** (built-in vocals + SW) | **12.46** | **10.55** | **8.20** | **6.51** | **9.43** | 212 s + vocals |

So the built-in engine is as good as UVR's best all-in-one model for vocals, and UVR is
ahead on bass and the remaining instruments. The dialog's default UVR recipe, "best of
both", takes each stem from the engine that scored higher on it: SW runs first (its vocals
are dropped), then the built-in vocal model runs on the same window. It falls back to SW's
vocals when the built-in vocal pack is not installed. The vocal ensemble trades SDR for fuller
vocals (its Resurrection model scored 8.5 dB alone, Big Beta 6X 13.1 dB on three of the
songs), so pick it by ear, not for clean stems. UVR's other recipes (drum kit pieces,
karaoke, de-reverb) do things the built-in engine has no model for. Caveats: 12 songs,
compressed excerpts (MUSDB18, not the HQ set), and the training data of the community
models is not published, so some may have seen these songs.

To repeat it: `StemcelStemRun [--quality best] <~/Stemcel/Models> <mixture.wav> <outDir>`
(a dev tool beside `StemcelStemEval`) writes the built-in engine's four stems;
`audio-separator <mixture.wav> -m BS-Roformer-SW.ckpt` writes UVR's; and
`StemcelStemEval <manifest>` (lines `<referenceWav>|<estimateWav>|<label>`) prints SI-SDR
and SDR per pair.

## Tempo map

The project's tempo can change over time: a tempo lane under the ruler (the **Tempo**
button beside Add Track, or opened automatically for a project that has a map) holds
tempo points. The tempo ramps in a straight line from one point to the next and holds
flat before the first and after the last; no points is the constant project tempo.
Double-click the line to add a point where the tempo already is, drag it (the time snaps
to the grid, Alt for free), double-click it to delete it; right-click for Clear. A drag is
one undo step (`TEMPO_MAP_SET`).

Everything musical follows the map: the ruler's bars, the grid and snapping, the transport
position (bars.beats), the metronome, beat repeat regions, and every warped clip whose
ratio follows the project (not one with a pinned ratio, and not an unwarped clip, which
plays at its own speed). The transport's BPM box shows the project tempo, or the map's at
the playhead. Click it to type a tempo, or drag it up and down or scroll on it (Shift for
hundredths); a drag or a run of scroll steps is one change, sent when it ends. With a map, a
new tempo scales the whole map so the playhead gets it, and like any project tempo change it
keeps every clip, marker and point on its beat. The first clip only seeds the tempo of a
project that has none set yet.

`TempoCurve` (`src/project/TempoCurve.h`) is the one model of it, shared by the engine, the
project state and the timeline. The tempo is linear between points, so the beats or source
time it covers are a quadratic and their inverse a square root: clip lengths, seek positions
and beat positions are exact, not sampled. Live and in export, a following clip's
`OffsetSource` renders in 256-sample stretches, each at the curve's average ratio over it,
so the stretcher's source position advances by exactly the curve's integral; the clip's
length and where a seek lands come from the curve too. `ProjectState::getClipTempoFollowBpm`
is the rule for which clips follow (playback, export and the timeline all use it), and the
dispatcher re-syncs the engine after every command, so a map edit, undo, a load, a warp
toggle or a re-analysis all land the same way.

Measured in the tests (`tests/TempoMapTests.cpp`), a click track on a 120-to-150 BPM ramp
comes out with every beat where the curve puts it: Re-Pitch to the sample, Pro to within
26 samples (0.5 ms) during the ramp and exactly once it settles. Getting there needed two
fixes that also help any live tempo change: libsamplerate's ratio is set as a step rather
than ramped across a block (a ramp consumes the average of the old and new ratios), and the
Signalsmith stretcher is fed what keeps its read-ahead at the size for the current rate
(its input latency plus the rate times its output latency), not just rate times the block.

## Transitions

DJ transitions between two clips on different tracks, where the incoming clip starts before
the outgoing one ends: right-click either clip, **Transition**, and pick a preset (or **Auto**,
the auto mixer's pick for that pair, with its reasons). A preset is a set of automation shapes
for the outgoing and incoming tracks over the overlap, plus the odd clip effect, written as
ordinary automation (so it shows in the lanes and can be edited afterwards) in one undo step,
and recorded (`MIX_TRANSITION_SET`) so the timeline labels the join and re-picking or **Remove
Transition** can take its effect back out. Shapes hand back to whatever curve was there just
before and just after the window, so other clips on those tracks are untouched. Bass, mid and
treble can now be cut to -40 dB (a DJ-mixer kill) by automation; the Track FX dials still stop
at -15.

The presets (`app/MixTransitions.cpp`), from the standard DJ repertoire:

| Category | Presets |
|---|---|
| Blends | Smooth Blend (equal-power crossfade), Bass Swap, Three-Band Blend (highs, mids, then a bass swap), Vocal-Safe Blend (mids swap so two vocals never overlap), Double Drop, Tempo Ramp Blend (the tempo map glides between the two tempos), Linear Fade |
| Filters | Low-Pass Filter Out, High-Pass Filter Out, Filter Swap, Muffled Intro (low-pass in), Thin Intro (high-pass in) |
| FX | Echo Out, Reverb Wash, Bitcrush Out, Echo Into the Drop |
| Cuts | Quick Cut, Bass Kill Cut, Vinyl Brake, Backspin, Loop Roll (beat repeats at 1/4, 1/8, 1/16 over the last bar) |

Echo and reverb throws give the project's echo (1/4 note, 55% feedback) or reverb a DJ setting
when the project has not set one, so the throw is heard.

### Outs and ins

A transition is also two halves picked apart: an **Out**, how the outgoing track leaves, and an
**In**, how the incoming one arrives. The **Transitions** tab in the bottom panel lists them in
three columns (Outs, Ins, and the full transitions above); drag one onto a clip on the timeline.
An Out lands on the join where that clip ends under a clip on another track, an In on the join
where it starts over one, and the join's other half stays as it was (a full preset's half
carries on as its nearest equivalent). Double-clicking a row puts it on the selected clip.

| Outs | Ins |
|---|---|
| Fade Out, Bass Swap Out, EQ Fade Out, Low-Pass Out, High-Pass Out, Echo Out, Reverb Wash, Bitcrush Out, Cut, Vinyl Brake, Backspin, Loop Roll | Fade In, Bass Swap In, EQ Fade In, Muffled Intro, Thin Intro, Bass Held, Drop In, Cut In |

Double-click a transition's label on the timeline (or **Transition > Edit Transition...**) to
tweak it. For each half: the move, the span of the overlap it covers (shown in bars), the curve
(equal power, linear, fast start, slow start, S-curve), what the fader does (fade over the span,
fade at its end or start, cut, or leave it), a low- or high-pass sweep and its depth, what each
EQ band does (untouched, swap at the swap point, or sweep over the span), how deep the bands
go, and for an Out an effect throw (echo, reverb, bitcrush) and an end move (brake, backspin,
loop roll); plus a tempo ramp across the window. A graph shows the automation each half
writes. Changes apply as they are made, each burst of changes one undo step. The window itself
is the clips' overlap: move or trim them to lengthen it. The halves are kept in the join's
record (`settings` on `MIX_TRANSITION_SET`), so Remove and re-picking take them back out.

## Key shift

A clip's **Key** menu shows the key it sounds in (its source's detected or rekordbox key, moved
by its pitch shift) with its Camelot code, shifts it a semitone at a time at the same tempo, or
**Match**es a clip it overlaps on another track: the smallest shift (up to six semitones) that
puts the two in key on the Camelot wheel. The shift is the clip's warp pitch, so its tempo is
untouched; a clip that was not warped is warped at its own speed (a pinned 1.0 ratio) to carry
it. The auto mixer's **In Harmonic Order, Matching Keys** does this for every join whose keys
clash when two semitones or less fixes it (larger shifts colour the sound), before it picks the
join's preset, so a fixed join gets a real blend.

## Auto mix

Select two or more tracks in the library, right-click, **Auto Mix**: in harmonic order or in
the order shown, with a style (Auto, Smooth, Club, Party). The mix goes after everything
already on the timeline, on two new decks (Mix A and Mix B), and the timeline scrolls to it.
Each track comes in over the previous one's outro: the join starts on an 8-bar phrase of the
outgoing track, lasts the chosen preset's length (1 to 16 bars), and the outgoing track is
trimmed to end with it. Clips warp to the project tempo as any dropped clip does.

`app/AutoMix.cpp` picks each join's preset from the pair: keys on the Camelot wheel (in key
when the same, one step round, or the relative major/minor), the tempo gap between the
sources, and an energy step from their waveforms' average level. A big tempo gap favours
moves that hide it (cuts, echoes, a brake), or a tempo ramp when it is under 16% and the keys
agree; clashing keys favour moves that never let the two harmonies play together for long
(cuts, filters, throws, the vocal-safe blend); matching keys favour real blends; a louder
incoming track favours lifts and a quieter one let-downs; the style weights the categories;
and the previous join's preset is avoided so a mix does not repeat one move throughout.
Harmonic order starts from the first selected track and always moves to the closest remaining
one (keys, then tempo, then energy, a rise costing less than a fall). Selecting several clips
on the timeline and choosing **Auto Transition Selected Clips** gives every join among them a
preset the same way without moving anything.

## Mashups and the sample library

**Mashup** (library, two tracks selected, right-click): the vocals of one over the instrumental
of the other. `app/Mashup.cpp` uses the stems already in the library, or separates what is
missing first (the vocals of one, then drums, bass and other of the other) with the built-in
engine, one job at a time; then it puts the four stems after the arrangement on their own
tracks (Mashup Vocals, Drums, Bass, Other), warped to the project tempo and on the bar grid,
with the vocals shifted into the instrumental's key when three semitones or less does it, as
one undo step, and scrolls to them.

**Save to Sample Library** (a stem or sample's menu) copies its audio into
`~/Stemcel/Sample Library/<type>` (Vocals, Drums, Bass and the other stem types, or Samples),
which is added to the Files tab's folders the first time, so the library of stem samples is
there to browse, audition and drag in from any project. The library's type filter (All,
Tracks, Stems, Samples) beside its search narrows the tiles to one kind.

## Project tabs

Several mixes can be open at once, as tabs above the workspace (`app/ProjectTabs.*`, kept by
`AppController`): New and Open add a tab instead of replacing the project, **+** starts a new
mix, a dot marks unsaved changes, and an open project's file opened again switches to its tab.
One project is in the engine at a time: switching parks the open one (with unsaved changes it
is written to `~/Stemcel/Tabs` and comes back unsaved through `PROJECT_LOAD_RECOVERY`, still
saving to its own file; a clean one simply reopens from its file, view and all) and opens the
other. Undo history does not survive a switch. Closing a tab with unsaved changes asks first,
and so does quitting while another tab has some.

Copy and paste work across tabs: copied clips carry their library items (and the items they
derive from), which a paste adds to a project that lacks them; **Copy Transition** in a join's
Transition menu, then **Paste Transition** on a join in any tab, applies the same preset there.

## Look

A terminal theme (`app/Theme.*`): phosphor green on black everywhere, the timeline (canvas,
rows, ruler, grid, tempo and automation lanes) in black and dark grey, square controls with thin green
rules, and JetBrains Mono (SIL OFL 1.1, built in from `backend/app/fonts/`) for every font,
JUCE's own menus and dialogs included. Clips keep their per-track colours.

The top bar has the timeline zoom slider (beside − + Fit) and the master section: the master
volume slider and a stereo meter (L and R from -60 dBFS in green, amber above -12 and red above
-3, a peak hold on each, and a clip light that a click clears). Transport commands live on the
bar and their shortcuts (Space plays and pauses); there is no Transport menu.

## The name

**Stemcel** (`Stemcel.exe`, `Stemcel.app`, bundle id `com.stemcel.app`; projects `.stemcel`)
was released earlier under another name. For the people who used it then, it still opens their
projects (`*.legacydaw`), and on its first launch it moves their old home and app-data folders to
`Stemcel` (models, settings, caches, tabs and the sample library carry over), rewrites the paths
saved in its preferences, and loads any project path that still points into the old folders from
the new one (`remapMovedAppPath`). Those two places (`NativeMain.cpp`, the project choosers) are
the only ones that name it; the stem models are still downloaded from the Hugging Face account
they were first published on.

## Mixer

**View > Mixer** (F9), or the **Mixer** button in the top bar, swaps the bottom panel for a
mixer docked in the workspace: a strip per track (colour, name, five insert slots, pan knob,
mute and solo, fader and stereo meter) and the master strip on the right (its own inserts,
the master fader and meter). Click an empty slot's **+ insert** to add a plugin (grouped by
format, then maker); click a filled slot to open, bypass, move or remove it; double-click to
open its editor. The Plugins tab's **Master bus** switch shows the same master chain.

The master inserts run over the summed mix after the project echo and reverb returns
(`BusGraph::masterInserts`), are stored on the project's `MASTER_PLUGINS` node through the
same commands as a track's (the reserved owner id `plugins::kMasterChainId`), restored on
load, saved with their state, and rendered in exports. Their latency delays every track
alike, so it takes no part in the tracks' alignment.

## Plugin formats

`plugins::addHostedFormats` registers every format a build hosts, for the catalogue, the
out-of-process scan worker and an export's own catalogue alike: VST3 everywhere, Audio Units
on macOS, and VST 2 (`STEMCEL_HOST_VST2`, on by default). Steinberg withdrew the VST2
SDK and its licence does not fit the AGPL, so VST 2 hosting compiles JUCE's host against
[FST](https://git.iem.at/zmoelnig/FST) (GPL-3.0-or-later, a documented clean-room
reverse-engineering of the interface), fetched at a pinned commit and reached through the
`pluginterfaces/vst2.x` paths JUCE includes. The build compiles a patched copy of FST's
header that gives three trailing arrays the fixed lengths JUCE sizes them by (eight speakers,
two event pointers, a 64-character key name and the two ints after it), so JUCE never
allocates a struct short. A scan walks each format in turn: VST3 in its two standard folders;
VST 2 in the registry's folder, `Program Files\VstPlugins`, `Program Files\Steinberg\VstPlugins`,
`Common Files\VST2` and `Common Files\Steinberg\VST2` (macOS: the Plug-Ins/VST folders); Audio Units from the
system's component registry. Checked on Windows: a VST 2 scan reads Image-Line's FL Studio
VSTi plugins' names, makers and categories through FST.

## Automation curves

Each automation point carries the shape of the segment to the next one (`AutomationShape`,
shared by the engine and the app): linear, smooth (S-curve), fast start, slow start, hold
(a step) or stairs, with a bend (`tension`, -1..1) for fast and slow starts. Right-click a
point or a segment for **Curve**; Shift-drag a segment up (fast start) or down (slow start)
to bend it. Playback and export follow the shapes. **Insert pattern** (the same menu) writes
a preset over the timeline selection, or four bars from the clicked bar: ramps, smooth fades,
swell, drop-off, sine and triangle waves (1 beat to 1 bar), saw, trance gates (1/8, 1/16),
sidechain pump, stairs and random steps, all on the beat, handing back to the curve either
side.

## Beat stutter

Right-click a track or a clip > **Beat Stutter**: beat-synced repeats on the Beat Repeat
engine (so tempo-locked in playback and export), from the clicked beat or across the
selection. Stutter at 1/8, 1/16 or 1/32; a build-up whose repeats shorten (1/4, 1/8, 1/16,
1/32) over a bar or two; a fill on a bar's last beat; and a gated stutter (1/16 repeats under
a 1/16 volume gate). Beat Repeat gains a 1/32 division.

## Recording

Record Audio's **Channels** row picks **Mono** (one input channel) or **Stereo** (an adjacent
pair), and the channel list follows: single inputs, or pairs.

## Time-stretch

A warped clip plays through one of five engines (`src/dsp/WarpProcessor.*`, the clip's
warp mode):

| Mode | Engine | Pitch | Use |
|---|---|---|---|
| **Pro** (default) | [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (MIT) | kept | anything: full mixes, vocals, drums |
| Rhythmic | Rubber Band R2, crisp transients | kept | the old default |
| Tonal | Rubber Band R2, smooth, long window | kept | sustained pads |
| Complex | Rubber Band R3 | kept | |
| **Re-Pitch** | libsamplerate, best sinc | follows tempo | master tempo off: a turntable's speed change |

**Master Tempo** in a clip's menu is the DJ name for the same choice: on stretches the
clip at its own pitch, off switches it to Re-Pitch. A semitone shift on a Re-Pitch clip is
added on top of its turntable pitch (through Signalsmith, as a resampler cannot shift pitch
at a fixed speed). Clips that saved a mode keep it; new clips and auto-warps get Pro.

### Time-stretch quality

`StemcelStretchEval <in.wav> <outDir> [ratio ...]` (a dev tool) runs a file through every
mode exactly as playback does (512-sample blocks, real-time options) at 0.8, 0.9, 1.1 and
1.25x, and plays each result back at the inverse ratio. Scored on 6 MUSDB18 mixes and the
isolated drums and vocals of 3 of the songs (lower is better):

| Mode | Round trip (LSD, dB) | One pass (LSD, dB) | Transient loss | Speed |
|---|---|---|---|---|
| **Pro** | **5.26** | **4.41** | **0.122** | **79x real time** |
| Rhythmic | 7.31 | 6.14 | 0.178 | 44x |
| Tonal | 8.62 | 7.10 | 0.196 | 41x |
| Complex | 7.73 | 6.77 | 0.142 | 12x |
| *StaffPad (reference)* | 6.88 | 4.80 | 0.143 | |

Round trip is the log-spectral distance of the twice-stretched audio to the original; one
pass compares a single stretch with the original's spectrogram moved along time by the
ratio; transient loss is one minus the correlation of their onset envelopes. Pro led on
every material (mixes, drums and vocals alike), also against an outside reference: StaffPad's
TimeAndPitch (the commercial-grade engine Audacity ships, run as Audacity configures it, with a
4096-point FFT at 44.1 kHz) scored second on one-pass distance and below Pro on all three
measures. zplane's élastique could not be included: it is a licensed SDK. Re-Pitch round-trips to within 0.015 dB and
matches an ideal offline resample to 62-66 dB SNR, so a turntable-style change adds nothing
audible. A phase vocoder places a single click to within part of a hop: Pro lands one within
0.7 ms of where it belongs, with no drift (the unit tests hold it to 1 ms; Re-Pitch is
sample-exact).

## Packaging

- Windows: `scripts\Package-Native.ps1` builds Release, checks the bundled runtime files,
  smoke-tests the staged folder and writes `dist/Stemcel-<version>-native-win-x64.zip`.
- macOS: `scripts/package-native-mac.sh` builds the Release `.app`, signs it (Developer
  ID from `STEMCEL_SIGN_IDENTITY`, otherwise ad-hoc), smoke-tests it from inside the
  bundle and writes a DMG and a zip.
- `.github/workflows/package-native.yml` runs both.

## Parity with the Electron app

The native app is a port, not a copy: the engine, the project format and every bridge
command are shared, so what the engine does (playback, warp, effects, export, saving) is
identical, while the interface is re-implemented in JUCE. It is **not** pixel-for-pixel,
and some screens are simpler than the Electron ones (listed below).

Ported: projects (new/open/save/save as/recent/properties, autosave and crash recovery,
missing-audio relink, import from project, view state kept with a clean project), arrange
view (clips, trim, split, duplicate, move, nudge, lock, colour, rename, clipboard,
markers, selection and loop, snap, zoom, follow, the clip-header status badges), tracks
(gain, pan, mute, solo, rename, colour, reorder, height, meters), library (import with
tags, key, cover art and transcode; info; BPM edit; reanalyse; audio type; images; save a
saved clip as a sample; grouping under the source; the Electron remove rules and file
clean-up), Files browser, rekordbox browser, audition, Track FX, Project FX, plugins,
automation lanes, crossfades, beat repeat, clip volume envelopes, tempo on drop (seed,
auto-warp, beat-grid alignment after analysis), the Clip Editor (trim window, warp, pitch,
grid, tempo correction, transforms, slicing, a preview that follows the draft live,
play/pause, skip, click to seek), stem separation with model downloads, stereo channel
split, Record Audio (latency calibration and its cancel, Save as Stereo, Split Channels),
the Scratch Editor (deck, crossfader, record, notation, backing, saved patterns to load /
rename / delete, save as sample / pattern / apply), MIDI controllers (transport, jog and
scrub, markers, dials with soft takeover, browse, deck selections), export mixdown (the
Electron form, settings kept per project), and preferences in the Electron tabs.

Also matching the Electron app: the library as tiles with cover art, key and BPM pills and
saved clips under their source; renaming in place (a clip's title strip, a track's name, a
library tile); the Clip Editor's playback selection, Crop and volume shape, and the editor
on library items (a saved clip saves back to every placement; any item saves a selection
as a new clip); the Scratch Editor's backing track options (tracks, start, length); and
the Record Audio review (waveform audition, Play With the Arrangement, its own backing
level).

Remaining differences:

- The interface is drawn by JUCE, not a browser, so fonts, spacing and small
  interactions differ even where the layout follows the Electron app.
- The native app adds a rekordbox tab; the Electron app has none.

### Electron removed

The Electron app (`frontend/`), its workflows and release scripts were removed on branch
`native-only`; they remain on `native-ui` and in git history.

### Platforms checked

- **Windows (MSVC):** full smoke journey, all engine tests, and the packaged Release zip.
- **macOS (Apple Silicon, GitHub runner):** `package-native.yml` builds, signs and
  smoke-tests the `.app` from inside the bundle, including the rekordbox reader.
- **Linux (Clang 15 + libc++, WSL, Xvfb):** builds cleanly and passes the full smoke
  journey (rekordbox skipped: built without cargo); the local stand-in for Apple Clang.

The smoke journey checks what is heard as well as what is shown where it can: its tempo
journey drops tracks before and after analysis and warps from the Clip Editor, and
fails unless the live engine's stretcher runs at the ratio the timeline draws.

Calibration figures are stored per device pair under `nativeLatencyCalibrations`: the
Electron app keys them with a NUL separator that JUCE strings cannot hold.
