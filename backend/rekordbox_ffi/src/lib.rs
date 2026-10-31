//! rekordbox library access for Stemcel's native app, over a C ABI.
//!
//! The heavy lifting (SQLCipher `master.db`, the columnar index, ANLZ analysis files, the
//! libraries on a USB or SD export) is rbxport's; this crate opens a library, flattens what
//! an importer needs into JSON, applies the few edits the app offers, and makes sure nothing
//! panics across the boundary. Every string returned is owned by this library and must go
//! back through `rbx_free_string`.
//!
//! Writes are refused while rekordbox is running (rbxport's rule), and the app copies the
//! database files aside before the first write of a session (see `Rekordbox.cpp`).

use std::ffi::{c_char, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};

use rbl_anlz::Anlz;
use rbl_db::{Library as Db, LibraryLocation, OpenMode};
use rbl_export::device_library::{self, Edit, Format};
use rbl_index::{Library, NO_ID};
use serde_json::{json, Map, Value};

/// An open library.
pub struct RbxLibrary {
    source: Source,
}

enum Source {
    /// A desktop `master.db` (the installed library, one kept on a drive, or a test
    /// fixture), loaded into rbxport's index.
    Desktop { index: Library, location: LibraryLocation },
    /// One of the two libraries on a USB or SD export, which rekordbox edits separately.
    Device { root: PathBuf, format: Format, library: device_library::Library },
}

fn format_name(format: Format) -> &'static str {
    match format {
        Format::DeviceLibrary => "deviceLibrary",
        Format::OneLibrary => "oneLibrary",
    }
}

fn parse_format(name: &str) -> Option<Format> {
    match name {
        "deviceLibrary" => Some(Format::DeviceLibrary),
        "oneLibrary" => Some(Format::OneLibrary),
        _ => None,
    }
}

fn to_c(text: &str) -> *mut c_char {
    // Interior NULs cannot occur in JSON text produced by serde_json.
    CString::new(text.replace('\0', "")).map_or(std::ptr::null_mut(), CString::into_raw)
}

fn error_json(message: &str) -> String {
    json!({ "ok": false, "error": message }).to_string()
}

/// Runs `f`, turning a panic into an error JSON rather than unwinding into C++.
fn guarded(f: impl FnOnce() -> String) -> *mut c_char {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(text) => to_c(&text),
        Err(_) => to_c(&error_json("internal error in the rekordbox reader")),
    }
}

/// # Safety
/// `ptr` must be null or a NUL-terminated string valid for the duration of the call.
unsafe fn opt_str<'a>(ptr: *const c_char) -> Option<&'a str> {
    if ptr.is_null() {
        return None;
    }
    CStr::from_ptr(ptr).to_str().ok().filter(|s| !s.is_empty())
}

// ---------------------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------------------

/// Opens a library read-only. With `master_db` null, the installed rekordbox library is
/// detected (agent `options.json`, passphrase unwrapped). Otherwise `master_db` is opened
/// with `passphrase`, or with rekordbox's own key when `passphrase` is null (a library kept
/// on a drive), and its analysis files are looked for in `share/` beside it.
///
/// Returns null on failure and, when `error_out` is non-null, stores an error message there
/// (free it with `rbx_free_string`).
///
/// # Safety
/// String arguments must be null or NUL-terminated; `error_out` null or writable.
#[no_mangle]
pub unsafe extern "C" fn rbx_open(
    master_db: *const c_char,
    passphrase: *const c_char,
    error_out: *mut *mut c_char,
) -> *mut RbxLibrary {
    let master_db = opt_str(master_db).map(PathBuf::from);
    let passphrase = opt_str(passphrase).unwrap_or_default().to_owned();
    let result = catch_unwind(AssertUnwindSafe(|| open(master_db, passphrase)));
    let message = match result {
        Ok(Ok(library)) => return Box::into_raw(Box::new(library)),
        Ok(Err(message)) => message,
        Err(_) => "internal error while opening the rekordbox library".to_owned(),
    };
    if !error_out.is_null() {
        *error_out = to_c(&message);
    }
    std::ptr::null_mut()
}

fn open(master_db: Option<PathBuf>, passphrase: String) -> Result<RbxLibrary, String> {
    let db = match master_db {
        None => Db::open_installed_read_only().map_err(|e| e.to_string())?,
        Some(master_db) if passphrase.is_empty() => {
            // A library rekordbox keeps on a drive: its key is the installed one's, and it
            // counts as a real library (writes wait for rekordbox to quit).
            let location = rbl_db::locate::Sources::installed()
                .and_then(|sources| sources.location_of(&master_db))
                .map_err(|e| e.to_string())?;
            Db::open(location, OpenMode::ReadOnly).map_err(|e| e.to_string())?
        }
        Some(master_db) => {
            let share_root = master_db.parent().map_or_else(|| PathBuf::from("share"), |p| p.join("share"));
            let location = LibraryLocation { master_db, share_root, passphrase, is_real_install: false };
            Db::open(location, OpenMode::ReadOnly).map_err(|e| e.to_string())?
        }
    };
    let (index, _) = rbl_index::load(&db).map_err(|e| format!("reading the library failed: {e}"))?;
    Ok(RbxLibrary { source: Source::Desktop { index, location: db.location().clone() } })
}

/// Opens one library on a USB or SD export mounted at `root`: `format` is `deviceLibrary`
/// (`export.pdb`) or `oneLibrary` (`exportLibrary.db`). Null on failure, with the message
/// in `error_out` as for `rbx_open`.
///
/// # Safety
/// String arguments must be null or NUL-terminated; `error_out` null or writable.
#[no_mangle]
pub unsafe extern "C" fn rbx_open_device(
    root: *const c_char,
    format: *const c_char,
    error_out: *mut *mut c_char,
) -> *mut RbxLibrary {
    let root = opt_str(root).map(PathBuf::from);
    let format = opt_str(format).and_then(parse_format);
    let result = catch_unwind(AssertUnwindSafe(|| match (root, format) {
        (Some(root), Some(format)) => device_library::read(&root, format)
            .map(|library| RbxLibrary { source: Source::Device { root, format, library } })
            .map_err(|e| e.to_string()),
        _ => Err("bad arguments".to_owned()),
    }));
    let message = match result {
        Ok(Ok(library)) => return Box::into_raw(Box::new(library)),
        Ok(Err(message)) => message,
        Err(_) => "internal error while opening the device's library".to_owned(),
    };
    if !error_out.is_null() {
        *error_out = to_c(&message);
    }
    std::ptr::null_mut()
}

/// # Safety
/// `library` must be null or a pointer returned by `rbx_open`, not yet closed.
#[no_mangle]
pub unsafe extern "C" fn rbx_close(library: *mut RbxLibrary) {
    if !library.is_null() {
        drop(Box::from_raw(library));
    }
}

/// # Safety
/// `text` must be null or a string returned by this library, not yet freed.
#[no_mangle]
pub unsafe extern "C" fn rbx_free_string(text: *mut c_char) {
    if !text.is_null() {
        drop(CString::from_raw(text));
    }
}

// ---------------------------------------------------------------------------------------
// Snapshot: tracks and the playlist tree
// ---------------------------------------------------------------------------------------

/// The whole library as JSON: `{ok, masterDb, shareRoot, tracks:[…], playlists:[…]}`.
///
/// # Safety
/// `library` must be a live pointer from `rbx_open`.
#[no_mangle]
pub unsafe extern "C" fn rbx_snapshot_json(library: *const RbxLibrary) -> *mut c_char {
    let Some(library) = library.as_ref() else { return to_c(&error_json("no library")) };
    guarded(|| snapshot(library).to_string())
}

fn non_empty(text: &str) -> Value {
    if text.is_empty() {
        Value::Null
    } else {
        Value::String(text.to_owned())
    }
}

fn track_json(lib: &Library, row: usize) -> Value {
    let r = u32::try_from(row).unwrap_or(u32::MAX);
    let bpm_x100 = lib.bpm_x100.get(row).copied().unwrap_or(0);
    let mut track = Map::new();
    track.insert("id".into(), Value::String(lib.ids[row].to_string()));
    track.insert("title".into(), Value::String(lib.title.get(row).to_owned()));
    track.insert("artist".into(), non_empty(lib.artist_name(r)));
    track.insert("album".into(), non_empty(lib.album_name(r)));
    track.insert("genre".into(), non_empty(lib.genre_name(r)));
    track.insert("label".into(), non_empty(lib.label_name(r)));
    track.insert("key".into(), non_empty(lib.key_name(r)));
    track.insert("bpm".into(), if bpm_x100 > 0 { json!(f64::from(bpm_x100) / 100.0) } else { Value::Null });
    track.insert("durationMs".into(), json!(u64::from(lib.length_sec.get(row).copied().unwrap_or(0)) * 1000));
    track.insert("rating".into(), json!(lib.rating.get(row).copied().unwrap_or(0)));
    track.insert("colour".into(), json!(lib.color.get(row).copied().unwrap_or(0)));
    track.insert("playCount".into(), json!(lib.play_count.get(row).copied().unwrap_or(0)));
    track.insert("sampleRate".into(), json!(lib.sample_rate.get(row).copied().unwrap_or(0)));
    track.insert("bitrate".into(), json!(lib.bitrate.get(row).copied().unwrap_or(0)));
    track.insert("year".into(), json!(lib.year.get(row).copied().unwrap_or(0)));
    track.insert("dateAdded".into(), non_empty(lib.date_added.get(row)));
    track.insert("comment".into(), non_empty(lib.comment.get(row)));
    track.insert("path".into(), Value::String(lib.folder_path.get(row).to_owned()));
    track.insert("analysisPath".into(), non_empty(lib.analysis_path.get(row)));
    track.insert("artworkPath".into(), non_empty(lib.artwork_path.get(row)));
    track.insert("hotCues".into(), Value::String(lib.cues().hot_letters_of(r)));
    Value::Object(track)
}

fn snapshot(library: &RbxLibrary) -> Value {
    match &library.source {
        Source::Desktop { index, location } => desktop_snapshot(index, location),
        Source::Device { root, format, library } => device_snapshot(root, *format, library),
    }
}

fn desktop_snapshot(lib: &Library, location: &LibraryLocation) -> Value {
    let tracks: Vec<Value> = (0..lib.len()).map(|row| track_json(lib, row)).collect();

    let playlists = lib.playlists();
    let mut lists = Vec::with_capacity(playlists.len());
    for i in 0..playlists.len() {
        let parent = playlists.parent[i];
        let parent_id = if parent == NO_ID {
            Value::Null
        } else {
            playlists.ids.get(parent as usize).map_or(Value::Null, |id| Value::String(id.to_string()))
        };
        let kind = if playlists.is_folder(i) {
            "folder"
        } else if playlists.is_smart(i) {
            "smart"
        } else {
            "playlist"
        };
        let track_ids: Vec<Value> = playlists.members[i]
            .iter()
            .filter_map(|&row| lib.ids.get(row as usize))
            .map(|id| Value::String(id.to_string()))
            .collect();
        lists.push(json!({
            "id": playlists.ids[i].to_string(),
            "name": playlists.name(i),
            "parentId": parent_id,
            "seq": playlists.seq[i],
            "kind": kind,
            "trackIds": track_ids,
        }));
    }

    json!({
        "ok": true,
        "masterDb": location.master_db.to_string_lossy(),
        "shareRoot": location.share_root.to_string_lossy(),
        "tracks": tracks,
        "playlists": lists,
    })
}

/// A path the device's library spells volume-relative (`/Contents/...`), under `root`.
fn on_device(root: &Path, relative: &str) -> PathBuf {
    relative.split(['/', '\\']).filter(|part| !part.is_empty()).fold(root.to_path_buf(), |path, part| path.join(part))
}

fn device_track_json(root: &Path, t: &device_library::Track) -> Value {
    json!({
        "id": t.id.to_string(),
        "title": t.title,
        "artist": non_empty(&t.artist),
        "album": non_empty(&t.album),
        "genre": non_empty(&t.genre),
        "label": non_empty(&t.label),
        "key": non_empty(&t.key),
        "bpm": if t.bpm_x100 > 0 { json!(f64::from(t.bpm_x100) / 100.0) } else { Value::Null },
        "durationMs": u64::from(t.duration_sec) * 1000,
        "rating": t.rating,
        "colour": t.color,
        "dateAdded": non_empty(&t.date_added),
        "comment": non_empty(&t.comment),
        "path": on_device(root, &t.path).to_string_lossy(),
        "analysisPath": non_empty(&t.analysis_path),
        "artworkPath": non_empty(&t.artwork_path),
        "hotCues": "",
    })
}

/// The device's database file, for the app to show and to back up beside.
fn device_database(root: &Path, format: Format) -> PathBuf {
    let export_root = rbl_export::export_root_name(root).unwrap_or("PIONEER");
    root.join(export_root).join("rekordbox").join(format.file_name())
}

fn device_snapshot(root: &Path, format: Format, lib: &device_library::Library) -> Value {
    let tracks: Vec<Value> = lib.tracks.iter().map(|t| device_track_json(root, t)).collect();
    let lists: Vec<Value> = lib
        .nodes
        .iter()
        .map(|n| {
            json!({
                "id": n.id.to_string(),
                "name": n.name,
                "parentId": if n.parent == 0 { Value::Null } else { Value::String(n.parent.to_string()) },
                "seq": n.sequence,
                "kind": if n.folder { "folder" } else { "playlist" },
                "trackIds": n.tracks.iter().map(|id| Value::String(id.to_string())).collect::<Vec<_>>(),
            })
        })
        .collect();
    json!({
        "ok": true,
        "masterDb": device_database(root, format).to_string_lossy(),
        "shareRoot": root.to_string_lossy(),
        "device": format_name(format),
        "tracks": tracks,
        "playlists": lists,
    })
}

// ---------------------------------------------------------------------------------------
// One track: cues, beat grid, phrases
// ---------------------------------------------------------------------------------------

/// `{ok, id, cues:[…], beats:[[timeMs, beatNumber, bpm], …], phrases:[…], mood}` for one
/// track by its rekordbox id. Missing analysis is not an error: the lists are empty.
///
/// # Safety
/// `library` must be a live pointer from `rbx_open`; `track_id` NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn rbx_track_json(library: *const RbxLibrary, track_id: *const c_char) -> *mut c_char {
    let Some(library) = library.as_ref() else { return to_c(&error_json("no library")) };
    let Some(id) = opt_str(track_id).and_then(|s| s.parse::<u64>().ok()) else {
        return to_c(&error_json("bad track id"));
    };
    guarded(|| match &library.source {
        Source::Desktop { index, location } => track_detail(index, &location.share_root, id).to_string(),
        Source::Device { root, library, .. } => device_track_detail(root, library, id).to_string(),
    })
}

/// A device track's grid and phrases from its analysis files, and its cues from the
/// extended cue lists there (a stick's databases keep no cue rows of their own).
fn device_track_detail(root: &Path, lib: &device_library::Library, id: u64) -> Value {
    let Some(track) = u32::try_from(id).ok().and_then(|id| lib.track(id)) else {
        return json!({ "ok": false, "error": "no such track" });
    };
    let mut detail = analysis(root, &track.analysis_path);
    let mut cues = Vec::new();
    if !track.analysis_path.is_empty() {
        let dat = rbl_anlz::resolve(root, &track.analysis_path);
        if let Ok(ext) = Anlz::read(&rbl_anlz::sibling(&dat, "EXT")) {
            for c in ext.cue_entries() {
                let hot = (c.hot_cue > 0)
                    .then(|| char::from_u32(u32::from(b'A') + c.hot_cue - 1))
                    .flatten()
                    .map(|l| l.to_string());
                cues.push(json!({
                    "positionMs": c.time_ms,
                    "endMs": if c.kind == 2 && c.loop_time_ms > c.time_ms { json!(c.loop_time_ms) } else { Value::Null },
                    "memory": hot.is_none(),
                    "hot": hot,
                    "colour": c.color_code.unwrap_or(c.color_id),
                }));
            }
        }
    }
    if let Value::Object(map) = &mut detail {
        map.insert("ok".into(), Value::Bool(true));
        map.insert("id".into(), Value::String(id.to_string()));
        map.insert("cues".into(), Value::Array(cues));
        map.insert("track".into(), device_track_json(root, track));
    }
    detail
}

fn track_detail(lib: &Library, share_root: &Path, id: u64) -> Value {
    let Some(row) = lib.ids.iter().position(|&x| x == id) else { return json!({ "ok": false, "error": "no such track" }) };
    let r = u32::try_from(row).unwrap_or(u32::MAX);

    let cues: Vec<Value> = lib
        .cues_of(r)
        .iter()
        .map(|c| {
            json!({
                "positionMs": c.position_ms,
                // rekordbox stores u32::MAX (or 0) for a cue that is not a loop.
                "endMs": if c.out_ms > c.position_ms && c.out_ms != u32::MAX { json!(c.out_ms) } else { Value::Null },
                "hot": c.hot_letter().map(|l| l.to_string()),
                "memory": c.is_memory(),
                "colour": c.colour,
            })
        })
        .collect();

    let mut detail = analysis(share_root, lib.analysis_path.get(row));
    if let Value::Object(map) = &mut detail {
        map.insert("ok".into(), Value::Bool(true));
        map.insert("id".into(), Value::String(id.to_string()));
        map.insert("cues".into(), Value::Array(cues));
        map.insert("track".into(), track_json(lib, row));
    }
    detail
}

fn analysis(share_root: &Path, analysis_path: &str) -> Value {
    let mut beats_json = Vec::new();
    let mut phrases_json = Vec::new();
    let mut mood = Value::Null;
    if analysis_path.is_empty() {
        return json!({ "beats": beats_json, "phrases": phrases_json, "mood": mood, "bands": [] });
    }
    let dat = rbl_anlz::resolve(share_root, analysis_path);
    let beats = Anlz::read(&dat).ok().and_then(|a| a.beat_grid()).unwrap_or_default();
    for b in &beats {
        beats_json.push(json!([b.time_ms, b.beat_number, f64::from(b.tempo_x100) / 100.0]));
    }

    // Phrases live in the extended file; their positions are 1-based beats of the grid.
    let beat_ms = |beat: u16| -> Option<u32> {
        let i = usize::from(beat).checked_sub(1)?;
        beats.get(i).map(|b| b.time_ms)
    };
    let ext = rbl_anlz::sibling(&dat, "EXT");
    if let Some(structure) = Anlz::read(&ext).ok().and_then(|a| a.song_structure()) {
        mood = Value::String(format!("{:?}", structure.mood).to_lowercase());
        let count = structure.phrases.len();
        for (i, p) in structure.phrases.iter().enumerate() {
            let end_beat = if i + 1 < count { structure.phrases[i + 1].beat } else { structure.end_beat };
            phrases_json.push(json!({
                "label": p.label,
                "kind": p.kind,
                "beat": p.beat,
                "endBeat": end_beat,
                "startMs": beat_ms(p.beat),
                "endMs": beat_ms(end_beat),
                "fillIn": p.fill_in,
            }));
        }
    }
    json!({ "beats": beats_json, "phrases": phrases_json, "mood": mood, "bands": bands(&dat) })
}

/// rekordbox's 3-band waveform (`PWV7` in the `.2EX` file: low, mid and high out of 127,
/// 150 columns a second), averaged to two values a second and scaled to 0..1, as
/// `[[low, mid, high], ...]`. Empty when the track has no such file.
fn bands(dat: &Path) -> Value {
    const COLUMNS_PER_SECOND: usize = 150;
    const PER_SECOND: usize = 2;
    let two_ex = rbl_anlz::sibling(dat, "2EX");
    let Ok(anlz) = Anlz::read(&two_ex) else { return Value::Array(Vec::new()) };
    let Some((stride, payload)) = anlz.waveform(b"PWV7") else { return Value::Array(Vec::new()) };
    let stride = (stride as usize).max(3);
    let columns: Vec<&[u8]> = payload.chunks_exact(stride).collect();
    let step = COLUMNS_PER_SECOND / PER_SECOND;
    let out: Vec<Value> = columns
        .chunks(step)
        .map(|chunk| {
            let mut sum = [0.0f64; 3];
            for column in chunk {
                for (band, total) in sum.iter_mut().enumerate() {
                    *total += f64::from(column[band] & 0x7f);
                }
            }
            let n = chunk.len().max(1) as f64 * 127.0;
            json!([sum[0] / n, sum[1] / n, sum[2] / n])
        })
        .collect();
    Value::Array(out)
}

// ---------------------------------------------------------------------------------------
// Tags of any audio file (Files browser, import)
// ---------------------------------------------------------------------------------------

/// `{ok, title, artist, album, genre, year, bpm, key, durationMs, sampleRate, channels,
/// bitrate, hasPicture}` read with lofty. Fields the file lacks are null.
///
/// # Safety
/// `path` must be a NUL-terminated UTF-8 path.
#[no_mangle]
pub unsafe extern "C" fn rbx_read_tags_json(path: *const c_char) -> *mut c_char {
    let Some(path) = opt_str(path).map(PathBuf::from) else { return to_c(&error_json("no path")) };
    guarded(|| read_tags(&path).unwrap_or_else(|e| error_json(&e)))
}

fn read_tags(path: &Path) -> Result<String, String> {
    use lofty::prelude::*;
    use lofty::tag::ItemKey;

    let tagged = lofty::probe::Probe::open(path)
        .map_err(|e| e.to_string())?
        .guess_file_type()
        .map_err(|e| e.to_string())?
        .read()
        .map_err(|e| e.to_string())?;
    let props = tagged.properties();
    let tag = tagged.primary_tag().or_else(|| tagged.first_tag());
    let text = |key: &ItemKey| tag.and_then(|t| t.get_string(key)).map(str::to_owned);
    let bpm = text(&ItemKey::Bpm)
        .or_else(|| text(&ItemKey::IntegerBpm))
        .and_then(|s| s.trim().parse::<f64>().ok())
        .filter(|b| *b > 0.0);
    let year = text(&ItemKey::Year)
        .or_else(|| text(&ItemKey::RecordingDate))
        .and_then(|s| s.get(..4).and_then(|y| y.parse::<u32>().ok()));
    Ok(json!({
        "ok": true,
        "title": tag.and_then(|t| t.title()).map(|s| s.into_owned()),
        "artist": tag.and_then(|t| t.artist()).map(|s| s.into_owned()),
        "album": tag.and_then(|t| t.album()).map(|s| s.into_owned()),
        "genre": tag.and_then(|t| t.genre()).map(|s| s.into_owned()),
        "year": year,
        "bpm": bpm,
        "key": text(&ItemKey::InitialKey),
        "durationMs": u64::try_from(props.duration().as_millis()).unwrap_or(0),
        "sampleRate": props.sample_rate(),
        "channels": props.channels(),
        "bitrate": props.audio_bitrate(),
        "hasPicture": tag.is_some_and(|t| !t.pictures().is_empty()),
    })
    .to_string())
}

/// The first embedded picture of an audio file, written to `out_path`. Returns 1 when one
/// was written, 0 when the file has none or cannot be read.
///
/// # Safety
/// Both paths must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rbx_extract_picture(path: *const c_char, out_path: *const c_char) -> i32 {
    let (Some(path), Some(out)) = (opt_str(path), opt_str(out_path)) else { return 0 };
    let (path, out) = (PathBuf::from(path), PathBuf::from(out));
    catch_unwind(AssertUnwindSafe(|| {
        use lofty::prelude::*;
        let tagged = lofty::read_from_path(&path).ok()?;
        let tag = tagged.primary_tag().or_else(|| tagged.first_tag())?;
        let picture = tag.pictures().first()?;
        std::fs::write(&out, picture.data()).ok()
    }))
    .ok()
    .flatten()
    .map_or(0, |()| 1)
}

// ---------------------------------------------------------------------------------------
// Decoding formats the engine cannot read
// ---------------------------------------------------------------------------------------

/// Decodes `path` (m4a/aac/alac, ogg/vorbis) to a 32-bit float WAV at `out_path`, written
/// beside it under a temporary name and renamed into place. Returns 1 on success.
///
/// # Safety
/// Both paths must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rbx_decode_to_wav(path: *const c_char, out_path: *const c_char) -> i32 {
    let (Some(path), Some(out)) = (opt_str(path), opt_str(out_path)) else { return 0 };
    let (path, out) = (PathBuf::from(path), PathBuf::from(out));
    match catch_unwind(AssertUnwindSafe(|| decode_to_wav(&path, &out))) {
        Ok(Ok(())) => 1,
        _ => 0,
    }
}

fn decode_to_wav(path: &Path, out: &Path) -> Result<(), String> {
    use std::io::{Seek, SeekFrom, Write};
    use symphonia::core::audio::SampleBuffer;
    use symphonia::core::codecs::{DecoderOptions, CODEC_TYPE_NULL};
    use symphonia::core::errors::Error;
    use symphonia::core::formats::FormatOptions;
    use symphonia::core::io::MediaSourceStream;
    use symphonia::core::meta::MetadataOptions;
    use symphonia::core::probe::Hint;

    let file = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let stream = MediaSourceStream::new(Box::new(file), Default::default());
    let mut hint = Hint::new();
    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
        hint.with_extension(ext);
    }
    let probed = symphonia::default::get_probe()
        .format(&hint, stream, &FormatOptions::default(), &MetadataOptions::default())
        .map_err(|e| e.to_string())?;
    let mut format = probed.format;
    let track = format
        .tracks()
        .iter()
        .find(|t| t.codec_params.codec != CODEC_TYPE_NULL)
        .ok_or("no audio track")?;
    let track_id = track.id;
    let mut decoder = symphonia::default::get_codecs()
        .make(&track.codec_params, &DecoderOptions::default())
        .map_err(|e| e.to_string())?;

    let tmp = out.with_extension("partial");
    let mut wav = std::io::BufWriter::new(std::fs::File::create(&tmp).map_err(|e| e.to_string())?);
    wav.write_all(&[0u8; 44]).map_err(|e| e.to_string())?;
    let mut rate = track.codec_params.sample_rate.unwrap_or(0);
    let mut channels = track.codec_params.channels.map_or(0, |c| c.count());
    let mut data_bytes: u64 = 0;
    let mut samples: Option<SampleBuffer<f32>> = None;

    loop {
        let packet = match format.next_packet() {
            Ok(p) => p,
            Err(Error::IoError(e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(Error::ResetRequired) => break,
            Err(e) => return Err(e.to_string()),
        };
        if packet.track_id() != track_id {
            continue;
        }
        let decoded = match decoder.decode(&packet) {
            Ok(d) => d,
            Err(Error::DecodeError(_)) => continue,
            Err(e) => return Err(e.to_string()),
        };
        let spec = *decoded.spec();
        rate = spec.rate;
        channels = spec.channels.count();
        let buffer = match &mut samples {
            Some(b) if b.capacity() >= decoded.capacity() * channels => b,
            _ => samples.insert(SampleBuffer::<f32>::new(decoded.capacity() as u64, spec)),
        };
        buffer.copy_interleaved_ref(decoded);
        for s in buffer.samples() {
            wav.write_all(&s.to_le_bytes()).map_err(|e| e.to_string())?;
        }
        data_bytes += (buffer.samples().len() * 4) as u64;
    }
    if rate == 0 || channels == 0 || data_bytes == 0 {
        drop(wav);
        let _ = std::fs::remove_file(&tmp);
        return Err("nothing decoded".into());
    }

    // RIFF header for IEEE float PCM.
    let channels16 = u16::try_from(channels).map_err(|_| "too many channels")?;
    let data32 = u32::try_from(data_bytes).map_err(|_| "decoded audio exceeds 4 GB")?;
    let mut header = Vec::with_capacity(44);
    header.extend_from_slice(b"RIFF");
    header.extend_from_slice(&(36 + data32).to_le_bytes());
    header.extend_from_slice(b"WAVEfmt ");
    header.extend_from_slice(&16u32.to_le_bytes());
    header.extend_from_slice(&3u16.to_le_bytes());
    header.extend_from_slice(&channels16.to_le_bytes());
    header.extend_from_slice(&rate.to_le_bytes());
    header.extend_from_slice(&(rate * u32::from(channels16) * 4).to_le_bytes());
    header.extend_from_slice(&(channels16 * 4).to_le_bytes());
    header.extend_from_slice(&32u16.to_le_bytes());
    header.extend_from_slice(b"data");
    header.extend_from_slice(&data32.to_le_bytes());
    let mut file = wav.into_inner().map_err(|e| e.to_string())?;
    file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
    file.write_all(&header).map_err(|e| e.to_string())?;
    file.sync_all().ok();
    drop(file);
    std::fs::rename(&tmp, out).map_err(|e| e.to_string())
}

/// `{ok, masterDb, shareRoot}` of the open library without reading it all: the database
/// file (a device's `export.pdb` or `exportLibrary.db`) the app backs up before writing.
///
/// # Safety
/// `library` must be a live pointer from `rbx_open` or `rbx_open_device`.
#[no_mangle]
pub unsafe extern "C" fn rbx_location_json(library: *const RbxLibrary) -> *mut c_char {
    let Some(library) = library.as_ref() else { return to_c(&error_json("no library")) };
    guarded(|| {
        let (database, share_root) = match &library.source {
            Source::Desktop { location, .. } => (location.master_db.clone(), location.share_root.clone()),
            Source::Device { root, format, .. } => (device_database(root, *format), root.clone()),
        };
        json!({ "ok": true, "masterDb": database.to_string_lossy(), "shareRoot": share_root.to_string_lossy() }).to_string()
    })
}

// ---------------------------------------------------------------------------------------
// Libraries on connected drives
// ---------------------------------------------------------------------------------------

/// What can be opened besides the installed library: `{ok, devices:[{name, root,
/// removable, formats:["deviceLibrary"|"oneLibrary", …]}], driveLibraries:[{name, volume,
/// masterDb}]}`. A device is listed only when it holds a rekordbox export; a library kept on
/// a drive is left out when it is the one rekordbox is using (that is "this computer").
#[no_mangle]
pub extern "C" fn rbx_devices_json() -> *mut c_char {
    guarded(|| devices().to_string())
}

fn devices() -> Value {
    let devices: Vec<Value> = rbl_devices::list()
        .into_iter()
        .filter_map(|d| {
            let formats: Vec<&str> = device_library::formats(&d.mount_point).into_iter().map(format_name).collect();
            (!formats.is_empty()).then(|| {
                json!({
                    "name": d.name,
                    "root": d.mount_point.to_string_lossy(),
                    "removable": d.removable,
                    "formats": formats,
                })
            })
        })
        .collect();
    let installed = rbl_db::detect().ok().map(|location| location.master_db);
    let drives: Vec<Value> = rbl_devices::libraries::discover()
        .into_iter()
        .filter(|l| installed.as_ref() != Some(&l.master_db))
        .map(|l| {
            json!({
                "name": l.name,
                "volume": l.volume.to_string_lossy(),
                "masterDb": l.master_db.to_string_lossy(),
            })
        })
        .collect();
    json!({ "ok": true, "devices": devices, "driveLibraries": drives })
}

// ---------------------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------------------

/// `{ok:true}` when the open library may be written now, else `{ok:false, error}`. A
/// desktop library waits for rekordbox and its agent to quit; a device only for rekordbox
/// itself (the agent leaves sticks alone).
///
/// # Safety
/// `library` must be a live pointer from `rbx_open` or `rbx_open_device`.
#[no_mangle]
pub unsafe extern "C" fn rbx_write_refusal_json(library: *const RbxLibrary) -> *mut c_char {
    let Some(library) = library.as_ref() else { return to_c(&error_json("no library")) };
    guarded(|| match write_refusal(library) {
        None => json!({ "ok": true }).to_string(),
        Some(reason) => error_json(reason),
    })
}

fn write_refusal(library: &RbxLibrary) -> Option<&'static str> {
    match &library.source {
        Source::Desktop { location, .. } if location.is_real_install && rbl_db::is_rekordbox_running() => {
            Some("rekordbox is running. Quit rekordbox and its agent (rekordboxAgent, in the system tray or menu bar), then try again.")
        }
        Source::Device { .. } if rbl_db::is_rekordbox_app_running() => {
            Some("rekordbox is running. Quit it before changing a device's library, then try again.")
        }
        _ => None,
    }
}

/// Applies one edit, given as JSON, to the open library and returns `{ok, id?, changed}` or
/// `{ok:false, error}`. The open snapshot is not updated: reopen the library to see it.
///
/// Actions: `createPlaylist {name, parentId?, folder?}`, `rename {id, name}`, `delete {id}`,
/// `addTracks {playlistId, trackIds}`, `removeTracks {playlistId, trackIds}`, and, for a
/// desktop library only, `importFiles {paths, playlistId? | playlist?}` (adds audio files
/// to the collection, and to that playlist, or to the playlist of that name at the top
/// level, made if missing).
///
/// The caller backs the database up first; this takes no backup of its own.
///
/// # Safety
/// `library` must be a live pointer from `rbx_open` or `rbx_open_device`; `request`
/// NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn rbx_write_json(library: *const RbxLibrary, request: *const c_char) -> *mut c_char {
    let Some(library) = library.as_ref() else { return to_c(&error_json("no library")) };
    let Some(request) = opt_str(request).and_then(|text| serde_json::from_str::<Value>(text).ok()) else {
        return to_c(&error_json("bad request"));
    };
    guarded(|| {
        if let Some(reason) = write_refusal(library) {
            return error_json(reason);
        }
        let result = match &library.source {
            Source::Desktop { location, .. } => desktop_write(location, &request),
            Source::Device { root, format, .. } => device_write(root, *format, &request),
        };
        result.unwrap_or_else(|e| error_json(&e))
    })
}

fn text<'a>(request: &'a Value, key: &str) -> Result<&'a str, String> {
    request.get(key).and_then(Value::as_str).filter(|s| !s.is_empty()).ok_or_else(|| format!("missing {key}"))
}

fn id_list(request: &Value, key: &str) -> Vec<String> {
    request
        .get(key)
        .and_then(Value::as_array)
        .map(|ids| ids.iter().filter_map(|v| v.as_str().map(str::to_owned)).collect())
        .unwrap_or_default()
}

fn desktop_write(location: &LibraryLocation, request: &Value) -> Result<String, String> {
    use rbl_db::write::{ImportOutcome, Writer, ROOT};
    let e = |e: rbl_db::DbError| e.to_string();
    // The app copies the database files aside before a session's first write, so the
    // writer's own snapshot backup is not taken as well.
    let backups = std::env::temp_dir().join("stemcel-rekordbox-writer");
    let mut writer = Writer::open(location.clone(), backups).map_err(e)?;
    writer.disable_automatic_backups();

    let reply = match text(request, "action")? {
        "createPlaylist" => {
            let name = text(request, "name")?;
            let parent = request.get("parentId").and_then(Value::as_str).filter(|s| !s.is_empty()).unwrap_or(ROOT);
            let folder = request.get("folder").and_then(Value::as_bool).unwrap_or(false);
            let id = if folder { writer.create_folder(name, parent) } else { writer.create_playlist(name, parent) }.map_err(e)?;
            json!({ "ok": true, "id": id, "changed": 1 })
        }
        "rename" => {
            let changed = writer.rename(text(request, "id")?, text(request, "name")?).map_err(e)?;
            json!({ "ok": true, "changed": changed.rows })
        }
        "delete" => {
            let changed = writer.delete_playlist(text(request, "id")?).map_err(e)?;
            json!({ "ok": true, "changed": changed.rows })
        }
        "addTracks" => {
            let changed = writer.add_tracks(text(request, "playlistId")?, &id_list(request, "trackIds")).map_err(e)?;
            json!({ "ok": true, "changed": changed.rows })
        }
        "removeTracks" => {
            let changed = writer.remove_tracks(text(request, "playlistId")?, &id_list(request, "trackIds")).map_err(e)?;
            json!({ "ok": true, "changed": changed.rows })
        }
        "importFiles" => {
            let paths: Vec<PathBuf> = id_list(request, "paths").into_iter().map(PathBuf::from).collect();
            let outcomes = writer.import_files(&paths).map_err(e)?;
            let (mut ids, mut added, mut existing, mut refused) = (Vec::new(), 0, 0, Vec::new());
            for (path, outcome) in paths.iter().zip(outcomes) {
                match outcome {
                    ImportOutcome::Added(id) => {
                        added += 1;
                        ids.push(id);
                    }
                    ImportOutcome::Existing(id) => {
                        existing += 1;
                        ids.push(id);
                    }
                    ImportOutcome::Refused(why) => refused.push(format!("{}: {why}", path.display())),
                }
            }
            let named = request.get("playlist").and_then(Value::as_str).filter(|s| !s.is_empty());
            let target = match request.get("playlistId").and_then(Value::as_str).filter(|s| !s.is_empty()) {
                Some(id) => Some(id.to_owned()),
                None => match named {
                    Some(name) => Some(match writer.child_named(ROOT, name).map_err(e)? {
                        Some(id) => id,
                        None => writer.create_playlist(name, ROOT).map_err(e)?,
                    }),
                    None => None,
                },
            };
            if let (Some(id), false) = (&target, ids.is_empty()) {
                writer.add_tracks(id, &ids).map_err(e)?;
            }
            let playlist = target.map_or(Value::Null, Value::String);
            json!({ "ok": true, "added": added, "existing": existing, "refused": refused, "trackIds": ids, "id": playlist, "changed": added })
        }
        other => return Err(format!("unknown action {other}")),
    };
    Ok(reply.to_string())
}

fn device_write(root: &Path, format: Format, request: &Value) -> Result<String, String> {
    let number = |key: &str| -> Result<u32, String> { text(request, key)?.parse::<u32>().map_err(|_| format!("bad {key}")) };
    let numbers = |key: &str| -> Vec<u32> { id_list(request, key).iter().filter_map(|id| id.parse().ok()).collect() };
    let edit = match text(request, "action")? {
        "createPlaylist" => Edit::Create {
            parent: request.get("parentId").and_then(Value::as_str).and_then(|id| id.parse().ok()).unwrap_or(0),
            name: text(request, "name")?.to_owned(),
            folder: request.get("folder").and_then(Value::as_bool).unwrap_or(false),
        },
        "rename" => Edit::Rename { id: number("id")?, name: text(request, "name")?.to_owned() },
        "delete" => Edit::Delete { id: number("id")? },
        "addTracks" => Edit::Add { playlist: number("playlistId")?, tracks: numbers("trackIds"), duplicates: false },
        "removeTracks" => Edit::Remove { playlist: number("playlistId")?, tracks: numbers("trackIds") },
        "importFiles" => {
            return Err("New audio goes onto a device through a rekordbox export. Add the tracks to this computer's library, then export or sync them from rekordbox.".to_owned())
        }
        other => return Err(format!("unknown action {other}")),
    };
    let applied = device_library::apply(root, format, &edit).map_err(|e| e.to_string())?;
    Ok(json!({ "ok": true, "id": applied.id.to_string(), "changed": applied.changed }).to_string())
}

// ---------------------------------------------------------------------------------------
// Test support
// ---------------------------------------------------------------------------------------

/// Builds rbxport's throwaway fixture library in `dir` (never touching the installed one)
/// for the app's smoke test: its first track points at `audio_path` and carries a
/// 128 BPM grid whose first downbeat is at 120 ms. Returns `{ok, masterDb, passphrase,
/// trackId}`.
///
/// # Safety
/// Both paths must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rbx_build_fixture(dir: *const c_char, audio_path: *const c_char) -> *mut c_char {
    let (Some(dir), Some(audio)) = (opt_str(dir), opt_str(audio_path)) else { return to_c(&error_json("bad arguments")) };
    let (dir, audio) = (PathBuf::from(dir), audio.to_owned());
    guarded(|| build_fixture(&dir, &audio).unwrap_or_else(|e| error_json(&e)))
}

fn build_fixture(dir: &Path, audio: &str) -> Result<String, String> {
    use rbl_db::fixture;
    std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    let location = fixture::build(dir, fixture::Shape::default()).map_err(|e| e.to_string())?;
    fixture::point_at_audio(&location, 0, audio, 4).map_err(|e| e.to_string())?;
    fixture::set_tempo(&location, 0, 12_800).map_err(|e| e.to_string())?;
    let beats: Vec<rbl_anlz::Beat> = (0..16u32)
        .map(|i| rbl_anlz::Beat {
            beat_number: u16::try_from(i % 4 + 1).unwrap_or(1),
            tempo_x100: 12_800,
            time_ms: 120 + i * 60_000 / 128,
        })
        .collect();
    let relative = "/PIONEER/USBANLZ/smoke/0001/ANLZ0000.DAT";
    let dat = rbl_anlz::resolve(&location.share_root, relative);
    if let Some(parent) = dat.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    std::fs::write(&dat, rbl_anlz::write::AnlzBuilder::new().beat_grid(&beats).finish()).map_err(|e| e.to_string())?;
    // Four seconds of rekordbox's 3-band waveform: low band (a kick) throughout.
    let columns: Vec<u8> = (0..600).flat_map(|_| [110u8, 40, 30]).collect();
    std::fs::write(
        rbl_anlz::sibling(&dat, "2EX"),
        rbl_anlz::write::AnlzBuilder::new().waveform_scroll(b"PWV7", 3, &columns).finish(),
    )
    .map_err(|e| e.to_string())?;
    fixture::set_analysis_path(&location, 0, relative).map_err(|e| e.to_string())?;
    Ok(json!({
        "ok": true,
        "masterDb": location.master_db.to_string_lossy(),
        "passphrase": fixture::FIXTURE_PASSPHRASE,
        "trackId": fixture::track_id(0),
    })
    .to_string())
}

/// Builds a throwaway USB export in `dir` (both libraries, written by rbxport's exporter)
/// holding `audio` as one 128 BPM track in a playlist called "Stick list", for the smoke
/// test's device picker and edits. Returns `{ok, root, trackId, playlistId}`.
///
/// # Safety
/// Both paths must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rbx_build_device_fixture(dir: *const c_char, audio_path: *const c_char) -> *mut c_char {
    let (Some(dir), Some(audio)) = (opt_str(dir), opt_str(audio_path)) else { return to_c(&error_json("bad arguments")) };
    let (dir, audio) = (PathBuf::from(dir), PathBuf::from(audio));
    guarded(|| build_device_fixture(&dir, &audio).unwrap_or_else(|e| error_json(&e)))
}

fn build_device_fixture(dir: &Path, audio: &Path) -> Result<String, String> {
    std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    let beats: Vec<rbl_anlz::Beat> = (0..16u32)
        .map(|i| rbl_anlz::Beat {
            beat_number: u16::try_from(i % 4 + 1).unwrap_or(1),
            tempo_x100: 12_800,
            time_ms: 120 + i * 60_000 / 128,
        })
        .collect();
    let track = rbl_export::SourceTrack {
        id: 1,
        source_path: audio.to_path_buf(),
        title: "Stick Track".into(),
        artist: "Stemcel".into(),
        bpm_x100: 12_800,
        duration_sec: 4,
        analysis: vec![("DAT".into(), rbl_anlz::write::AnlzBuilder::new().beat_grid(&beats).finish())],
        ..rbl_export::SourceTrack::default()
    };
    let playlist = rbl_export::SourcePlaylist { id: 10, name: "Stick list".into(), track_indices: vec![0], ..Default::default() };
    rbl_export::export_full(dir, &[track], &[playlist], &[], None, None, &mut |_| {}).map_err(|e| e.to_string())?;
    let library = device_library::read(dir, Format::OneLibrary).map_err(|e| e.to_string())?;
    let track_id = library.tracks.first().map(|t| t.id.to_string()).unwrap_or_default();
    let playlist_id = library.nodes.iter().find(|n| n.name == "Stick list").map(|n| n.id.to_string()).unwrap_or_default();
    Ok(json!({ "ok": true, "root": dir.to_string_lossy(), "trackId": track_id, "playlistId": playlist_id }).to_string())
}

#[cfg(test)]
mod tests;
