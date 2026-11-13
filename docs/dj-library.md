# Library mode: a DJ library without rekordbox

Library mode is Stemcel's own DJ collection. It holds the music you play out, with each
track's beat grid, key, hot and memory cues, rating, colour and My Tags. It is organised in
playlists and folders, and goes onto USB sticks that CDJs and XDJs read. rekordbox doesn't
have to be installed, open or involved.

Open it from **View ▸ Library Mode** (`Ctrl`/`Cmd`+`Shift`+`B`). **Back to Timeline**, `Esc`
or the same command returns to the project.

The collection is shared by every project. It is kept in `~/Stemcel/Collection`:

- `collection.json` holds the tracks, playlists and histories.
- `collection.previous.json` is the version before the last save.
- `artwork/` holds the cover images.
- `anlz/` holds the analysis files made for exports.

## Bringing music in

- **Add Files…** and **Add Folder…**, or drop files and folders onto the screen. Dropped onto
  a playlist on show, they go into it too.
- **From rekordbox…** brings in this computer's rekordbox library:
  - every track with its beat grid, key, hot and memory cues, rating, colour and comment;
  - every playlist, inside a "rekordbox" folder.

  Tracks already in the collection keep their own grid and cues. rekordbox's library is
  only read, never changed. Bringing it in again replaces the "rekordbox" folder, so it
  isn't doubled.

New tracks are analysed in the background:

- tags and artwork;
- tempo and beat grid, with Stemcel's detection;
- key;
- loudness, for auto gain.

A track shows `~` until it is done, `x` if Stemcel couldn't read it, and `!` if its file
has gone missing (**Relocate…** in its menu finds it).

## Playlists

Right-click the tree for **New Playlist**, **New Folder**, **Rename** and **Delete**.

- Drag tracks onto a playlist to add them.
- Drag a playlist onto a folder to move it.
- Within a playlist sorted by `#`, drag rows to reorder them.
- `Delete` takes the selected tracks out of the playlist on show. In **Collection**, it
  removes them from the collection and every playlist; the audio files stay.

Right-click tracks for:

- **Add to Playlist**
- **Add to Project**, with the grid, key and cues
- **Colour**, **Rating** and **My Tags**
- **Analyse Again**
- **Edit Title and Artist**
- **Relocate**
- **Show in Folder**

The search box matches title, artist, album, genre, key, label, comment, My Tags and BPM.

## Beat grid and cues

The editor under the list shows the selected track:

- **The overview:** the whole track with its bars and cues. Click or drag to move the
  playhead.
- **The zoomed view:** the beats numbered 1–4.
  - Drag it to scrub.
  - The mouse wheel zooms.
  - Right-click to add or delete memory cues, or to put beat 1 there.

Grid controls:

| Control | Does |
|---|---|
| BPM field | Type a tempo to set the grid to it |
| `/2` `x2` | Half or double the tempo |
| `<` `>` | Move the grid 5 ms earlier or later (1 ms with `Shift`) |
| **1 Here** | Beat 1 of a bar at the playhead, keeping the tempo |
| **Tap** | Tap along to set the tempo |
| **Q** | Quantize: cues snap to the nearest beat |

A grid changed here is never replaced by analysis, unless you choose **Analyse Again**.

Hot cues **A–H**:

- An empty pad sets the cue at the playhead.
- A set pad jumps to its cue.
- Right-click a pad to set it here, make it a loop of 1–32 beats, name it (the CDJ shows
  the name) or delete it.
- **+ Memory** adds a memory cue.

Keys:

| Key | Does |
|---|---|
| `Space` | Play or pause |
| `←` `→` | Move a beat (a bar with `Shift`) |
| `1`–`8` | Hot cues A–H |
| `M` | Add a memory cue |

## Export to USB

**Export to USB…** (or a playlist's **Export to USB…**) writes playlists to a USB stick or SD
card as a rekordbox export:

- `PIONEER/rekordbox/export.pdb`, which every player reads;
- `exportLibrary.db` (Device Library Plus / OneLibrary), for newer players and rekordbox;
- the audio under `Contents/`;
- the analysis under `PIONEER/USBANLZ`.

Choose the drive and the playlists (a folder takes everything in it), then one of:

- **Sync** makes the stick hold exactly these playlists, as rekordbox's SYNC does. Tracks
  and playlists Stemcel put there before that aren't chosen now come off.
- **Add** puts the choice beside what the stick already has.

Each track's analysis is made from Stemcel's beat grid. Its bars come from the grid's beat 1
where you set one, and from rbxport's downbeat detection where you didn't. The waveforms
(the colour and 3-band ones CDJ-3000s show, and the older ones) are drawn by rbxport's
analyser. They are kept in `~/Stemcel/Collection/anlz`, so the next export of an unchanged
track reuses them.

Hot cues, memory cues, loops and their names go onto the stick as the CDJ's own. Ratings,
colours, keys and My Tags go on too.

Only new or changed audio is copied. The export checks the stick afterwards and says whether
it verified. **Eject when done** ejects it when it has finished. An export that is stopped
part way leaves the stick as it was.

The writing is done by [rbxport](https://github.com/chrisle/rbxport)'s export code. Its
authors check it against rekordbox's own exports and on players. Stemcel's own tests check
each export with rbxport's verifier and read it back as a player's stick would.

Neither has been tried on your players. Before relying on a stick at a gig, check it:

1. Load a few tracks on your players.
2. Check the grid, the waveform and the hot cues.
3. Check that the playlists browse.

## Link Export: players on the network

**LINK** in library mode serves the collection to players on the same network, as
rekordbox's LINK button does. Connect the computer to the players' network (a LINK hub, a
switch, or a player's LINK port), press **LINK**, and pick the network if there is more than
one.

The players see Stemcel as a rekordbox library:

- all tracks and every playlist;
- grids, waveforms and cues from the same analysis as a USB export.

The status line shows the players on the network and which are playing. Link keeps running
when you go back to the timeline or into performance mode. Press **LINK** again to stop it.

To serve the collection, Stemcel builds a rekordbox-format copy of it in its own folder
(`~/Stemcel/Collection/link`), made afresh each time Link starts. rekordbox's own library is
never used. rekordbox can't run Link at the same time: the players expect the same network
ports.

The serving is done by rbxport's Link code. Its authors test the network protocol against
captured player traffic, but say outright that this isn't the same as testing on players'
firmware. Neither they nor Stemcel's tests have tried it on your players: check it with
yours before a gig, and keep a USB stick as a backup.

## Play histories

The players keep a history of what was played on the stick. **Histories from USB** reads
them into **Histories**, one per session and stick:

- A track the stick got from Stemcel's export is matched to its collection track.
- Any other track is matched by its title and artist.
- A track not in the collection is left out.

Right-click a history to **Copy to a Playlist**.

## Auto gain

Each deck in [performance mode](performance-mode.md) measures its track's loudness as it
loads. The collection keeps the measurement. With **AUTO GAIN** on (the default), the
channel is evened out toward a reference loudness:

- quieter tracks come up by as much as 6 dB;
- louder ones go down by as much as 12 dB.

The trim knob works on top of it. A recorded take includes it, so it sounds as played.

The players' own auto gain reads a gain rekordbox writes from its analysis. An export
doesn't write that value yet: with your players' auto gain on, they won't adjust the
level for tracks from Stemcel.

## Performance mode

Performance mode's browser lists the collection under **DJ Library**: all tracks, the
playlists and the histories.

- Loading a track adds it to the project with the collection's grid, key and cues. If the
  project already has the file, its item is loaded.
- Hot and memory cues set on a deck are saved back to the collection, so they go onto the
  next USB export.
