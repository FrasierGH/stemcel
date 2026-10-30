# Third-Party Licences

Stemcel is released under the **GNU Affero General Public License v3.0 or
later** (see [`LICENSE`](LICENSE)). It bundles or links the following
third-party components, each retained under its own licence. The notices below
satisfy the attribution requirements of those licences.

## Backend (C++)

### JUCE 8 — © Raw Material Software Limited

- **Licence**: GNU Affero General Public License v3 (the free / open-source
  option of JUCE's dual licence; the alternative is a commercial JUCE licence).
- **Project**: <https://juce.com>
- **Source**: <https://github.com/juce-framework/JUCE>
- **Notice**: Stemcel's audio engine is powered by JUCE, used under the
  terms of the AGPLv3. Stemcel uses the headless audio modules
  (`juce_audio_basics`, `juce_audio_devices`, `juce_audio_formats`,
  `juce_audio_processors`, `juce_audio_utils`, `juce_core`,
  `juce_data_structures`, `juce_dsp`, `juce_events`) plus `juce_gui_basics`,
  which exists solely to host the native editor window of a VST3 plugin; the
  Stemcel interface itself is drawn by the Electron frontend. A copy of the
  licence is included with the JUCE source obtained via CMake `FetchContent`
  (`backend/build/_deps/juce-src/LICENSE.md`).

### VST3 SDK — © 2025 Steinberg Media Technologies GmbH

- **Licence**: MIT.
- **Project**: <https://www.steinberg.net/developers/>
- **Notice**: Stemcel hosts third-party VST3 effect plugins (ADR 0025). The
  SDK is not vendored here — it ships inside the JUCE source fetched by CMake
  (`backend/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/VST3_SDK/`)
  and is compiled in only because `JUCE_PLUGINHOST_VST3=1`. VST is a trademark
  of Steinberg Media Technologies GmbH, registered in Europe and other
  countries. The MIT licence requires its copyright and permission notice to be
  reproduced, so it is included in full:

```text
MIT License

Copyright (c) 2025, Steinberg Media Technologies GmbH

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```


### FST (Free Studio Technology) — © IOhannes m zmölnig and contributors

- Source: https://git.iem.at/zmoelnig/FST (pinned commit, fetched at build time)
- Licence: GNU General Public License v3.0 or later
- Use: reverse-engineered VST 2 interface headers, so JUCE can host VST 2 plugins without
  Steinberg's withdrawn SDK (`STEMCEL_HOST_VST2`). A build-time copy of `fst.h` fixes three
  open-ended arrays to the lengths JUCE expects. "VST" is a trademark of Steinberg Media
  Technologies GmbH.

### IXWebSocket — © Machine Zone Inc. and contributors

- **Licence**: BSD 3-Clause.
- **Project**: <https://github.com/machinezone/IXWebSocket>
- **Notice**: Source obtained via CMake `FetchContent`; the full BSD-3-Clause
  text ships alongside the source at
  `backend/build/_deps/ixwebsocket-src/LICENSE.txt`.

### Rubber Band Library — © Chris Cannam and contributors

- **Licence**: GNU General Public License v2 or later (GPL-2.0+).
- **Project**: <https://breakfastquay.com/rubberband/>
- **Source**: <https://github.com/breakfastquay/rubberband>
- **Notice**: Used by the backend for non-destructive time-stretching and
  pitch shifting. Source is obtained via CMake `FetchContent`; the upstream
  licence text is included with the fetched source at
  `backend/build/_deps/rubberband-src/COPYING`.

### libsamplerate — © Erik de Castro Lopo and contributors

- **Licence**: BSD 2-Clause.
- **Project**: <https://libsndfile.github.io/libsamplerate/>
- **Source**: <https://github.com/libsndfile/libsamplerate>
- **Notice**: Used for offline analysis resampling and Rubber Band support.
  Source is obtained via CMake `FetchContent`.

### BTrack / KISS FFT

- **Licence**: GPL-3.0 for BTrack; BSD-style licence for KISS FFT.
- **Project**: <https://github.com/adamstark/BTrack>
- **Notice**: A patched vendored copy lives under `backend/third_party/btrack/`
  and powers BPM / beat detection. See
  [`backend/third_party/btrack/PATCHES.md`](backend/third_party/btrack/PATCHES.md)
  for the local MSVC-compatibility changes.

### MiniBPM — © 2012-2025 Particular Programs Ltd.

- **Licence**: GPL-2.0-or-later. A commercial licence is also offered by the
  copyright holders; Stemcel relies on the GPL grant only.
- **Project**: <https://breakfastquay.com/minibpm/>
- **Source**: <https://github.com/breakfastquay/minibpm>
- **Notice**: An unmodified vendored copy of `src/MiniBpm.h` and
  `src/MiniBpm.cpp` lives under `backend/third_party/minibpm/` and provides a
  second, independent fixed-tempo estimate used to corroborate BTrack's. The
  upstream licence text is included verbatim as
  `backend/third_party/minibpm/LICENSE.txt`. The **or later** clause is what
  makes this compatible with the project's AGPL-3.0-or-later licence: the code
  is taken under GPL-3.0, which AGPL-3.0 §13 expressly permits combining with.
  A GPL-2.0-**only** dependency would not have been usable here.

### Signalsmith Stretch and Signalsmith Linear — © Geraint Luff / Signalsmith Audio

- Use: time-stretch and pitch engine of the "pro" warp mode (the default).
- License: MIT. Fetched at configure time (`signalsmith-stretch` 1.4.0, `linear` 0.6.6);
  their `LICENSE.txt` files are in `_deps/signalsmith-*-src/`.

### RNNoise — © 2017 Mozilla; © 2018 Gregor Richards

- **Licence**: BSD 2-Clause.
- **Project**: <https://jmvalin.ca/demo/rnnoise/>
- **Source**: <https://github.com/xiph/rnnoise>
- **Notice**: Used by the backend for optional post-separation **vocal-stem
  denoise** (the RNN noise-suppression network; see `backend/src/dsp/`). The
  pinned `v0.1.1` release is obtained via CMake `FetchContent`, and its bundled
  trained model (`src/rnn_data.c`) ships under the same BSD-2-Clause licence. To
  build under MSVC, five C99 variable-length-array declarations in `pitch.c` /
  `celt_lpc.c` are rewritten to `_alloca` at configure time (a mechanical change,
  no behaviour difference); all other sources are used verbatim. The upstream
  licence text is included with the fetched source at
  `backend/build/_deps/rnnoise-src/COPYING`.

### LAME MP3 encoder and decoder — © The LAME Project

- **Licence**: GNU Lesser General Public License v2.1 or later (LGPL-2.1+).
- **Project**: <https://lame.sourceforge.io/>
- **Source**: <https://sourceforge.net/projects/lame/files/lame/>
- **Notice**: Stemcel bundles an unmodified Windows `lame.exe` build
  (typically sourced from the
  [RareWares LAME bundle](https://www.rarewares.org/mp3-lame-bundle.php))
  next to `StemcelBackend.exe` in `resources/backend/`. Stemcel invokes
  it as a separate child process for both MP3 encoding on export (via JUCE's
  `LAMEEncoderAudioFormat`) and MP3 decoding on import; there is no static or
  dynamic linking against the LAME library, so the LGPL boundary is the
  process boundary. Per the LGPL, the unmodified upstream source is available
  from the project links above.

## rekordbox reader (Rust, `backend/rekordbox_ffi`)

### rbxport — © Chris Le and contributors

- **Licence**: GPL-2.0-or-later.
- **Source**: <https://github.com/chrisle/rbxport>, pinned by git revision in
  `backend/rekordbox_ffi/Cargo.toml`.
- **Notice**: The crates `rbl-db`, `rbl-index`, `rbl-anlz`, `rbl-export` and
  `rbl-devices` are built, unmodified, into the `stemcel_rekordbox` library
  the app loads at runtime: they read a rekordbox library (SQLCipher
  `master.db`, ANLZ analysis files, a USB export's `export.pdb` and
  `exportLibrary.db`) and apply its playlist edits. As with MiniBPM, the
  **or later** clause lets the code be taken under GPL-3.0, which AGPL-3.0 §13
  permits combining with.

### LAME (through the `mp3lame-encoder` crate) — © The LAME Project

- **Licence**: LGPL-2.1-or-later (LAME); MIT or Apache-2.0 (the Rust bindings).
- **Notice**: rbxport's export crate links LAME statically into
  `stemcel_rekordbox`. Stemcel does not use its encoder; it is linked because
  the export crate depends on it. Stemcel's complete source, including this
  library's, is published under the AGPL, so the LGPL's requirement that the
  combined work can be rebuilt against a modified LAME is met.

Their other Rust dependencies (SQLCipher with a vendored OpenSSL, symphonia,
lofty, serde and so on) are under permissive or Apache-2.0 licences; `cargo
license` in `backend/rekordbox_ffi` lists them.

## Frontend (Electron + Vue)

### Electron, Chromium, Node.js

- **Licences**: MIT (Electron, Node.js), BSD-3-Clause (Chromium / V8), and
  many transitive component licences enumerated in Electron's bundled
  `LICENSES.chromium.html`.
- **Project**: <https://www.electronjs.org>

### Vue 3, Pinia, Vue Router

- **Licence**: MIT — © Yuxi (Evan) You and Vue contributors.
- **Project**: <https://vuejs.org>

### PixiJS

- **Licence**: MIT — © Mat Groves / Goodboy Digital and contributors.
- **Project**: <https://pixijs.com>

### Tailwind CSS, Vite, electron-vite, vue-tsc, ESLint, TypeScript

- **Licence**: MIT or Apache-2.0 (per package).
- The full set of npm dependency licences is enumerated by
  `pnpm licenses list` and is included in production release packages.

### music-metadata

- **Licence**: MIT — © Borewit and contributors.
- **Project**: <https://github.com/Borewit/music-metadata>

## Stem separation (installed by the user, not bundled)

### audio-separator — © Andrew Beveridge (nomadkaraoke) and contributors

- **Licence**: MIT.
- **Project**: <https://github.com/nomadkaraoke/python-audio-separator>
- **Use**: runs Ultimate Vocal Remover's separation models. The user installs it;
  Stemcel runs it as a separate process and imports the files it writes. It
  downloads each model on first use from the model's own published location;
  each model keeps its author's licence, shown by `audio-separator --list_models`
  and the model's page.

### ONNX Runtime (+ DirectML) — © Microsoft

- **Licence**: MIT.
- **Use**: bundled with the backend, which still carries an ONNX separation
  engine; the native app separates stems through audio-separator instead.
- **Project**: <https://github.com/microsoft/onnxruntime>

## Fonts

### JetBrains Mono — © 2020 The JetBrains Mono Project Authors

- Use: the native app's interface font (built in from `backend/app/fonts/`).
- License: SIL Open Font License 1.1, `backend/app/fonts/OFL.txt`.

## Branding

The Stemcel name, logo, and icons are © the Stemcel contributors. They are
**not** covered by the AGPL grant and may not be used to promote, endorse, or
identify derived works without permission.

## Updating this file

When adding or upgrading a runtime dependency, append its licence + attribution
here so the bundled release package remains compliant.
