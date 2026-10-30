//! Against rbxport's throwaway fixture library — never the installed one.

use super::*;
use rbl_anlz::Beat;
use rbl_db::fixture;

fn c(text: &str) -> CString {
    CString::new(text).unwrap()
}

/// Takes ownership of a returned string and parses it.
fn take_json(ptr: *mut c_char) -> Value {
    assert!(!ptr.is_null());
    let text = unsafe { CStr::from_ptr(ptr) }.to_str().unwrap().to_owned();
    unsafe { rbx_free_string(ptr) };
    serde_json::from_str(&text).unwrap()
}

fn fixture_library(dir: &Path) -> LibraryLocation {
    let location = fixture::build(dir, fixture::Shape::default()).unwrap();
    fixture::set_tempo(&location, 0, 12_800).unwrap();
    fixture::point_at_audio(&location, 0, "/music/Track Zero.mp3", 245).unwrap();

    // A beat grid at 128 BPM whose first downbeat is at 120 ms.
    let beats: Vec<Beat> = (0..64u32)
        .map(|i| Beat {
            beat_number: u16::try_from(i % 4 + 1).unwrap(),
            tempo_x100: 12_800,
            time_ms: 120 + i * 60_000 / 128,
        })
        .collect();
    let relative = "/PIONEER/USBANLZ/abc/0001/ANLZ0000.DAT";
    let dat = rbl_anlz::resolve(&location.share_root, relative);
    std::fs::create_dir_all(dat.parent().unwrap()).unwrap();
    std::fs::write(&dat, rbl_anlz::write::AnlzBuilder::new().beat_grid(&beats).finish()).unwrap();
    fixture::set_analysis_path(&location, 0, relative).unwrap();
    location
}

fn open_fixture(location: &LibraryLocation) -> *mut RbxLibrary {
    let db = c(location.master_db.to_str().unwrap());
    let pass = c(fixture::FIXTURE_PASSPHRASE);
    let mut error: *mut c_char = std::ptr::null_mut();
    let library = unsafe { rbx_open(db.as_ptr(), pass.as_ptr(), &mut error) };
    if library.is_null() {
        panic!("open failed: {}", take_json_text(error));
    }
    library
}

fn take_json_text(ptr: *mut c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let text = unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned();
    unsafe { rbx_free_string(ptr) };
    text
}

#[test]
fn snapshot_lists_tracks_and_playlists() {
    let dir = tempfile::tempdir().unwrap();
    let location = fixture_library(dir.path());
    let library = open_fixture(&location);

    let snapshot = take_json(unsafe { rbx_snapshot_json(library) });
    assert_eq!(snapshot["ok"], true);
    let tracks = snapshot["tracks"].as_array().unwrap();
    assert_eq!(tracks.len(), 40);
    let first = tracks.iter().find(|t| t["id"] == fixture::track_id(0)).unwrap();
    assert_eq!(first["bpm"], 128.0);
    assert_eq!(first["path"], "/music/Track Zero.mp3");
    assert_eq!(first["durationMs"], 245_000);

    let playlists = snapshot["playlists"].as_array().unwrap();
    let lists: Vec<_> = playlists.iter().filter(|p| p["kind"] == "playlist").collect();
    assert!(lists.len() >= 3);
    assert!(lists.iter().any(|p| p["trackIds"].as_array().unwrap().len() == 5));

    unsafe { rbx_close(library) };
}

#[test]
fn track_detail_reads_the_beat_grid() {
    let dir = tempfile::tempdir().unwrap();
    let location = fixture_library(dir.path());
    let library = open_fixture(&location);

    let id = c(&fixture::track_id(0));
    let detail = take_json(unsafe { rbx_track_json(library, id.as_ptr()) });
    assert_eq!(detail["ok"], true);
    let beats = detail["beats"].as_array().unwrap();
    assert_eq!(beats.len(), 64);
    assert_eq!(beats[0][0], 120);
    assert_eq!(beats[0][1], 1);
    assert_eq!(beats[0][2], 128.0);

    let missing = c("999999999");
    let none = take_json(unsafe { rbx_track_json(library, missing.as_ptr()) });
    assert_eq!(none["ok"], false);

    unsafe { rbx_close(library) };
}

#[test]
fn bad_open_reports_an_error() {
    let dir = tempfile::tempdir().unwrap();
    let db = c(dir.path().join("missing.db").to_str().unwrap());
    let pass = c("nope");
    let mut error: *mut c_char = std::ptr::null_mut();
    let library = unsafe { rbx_open(db.as_ptr(), pass.as_ptr(), &mut error) };
    assert!(library.is_null());
    assert!(!take_json_text(error).is_empty());
}

#[test]
fn tags_of_a_wav() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("tone.wav");
    // 0.5 s of 16-bit stereo silence at 44.1 kHz.
    let samples = 22_050u32;
    let data_len = samples * 4;
    let mut wav = Vec::new();
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&(36 + data_len).to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt ");
    wav.extend_from_slice(&16u32.to_le_bytes());
    wav.extend_from_slice(&1u16.to_le_bytes());
    wav.extend_from_slice(&2u16.to_le_bytes());
    wav.extend_from_slice(&44_100u32.to_le_bytes());
    wav.extend_from_slice(&(44_100u32 * 4).to_le_bytes());
    wav.extend_from_slice(&4u16.to_le_bytes());
    wav.extend_from_slice(&16u16.to_le_bytes());
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&data_len.to_le_bytes());
    wav.resize(wav.len() + data_len as usize, 0);
    std::fs::write(&path, wav).unwrap();

    let p = c(path.to_str().unwrap());
    let tags = take_json(unsafe { rbx_read_tags_json(p.as_ptr()) });
    assert_eq!(tags["ok"], true, "{tags}");
    assert_eq!(tags["sampleRate"], 44_100);
    assert_eq!(tags["channels"], 2);
    assert_eq!(tags["durationMs"], 500);
}
