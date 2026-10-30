# Installing Stemcel

Stemcel is a native desktop application for Windows (10 version 1809 or later, or 11,
64-bit) and macOS (14 Sonoma or later, Apple Silicon).

## Windows

Download `Stemcel-<version>-native-win-x64.zip`, extract it to any folder you can
write to, and run `Stemcel.exe`. Nothing is installed and no administrator rights are
needed; to remove it, delete the folder.

1. *(Optional, avoids the SmartScreen prompt)* right-click the zip ▸ **Properties** ▸
   tick **Unblock** ▸ **OK**.
2. Extract it.
3. Run `Stemcel.exe`. The first launch may show *"Windows protected your PC"*: choose
   **More info** ▸ **Run anyway**.

Everything the app needs sits beside `Stemcel.exe` (the MP3 encoder, MIDI controller
profiles, ONNX Runtime for stem separation, and the rekordbox reader), so keep the
folder together.

## macOS

Open `Stemcel-<version>-native-mac-arm64.dmg` and drag **Stemcel** to
**Applications** (or unzip the `.zip` alongside it).

The app is not notarized, so macOS quarantines it on download. Clear the flag once:

```bash
xattr -dr com.apple.quarantine /Applications/Stemcel.app
```

Recording asks for microphone access the first time you open an input. If you declined,
re-enable **Stemcel** under **System Settings ▸ Privacy & Security ▸ Microphone**.

## rekordbox

If rekordbox is installed, the **rekordbox** tab in the bottom panel shows your library
(read-only — Stemcel never writes to it). Importing a track brings rekordbox's beat
grid and key with it.

## Stem separation models

Stem separation downloads its models on first use (Preferences ▸ Stems, or the Separate
Stems dialog) into `~/Stemcel/Models`. They are verified before use.

### UVR models (optional)

For the community's best separation models, the ones Ultimate Vocal Remover uses, pick
**UVR models (audio-separator)** in the Separate Stems dialog. They run through
[audio-separator](https://github.com/nomadkaraoke/python-audio-separator), which you install
once with [pipx](https://pipx.pypa.io) (Python 3.10 or later; the commands install pipx
too). **Copy install command** in the dialog copies the right one for your system.

Windows (PowerShell):

```powershell
python -m pip install --user pipx; python -m pipx install "audio-separator[cpu]"; python -m pipx inject audio-separator audioread
```

macOS (Terminal, with [Homebrew](https://brew.sh)):

```bash
brew install pipx && pipx install "audio-separator[cpu]" && pipx inject audio-separator audioread
```

On Windows with an NVIDIA GPU, also put the CUDA build of PyTorch into the tool's environment
(pip's default PyTorch on Windows is CPU-only, and on the CPU a full song takes minutes per
model). The Separate Stems dialog says whether the tool will use the GPU, and copies this for you:

```powershell
python -m pipx runpip audio-separator install --force-reinstall --no-deps torch torchvision --index-url https://download.pytorch.org/whl/cu130
```

On a Mac it uses the GPU (MPS) as installed. Stemcel finds the
`audio-separator` command on your PATH or in pip's and pipx's usual folders; use **Locate…**
in the dialog if it is somewhere else. Models download on first use into
`~/Stemcel/Models/uvr`.

The dialog offers the guide's current picks: **6 stems** (BS-Roformer SW: vocals, drums,
bass, guitar, piano, other), vocal and instrumental ensembles, karaoke (lead vocals out),
drum-kit pieces (MDX23C DrumSep, for a drums stem), de-reverb, or any model by file name
(`audio-separator --list_models`).

How they compare with the built-in models ([measured](docs/native-app.md#measured-quality)):
BS-Roformer SW gives better bass, drums and other stems; the built-in vocals are as good
or better. The default **6 stems, best of both** recipe combines the two.

## Troubleshooting: recording is silent

Recording needs the operating system to let Stemcel use your input.

- **Windows:** open **Settings ▸ Privacy & security ▸ Microphone** and check that
  *Microphone access* and *Let apps access your microphone* are on, and that
  **Stemcel** is on in the list. Windows can block an input without a prompt; a
  blocked input opens normally and returns silence.
- **macOS:** **System Settings ▸ Privacy & Security ▸ Microphone** ▸ **Stemcel**.

## Troubleshooting: diagnostics

Every launch writes a short startup log, and a crash report if anything faults, to:

```text
~/Stemcel/Diagnostics            (Windows: %USERPROFILE%\Stemcel\Diagnostics)
```

- `backend.log` — the last launch's startup log (the whole session when
  **Preferences ▸ Developer ▸ Write a full diagnostic log** is on).
- `backend-crash-<timestamp>.log` — written only after a crash.

File paths in logs have your user name removed. Attach these files when reporting a
problem.
