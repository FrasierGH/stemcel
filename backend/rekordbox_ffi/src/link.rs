//! Link Export without rekordbox: Stemcel's collection served to players on the same
//! network (Pro DJ Link), as rekordbox's LINK button does, so a CDJ browses and loads it
//! with no USB stick.
//!
//! rbxport's Link code (`rbl-link`) serves a rekordbox-format library. Stemcel builds one
//! of its own for it from the collection: a private library in its own folder, never
//! rekordbox's, made afresh each time Link starts. It holds the tracks, their analysis
//! files (made as for a USB export, from the same cache), their cues and ratings, and the
//! playlists.

use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use rbl_db::write::{AnalysisWrite, ImportOutcome, Writer, ROOT};
use rbl_db::{Library as Db, OpenMode};
use rbl_link::{Interface, LinkExport, Ports, StaticSource};
use serde_json::{json, Value};

use crate::analyse;

static LINK: Mutex<Option<LinkExport>> = Mutex::new(None);

fn string(v: &Value, key: &str) -> String {
    v.get(key).and_then(Value::as_str).unwrap_or_default().to_owned()
}

/// The network interfaces players could be on.
pub fn interfaces() -> Value {
    let list: Vec<Value> = rbl_link::interfaces()
        .into_iter()
        .map(|i| json!({ "name": i.name, "address": i.address.to_string() }))
        .collect();
    json!({ "ok": true, "interfaces": list })
}

/// Builds the private library from the collection (`request`: libraryDir, cacheDir,
/// tracks and playlists as for an export) and starts serving it on `interface` (a name;
/// the first when empty). Stops a session already running first.
pub fn start(request: &Value, progress: &mut dyn FnMut(Value)) -> Result<Value, String> {
    stop();
    let dir = PathBuf::from(string(request, "libraryDir"));
    if dir.as_os_str().is_empty() {
        return Err("no folder for the Link library".to_owned());
    }
    let cache = PathBuf::from(string(request, "cacheDir"));
    // Made afresh: it is a copy of the collection, nothing of its own.
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let shape = rbl_db::fixture::Shape { tracks: 0, playlists: 0, tracks_per_playlist: 0, history_sessions: 0, start_usn: 1000 };
    let location = rbl_db::fixture::build(&dir, shape).map_err(|e| e.to_string())?;
    let e = |e: rbl_db::DbError| e.to_string();
    let mut writer = Writer::open(location.clone(), dir.join("backups")).map_err(e)?;
    writer.disable_automatic_backups();

    let entries = request.get("tracks").and_then(Value::as_array).cloned().unwrap_or_default();
    let paths: Vec<PathBuf> = entries.iter().map(|t| PathBuf::from(string(t, "path"))).collect();
    let outcomes = writer.import_files(&paths).map_err(e)?;
    let mut ids: Vec<Option<String>> = Vec::with_capacity(entries.len());
    let mut failed = Vec::new();
    let total = entries.len();
    for (i, (entry, outcome)) in entries.iter().zip(outcomes).enumerate() {
        progress(json!({ "stage": "analysing", "done": i, "total": total, "title": string(entry, "title") }));
        let id = match outcome {
            ImportOutcome::Added(id) | ImportOutcome::Existing(id) => id,
            ImportOutcome::Refused(why) => {
                failed.push(format!("{}: {why}", string(entry, "title")));
                ids.push(None);
                continue;
            }
        };
        let path = PathBuf::from(string(entry, "path"));
        match crate::stick::analysis_files(entry, &path, &cache) {
            Ok(files) => {
                let relative = writer.analysis_data_path_for(&id).map_err(e)?;
                let dat = rbl_anlz::resolve(&location.share_root, &relative);
                if let Some(parent) = dat.parent() {
                    std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
                }
                for (extension, bytes) in &files {
                    std::fs::write(dat.with_extension(extension), bytes).map_err(|e| e.to_string())?;
                }
                let bpm = entry.get("bpm").and_then(Value::as_f64).unwrap_or(0.0);
                let key = analyse::key_spellings(&string(entry, "key")).into_iter().next();
                writer
                    .set_analysis(
                        &id,
                        &AnalysisWrite {
                            bpm_x100: (bpm * 100.0).round().max(0.0) as u32,
                            key: key.as_deref(),
                            analysis_path: &relative,
                            length_sec: entry.get("durationSec").and_then(Value::as_f64).map(|d| d.round() as u32),
                        },
                    )
                    .map_err(e)?;
            }
            Err(why) => failed.push(format!("{}: {why}", string(entry, "title"))),
        }
        // Cues: hot A–H are kinds 1–3 and 5–9, memory 0.
        for cue in entry.get("cues").and_then(Value::as_array).into_iter().flatten() {
            let slot = cue.get("slot").and_then(Value::as_i64).unwrap_or(-1);
            let pos = cue.get("pos").and_then(Value::as_f64).unwrap_or(-1.0);
            if pos < 0.0 || slot > 7 {
                continue;
            }
            let kind: u8 = match slot {
                s if s < 0 => 0,
                s if s < 3 => (s + 1) as u8,
                s => (s + 2) as u8,
            };
            let start = (pos * 1000.0).round() as u32;
            let made = match cue.get("end").and_then(Value::as_f64).filter(|end| *end > pos) {
                Some(end) => writer.add_loop(&id, kind, start, (end * 1000.0).round() as u32, 0),
                None => writer.add_cue(&id, kind, start),
            };
            if let (Ok(cue_id), Some(name)) = (made, cue.get("name").and_then(Value::as_str).filter(|n| !n.is_empty())) {
                let _ = writer.set_cue_comment(&cue_id, name);
            }
        }
        if let Some(stars) = entry.get("rating").and_then(Value::as_u64).filter(|s| *s > 0) {
            let _ = writer.set_rating(&id, stars.min(5) as u8);
        }
        ids.push(Some(id));
    }

    // The playlists, parents before children (the request lists them that way).
    let mut made: std::collections::HashMap<u64, String> = std::collections::HashMap::new();
    for p in request.get("playlists").and_then(Value::as_array).into_iter().flatten() {
        let parent = p.get("parent").and_then(Value::as_u64).and_then(|id| made.get(&id).cloned()).unwrap_or_else(|| ROOT.to_owned());
        let name = string(p, "name");
        let folder = p.get("folder").and_then(Value::as_bool).unwrap_or(false);
        let id = if folder { writer.create_folder(&name, &parent) } else { writer.create_playlist(&name, &parent) }.map_err(e)?;
        if !folder {
            let tracks: Vec<String> = p
                .get("tracks")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
                .filter_map(Value::as_u64)
                .filter_map(|i| ids.get(i as usize).cloned().flatten())
                .collect();
            if !tracks.is_empty() {
                writer.add_tracks(&id, &tracks).map_err(e)?;
            }
        }
        if let Some(own) = p.get("id").and_then(Value::as_u64) {
            made.insert(own, id);
        }
    }
    drop(writer);

    let db = Db::open(location.clone(), OpenMode::ReadOnly).map_err(|e| e.to_string())?;
    let (library, _) = rbl_index::load(&db).map_err(|e| e.to_string())?;
    let tracks = library.len();
    let source = Arc::new(StaticSource { library: Arc::new(library), share_root: location.share_root.clone() });
    let wanted = string(request, "interface");
    let available = rbl_link::interfaces();
    let interface = if request.get("loopback").and_then(Value::as_bool).unwrap_or(false) {
        Interface::loopback()
    } else {
        available
            .iter()
            .find(|i| wanted.is_empty() || i.name == wanted || i.address.to_string() == wanted)
            .cloned()
            .ok_or_else(|| "no network interface to run Link on: connect the computer to the players' network".to_owned())?
    };
    let ports = if request.get("loopback").and_then(Value::as_bool).unwrap_or(false) { Ports::EPHEMERAL } else { Ports::REKORDBOX };
    let link = LinkExport::start(source, interface.clone(), ports).map_err(|e| e.to_string())?;
    *LINK.lock().map_err(|_| "Link is unavailable")? = Some(link);
    Ok(json!({
        "ok": true,
        "tracks": tracks,
        "interface": interface.name,
        "address": interface.address.to_string(),
        "failed": failed,
    }))
}

/// Whether Link is running, and what it sees: `{ok, running, interface, address, state,
/// players: [{number, name, address, loaded, playing, master}], mounted}`.
pub fn status() -> Value {
    let Ok(guard) = LINK.lock() else { return json!({ "ok": false, "error": "Link is unavailable" }) };
    let Some(link) = guard.as_ref() else { return json!({ "ok": true, "running": false }) };
    let s = link.snapshot();
    let state = match &s.link {
        rbl_link::LinkState::Waiting => "waiting for a player".to_owned(),
        rbl_link::LinkState::Joining => "joining".to_owned(),
        rbl_link::LinkState::Up { number } => format!("on the link as {number}"),
        rbl_link::LinkState::Down(why) => format!("down: {why}"),
    };
    let players: Vec<Value> = s
        .players
        .iter()
        .map(|p| {
            json!({
                "number": p.number,
                "name": p.name,
                "address": p.address.to_string(),
                "loaded": p.loaded,
                "playing": p.playing,
                "master": p.master,
            })
        })
        .collect();
    json!({
        "ok": true,
        "running": true,
        "interface": s.interface.name,
        "address": s.interface.address.to_string(),
        "state": state,
        "players": players,
        "mounted": s.mounted.len(),
    })
}

pub fn stop() {
    if let Ok(mut guard) = LINK.lock() {
        if let Some(link) = guard.take() {
            link.stop();
        }
    }
}
