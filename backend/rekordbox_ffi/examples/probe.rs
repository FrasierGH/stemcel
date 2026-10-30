//! Opens the installed rekordbox library read-only and prints what the app would see:
//! counts, timing, and how many tracks have a readable beat grid. No track names.
//! `cargo run --release --example probe`

use std::ffi::{CStr, CString};
use std::time::Instant;

use stemcel_rekordbox::*;

fn take(ptr: *mut std::ffi::c_char) -> serde_json::Value {
    let text = unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned();
    unsafe { rbx_free_string(ptr) };
    serde_json::from_str(&text).unwrap_or_default()
}

fn main() {
    let started = Instant::now();
    let mut error = std::ptr::null_mut();
    let library = unsafe { rbx_open(std::ptr::null(), std::ptr::null(), &mut error) };
    if library.is_null() {
        let message = if error.is_null() { String::new() } else { unsafe { CStr::from_ptr(error) }.to_string_lossy().into_owned() };
        println!("open failed: {message}");
        return;
    }
    let snapshot = take(unsafe { rbx_snapshot_json(library) });
    let tracks = snapshot["tracks"].as_array().cloned().unwrap_or_default();
    let playlists = snapshot["playlists"].as_array().cloned().unwrap_or_default();
    println!("opened in {:?}: {} tracks, {} playlists/folders", started.elapsed(), tracks.len(), playlists.len());
    let with_bpm = tracks.iter().filter(|t| t["bpm"].as_f64().unwrap_or(0.0) > 0.0).count();
    let with_key = tracks.iter().filter(|t| t["key"].is_string()).count();
    let present = tracks.iter().filter(|t| std::path::Path::new(t["path"].as_str().unwrap_or("")).exists()).count();
    println!("bpm {with_bpm}, key {with_key}, audio present {present}");
    let mut grids = 0;
    let mut phrases = 0;
    let sample = tracks.iter().take(200);
    let t0 = Instant::now();
    for t in sample {
        let id = CString::new(t["id"].as_str().unwrap_or("")).unwrap_or_default();
        let detail = take(unsafe { rbx_track_json(library, id.as_ptr()) });
        if detail["beats"].as_array().is_some_and(|b| !b.is_empty()) { grids += 1; }
        if detail["phrases"].as_array().is_some_and(|b| !b.is_empty()) { phrases += 1; }
    }
    println!("first 200: {grids} with beat grid, {phrases} with phrases ({:?})", t0.elapsed());
    unsafe { rbx_close(library) };
}
