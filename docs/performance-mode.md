# Performance mode

Performance mode puts two decks, a DJ mixer, effects, a sampler and a browser on screen,
and records what you play into the timeline. You can drive it with a DJ controller (the
Pioneer DJ DDJ-400, as it works with rekordbox) or with the mouse.

Open it from **View ▸ Performance Mode** (`Ctrl`/`Cmd`+`Shift`+`L`). Press **Exit** or `Esc`
to return to the timeline.

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

In the browser, the **Timeline** folder lists the selected range (or the whole arrangement
if nothing is selected): **The mix**, then each track playing in it. **LOAD** renders that
stretch, as it sounds, onto the deck.

With REC on, the take replaces that stretch:

- **Lining up:** the take is placed so the deck's audio lines up with where it came from.
- **Clearing:** the stretch is cleared in the arrangement. For a track, only that track;
  for the mix, every track. Clips are split at its edges and nothing after it moves.
- **Undo:** it's one undo step.

## Headphones

When the output device has four channels or more, the master plays on outputs 1–2 and
the headphone cue on 3–4. The DDJ-400's built-in sound card works this way. The DDJ-400's
own MASTER LEVEL, HEADPHONES MIX and HEADPHONES LEVEL knobs act in its hardware.

## Pioneer DJ DDJ-400

Connect the controller before opening the mode. It is enabled for you while the mode is
open. While the mode is open the controller drives the decks, not the timeline. The MIDI
line at the top right shows the last message and what it was mapped to.

### Deck

| Control | Does | With SHIFT |
|---|---|---|
| PLAY/PAUSE | Play or pause. Pressed during a CUE or hot cue hold, playback carries on after you let go | Play/pause |
| CUE | Paused, away from the cue point: sets it (snapped to the beat with quantize). Paused on it: plays while held and returns on release. Playing: back to the cue point, paused | Back to the start |
| BEAT SYNC | Sync on/off (tempo and beats to the master deck). Held: this deck becomes the master | Tempo range ±6 / 10 / 16 % / WIDE |
| TEMPO | Tempo, − at the top | |
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
| SAMPLER | Pads 1–8 play slots 1–8 (deck 1) or 9–16 (deck 2) of the bank, from the start | Stops a playing slot; on an idle slot, loads the browser's track into it |
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
| FX SELECT | Next effect (SHIFT: previous): Delay, Echo, Ping Pong, Spiral, Reverb, Trans, Filter, Flanger, Phaser, Pitch, Slip Roll, Roll, Vinyl Brake, Helix |
| CH SELECT | Channel 1, channel 2 or the master |
| LEVEL/DEPTH | The effect's amount |
| ON/OFF | On or off. Echoes and reverbs ring on after OFF. SHIFT: the Release FX (Vinyl Brake, Echo or Spin Back, chosen on screen) |

The pad FX presets, the Sound Color FX type, the Release FX type and the sampler's bank and
volume are on screen.
