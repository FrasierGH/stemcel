# Performance mode

Performance mode puts two decks, a DJ mixer, effects, a sampler and a browser on screen,
and records what you play into the timeline. You can drive it with a DJ controller (the
Pioneer DJ DDJ-400, as it works with rekordbox) or with the mouse.

Open it with the **Perform** button in the transport bar, or **View ▸ Performance Mode**
(`Ctrl`/`Cmd`+`Shift`+`L`). Press **Exit** or `Esc` to return to the timeline.

## On screen

- **The timeline**, across the top: every track's clips in a lane, the playhead and the
  selected range.
  - While REC is on, the take is drawn as it grows: each deck's audible stretches in red,
    from where it will land.
  - Click to move the playhead.
  - Drag across the lanes to select a range; double-click to clear it.
- **Platters** in each deck screen's outer top corner, beside the title and overview, turning
  at 33⅓ with the track, as rekordbox and Serato draw them.
  - The ring shows how far through the track it is (amber in the last 30 seconds), with the
    cue point on it.
  - The record glows while its jog is touched.
  - **DISCS** in the top bar turns them off.
- **Meters:** each channel's L and R beside its fader, drawn as the timeline's master meter
  is: segments green to -12 dB, amber to -3 dB and red above, a peak hold over each, the
  scale's marks on the outside, and the level in dB under the fader.
- **The Beat FX display** opens a list of every effect when clicked, as rekordbox's FX panel
  does; FX SELECT on the controller still steps through them.
- **Stem Mix:** see [below](#stem-mix).
- **4 DECKS** in the top bar: decks 3 and 4 below 1 and 2, the mixer with four channels (3 1 2
  4, as rekordbox's), CH SELECT with 3 and 4, and the four close-ups stacked across the top.
  Decks 1 and 3 are on the crossfader's left, 2 and 4 on its right; 3 and 4 share the sampler's
  pads with 1 and 2. **CTRL 3** and **CTRL 4** put a two-deck controller's left or right side on
  deck 3 or 4 (its lights follow); a controller with deck buttons of its own (the Mixtrack
  Platinum FX / Pro FX, the MC7000) reaches them directly.
- **The timeline strip** names each track beside its lane (hover a thin lane for its name).
- **Debug > Rescan Controllers** looks for controllers again, for one plugged in after the
  mode opened or freed by another program.
- **STACKED** in the top bar puts both decks' close-up waveforms one above the other across
  the width, on one time scale around one centre line, so two decks in sync line up beat for
  beat. The deck screens keep the title, the time and the overview.
- **AUTO GAIN:** see [the library guide](dj-library.md#auto-gain).
- **Loading:** a deck plays as soon as the first seconds of a track are read, and the rest
  follows in the background. A track the browser's cursor rests on is read ahead, so LOAD on
  it is instant. A track from the DJ collection or rekordbox goes on the deck straight from its
  file while it joins the project's library.
- **The filter** is voiced after Pioneer's DJM mixers: a small turn off the centre detent is
  already clearly heard, a full turn leaves only the hats (high-pass) or the sub (low-pass),
  and the resonance rises as it closes. The timeline's filter is the same, so takes play back
  as they sounded.

## Stem Mix

A deck's track in its stems: the vocals, the instrumental (everything but the vocals, drums
and bass), the bass and the drums, each taken in and out of the deck's mix on the pads.

- **Double-press SAMPLER** on the controller (or on screen), or press the deck's **STEMS**
  button: the deck's pads become the stems. SAMPLER once more goes back to the sampler.
- **STEMS** on a track without stems asks how to split it: **vocals and instrumental** (two
  stems from one model, a good deal faster; BASS and DRUMS stay dark) or the **ensemble** (all
  four). The controller's double-press uses the split chosen last.
- Pads **1–4** are VOCAL, INST, BASS and DRUMS: lit while the stem plays, a press takes it out
  or back in (in a few milliseconds, without a click). Pads **5–8** play that stem alone; the
  same pad again brings them all back. **SHIFT + a pad** brings them all back too.
- A key over the pads shows which pad is which stem, in its colour, dimmed when out; the deck
  screen shows the stems (V I B D) whatever the pads are doing.
- With every stem in, the deck plays the track itself, untouched.

**Where the stems come from.** A track whose stems are in the library (separated before) has
them on the deck as it loads. Otherwise STEMS (or Stem Mix chosen on the controller) separates
them: the vocals with Mel-Band RoFormer, the drums and bass with BS-Roformer SW, as the
library's Separate Stems does (the button shows the progress), and the deck takes them when
it ends. The instrumental is written once as a library stem of its own ("Melody — title").

**Recording.** A stretch played with stems out goes into the take as the stems that were in:
each on the deck's track for that stem ("Deck 1 Vocal", "Deck 1 Drums", ...) with the deck's
automation, so it plays back as it sounded.

## Recording into the timeline

Press **REC** to record (it is off when the mode opens). A take is written when you turn
REC off or leave the mode. It lands at the timeline playhead, as one undo step
(**Edit ▸ Undo** removes it). The playhead then moves to the end of the take, so the next
take follows it.

Each take adds:

- A **Deck 1** / **Deck 2** track. Each stretch of a track that reached the master becomes a
  clip: playing, or held on CUE or a hot cue, with the channel fader up and the
  crossfader not cutting it. A clip is split wherever the track jumped (cue, loop pass, hot
  cue, beat jump), the tempo or key changed, or the jog wheel took over. Each clip plays at
  the speed it was played, with key lock where it was on. Scratches become forward and
  reversed clips that follow the platter.
- **Automation** on each deck track:
  - its level (trim × channel fader × crossfader)
  - EQ and filter
  - Sound Color FX
  - Beat FX and Pad FX: the effect, its beats, its level and on/off
  - the tempo the effects follow

  Beat FX on the master go on the master's lanes.
- **Sampler** tracks, one per sampler slot used, with a clip for every hit.
- A marker at the start of the take.

All of it can be edited afterwards like anything else in the timeline. Played back, the
take sounds as it did live. The same EQ, filter and effects process the timeline track,
and the smoke test checks a take against the live output.

## Playing over the timeline

Each deck has a picker above its screen: **Deck 1 / Deck 2**, or any track in the timeline.

### A channel on a timeline track

Pick a track, and that channel works the timeline instead of its deck:

- **Transport:** PLAY starts and stops the timeline from the playhead. CUE stops it and goes
  back to where it started. With the timeline stopped, the jog wheel moves the playhead
  (SHIFT for faster).
- **Mixer and effects:** the trim, EQ, filter or Sound Color FX, the channel fader, the
  crossfader, and Beat FX and Pad FX all act on the track as it plays.
- **Holding:** a control you move holds where you left it, as a knob does, until the
  timeline stops. The track then goes back to its own automation.
- **Writing:** with **REC** on, what you moved while the timeline played is written into the
  track's automation when it stops, as one undo step. It covers the stretch from where you
  first moved each control to where the timeline stopped. Before and after that stretch the
  track keeps its own automation, and a control you set while stopped is written from where
  the timeline starts.

### A stretch of the timeline on a deck

Select the range first: drag across the timeline strip at the top of performance mode (or
select it on the timeline before opening performance mode). In the browser, the **Timeline**
folder then lists that range (or the whole arrangement if nothing is selected): **The mix**,
then each track playing in it. **LOAD** renders that stretch, as it sounds, onto the deck.

With REC on, the take replaces that stretch:

- **Lining up:** the take is placed so the deck's audio lines up with where it came from.
- **Clearing:** the stretch is cleared in the arrangement. For a track, only that track;
  for the mix, every track. Clips are split at its edges and nothing after it moves.
- **Undo:** it's one undo step.

## Redo from here

**REDO** goes back over a recorded take to redo part of it, a transition say, in real time.

1. Put the timeline's playhead a little before the stretch to redo, inside the take.
2. Press **REDO**. Each channel is put on its deck's track of the take, and the timeline plays.
   Untouched, the take plays exactly as it was.
3. Touch a control and it is yours from that moment:
   - **A knob or fader** (trim, EQ, filter, the channel fader, effects) takes over once you move
     it to the value the take has there (or past it), so a knob left elsewhere doesn't jump.
     Everything you don't touch keeps following the take.
   - **A deck control** (the tempo slider, SYNC, loops, hot cues and pads, the jog while it
     plays) takes the deck over: it loads what the take was playing, at the point and tempo the
     take had reached, and plays on live while the take's track falls silent. The tempo slider
     picks the tempo up when you move it to it.
4. Stop the timeline (PLAY on the controller, or Space) to end the pass. Press PLAY again for
   another pass from where it stopped; press **REDO** again to finish.

With **REC on**, each pass is written when it stops, as one undo step:

- **Mixer moves** are written from your first touch to the stop, ramping back to the take's
  values over a beat after it.
- **A deck taken over:** its playing replaces the take's track from the takeover to the stop,
  and the rest of the take joins on:
  - The **JOIN** light is lit while stopping now would join seamlessly: the deck is playing a
    moment the rest of the take also plays (the same track, at about the same tempo).
  - Stopping there splices the take at that moment. If the deck got there earlier or later
    than the take did, everything after the stop moves back or up by the difference, so the
    rest of the mix carries on as it was.
  - Stopping elsewhere asks whether to replace the rest of that deck's track or leave it.

With **REC off**, passes are rehearsals: nothing is written, and **KEEP** writes the last one's
mixer moves. A deck taken over plays but isn't written with REC off.

## Headphones

When the output device has four channels or more, the master plays on outputs 1–2 and
the headphone cue on 3–4. The DDJ-400's built-in sound card works this way.

When the master plays on another device (speakers on the computer's sound card, an
interface), the cue still goes to the controller's own sound card, its outputs 3–4, so
headphones plugged into the controller hear it. A copy of the master goes to its outputs 1–2,
so the HEADPHONES MIX knob still blends the two. The status line above the decks says where
the headphones are. The DDJ-400's
own MASTER LEVEL, HEADPHONES MIX and HEADPHONES LEVEL knobs act in its hardware.

## Hot cues and rekordbox

Hot cues and memory cues set in performance mode are Stemcel's own. They are saved with
the library item in the project, and Stemcel never reads or changes rekordbox's cues
while you play.

To take them into rekordbox, right-click the item in the library and choose **Send Cues to
rekordbox**:

- **Into Empty Hot Cue Slots** adds Stemcel's hot cues only where rekordbox has none, so
  your rekordbox hot cues stay as they are.
- **Replacing rekordbox's Hot Cues** puts Stemcel's hot cue in each slot Stemcel uses.
  rekordbox's cues in the other slots stay.

Memory cues are added, except where rekordbox already has one within 10 ms. The track is
found in this computer's rekordbox collection by its file, and the file is added to the
collection first if it isn't there yet.

This works like Add to rekordbox:

- rekordbox must be closed.
- Before the first change in a session, Stemcel copies rekordbox's database to
  ~/Stemcel/Backups/rekordbox.

To put cues on a USB stick, send them to the computer's library, then export or sync from
rekordbox. Or skip rekordbox: tracks in Stemcel's own [DJ library](dj-library.md) keep the
cues set on a deck, and its **Export to USB** puts them on the stick.

## Pioneer DJ DDJ-400

Other controllers have profiles too: see
[the controller guide](midi-controllers.md#performance-mode-controllers).

Connect the controller before opening the mode. It is enabled for you while the mode is
open. While the mode is open the controller drives the decks, not the timeline. The MIDI
line at the top right shows the last message and what it was mapped to.

### Deck

| Control | Does | With SHIFT |
|---|---|---|
| PLAY/PAUSE | Play or pause. Pressed during a CUE or hot cue hold, playback carries on after you let go | Play/pause |
| CUE | Paused, away from the cue point: sets it (snapped to the nearest half beat with quantize). Paused on it: plays while held and returns on release. Playing: back to the cue point, paused | Back to the start |
| BEAT SYNC | Sync on/off (tempo and beats to the master deck). Held: this deck becomes the master | Tempo range ±6 / 10 / 16 % / WIDE |
| TEMPO | Tempo, − at the top. After a tempo range change the tempo stays put; move the slider to where that tempo sits in the new range and it takes over from there | |
| Jog, top | Scratch (vinyl mode) | Fast search |
| Jog, side | Nudge (pitch bend); paused, moves the position | Fast search |
| LOOP IN / 4BEAT | Loop in point; held: a 4-beat loop. In a loop: adjust the in point with the jog | |
| LOOP OUT | Loop out point, the loop starts. In a loop: adjust the out point with the jog | |
| RELOOP/EXIT | Exit the loop, or back into the last one | Active loop (arms it) |
| CUE/LOOP CALL ◁ ▷ | In a loop: halve / double it. Otherwise: previous / next memory cue | ◁ deletes the memory cue, ▷ stores one |

### Pads

| Mode | Pads | With SHIFT |
|---|---|---|
| HOT CUE | An empty pad stores a hot cue (a loop hot cue in a loop); a set pad jumps to it, keeping the beat with quantize. Paused, it plays while held | Deletes the hot cue |
| BEAT LOOP | 1/4, 1/2, 1, 2 / 4, 8, 16, 32-beat loops; the same pad again ends it | |
| BEAT JUMP | Odd pads back, even pads forward: FINE (5 ms) … 128 beats on three pages | Pads 7 / 8: previous / next page |
| SAMPLER | Pads 1–8 play slots 1–8 (deck 1) or 9–16 (deck 2) of the bank, from the start. Pressed twice quickly: [Stem Mix](#stem-mix) | Stops a playing slot; on an idle slot, loads the browser's track into it |
| KEYBOARD | The first pad picks (or sets) a hot cue; then the pads play it at pitches | Pads 7 / 8: page; 1–6: pick another hot cue |
| KEY SHIFT | Shifts the key: +4…+7 / ±0…+3, and two more pages | Pads 7 / 8: page; pad 1: key reset |
| PAD FX1 / PAD FX2 | Hold for an effect: two banks of 16, the second 8 with SHIFT. PAD FX2 pad 8 is the Release FX | The other 8 |

### Mixer and browser

| Control | Does |
|---|---|
| TRIM, EQ HI/MID/LOW | Gain (−∞ to +12 dB), EQ (kill to +6 dB) |
| FILTER (CFX) | The channel filter, or the Sound Color FX chosen on screen |
| Channel fader, crossfader | Level; deck 1 is on the crossfader's left, deck 2 on its right. SHIFT + fader: fader start |
| Headphone CUE | Sends the channel to the headphones. SHIFT: tap the effect tempo |
| BROWSE turn / press | Move the cursor / move between the tree and the list, or open a folder. SHIFT + press closes it; SHIFT + turn zooms the waveforms |
| LOAD 1 / 2 | Load the track under the cursor; twice quickly: Instant Doubles (the other deck's track, where it is) |

### Beat FX

| Control | Does |
|---|---|
| BEAT ◁ ▷ | Beats: 1/16 … 16. While a Pad FX pad is held, that pad's beats. SHIFT: ◁ BPM AUTO, ▷ TAP |
| FX SELECT | Next effect (SHIFT: previous): Delay, Echo, Low Cut Echo, Multi Tap Delay, Ping Pong, Spiral, Reverb, Shimmer, Trans, Filter, Triplet Filter, Flanger, Phaser, Pitch, Slip Roll, Roll, Triplet Roll, Reverse Roll, Vinyl Brake, Helix, Enigma Jet, Mobius (Saw), Mobius (Triangle) |
| CH SELECT | Channel 1, channel 2 or the master |
| LEVEL/DEPTH | The effect's amount |
| ON/OFF | On or off. Echoes and reverbs ring on after OFF. SHIFT: the Release FX (Vinyl Brake, Echo or Spin Back, chosen on screen) |

The pad FX presets, the Sound Color FX type, the Release FX type and the sampler's bank and
volume are on screen. The Sound Color FX are Filter, Space, Dub Echo, Sweep, Noise, Crush,
Jet, Pitch and Gate/Comp. Hot cues and the CUE point snap to the nearest half beat with
quantize on; loops and beat jumps keep to whole beats.
