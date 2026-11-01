cmake_minimum_required(VERSION 3.22)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# macOS: target a fixed minimum rather than the build machine's SDK, so the engine
# runs everywhere the app does (electron-builder.yml `mac.minimumSystemVersion`).
# Must precede project().
# scripts/fetch-lame.sh builds the bundled LAME for the same target.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version for the backend")

project(StemcelBackend
    VERSION 0.1.0
    DESCRIPTION "Stemcel headless audio backend"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # The Electron/NSIS installer does not bootstrap the Visual C++
    # Redistributable. Link the backend against the static MSVC runtime
    # so clean Windows installs don't fail to spawn StemcelBackend.exe
    # due to missing MSVCP140/VCRUNTIME DLLs.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Export compile_commands.json for clangd / IntelliSense.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── JUCE ────────────────────────────────────────────────────────────────
# JUCE is fetched at configure time. The first configure downloads ~250MB
# into `backend/build/_deps/juce-src/`; subsequent configures are cached.
# Bump JUCE_GIT_TAG to update the pinned version.
set(JUCE_GIT_TAG "8.0.12" CACHE STRING "JUCE version to fetch")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${JUCE_GIT_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

# ─── IXWebSocket ─────────────────────────────────────────────────────────
# Local-only WebSocket bridge between the JUCE backend and the Electron
# renderer. TLS + zlib are disabled — we only ever talk to ws://localhost.
set(IXWEBSOCKET_GIT_TAG "v11.4.5" CACHE STRING "IXWebSocket version to fetch")
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(USE_OPEN_SSL OFF CACHE BOOL "" FORCE)
set(USE_MBED_TLS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(IXWebSocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        ${IXWEBSOCKET_GIT_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(IXWebSocket)

# ─── libsamplerate ───────────────────────────────────────────────────────
# BTrack (below) needs a sample-rate converter to resample its onset-
# detection function. We use the upstream `libsamplerate` library
# (BSD-2-Clause) fetched at configure time so there's no system
# dependency to manage on Windows.
set(LIBSAMPLERATE_GIT_TAG "0.2.2" CACHE STRING "libsamplerate version to fetch")
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# libsamplerate 0.2.2 declares an obsolete policy floor. Apply the
# current CMake compatibility floor without patching upstream source.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE STRING "" FORCE)

FetchContent_Declare(libsamplerate
    GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
    GIT_TAG        ${LIBSAMPLERATE_GIT_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(libsamplerate)

# ─── BTrack (BPM + beat tracking) ────────────────────────────────────────
# We vendor a patched copy of BTrack under `backend/third_party/btrack/`:
# the upstream sources use VLAs (a GCC extension MSVC rejects) and rely
# on the non-portable `M_PI` macro. The patches (documented at the top of
# `BTrack.cpp`) replace the VLAs with `std::vector` and add
# `#define _USE_MATH_DEFINES`. The KISS FFT backend is used (BSD-licensed,
# also vendored) so there's no FFTW dependency.
set(BTRACK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/btrack)
add_library(btrack_local STATIC
    ${BTRACK_DIR}/BTrack.cpp
    ${BTRACK_DIR}/OnsetDetectionFunction.cpp
    ${BTRACK_DIR}/kiss_fft130/kiss_fft.c)
target_include_directories(btrack_local PUBLIC
    ${BTRACK_DIR}
    ${BTRACK_DIR}/kiss_fft130)
target_compile_definitions(btrack_local PUBLIC USE_KISS_FFT)
# Silence MSVC's "unsafe" warnings inside BTrack's third-party code so
# we don't drown out our own warnings.
if(MSVC)
    target_compile_options(btrack_local PRIVATE /W0 /wd4267 /wd4244)
endif()
target_link_libraries(btrack_local PRIVATE SampleRate::samplerate)

# ─── MiniBPM (second, independent fixed-tempo estimator) ─────────────────
# Vendored verbatim (two self-contained files, no dependencies) under
# `backend/third_party/minibpm/`, following the BTrack precedent. BTrack is a
# CAUSAL tracker whose tempo seed constrains every later refinement stage to
# +/-10% of its period, so a bad seed is unrecoverable. MiniBPM is a whole-file
# estimator with a different failure profile, which is exactly what makes it
# useful as a corroborating second opinion rather than a replacement.
# Licence: GPL-2.0-or-later, compatible with the project's AGPL-3.0-or-later
# (see THIRD_PARTY_LICENSES.md and ADR 0028).
set(MINIBPM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minibpm)
add_library(minibpm_local STATIC ${MINIBPM_DIR}/MiniBpm.cpp)
target_include_directories(minibpm_local PUBLIC ${MINIBPM_DIR})
# Silence MSVC's "unsafe" warnings inside third-party code so our own warnings
# stay visible, matching the btrack_local treatment above.
if(MSVC)
    target_compile_options(minibpm_local PRIVATE /W0 /wd4267 /wd4244)
endif()

# ─── Rubber Band Library (time-stretch + pitch-shift) ────────────────────
# Rubber Band ships a Meson-only build; there's no upstream CMake target.
# We FetchContent the source tree (downloads only, no add_subdirectory) and
# build it ourselves as a static library with the KissFFT backend (also
# vendored inside the Rubber Band source under `src/ext/kissfft/`) and
# libsamplerate for the resampling stage. That's exactly the FFT + SRC
# pair BTrack already uses, so we don't pick up any new transitive
# dependencies for the warp engine.
#
# Licence: Rubber Band Library is GPL-2.0+. Stemcel ships under
# AGPL-3.0+, which is GPL-2.0+ compatible (per the AGPL §13 / GPL §6
# relicensing clauses), so we can link statically without further
# action; the upstream `COPYING` lives at `_deps/rubberband-src/COPYING`
# and is picked up by the THIRD_PARTY_LICENSES.md aggregation step.
set(RUBBERBAND_GIT_TAG "v3.3.0" CACHE STRING "Rubber Band Library version to fetch")

FetchContent_Declare(rubberband
    GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
    GIT_TAG        ${RUBBERBAND_GIT_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
# With no upstream CMakeLists.txt, MakeAvailable populates the sources
# without adding a dependency target; Stemcel defines that target below.
FetchContent_MakeAvailable(rubberband)

set(RB_DIR ${rubberband_SOURCE_DIR})

# Rubber Band v3.3.0 has a stray `#include "system/sysutils.h"` in
# `src/common/VectorOpsComplex.cpp` — the actual header lives at
# `src/common/sysutils.h` and no `system/` directory exists on disk.
# Confirmed against an upstream checkout; almost certainly a typo from
# a half-finished directory rename. Rather than patch the upstream
# source, generate a one-line shim header in the build directory and
# put it on the include path so the broken include resolves cleanly.
set(RB_SHIM_DIR ${CMAKE_CURRENT_BINARY_DIR}/rubberband_shims)
file(WRITE ${RB_SHIM_DIR}/system/sysutils.h
    "// Shim generated by Stemcel's CMake build to work around an\n"
    "// upstream include typo in Rubber Band v3.3.0 — see backend/CMakeLists.txt.\n"
    "#pragma once\n"
    "#include \"common/sysutils.h\"\n")

add_library(rubberband_local STATIC
    ${RB_DIR}/src/RubberBandStretcher.cpp
    ${RB_DIR}/src/common/Allocators.cpp
    ${RB_DIR}/src/common/BQResampler.cpp
    ${RB_DIR}/src/common/FFT.cpp
    ${RB_DIR}/src/common/Log.cpp
    ${RB_DIR}/src/common/mathmisc.cpp
    ${RB_DIR}/src/common/Profiler.cpp
    ${RB_DIR}/src/common/Resampler.cpp
    ${RB_DIR}/src/common/StretchCalculator.cpp
    ${RB_DIR}/src/common/sysutils.cpp
    ${RB_DIR}/src/common/Thread.cpp
    ${RB_DIR}/src/common/VectorOpsComplex.cpp
    ${RB_DIR}/src/faster/AudioCurveCalculator.cpp
    ${RB_DIR}/src/faster/CompoundAudioCurve.cpp
    ${RB_DIR}/src/faster/HighFrequencyAudioCurve.cpp
    ${RB_DIR}/src/faster/PercussiveAudioCurve.cpp
    ${RB_DIR}/src/faster/R2Stretcher.cpp
    ${RB_DIR}/src/faster/SilentAudioCurve.cpp
    ${RB_DIR}/src/faster/StretcherChannelData.cpp
    ${RB_DIR}/src/faster/StretcherProcess.cpp
    ${RB_DIR}/src/finer/R3Stretcher.cpp
    ${RB_DIR}/src/ext/kissfft/kiss_fft.c
    ${RB_DIR}/src/ext/kissfft/kiss_fftr.c)
target_include_directories(rubberband_local PUBLIC
    ${RB_DIR}
    ${RB_DIR}/rubberband
    ${RB_DIR}/src
    ${RB_SHIM_DIR})
target_include_directories(rubberband_local PRIVATE
    ${RB_DIR}/src/ext/kissfft)
target_compile_definitions(rubberband_local PUBLIC
    # Use the vendored KissFFT for all FFT work; matches the BTrack
    # build's FFT choice so the two libraries share the same lineage.
    HAVE_KISSFFT
    # Use libsamplerate (already linked via SampleRate::samplerate) for
    # the resampler path. Without this, Rubber Band falls back to its
    # own BQResampler.
    HAVE_LIBSAMPLERATE
    # Stop windows.h from defining `min` / `max` as preprocessor macros
    # — Rubber Band's `finer/Guide.h` calls `std::min(...)` /
    # `std::max(...)`, and the macro expansion shreds the call site
    # ("error C2589: '(': illegal token on right side of '::'"). The
    # define is one-time, picked up by every Windows-API include
    # transitively pulled in by Rubber Band's source files.
    NOMINMAX
    # Disable Rubber Band's internal asserts in release builds — the
    # library is mature and the per-process-call asserts are noticeable
    # at warp-engine block sizes.
    NDEBUG_RUBBERBAND)
# Off Windows Rubber Band needs its threading backend named, which its own Meson
# build does per platform; mirror those defines. macOS has no `sincos`/`sincosf`
# (only Apple's `__sincos`), so it also takes the portable sin/cos path there.
if(APPLE)
    target_compile_definitions(rubberband_local PRIVATE
        USE_PTHREADS MALLOC_IS_ALIGNED LACK_SINCOS)
elseif(UNIX)
    target_compile_definitions(rubberband_local PRIVATE
        USE_PTHREADS HAVE_POSIX_MEMALIGN)
endif()
# Rubber Band's source liberally uses ints in size-comparison contexts;
# MSVC's signed/unsigned warnings would flood the build with noise from a
# third-party library we don't control. The pattern matches BTrack's
# treatment above.
if(MSVC)
    target_compile_options(rubberband_local PRIVATE
        /W0 /wd4267 /wd4244 /wd4305 /wd4018 /wd4146 /wd4715)
endif()
target_link_libraries(rubberband_local PUBLIC SampleRate::samplerate)

# ─── Signalsmith Stretch (time-stretch / pitch engine) ──────────────────
# MIT-licensed polyphonic time-stretch and pitch-shift library (header only),
# the engine behind the "pro" warp mode, and its one dependency, Signalsmith
# Linear (MIT). Both are fetched as plain sources (SOURCE_SUBDIR names a folder
# with no CMakeLists.txt, so neither project is added): their own CMake needs
# 3.24, above this project's floor, and a local target can mark the headers as
# system ones, keeping their warnings out of ours.
set(SIGNALSMITH_STRETCH_GIT_TAG "1.4.0" CACHE STRING "Signalsmith Stretch version to fetch")
set(SIGNALSMITH_LINEAR_GIT_TAG "0.6.6" CACHE STRING "Signalsmith Linear version to fetch")
FetchContent_Declare(signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        ${SIGNALSMITH_STRETCH_GIT_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  not-a-cmake-project)
FetchContent_Declare(signalsmith-linear
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
    GIT_TAG        ${SIGNALSMITH_LINEAR_GIT_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  not-a-cmake-project)
FetchContent_MakeAvailable(signalsmith-stretch signalsmith-linear)
add_library(signalsmith_stretch_local INTERFACE)
target_include_directories(signalsmith_stretch_local SYSTEM INTERFACE
    ${signalsmith-stretch_SOURCE_DIR}/include
    ${signalsmith-linear_SOURCE_DIR}/include)
if(MSVC)
    # The library's templates need the bigger object format.
    target_compile_options(signalsmith_stretch_local INTERFACE /bigobj)
endif()
if(APPLE)
    # Its FFTs run on Accelerate on Apple platforms, as upstream's CMake does.
    target_link_libraries(signalsmith_stretch_local INTERFACE "-framework Accelerate")
    target_compile_definitions(signalsmith_stretch_local INTERFACE
        SIGNALSMITH_USE_ACCELERATE ACCELERATE_NEW_LAPACK)
endif()

# ─── RNNoise (vocal-stem denoise) ────────────────────────────────────────
# RNNoise is an RNN-based noise/artefact suppressor (BSD-2-Clause, pure C).
# We use it for offline cleanup of the *vocals* stem after separation. We pin
# the classic v0.1.1 release because — unlike current `main` — it ships the
# trained model in-tree (`src/rnn_data.c`), so the build needs no model
# download step and stays fully reproducible through FetchContent.
#
# Populate-only (no add_subdirectory): upstream builds with autotools, so we
# compile the handful of self-contained C sources ourselves into a static
# library, exactly like the Rubber Band block above. The default model is the
# built-in one selected by passing NULL to `rnnoise_create`.
#
# Licence: BSD-2-Clause. `_deps/rnnoise-src/COPYING` is picked up by the
# THIRD_PARTY_LICENSES.md aggregation step.
set(RNNOISE_GIT_TAG "v0.1.1" CACHE STRING "RNNoise version to fetch")

FetchContent_Declare(rnnoise
    GIT_REPOSITORY https://github.com/xiph/rnnoise.git
    GIT_TAG        ${RNNOISE_GIT_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(rnnoise)

set(RNN_DIR ${rnnoise_SOURCE_DIR})

# pitch.c and celt_lpc.c (CELT-derived) declare a handful of C99 variable-length
# arrays, which the GNU toolchain accepts but MSVC rejects (same incompatibility
# the vendored BTrack copy is patched for). Rather than fork the upstream tree,
# we rewrite just those five declarations to `_alloca` at configure time and
# compile the patched copies; every other source — including the in-tree model —
# is used verbatim from the pinned checkout. Each VLA declaration is a unique
# single line, so the replacements are unambiguous. Clang and GCC compile the
# VLAs as written (and have no `<malloc.h>` / `_alloca` on macOS), so the patch
# is MSVC-only and other toolchains build the upstream files directly.
if(MSVC)
    set(RNN_PATCH_DIR ${CMAKE_CURRENT_BINARY_DIR}/rnnoise_patched)
    file(MAKE_DIRECTORY ${RNN_PATCH_DIR})

    file(READ ${RNN_DIR}/src/pitch.c RNN_PITCH_SRC)
    string(REPLACE "opus_val16 x_lp4[len>>2];"
        "opus_val16 *x_lp4 = (opus_val16*)_alloca(sizeof(opus_val16)*(len>>2));"
        RNN_PITCH_SRC "${RNN_PITCH_SRC}")
    string(REPLACE "opus_val16 y_lp4[lag>>2];"
        "opus_val16 *y_lp4 = (opus_val16*)_alloca(sizeof(opus_val16)*(lag>>2));"
        RNN_PITCH_SRC "${RNN_PITCH_SRC}")
    string(REPLACE "opus_val32 xcorr[max_pitch>>1];"
        "opus_val32 *xcorr = (opus_val32*)_alloca(sizeof(opus_val32)*(max_pitch>>1));"
        RNN_PITCH_SRC "${RNN_PITCH_SRC}")
    string(REPLACE "opus_val32 yy_lookup[maxperiod+1];"
        "opus_val32 *yy_lookup = (opus_val32*)_alloca(sizeof(opus_val32)*(maxperiod+1));"
        RNN_PITCH_SRC "${RNN_PITCH_SRC}")
    file(WRITE ${RNN_PATCH_DIR}/pitch.c "#include <malloc.h>\n${RNN_PITCH_SRC}")

    file(READ ${RNN_DIR}/src/celt_lpc.c RNN_LPC_SRC)
    string(REPLACE "opus_val16 xx[n];"
        "opus_val16 *xx = (opus_val16*)_alloca(sizeof(opus_val16)*(n));"
        RNN_LPC_SRC "${RNN_LPC_SRC}")
    file(WRITE ${RNN_PATCH_DIR}/celt_lpc.c "#include <malloc.h>\n${RNN_LPC_SRC}")

    set(RNN_PITCH_C ${RNN_PATCH_DIR}/pitch.c)
    set(RNN_CELT_LPC_C ${RNN_PATCH_DIR}/celt_lpc.c)
else()
    set(RNN_PITCH_C ${RNN_DIR}/src/pitch.c)
    set(RNN_CELT_LPC_C ${RNN_DIR}/src/celt_lpc.c)
endif()

add_library(rnnoise_local STATIC
    ${RNN_DIR}/src/denoise.c
    ${RNN_DIR}/src/rnn.c
    ${RNN_DIR}/src/rnn_data.c
    ${RNN_DIR}/src/rnn_reader.c
    ${RNN_PITCH_C}
    ${RNN_CELT_LPC_C}
    ${RNN_DIR}/src/kiss_fft.c)
# Consumers only need the public C API; the internal CELT-derived headers stay
# private to the library's own translation units.
target_include_directories(rnnoise_local PUBLIC ${RNN_DIR}/include)
target_include_directories(rnnoise_local PRIVATE ${RNN_DIR}/src)
# `_USE_MATH_DEFINES` exposes M_PI under MSVC (same patch BTrack needs). Silence
# the third-party C sources' warnings so they don't drown out ours.
target_compile_definitions(rnnoise_local PRIVATE _USE_MATH_DEFINES)
if(MSVC)
    target_compile_options(rnnoise_local PRIVATE /W0 /wd4244 /wd4267 /wd4305 /wd4996)
endif()

option(STEMCEL_BUILD_TESTS "Build backend unit tests" OFF)
option(STEMCEL_ENABLE_COVERAGE "Instrument backend unit tests for code coverage" OFF)

if(STEMCEL_ENABLE_COVERAGE AND NOT STEMCEL_BUILD_TESTS)
    message(FATAL_ERROR "STEMCEL_ENABLE_COVERAGE requires STEMCEL_BUILD_TESTS=ON")
endif()

# ─── Generated version header ────────────────────────────────────────────
# Single source of truth for the backend version is the project() VERSION
# above; generate stemcel::kBackendVersion from it so no source file
# hardcodes the number.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h
    @ONLY)

# macOS: every executable built from here on searches its own directory first for
# the libraries bundled beside it.
if(APPLE)
    set(CMAKE_BUILD_RPATH "@executable_path")
endif()

add_library(stemcel_backend_core STATIC
    # bridge
    src/bridge/BridgeAuth.cpp
    src/bridge/BridgeDispatch.cpp
    src/bridge/BridgeServer.cpp
    src/bridge/CommandHelpers.cpp
    src/bridge/PlayheadEmitter.cpp
    # commands
    src/commands/AudioDeviceCommands.cpp
    src/commands/BeatRepeatCommands.cpp
    src/commands/PluginCommands.cpp
    src/commands/ClipCommands.cpp
    src/commands/LibraryCommands.cpp
    src/commands/MarkerCommands.cpp
    src/commands/MidiDeviceCommands.cpp
    src/commands/MixdownCommands.cpp
    src/commands/PeakJobCoordinator.cpp
    src/commands/PreviewCommands.cpp
    src/commands/ProjectCommands.cpp
    src/commands/ProjectImportCommands.cpp
    src/commands/ProjectImportSource.cpp
    src/commands/ProjectFxCommands.cpp
    src/commands/ProjectSettingsCommands.cpp
    src/commands/TempoCorrectionCommands.cpp
    src/commands/ScratchSessionCommands.cpp
    src/commands/ScratchPatternCommands.cpp
    src/commands/ScratchSaveCommands.cpp
    src/commands/RecordingCalibrationCommands.cpp
    src/commands/RecordingCommands.cpp
    src/commands/ScratchPatternReplayCommands.cpp
    src/commands/TrackCommands.cpp
    src/commands/TempoMapCommands.cpp
    src/commands/TransitionCommands.cpp
    src/commands/TransportCommands.cpp
    src/commands/UndoCommands.cpp
    src/commands/WaveformCommands.cpp
    # core
    src/core/AppPaths.cpp
    src/core/CrashHandler.cpp
    src/core/EngineHost.cpp
    src/core/LamePath.cpp
    src/core/Log.cpp
    src/core/NativeApplication.cpp
    # midi
    src/midi/MidiControllerMapping.cpp
    src/midi/MidiControllerProfiles.cpp
    src/midi/MidiScratchRouting.cpp
    # scratch
    src/scratch/ScratchProtocol.cpp
    src/scratch/ScratchProtocolPattern.cpp
    src/scratch/ScratchAudioSource.cpp
    src/scratch/ScratchRealismProcessor.cpp
    src/scratch/BackingMonitorSource.cpp
    src/scratch/ScratchBackingPreparation.cpp
    src/scratch/ScratchSourcePreparation.cpp
    src/scratch/ScratchActionRecorder.cpp
    src/scratch/ScratchSessionController.cpp
    src/scratch/ScratchSessionBacking.cpp
    src/scratch/ScratchSessionRecording.cpp
    src/scratch/ScratchSessionMidi.cpp
    src/scratch/ScratchPatternEvaluator.cpp
    src/scratch/ScratchPatternBake.cpp
    # recording
    src/recording/CaptureDevice.cpp
    src/recording/InputCaptureTap.cpp
    src/recording/InputMonitorSource.cpp
    src/recording/RecordingWriter.cpp
    src/recording/RecordingFinalise.cpp
    src/recording/RecordingCleanup.cpp
    src/recording/LatencyCalibration.cpp
    src/recording/LatencyCalibrator.cpp
    src/recording/RecordingSessionController.cpp
    # dsp
    src/dsp/BpmDetector.cpp
    src/dsp/BpmAnalysisHelpers.cpp
    src/dsp/BpmAudioLoader.cpp
    src/dsp/TempoDrift.cpp
    src/dsp/MiniBpmEstimator.cpp
    src/dsp/PercussiveEmphasis.cpp
    src/dsp/LoudnessAnalyzer.cpp
    src/dsp/SafetyLimiter.cpp
    src/dsp/SharedFx.cpp
    src/dsp/ToneEq.cpp
    src/dsp/VinylScratchProcessor.cpp
    src/dsp/VocalDenoiser.cpp
    src/dsp/WarpProcessor.cpp
    src/dsp/Waveform.cpp
    # engine
    src/engine/AudioEngine.cpp
    src/engine/AudioEngineBeatRepeat.cpp
    src/engine/AudioEngineAudibility.cpp
    src/engine/AudioEngineDevice.cpp
    src/engine/DeviceCallbackGuard.cpp
    src/engine/AudioEngineTransport.cpp
    src/engine/MasterClockSource.cpp
    src/engine/AudioEngineMix.cpp
    src/engine/AudioEngineClipEdit.cpp
    src/engine/AudioEngineClips.cpp
    src/engine/AudioEnginePreview.cpp
    src/engine/AudioEnginePlugins.cpp
    src/engine/TrackPluginRestore.cpp
    src/engine/AudioEngineScratch.cpp
    src/engine/AudioEnginePatternReplay.cpp
    src/engine/BusGraph.cpp
    src/engine/BusGraphTrackFx.cpp
    src/engine/BusGraphRender.cpp
    src/engine/BeatRepeatProcessor.cpp
    src/engine/BeatRepeatSnapshot.cpp
    src/engine/DecodedCache.cpp
    src/engine/EnginePlaybackPath.cpp
    src/engine/OffsetSource.cpp
    src/engine/OffsetSourceWarpRender.cpp
    src/engine/OffsetSourceTailRender.cpp
    # mixdown
    src/mixdown/MixdownBroadcast.cpp
    src/mixdown/MixdownEngine.cpp
    src/mixdown/MixdownExport.cpp
    src/mixdown/MixdownGraph.cpp
    src/mixdown/MixdownNormalize.cpp
    src/mixdown/MixdownRender.cpp
    src/mixdown/MixdownRenderPass1.cpp
    src/mixdown/MixdownSnapshot.cpp
    # project
    src/project/EditUndoState.cpp
    src/project/LibraryAnalysis.cpp
    src/project/PeaksCache.cpp
    src/project/ProjectFile.cpp
    src/project/ProjectSession.cpp
    src/project/ProjectState.cpp
    src/project/ProjectStateAutomation.cpp
    src/project/ProjectStateBeatRepeat.cpp
    src/project/ProjectStateTrackPlugins.cpp
    src/project/ProjectStateClips.cpp
    src/project/ProjectStateLibrary.cpp
    src/project/ProjectStateLibraryAnalysis.cpp
    src/project/ProjectStateMarkers.cpp
    src/project/ProjectStateTempoMap.cpp
    src/project/ProjectStateMixTransitions.cpp
    src/project/ProjectStateScratchPatterns.cpp
    src/project/ProjectStateProjectFx.cpp
    src/project/ProjectStateSerialization.cpp
    src/project/ProjectStateSettings.cpp
    src/project/ProjectStateTrackBitCrusher.cpp
    src/project/ProjectStateTracks.cpp
    src/project/ProjectTransitions.cpp
    src/project/SampleExport.cpp
    src/project/ValueTreeJson.cpp
    # plugins (VST3 hosting — ADR 0025)
    src/plugins/PluginCatalogue.cpp
    src/plugins/PluginChain.cpp
    src/plugins/PluginEditorWindow.cpp
    src/plugins/PluginScanCoordinator.cpp
    src/plugins/PluginScanJob.cpp
    src/plugins/PluginScanWorker.cpp)

target_compile_definitions(stemcel_backend_core PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_LAME_AUDIO_FORMAT=1
    JUCE_STANDALONE_APPLICATION=1
    # Hosted plugin formats (ADR 0025): VST3 everywhere, Audio Units on macOS, and VST 2
    # through FST's reverse-engineered headers (below) when STEMCEL_HOST_VST2 is on. LV2
    # and ARA stay pinned off so their SDKs are never compiled in by a JUCE default.
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_ARA=0)
if(APPLE)
    target_compile_definitions(stemcel_backend_core PUBLIC JUCE_PLUGINHOST_AU=1)
else()
    target_compile_definitions(stemcel_backend_core PUBLIC JUCE_PLUGINHOST_AU=0)
endif()

# VST 2 hosting. Steinberg withdrew the VST2 SDK and its licence does not fit the AGPL, so
# JUCE's VST host compiles against FST (Free Studio Technology, GPL-3.0-or-later: a
# clean-room, documented reverse-engineering of the interface), reached through the two
# header paths JUCE includes.
option(STEMCEL_HOST_VST2 "Host VST 2 plugins through FST's headers" ON)
if(STEMCEL_HOST_VST2)
    set(FST_GIT_TAG "647af068765b75867e3a28b4dd8991ab9ed47f7c" CACHE STRING "FST commit to fetch")
    FetchContent_Declare(fst
        GIT_REPOSITORY https://git.iem.at/zmoelnig/FST.git
        GIT_TAG        ${FST_GIT_TAG}
        GIT_SHALLOW    FALSE
        SOURCE_SUBDIR  do-not-add)
    FetchContent_MakeAvailable(fst)
    set(STEMCEL_VST2_COMPAT ${CMAKE_CURRENT_BINARY_DIR}/vst2compat)
    # FST leaves three trailing arrays open-ended; JUCE sizes these structs as fixed (it
    # allocates a speaker arrangement as sizeof plus speakers beyond eight, and puts a
    # MidiKeyName on the stack), so a patched copy gives them their fixed lengths: eight
    # speakers, two event pointers, and a 64-character key name with the two ints after it.
    file(READ ${fst_SOURCE_DIR}/fst/fst.h STEMCEL_FST_HEADER)
    function(stemcel_fst_fix from to)
        string(FIND "${STEMCEL_FST_HEADER}" "${from}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "FST header changed: '${from}' not found")
        endif()
        string(REPLACE "${from}" "${to}" patched "${STEMCEL_FST_HEADER}")
        set(STEMCEL_FST_HEADER "${patched}" PARENT_SCOPE)
    endfunction()
    stemcel_fst_fix("t_fstEvent*events[]" "t_fstEvent*events[2]")
    stemcel_fst_fix("t_fstSpeakerProperties speakers[]" "t_fstSpeakerProperties speakers[8]")
    stemcel_fst_fix("char keyName[]" "char keyName[64]; t_fstInt32 _reserved[2]")
    file(WRITE ${STEMCEL_VST2_COMPAT}/fst/fst.h "${STEMCEL_FST_HEADER}")
    file(WRITE ${STEMCEL_VST2_COMPAT}/pluginterfaces/vst2.x/aeffect.h
         "#pragma once\n#define FST2VST 1\n#include \"../../fst/fst.h\"\n")
    file(WRITE ${STEMCEL_VST2_COMPAT}/pluginterfaces/vst2.x/aeffectx.h
         "#pragma once\n#define FST2VST 1\n#include \"../../fst/fst.h\"\n")
    target_include_directories(stemcel_backend_core PUBLIC ${STEMCEL_VST2_COMPAT})
    target_compile_definitions(stemcel_backend_core PUBLIC JUCE_PLUGINHOST_VST=1)
else()
    target_compile_definitions(stemcel_backend_core PUBLIC JUCE_PLUGINHOST_VST=0)
endif()

target_include_directories(stemcel_backend_core PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/src/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mixdown
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins
    ${CMAKE_CURRENT_SOURCE_DIR}/src/project
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recording
    ${CMAKE_CURRENT_SOURCE_DIR}/src/stems)

target_link_libraries(stemcel_backend_core PUBLIC
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    # Plugin editors are native windows owned by this process (ADR 0025), so
    # the GUI module is a direct dependency rather than a transitive one.
    juce::juce_gui_basics
    ixwebsocket
    btrack_local
    minibpm_local
    rubberband_local
    signalsmith_stretch_local
    rnnoise_local
    SampleRate::samplerate
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(STEMCEL_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(stemcel_backend_core PRIVATE
            -O0 -g -fprofile-instr-generate -fcoverage-mapping)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(stemcel_backend_core PRIVATE
            -O0 -g --coverage)
    elseif(MSVC)
        # MSVC coverage is produced at runtime by OpenCppCoverage (see the
        # StemcelBackendCoverage target below), which reads the Debug PDBs.
        # No source instrumentation flags are needed here.
    else()
        message(FATAL_ERROR
            "Backend coverage requires Clang, GNU, or MSVC (OpenCppCoverage). "
            "The current compiler is ${CMAKE_CXX_COMPILER_ID}.")
    endif()
endif()

# ─── Target ──────────────────────────────────────────────────────────────
juce_add_console_app(StemcelBackend
    PRODUCT_NAME "Stemcel Backend"
    COMPANY_NAME "Stemcel")

# JUCE generates the Windows VERSIONINFO resource with
# `add_custom_command(OUTPUT ..._resources.rc COMMAND juceaide rcfile Info.txt ...)`
# but declares no DEPENDS on Info.txt, so once the .rc exists it is never
# regenerated. A version bump then leaves the built exe reporting the old
# FileVersion/ProductVersion even though Info.txt and kBackendVersion are
# current. Drop a stale .rc at configure time so the version the exe reports
# always matches project() above.
get_target_property(STEMCEL_RC_DIR StemcelBackend JUCE_GENERATED_SOURCES_DIRECTORY)
set(STEMCEL_RC_FILE "${STEMCEL_RC_DIR}/StemcelBackend_resources.rc")
if(EXISTS "${STEMCEL_RC_FILE}")
    file(READ "${STEMCEL_RC_FILE}" STEMCEL_RC_CONTENTS)
    if(NOT STEMCEL_RC_CONTENTS MATCHES "\"ProductVersion\",[ \t]+\"${PROJECT_VERSION}")
        message(STATUS "Removing stale VERSIONINFO resource so it regenerates as ${PROJECT_VERSION}")
        file(REMOVE "${STEMCEL_RC_FILE}")
    endif()
endif()

target_sources(StemcelBackend PRIVATE
    src/core/Main.cpp)

target_link_libraries(StemcelBackend PRIVATE
    stemcel_backend_core)

# ─── Windows application icon ────────────────────────────────────────────
# Plugin editor windows are real desktop windows owned by this process, so Windows takes
# their title-bar and taskbar icon from the running executable — JUCE registers its window
# class with `ExtractAssociatedIcon` on the module path. A console app carries no icon
# resource, which is why those windows show the generic executable glyph. Use the plug-badged
# variant of the app icon rather than the app icon itself: an editor's taskbar button sits
# next to the shell app's own, and two identical buttons read as one window opened twice.
# `scripts/Build-InstallerArt.py` derives it from the same source logo, so the two stay in step.
if(WIN32)
    set(STEMCEL_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/app/resources/stemcel-plugin.ico")
    if(EXISTS "${STEMCEL_APP_ICON}")
        set(STEMCEL_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/StemcelBackendIcon.rc")
        file(GENERATE OUTPUT "${STEMCEL_ICON_RC}"
             CONTENT "1 ICON \"${STEMCEL_APP_ICON}\"\n")
        set_source_files_properties("${STEMCEL_ICON_RC}" PROPERTIES GENERATED TRUE)
        target_sources(StemcelBackend PRIVATE "${STEMCEL_ICON_RC}")
    else()
        message(WARNING "Backend icon not found at ${STEMCEL_APP_ICON}; "
                        "plugin editor windows will use the default executable icon")
    endif()
endif()

set(STEMCEL_MIDI_MAPPING_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/midi-mappings")
target_compile_definitions(stemcel_backend_core PRIVATE
    STEMCEL_MIDI_MAPPING_DIR="${STEMCEL_MIDI_MAPPING_DIR}")
file(GLOB STEMCEL_MIDI_MAPPING_FILES CONFIGURE_DEPENDS
    "${STEMCEL_MIDI_MAPPING_DIR}/*.json")
add_custom_target(StemcelMidiMappings
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:StemcelBackend>/midi-mappings"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${STEMCEL_MIDI_MAPPING_DIR}"
        "$<TARGET_FILE_DIR:StemcelBackend>/midi-mappings"
    DEPENDS ${STEMCEL_MIDI_MAPPING_FILES}
    COMMENT "Copying MIDI controller profiles next to backend executable")
add_dependencies(StemcelBackend StemcelMidiMappings)

# ─── LAME MP3 encoder/decoder (required, bundled) ────────────────────────
# LAME is spawned as a child process for two jobs: MP3 *encoding* on export
# (via JUCE's LAMEEncoderAudioFormat) and MP3 *decoding* on import (see
# DecodedCache). It is required, not optional — MP3 is the most common source
# format, and JUCE's own MP3 reader mis-parses some files badly enough that
# they cannot be played or analysed at all. A build without LAME would be
# unable to export MP3 and would silently fall back to that broken reader, so
# fail the configure step instead of shipping a crippled binary.
#
# Windows: drop a lame.exe build into `backend/third_party/lame/lame.exe`
# (scripts/Fetch-Lame.ps1). macOS: build `backend/third_party/lame/lame` from
# source with scripts/fetch-lame.sh. See third_party/lame/README.md.
if(WIN32)
    set(STEMCEL_LAME_NAME "lame.exe")
    set(STEMCEL_LAME_HINT "scripts/Fetch-Lame.ps1")
else()
    set(STEMCEL_LAME_NAME "lame")
    set(STEMCEL_LAME_HINT "scripts/fetch-lame.sh")
endif()
set(STEMCEL_LAME_EXE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lame/${STEMCEL_LAME_NAME}"
    CACHE FILEPATH "Path to the bundled LAME executable (MP3 encoder/decoder)")
if(NOT EXISTS "${STEMCEL_LAME_EXE}")
    message(FATAL_ERROR
        "Required ${STEMCEL_LAME_NAME} not found at ${STEMCEL_LAME_EXE}. "
        "MP3 import and export both depend on it. Run ${STEMCEL_LAME_HINT}, "
        "or see backend/third_party/lame/README.md for how to obtain it.")
endif()
message(STATUS "MP3 encode/decode: ${STEMCEL_LAME_NAME} found at ${STEMCEL_LAME_EXE}")
add_custom_command(TARGET StemcelBackend POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STEMCEL_LAME_EXE}" "$<TARGET_FILE_DIR:StemcelBackend>/${STEMCEL_LAME_NAME}"
    COMMENT "Copying ${STEMCEL_LAME_NAME} next to backend executable")

# ─── Native app (JUCE UI + engine in one process) ────────────────────────
# The native replacement for the Electron UI: the same engine library and bridge
# commands, driven in-process by a JUCE interface (backend/app/). Built beside the
# console backend until it reaches parity; both read and write the same projects.
option(STEMCEL_BUILD_NATIVE_APP "Build the native JUCE app (StemcelNative)" ON)
if(STEMCEL_BUILD_NATIVE_APP)
    juce_add_gui_app(StemcelNative
        PRODUCT_NAME "Stemcel"
        COMPANY_NAME "Stemcel"
        BUNDLE_ID "com.stemcel.app"
        VERSION ${PROJECT_VERSION}
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/app/resources/256x256.png"
        DOCUMENT_EXTENSIONS stemcel
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "Stemcel records audio from the input you choose in Record Audio."
        HARDENED_RUNTIME_ENABLED TRUE
        HARDENED_RUNTIME_OPTIONS
            com.apple.security.device.audio-input
            com.apple.security.cs.disable-library-validation)

    target_sources(StemcelNative PRIVATE
        app/AppController.cpp
        app/AppPreferences.cpp
        app/ClipEditor.cpp
        app/DebugAssertHook.cpp
        app/AudioImport.cpp
        app/Automation.cpp
        app/Autosave.cpp
        app/FilesBrowser.cpp
        app/FxPanels.cpp
        app/KeyDetection.cpp
        app/LibraryInfoDialog.cpp
        app/LibraryPanel.cpp
        app/MediaStore.cpp
        app/MidiControllerActions.cpp
        app/Rekordbox.cpp
        app/SectionAnalysis.cpp
        app/SectionAnalyser.cpp
        app/Tracklist.cpp
        app/TempoReport.cpp
        app/KeyWheel.cpp
        app/RekordboxBrowser.cpp
        app/EngineClient.cpp
        app/ExportDialog.cpp
        app/MainComponent.cpp
        app/MainWindow.cpp
        app/NativeMain.cpp
        app/PreferencePages.cpp
        app/PreferencesDialog.cpp
        app/ProjectDialogs.cpp
        app/ProjectTabs.cpp
        app/RecordDialog.cpp
        app/RecoveryDialog.cpp
        app/ScratchEditor.cpp
        app/SmokeTest.cpp
        app/AutoMix.cpp
        app/BeatStutter.cpp
        app/Mashup.cpp
        app/MixerPanel.cpp
        app/MixTransitions.cpp
        app/Stems.cpp
        app/LiveState.cpp
        app/ProjectModel.cpp
        app/StartupScreen.cpp
        app/Theme.cpp
        app/TimelineActions.cpp
        app/TimelineView.cpp
        app/Toasts.cpp
        app/TransitionEditor.cpp
        app/TransitionsPanel.cpp
        app/TrackHeaders.cpp
        app/UvrSeparator.cpp
        app/TransportBar.cpp
        app/WaveformStore.cpp
        app/Workspace.cpp)
    target_include_directories(StemcelNative PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
    # The interface font, JetBrains Mono (SIL OFL 1.1, app/fonts/OFL.txt), built in.
    juce_add_binary_data(StemcelFonts
        HEADER_NAME StemcelFonts.h
        NAMESPACE StemcelFonts
        SOURCES app/fonts/JetBrainsMono-Regular.ttf app/fonts/JetBrainsMono-Bold.ttf)
    target_link_libraries(StemcelNative PRIVATE
        StemcelFonts
        stemcel_backend_core
        juce::juce_cryptography
        juce::juce_gui_extra)

    # Runtime code lives beside the executable (Contents/MacOS in the macOS bundle),
    # where findLameExecutable() and the @executable_path rpath look for it. The MIDI
    # controller profiles are data: beside the executable on Windows, in
    # Contents/Resources on macOS (codesign rejects non-code files in Contents/MacOS).
    if(APPLE)
        set(_stemcel_native_profiles "$<TARGET_BUNDLE_CONTENT_DIR:StemcelNative>/Resources/midi-mappings")
    else()
        set(_stemcel_native_profiles "$<TARGET_FILE_DIR:StemcelNative>/midi-mappings")
    endif()
    add_custom_command(TARGET StemcelNative POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STEMCEL_LAME_EXE}" "$<TARGET_FILE_DIR:StemcelNative>/${STEMCEL_LAME_NAME}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_stemcel_native_profiles}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${STEMCEL_MIDI_MAPPING_DIR}" "${_stemcel_native_profiles}"
        COMMENT "Copying LAME and MIDI controller profiles into StemcelNative")

    # rekordbox library access (rekordbox_ffi/, Rust over rbxport). A shared library the
    # app loads at runtime, so a build without cargo still produces a working app (the
    # rekordbox browser then says it is unavailable). OpenSSL's vendored build needs a
    # native Perl on Windows: pass -DSTEMCEL_PERL=<perl.exe> when the first perl on
    # PATH is MSYS/Git Bash's.
    find_program(STEMCEL_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
    option(STEMCEL_BUILD_REKORDBOX "Build the rekordbox library reader (needs cargo)" ON)
    set(STEMCEL_PERL "" CACHE FILEPATH "Perl used by OpenSSL's vendored build (Windows)")
    if(STEMCEL_BUILD_REKORDBOX AND STEMCEL_CARGO)
        set(_rbx_dir "${CMAKE_CURRENT_SOURCE_DIR}/rekordbox_ffi")
        set(_rbx_target_dir "${CMAKE_BINARY_DIR}/rekordbox_ffi")
        if(WIN32)
            set(_rbx_lib "stemcel_rekordbox.dll")
        elseif(APPLE)
            set(_rbx_lib "libstemcel_rekordbox.dylib")
        else()
            set(_rbx_lib "libstemcel_rekordbox.so")
        endif()
        set(_rbx_env "")
        if(STEMCEL_PERL)
            set(_rbx_env "PERL=${STEMCEL_PERL}")
        endif()
        set(_rbx_cargo_args build --release --manifest-path "${_rbx_dir}/Cargo.toml" --target-dir "${_rbx_target_dir}")
        if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
            list(APPEND _rbx_cargo_args --target x86_64-apple-darwin)
            set(_rbx_out "${_rbx_target_dir}/x86_64-apple-darwin/release/${_rbx_lib}")
        else()
            set(_rbx_out "${_rbx_target_dir}/release/${_rbx_lib}")
        endif()
        add_custom_target(stemcel_rekordbox
            COMMAND ${CMAKE_COMMAND} -E env ${_rbx_env} MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
                "${STEMCEL_CARGO}" ${_rbx_cargo_args}
            WORKING_DIRECTORY "${_rbx_dir}"
            BYPRODUCTS "${_rbx_out}"
            COMMENT "Building the rekordbox library reader (cargo)"
            USES_TERMINAL)
        add_dependencies(StemcelNative stemcel_rekordbox)
        add_custom_command(TARGET StemcelNative POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_rbx_out}" "$<TARGET_FILE_DIR:StemcelNative>/${_rbx_lib}"
            COMMENT "Copying the rekordbox reader next to StemcelNative")
    elseif(STEMCEL_BUILD_REKORDBOX)
        message(STATUS "cargo not found: StemcelNative builds without rekordbox support")
    endif()

    # The rekordbox reader (a Rust DLL) links the VC++ runtime dynamically; ship it beside
    # the app so a machine without the redistributable still loads it.
    if(MSVC)
        set(_stemcel_vc_runtime_dlls msvcp140.dll msvcp140_1.dll vcruntime140.dll vcruntime140_1.dll)
        # cl.exe lives at <VC>/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe, so the VC root is
        # 7 levels up; the runtime DLLs ship under <VC>/Redist/MSVC/<ver>/x64/Microsoft.VC*.CRT.
        get_filename_component(_stemcel_vc_root "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(_i RANGE 1 6)
            get_filename_component(_stemcel_vc_root "${_stemcel_vc_root}" DIRECTORY)
        endforeach()
        file(GLOB _stemcel_vc_crt_dirs "${_stemcel_vc_root}/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
        set(_stemcel_vc_crt_dir "")
        if(_stemcel_vc_crt_dirs)
            list(SORT _stemcel_vc_crt_dirs)
            list(GET _stemcel_vc_crt_dirs -1 _stemcel_vc_crt_dir)
        endif()
        if(_stemcel_vc_crt_dir STREQUAL "" OR NOT EXISTS "${_stemcel_vc_crt_dir}/vcruntime140.dll")
            message(WARNING "VC++ runtime redist not found under ${_stemcel_vc_root}/Redist; the rekordbox "
                            "reader may fail to load on machines without the VC++ redistributable.")
        else()
            foreach(_dll IN LISTS _stemcel_vc_runtime_dlls)
                add_custom_command(TARGET StemcelNative POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_stemcel_vc_crt_dir}/${_dll}"
                        "$<TARGET_FILE_DIR:StemcelNative>/${_dll}"
                    COMMENT "Copying VC++ runtime ${_dll} next to StemcelNative")
            endforeach()
        endif()
    endif()
endif()

if(STEMCEL_BUILD_TESTS)
    enable_testing()

    add_executable(StemcelBackendTests
        tests/BackendTests.cpp
        tests/ProjectStateTests.cpp
        tests/ProjectStateFxTests.cpp
        tests/ProjectStateTrackPluginTests.cpp
        tests/PersistenceTests.cpp
        tests/BridgeTests.cpp
        tests/MidiControllerMappingTests.cpp
        tests/WarpTests.cpp
        tests/TempoMapTests.cpp
        tests/ScratchDspTests.cpp
        tests/BackingMonitorSourceTests.cpp
        tests/ScratchProtocolTests.cpp
        tests/ScratchSessionTests.cpp
        tests/ScratchRecorderTests.cpp
        tests/ScratchPatternPersistenceTests.cpp
        tests/ScratchPatternEvaluatorTests.cpp
        tests/ScratchPatternReplayProjectStateTests.cpp
        tests/PluginCatalogueTests.cpp
        tests/PluginChainTests.cpp
        tests/PluginLatencyTests.cpp
        tests/RecordingTests.cpp
        tests/AudioEngineTests.cpp
        tests/FxDspTests.cpp
        tests/LoudnessTests.cpp
        tests/EnvelopeFadeTests.cpp
        tests/AutomationTests.cpp
        tests/BeatRepeatTests.cpp
tests/MixdownRenderTests.cpp
        tests/BpmDetectorTests.cpp
        tests/VocalDenoiserTests.cpp
        tests/LibraryCleanupTests.cpp
        tests/DecodedCacheTests.cpp)
    target_link_libraries(StemcelBackendTests PRIVATE
        stemcel_backend_core)

    # Offline BPM/beat accuracy harness (dev tool, not shipped, not a CTest test).
    # Run: StemcelBpmEval <manifest>  (manifest lines: <path>|<referenceBpm>)
    add_executable(StemcelBpmEval tools/bpm_eval/BpmEval.cpp)
    target_link_libraries(StemcelBpmEval PRIVATE stemcel_backend_core)

    # Offline scratch-DSP quality/performance harness (dev tool, not shipped).
    add_executable(StemcelScratchDspEval
        tools/scratch_dsp_eval/ScratchDspEval.cpp)
    target_link_libraries(StemcelScratchDspEval PRIVATE
        stemcel_backend_core)

    # Phase A capture spike for ADR 0030: measures whether a standalone
    # input-only device disturbs the running playback stream, and reports
    # round-trip latency, relative clock drift and capture failure signals on
    # real hardware (dev tool, not shipped, not a CTest test). Run:
    # StemcelCaptureProbe [--seconds N] [--input "<name>"] [--type "<type>"]
    #                       [--tone] [--list]
    add_executable(StemcelCaptureProbe tools/capture_probe/CaptureProbe.cpp)
    target_link_libraries(StemcelCaptureProbe PRIVATE stemcel_backend_core)

    # Offline stem-separation quality harness, for scoring UVR's stems against references
    # (dev tool, not shipped, not a CTest test). Run: StemcelStemEval <manifest>
    # (manifest lines: <referenceWav>|<estimateWav>[|label]). Reports SI-SDR / SDR.
    add_executable(StemcelStemEval tools/stem_eval/StemEval.cpp)
    target_link_libraries(StemcelStemEval PRIVATE stemcel_backend_core)

    # Offline time-stretch comparison: every warp mode at several ratios, with round
    # trips, for scoring by script. Run: StemcelStretchEval <in.wav> <outDir> [ratio ...]
    add_executable(StemcelStretchEval tools/stretch_eval/StretchEval.cpp)
    target_link_libraries(StemcelStretchEval PRIVATE stemcel_backend_core)

    if(STEMCEL_ENABLE_COVERAGE)
        # Write reports to a dedicated, non-hidden folder (backend/build-coverage)
        # rather than inside backend/build, which the VS Code Explorer hides.
        set(STEMCEL_BACKEND_COVERAGE_DIR ${CMAKE_SOURCE_DIR}/build-coverage)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(StemcelBackendTests PRIVATE
                -O0 -g -fprofile-instr-generate -fcoverage-mapping)
            target_link_options(StemcelBackendTests PRIVATE
                -fprofile-instr-generate -fcoverage-mapping)
            find_program(LLVM_PROFDATA_EXE llvm-profdata)
            find_program(LLVM_COV_EXE llvm-cov)
            if(LLVM_PROFDATA_EXE AND LLVM_COV_EXE)
                add_custom_target(StemcelBackendCoverage
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${STEMCEL_BACKEND_COVERAGE_DIR}/html
                    COMMAND ${CMAKE_COMMAND} -E env
                        LLVM_PROFILE_FILE=${STEMCEL_BACKEND_COVERAGE_DIR}/backend.profraw
                        $<TARGET_FILE:StemcelBackendTests>
                    COMMAND ${LLVM_PROFDATA_EXE} merge -sparse
                        ${STEMCEL_BACKEND_COVERAGE_DIR}/backend.profraw
                        -o ${STEMCEL_BACKEND_COVERAGE_DIR}/backend.profdata
                    COMMAND ${LLVM_COV_EXE} report
                        $<TARGET_FILE:StemcelBackendTests>
                        -instr-profile=${STEMCEL_BACKEND_COVERAGE_DIR}/backend.profdata
                        "-ignore-filename-regex=.*[/\\\\](_deps|third_party|tests)[/\\\\].*"
                    COMMAND ${LLVM_COV_EXE} show
                        $<TARGET_FILE:StemcelBackendTests>
                        -instr-profile=${STEMCEL_BACKEND_COVERAGE_DIR}/backend.profdata
                        -format=html
                        -output-dir=${STEMCEL_BACKEND_COVERAGE_DIR}/html
                        "-ignore-filename-regex=.*[/\\\\](_deps|third_party|tests)[/\\\\].*"
                    DEPENDS StemcelBackendTests
                    COMMENT "Running backend tests with LLVM coverage")
            else()
                message(WARNING "llvm-profdata and llvm-cov are required for StemcelBackendCoverage")
            endif()
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(StemcelBackendTests PRIVATE
                -O0 -g --coverage)
            target_link_options(StemcelBackendTests PRIVATE
                --coverage)
            find_program(GCOVR_EXE gcovr)
            if(GCOVR_EXE)
                add_custom_target(StemcelBackendCoverage
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${STEMCEL_BACKEND_COVERAGE_DIR}
                    COMMAND $<TARGET_FILE:StemcelBackendTests>
                    COMMAND ${GCOVR_EXE}
                        -r ${CMAKE_CURRENT_SOURCE_DIR}
                        --filter ${CMAKE_CURRENT_SOURCE_DIR}/src
                        --exclude ${CMAKE_CURRENT_SOURCE_DIR}/third_party
                        --exclude ${CMAKE_CURRENT_SOURCE_DIR}/tests
                        --txt
                        --html-details
                        -o ${STEMCEL_BACKEND_COVERAGE_DIR}/index.html
                    DEPENDS StemcelBackendTests
                    COMMENT "Running backend tests with gcovr coverage")
            else()
                message(WARNING "gcovr is required for StemcelBackendCoverage")
            endif()
        elseif(MSVC)
            # MSVC has no built-in source coverage. Use OpenCppCoverage over the
            # already-built Debug binary (needs PDBs) — a runtime tool, so no
            # instrumentation flags are added; the target just runs it.
            find_program(OPENCPPCOVERAGE_EXE OpenCppCoverage
                HINTS "$ENV{ProgramFiles}/OpenCppCoverage"
                      "$ENV{ProgramFiles\(x86\)}/OpenCppCoverage")
            if(OPENCPPCOVERAGE_EXE)
                add_custom_target(StemcelBackendCoverage
                    COMMAND ${CMAKE_COMMAND}
                        -D "OPENCPPCOVERAGE=${OPENCPPCOVERAGE_EXE}"
                        -D "TEST_EXECUTABLE=$<TARGET_FILE:StemcelBackendTests>"
                        -D "SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src"
                        -D "OUTPUT_DIR=${STEMCEL_BACKEND_COVERAGE_DIR}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StemcelRunCoverage.cmake"
                    DEPENDS StemcelBackendTests
                    USES_TERMINAL
                    COMMENT "Running backend tests with OpenCppCoverage")
            else()
                message(WARNING "OpenCppCoverage is required for StemcelBackendCoverage on MSVC "
                    "(winget install OpenCppCoverage.OpenCppCoverage)")
            endif()
        endif()
    endif()

    # Register each backend test case as its own CTest test so it shows up
    # individually (VS Code Testing panel, `ctest` output) rather than as one
    # aggregate run. A POST_BUILD step runs the harness with `--list` and
    # regenerates the include file; CTest reads it via TEST_INCLUDE_FILES.
    set(_stemcel_tests_include "${CMAKE_CURRENT_BINARY_DIR}/StemcelBackendTests_tests.cmake")

    add_custom_command(TARGET StemcelBackendTests POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D "TEST_EXECUTABLE=$<TARGET_FILE:StemcelBackendTests>"
            -D "CTEST_FILE=${_stemcel_tests_include}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StemcelDiscoverTests.cmake"
        BYPRODUCTS "${_stemcel_tests_include}"
        COMMENT "Discovering backend test cases for CTest"
        VERBATIM)

    # Ensure the include file exists at configure time so CTest doesn't error on
    # a clean build dir before the first build has generated the real list.
    if(NOT EXISTS "${_stemcel_tests_include}")
        file(WRITE "${_stemcel_tests_include}"
            "# Regenerated at build time by StemcelDiscoverTests.cmake\n")
    endif()

    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_stemcel_tests_include}")
endif()
