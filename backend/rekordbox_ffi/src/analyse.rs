//! Tracks added to a desktop library with an analysis already in place, so rekordbox
//! shows them analysed and does not analyse them again.
//!
//! For each track, the analysis files are written first and the row is pointed at them
//! after. A row is never left naming files that are not there. Files are only ever made
//! new: a track whose `.DAT` exists, or that rekordbox has analysed itself, is left as it
//! is.
//!
//! The beat grid, BPM and key are Stemcel's when the request carries them, so rekordbox
//! shows what Stemcel shows. The bar positions (which beat is a downbeat) and the
//! waveforms come from rbxport's analyser, which is also the whole analysis when
//! Stemcel has none.

use std::path::Path;

use rbl_anlz::Beat;
use rbl_db::write::{AnalysisWrite, Writer};
use rbl_db::LibraryLocation;
use serde_json::Value;

/// What Stemcel knows of a track: `{bpm, beats: [seconds], anchorSec, key}`, any of
/// them absent.
#[derive(Debug, Default, Clone)]
pub struct Known {
    pub bpm: f64,
    pub beats_secs: Vec<f64>,
    pub anchor_secs: Option<f64>,
    pub key: String,
}

impl Known {
    pub fn from_json(value: &Value) -> Self {
        Self {
            bpm: value.get("bpm").and_then(Value::as_f64).filter(|b| b.is_finite() && *b > 0.0).unwrap_or(0.0),
            beats_secs: value
                .get("beats")
                .and_then(Value::as_array)
                .map(|beats| beats.iter().filter_map(Value::as_f64).filter(|t| t.is_finite() && *t >= 0.0).collect())
                .unwrap_or_default(),
            anchor_secs: value.get("anchorSec").and_then(Value::as_f64).filter(|t| t.is_finite()),
            key: value.get("key").and_then(Value::as_str).unwrap_or_default().to_owned(),
        }
    }
}

pub enum Outcome {
    Analysed,
    /// rekordbox's own analysis, or files already there: left alone.
    Kept,
}

/// Analyses `content` (the track of `audio`) and registers it.
pub fn analyse_track(
    writer: &mut Writer, location: &LibraryLocation, content: &str, audio: &Path, known: &Known,
) -> Result<Outcome, String> {
    let (analysed, existing_path): (Option<i64>, Option<String>) = writer
        .library()
        .connection()
        .query_row("SELECT Analysed, AnalysisDataPath FROM djmdContent WHERE ID = ?1", [content], |r| {
            Ok((r.get(0)?, r.get(1)?))
        })
        .map_err(|e| e.to_string())?;
    let has_files = existing_path
        .filter(|p| !p.is_empty())
        .is_some_and(|p| rbl_anlz::resolve(&location.share_root, &p).exists());
    if has_files || !matches!(analysed.unwrap_or(0) & 127, 0 | 1) {
        return Ok(Outcome::Kept);
    }
    let relative = writer.analysis_data_path_for(content).map_err(|e| e.to_string())?;
    let dat = rbl_anlz::resolve(&location.share_root, &relative);
    if dat.exists() {
        return Ok(Outcome::Kept);
    }

    let decoded = rbl_audio::decode_mono(audio, None).map_err(|e| format!("couldn't decode {}: {e}", audio.display()))?;
    if decoded.samples.is_empty() || decoded.sample_rate == 0 {
        return Err(format!("{} has no audio", audio.display()));
    }
    let length_secs = decoded.samples.len() as f64 / f64::from(decoded.sample_rate);
    let analysis = rbl_analysis::analyse_with(
        &decoded.samples,
        decoded.sample_rate,
        rbl_analysis::AnalysisPreset::Rekordbox.options(),
    );
    let theirs: Vec<Beat> = analysis
        .tempo
        .beats
        .iter()
        .map(|b| Beat { beat_number: b.beat_number, tempo_x100: b.tempo_x100, time_ms: b.time_ms })
        .collect();
    let grid = stemcel_grid(known, length_secs, &theirs, None).unwrap_or(theirs);
    let bpm_x100 = if known.bpm > 0.0 {
        (known.bpm * 100.0).round() as u32
    } else {
        (analysis.tempo.bpm * 100.0).round() as u32
    };
    let key = key_name(writer, &known.key).or_else(|| analysis.key.as_ref().map(|k| k.name.clone()));

    let columns: Vec<rbl_anlz::BandColumn> = analysis
        .waveform
        .columns
        .iter()
        .map(|c| rbl_anlz::BandColumn { low: c.low, mid: c.mid, high: c.high, peak: c.peak })
        .collect();
    let overview: Option<&[[u8; 3]; 1200]> = analysis.waveform.overview.as_slice().try_into().ok();
    let files = rbl_anlz::author_with_overview(
        &rbl_anlz::share_tree_ppth(&audio.to_string_lossy()),
        &grid,
        &columns,
        overview,
        rbl_anlz::Existing::default(),
    );
    write_new_files(&dat, &[("DAT", &files.dat), ("EXT", &files.ext), ("2EX", &files.two_ex)])?;

    let registered = writer.set_analysis(
        content,
        &AnalysisWrite {
            bpm_x100,
            key: key.as_deref(),
            analysis_path: &relative,
            length_sec: Some(length_secs.round() as u32),
        },
    );
    if let Err(e) = registered {
        // Nothing names the files: take them away again.
        for extension in ["DAT", "EXT", "2EX"] {
            let _ = std::fs::remove_file(dat.with_extension(extension));
        }
        return Err(e.to_string());
    }
    Ok(Outcome::Analysed)
}

/// Each file through a temporary name, so none is ever half written. On a failure the
/// ones already written are removed.
fn write_new_files(dat: &Path, files: &[(&str, &Vec<u8>)]) -> Result<(), String> {
    let folder = dat.parent().ok_or("no analysis folder")?;
    std::fs::create_dir_all(folder).map_err(|e| format!("couldn't make {}: {e}", folder.display()))?;
    let mut written = Vec::new();
    for (extension, bytes) in files {
        let path = dat.with_extension(extension);
        let staged = dat.with_extension(format!("{extension}.tmp"));
        let result = std::fs::write(&staged, bytes).and_then(|()| std::fs::rename(&staged, &path));
        if let Err(e) = result {
            let _ = std::fs::remove_file(&staged);
            for done in &written {
                let _ = std::fs::remove_file(done);
            }
            return Err(format!("couldn't write {}: {e}", path.display()));
        }
        written.push(path);
    }
    Ok(())
}

/// Stemcel's beats, numbered in bars: from `first` (the bar position of the first beat)
/// when known, else as the analyser places the downbeats. `None` when Stemcel has no grid.
pub fn stemcel_grid(known: &Known, length_secs: f64, theirs: &[Beat], first: Option<u16>) -> Option<Vec<Beat>> {
    let mut beats = known.beats_secs.clone();
    if beats.len() < 2 {
        // A steady grid: the anchor and the tempo, over the whole track.
        let anchor = known.anchor_secs?;
        if known.bpm <= 0.0 {
            return None;
        }
        let period = 60.0 / known.bpm;
        let first = anchor - (anchor / period).floor() * period;
        beats = (0..)
            .map(|i| first + f64::from(i) * period)
            .take_while(|t| *t < length_secs)
            .collect();
    }
    beats.retain(|t| *t >= 0.0 && *t < length_secs.max(0.0) + 1.0);
    beats.sort_by(f64::total_cmp);
    if beats.len() < 2 {
        return None;
    }
    // The downbeat: the bar position of the analyser's beat nearest Stemcel's first.
    let first_ms = beats[0] * 1000.0;
    let interval_ms = (beats[1] - beats[0]) * 1000.0;
    let start = first.filter(|f| (1..=4).contains(f)).unwrap_or_else(|| theirs
        .iter()
        .min_by(|a, b| (f64::from(a.time_ms) - first_ms).abs().total_cmp(&(f64::from(b.time_ms) - first_ms).abs()))
        .filter(|b| (f64::from(b.time_ms) - first_ms).abs() < interval_ms / 2.0)
        .map_or(1, |b| b.beat_number.clamp(1, 4)));
    // A steady tempo (every beat within 1% of the BPM) is stored as that BPM, as a
    // rekordbox grid holds it; otherwise each beat carries its own.
    let steady = known.bpm > 0.0
        && beats.windows(2).all(|w| ((w[1] - w[0]) * known.bpm / 60.0 - 1.0).abs() < 0.01);
    let mut grid = Vec::with_capacity(beats.len());
    for (i, t) in beats.iter().enumerate() {
        let bpm = if steady {
            known.bpm
        } else {
            let next = beats.get(i + 1).copied().unwrap_or(*t);
            let previous = if i > 0 { beats[i - 1] } else { *t };
            let gap = if next > *t { next - *t } else { *t - previous };
            if gap > 0.0 { 60.0 / gap } else { known.bpm.max(120.0) }
        };
        grid.push(Beat {
            beat_number: ((usize::from(start) - 1 + i) % 4 + 1) as u16,
            tempo_x100: (bpm * 100.0).round().clamp(1.0, f64::from(u16::MAX)) as u16,
            time_ms: (t * 1000.0).round() as u32,
        });
    }
    Some(grid)
}

/// Stemcel's key ("A minor", "Eb major") as the library's own `djmdKey` spells it
/// ("Am", "Ebm" or "D#m"): whichever spelling the library has, else the plain one.
fn key_name(writer: &Writer, stemcel: &str) -> Option<String> {
    let names = key_spellings(stemcel);
    let conn = writer.library().connection();
    for name in &names {
        let found: Option<i64> = conn
            .query_row(
                "SELECT 1 FROM djmdKey WHERE ScaleName = ?1 AND rb_local_deleted = 0 LIMIT 1",
                [name],
                |r| r.get(0),
            )
            .ok();
        if found.is_some() {
            return Some(name.clone());
        }
    }
    names.into_iter().next()
}

/// "A minor" -> ["Am"]; "C# minor" -> ["C#m", "Dbm"]. Empty when it isn't a key.
pub fn key_spellings(stemcel: &str) -> Vec<String> {
    let text = stemcel.trim();
    let mut chars = text.chars();
    let Some(letter) = chars.next().map(|c| c.to_ascii_uppercase()).filter(|c| ('A'..='G').contains(c)) else {
        return Vec::new();
    };
    let rest: String = chars.collect();
    let (accidental, rest) = match rest.chars().next() {
        Some(c @ ('#' | '♯')) => ("#", &rest[c.len_utf8()..]),
        Some(c @ ('b' | '♭')) => ("b", &rest[c.len_utf8()..]),
        _ => ("", rest.as_str()),
    };
    let rest = rest.trim().to_ascii_lowercase();
    let minor = rest.starts_with("min") || rest == "m";
    let suffix = if minor { "m" } else { "" };
    let tonic = format!("{letter}{accidental}");
    let mut names = vec![format!("{tonic}{suffix}")];
    const SHARPS: [&str; 5] = ["C#", "D#", "F#", "G#", "A#"];
    const FLATS: [&str; 5] = ["Db", "Eb", "Gb", "Ab", "Bb"];
    if let Some(i) = SHARPS.iter().position(|s| *s == tonic) {
        names.push(format!("{}{suffix}", FLATS[i]));
    } else if let Some(i) = FLATS.iter().position(|s| *s == tonic) {
        names.push(format!("{}{suffix}", SHARPS[i]));
    }
    names
}
