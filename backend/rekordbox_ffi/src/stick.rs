//! USB sticks without rekordbox: the drives to export to, Stemcel's collection written to
//! one as a rekordbox export (both databases, the audio, the analysis), the play history a
//! player kept on it, and ejecting it.
//!
//! The export itself is rbxport's (`rbl-export`). What Stemcel adds is its own tracks: the
//! analysis files are made from Stemcel's beat grid (numbered in bars, from the grid's own
//! bar positions or the analyser's downbeats) and rbxport's waveforms, and kept in a cache
//! so the next sync doesn't make them again; the cues go on as the stick's hot and memory
//! cues.

use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};

use rbl_anlz::cues::ExportCue;
use rbl_anlz::Beat;
use rbl_export::{ExportOptions, ExportProgress, SourceMyTag, SourcePlaylist, SourceTrack};
use serde_json::{json, Value};

use crate::analyse::{self, Known};

/// Every mounted drive, with whether it holds a rekordbox export, for choosing where to
/// export.
pub fn volumes() -> Value {
    let list: Vec<Value> = rbl_devices::list()
        .into_iter()
        .map(|d| {
            let export = rbl_devices::inspect(&d.mount_point);
            json!({
                "name": d.name,
                "root": d.mount_point.to_string_lossy(),
                "removable": d.removable,
                "fileSystem": d.file_system,
                "totalBytes": d.total_bytes,
                "freeBytes": d.free_bytes,
                "hasExport": export.is_some(),
                "exportTracks": export.as_ref().map_or(0, |e| e.tracks),
                "exportPlaylists": export.as_ref().map_or(0, |e| e.playlists),
            })
        })
        .collect();
    json!({ "ok": true, "volumes": list })
}

pub fn eject(root: &str) -> Value {
    match rbl_devices::eject::eject(Path::new(root)) {
        Ok(()) => json!({ "ok": true }),
        Err(e) => json!({ "ok": false, "error": e.to_string() }),
    }
}

fn num(v: &Value, key: &str) -> f64 {
    v.get(key).and_then(Value::as_f64).unwrap_or(0.0)
}

fn string(v: &Value, key: &str) -> String {
    v.get(key).and_then(Value::as_str).unwrap_or_default().to_owned()
}

/// A cue as the stick's databases hold it: memory 0, hot cues A–C 1–3, D–H 5–9.
fn export_cue(cue: &Value) -> Option<ExportCue> {
    let slot = cue.get("slot").and_then(Value::as_i64).unwrap_or(-1);
    let pos = num(cue, "pos");
    if !(pos >= 0.0) || slot > 7 {
        return None;
    }
    let kind = match slot {
        s if s < 0 => 0,
        s if s < 3 => (s + 1) as u8,
        s => (s + 2) as u8,
    };
    let end = cue.get("end").and_then(Value::as_f64).filter(|e| *e > pos);
    Some(ExportCue {
        kind,
        time_ms: (pos * 1000.0).round() as u32,
        loop_time_ms: end.map(|e| (e * 1000.0).round() as u32),
        color_id: cue.get("colour").and_then(Value::as_u64).unwrap_or(0).min(255) as u8,
        color_code: 0,
        comment: string(cue, "name"),
        active_loop: false,
        loop_numerator: 0,
        loop_denominator: 0,
    })
}

/// The analysis files of one track: from the cache when the file and grid are as they
/// were, else made (and cached).
pub(crate) fn analysis_files(entry: &Value, path: &Path, cache: &Path) -> Result<Vec<(String, Vec<u8>)>, String> {
    let beats: Vec<f64> = entry
        .get("beats")
        .and_then(Value::as_array)
        .map(|b| b.iter().filter_map(Value::as_f64).collect())
        .unwrap_or_default();
    let first = entry.get("firstBeat").and_then(Value::as_u64).filter(|f| (1..=4).contains(f)).map(|f| f as u16);
    let bpm = num(entry, "bpm");
    let meta = std::fs::metadata(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut hasher = DefaultHasher::new();
    path.hash(&mut hasher);
    meta.len().hash(&mut hasher);
    meta.modified().ok().hash(&mut hasher);
    bpm.to_bits().hash(&mut hasher);
    first.hash(&mut hasher);
    for b in &beats {
        ((b * 1000.0).round() as i64).hash(&mut hasher);
    }
    let stem = cache.join(format!("{:016x}", hasher.finish()));
    let extensions = ["DAT", "EXT", "2EX"];
    if extensions.iter().all(|x| stem.with_extension(x).is_file()) {
        let mut files = Vec::new();
        for x in extensions {
            files.push((x.to_owned(), std::fs::read(stem.with_extension(x)).map_err(|e| e.to_string())?));
        }
        return Ok(files);
    }

    let audio = rbl_audio::decode_mono(path, None).map_err(|e| format!("couldn't decode {}: {e}", path.display()))?;
    if audio.samples.is_empty() || audio.sample_rate == 0 {
        return Err(format!("{} has no audio", path.display()));
    }
    let length = audio.samples.len() as f64 / f64::from(audio.sample_rate);
    // The bars are known from the grid, or found by the analyser (which also makes the
    // waveforms, and the whole grid when Stemcel has none).
    let (theirs, waveform) = if first.is_some() && beats.len() >= 2 {
        (Vec::new(), rbl_analysis::waveform::compute(&audio.samples, audio.sample_rate))
    } else {
        let analysis = rbl_analysis::analyse_with(&audio.samples, audio.sample_rate, rbl_analysis::AnalysisPreset::Rekordbox.options());
        let theirs: Vec<Beat> = analysis
            .tempo
            .beats
            .iter()
            .map(|b| Beat { beat_number: b.beat_number, tempo_x100: b.tempo_x100, time_ms: b.time_ms })
            .collect();
        (theirs, analysis.waveform)
    };
    let known = Known { bpm, beats_secs: beats, anchor_secs: None, key: String::new() };
    let grid = analyse::stemcel_grid(&known, length, &theirs, first).unwrap_or(theirs);
    let columns: Vec<rbl_anlz::BandColumn> = waveform
        .columns
        .iter()
        .map(|c| rbl_anlz::BandColumn { low: c.low, mid: c.mid, high: c.high, peak: c.peak })
        .collect();
    let overview: Option<&[[u8; 3]; 1200]> = waveform.overview.as_slice().try_into().ok();
    let made = rbl_anlz::author_with_overview(
        &rbl_anlz::share_tree_ppth(&path.to_string_lossy()),
        &grid,
        &columns,
        overview,
        rbl_anlz::Existing::default(),
    );
    let files = vec![("DAT".to_owned(), made.dat), ("EXT".to_owned(), made.ext), ("2EX".to_owned(), made.two_ex)];
    if std::fs::create_dir_all(cache).is_ok() {
        for (x, bytes) in &files {
            let staged = stem.with_extension(format!("{x}.tmp"));
            if std::fs::write(&staged, bytes).is_ok() {
                let _ = std::fs::rename(&staged, stem.with_extension(x));
            }
        }
    }
    Ok(files)
}

fn source_track(entry: &Value, cache: &Path, tag_ids: &std::collections::BTreeMap<String, u64>) -> Result<SourceTrack, String> {
    let path = PathBuf::from(string(entry, "path"));
    let size = std::fs::metadata(&path).map_err(|e| format!("{}: {e}", path.display()))?.len();
    let analysis = analysis_files(entry, &path, cache)?;
    let cues: Vec<ExportCue> = entry
        .get("cues")
        .and_then(Value::as_array)
        .map(|c| c.iter().filter_map(export_cue).collect())
        .unwrap_or_default();
    let my_tags = entry
        .get("tags")
        .and_then(Value::as_array)
        .map(|t| t.iter().filter_map(|t| t.as_str().and_then(|t| tag_ids.get(t).copied())).collect())
        .unwrap_or_default();
    let key = analyse::key_spellings(&string(entry, "key")).into_iter().next().unwrap_or_default();
    let artwork = entry.get("artwork").and_then(Value::as_str).filter(|a| !a.is_empty()).map(PathBuf::from);
    Ok(SourceTrack {
        id: entry.get("id").and_then(Value::as_u64).unwrap_or(0),
        source_path: path,
        title: string(entry, "title"),
        artist: string(entry, "artist"),
        album: string(entry, "album"),
        genre: string(entry, "genre"),
        label: string(entry, "label"),
        key,
        comment: string(entry, "comment"),
        date_added: string(entry, "dateAdded"),
        bpm_x100: (num(entry, "bpm") * 100.0).round().max(0.0) as u32,
        duration_sec: num(entry, "durationSec").round().clamp(0.0, f64::from(u16::MAX)) as u16,
        rating: num(entry, "rating").clamp(0.0, 5.0) as u8,
        color_id: num(entry, "colour").clamp(0.0, 8.0) as u8,
        year: num(entry, "year").clamp(0.0, f64::from(u16::MAX)) as u16,
        bitrate: num(entry, "bitrate").max(0.0) as u32,
        sample_rate: num(entry, "sampleRate").max(0.0) as u32,
        file_size: size,
        analysis,
        cues: Some(cues),
        artwork,
        my_tags,
        ..SourceTrack::default()
    })
}

/// Writes the collection's selection to a stick (`request`: root, sync, cacheDir, tracks,
/// playlists — Collection::exportRequest). `sync` makes the stick hold exactly the
/// selection, as rekordbox's SYNC does; without it, the selection is added to what is
/// there.
pub fn export(request: &Value, progress: &mut dyn FnMut(Value), cancelled: &dyn Fn() -> bool) -> Result<Value, String> {
    let root = PathBuf::from(string(request, "root"));
    if !root.is_dir() {
        return Err(format!("{} is not a drive Stemcel can write to", root.display()));
    }
    let cache = PathBuf::from(string(request, "cacheDir"));
    let entries = request.get("tracks").and_then(Value::as_array).cloned().unwrap_or_default();

    // My Tags: one category, "Stemcel", with every tag the tracks carry.
    const CATEGORY: u64 = 1;
    let mut tag_ids = std::collections::BTreeMap::new();
    for entry in &entries {
        for tag in entry.get("tags").and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_str) {
            let next = tag_ids.len() as u64 + 2;
            tag_ids.entry(tag.to_owned()).or_insert(next);
        }
    }
    let mut my_tags = Vec::new();
    if !tag_ids.is_empty() {
        my_tags.push(SourceMyTag { id: CATEGORY, seq: 1, name: "Stemcel".to_owned(), attribute: 1, parent: 0 });
        for (seq, (name, id)) in tag_ids.iter().enumerate() {
            my_tags.push(SourceMyTag { id: *id, seq: seq as u32 + 1, name: name.clone(), attribute: 0, parent: CATEGORY });
        }
    }

    // The tracks, with their analysis (the slow part on a first export).
    let total = entries.len();
    let mut tracks = Vec::with_capacity(total);
    let mut index_map = vec![usize::MAX; total];
    let mut failed = Vec::new();
    for (i, entry) in entries.iter().enumerate() {
        if cancelled() {
            return Err("Export stopped.".to_owned());
        }
        progress(json!({ "stage": "analysing", "done": i, "total": total, "title": string(entry, "title") }));
        match source_track(entry, &cache, &tag_ids) {
            Ok(track) => {
                index_map[i] = tracks.len();
                tracks.push(track);
            }
            Err(why) => failed.push(format!("{}: {why}", string(entry, "title"))),
        }
    }
    let playlists: Vec<SourcePlaylist> = request
        .get("playlists")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .map(|p| SourcePlaylist {
            id: p.get("id").and_then(Value::as_u64).unwrap_or(0),
            name: string(p, "name"),
            parent_id: p.get("parent").and_then(Value::as_u64).unwrap_or(0),
            folder: p.get("folder").and_then(Value::as_bool).unwrap_or(false),
            track_indices: p
                .get("tracks")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
                .filter_map(Value::as_u64)
                .filter_map(|i| index_map.get(i as usize).copied().filter(|m| *m != usize::MAX))
                .collect(),
            ..SourcePlaylist::default()
        })
        .collect();

    let sync = request.get("sync").and_then(Value::as_bool).unwrap_or(false);
    let options = ExportOptions { keep_unlisted: !sync, ..ExportOptions::default() };
    let mut report_progress = |p: &ExportProgress| {
        progress(json!({
            "stage": p.stage,
            "done": p.done,
            "total": p.total,
            "title": p.title,
            "bytesDone": p.bytes_done,
            "bytesTotal": p.bytes_total,
        }));
    };
    let report = rbl_export::export_cancellable(&root, &tracks, &playlists, &my_tags, &options, &mut report_progress, cancelled)
        .map_err(|e| e.to_string())?;
    failed.extend(report.failed.iter().cloned());
    let warnings: Vec<String> = report.warnings.iter().map(|w| format!("{}: {}", w.title, w.reason)).collect();
    Ok(json!({
        "ok": true,
        "verified": report.verified,
        "upToDate": report.up_to_date,
        "tracks": report.tracks,
        "playlists": report.playlists,
        "added": report.tracks_added,
        "updated": report.tracks_updated,
        "reused": report.reused,
        "removed": report.removed,
        "bytesCopied": report.bytes_copied,
        "elapsedMs": report.elapsed_ms,
        "failed": failed,
        "skipped": report.skipped,
        "warnings": warnings,
    }))
}

/// The play histories a player kept on the stick at `root`: each with its tracks, named by
/// the collection id they were exported from where the stick records it, and by title,
/// artist and file otherwise.
pub fn histories(root: &str) -> Result<Value, String> {
    let snapshot = rbl_export::snapshot::Snapshot::read(Path::new(root)).map_err(|e| e.to_string())?;
    let library = snapshot.legacy.as_ref().or(snapshot.one.as_ref());
    let track = |id: u32| -> Value {
        let row = library.and_then(|l| l.tracks.iter().find(|t| t.id == id));
        let source = snapshot.identity.get(&id).map(|(_, content)| *content).filter(|c| *c != 0);
        json!({
            "deviceId": id,
            "collectionId": source,
            "title": row.map(|r| r.title.clone()).unwrap_or_default(),
            "artist": row.map(|r| r.artist.clone()).unwrap_or_default(),
            "path": row.map(|r| r.path.clone()).unwrap_or_default(),
        })
    };
    // OneLibrary's histories where the stick has them, else export.pdb's.
    let lists = if snapshot.history.is_empty() { &snapshot.legacy_history } else { &snapshot.history };
    let out: Vec<Value> = lists
        .iter()
        .filter(|h| !h.folder && !h.tracks.is_empty())
        .map(|h| json!({ "name": h.name, "tracks": h.tracks.iter().map(|id| track(*id)).collect::<Vec<_>>() }))
        .collect();
    Ok(json!({ "ok": true, "histories": out }))
}
