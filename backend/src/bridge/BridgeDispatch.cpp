#include "BridgeDispatch.h"

#include "AudioEngine.h"
#include "BeatRepeatCommands.h"
#include "AudioDeviceCommands.h"
#include "BridgeServer.h"
#include "ClipCommands.h"
#include "CommandHelpers.h"
#include "DecodedCache.h"
#include "EditUndoState.h"
#include "LibraryCommands.h"
#include "Log.h"
#include "MarkerCommands.h"
#include "MidiDeviceCommands.h"
#include "MixdownCommands.h"
#include "PayloadHelpers.h"
#include "PeakJobCoordinator.h"
#include "PeaksCache.h"
#include "PluginCommands.h"
#include "PreviewCommands.h"
#include "ProjectCommands.h"
#include "ProjectFxCommands.h"
#include "ProjectSession.h"
#include "ProjectSettingsCommands.h"
#include "RecordingCalibrationCommands.h"
#include "RecordingCommands.h"
#include "TempoCorrectionCommands.h"
#include "ProjectState.h"
#include "SampleExport.h"
#include "ScratchSessionCommands.h"
#include "ScratchPatternCommands.h"
#include "ScratchSaveCommands.h"
#include "ScratchPatternReplayCommands.h"
#include "TrackCommands.h"
#include "TempoMapCommands.h"
#include "TransitionCommands.h"
#include "TransportCommands.h"
#include "UndoCommands.h"
#include "WaveformCommands.h"

#include <atomic>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace stemcel
{
namespace
{
// Mixdown state gates playback and carries the engine-polled cancel flag.
std::atomic<bool> g_mixdownBusy{false};
std::atomic<bool> g_mixdownCancel{false};

// Bundles the long, shared dispatch parameter list so each per-domain router
// stays readable. References only: the context outlives every dispatch call.
struct DispatchContext
{
    const juce::String& type;
    const juce::var& payload;
    stemcel::AudioEngine& engine;
    stemcel::ProjectState& projectState;
    stemcel::BridgeServer& bridge;
    juce::ThreadPool& peakPool;
    const stemcel::PeaksCache& cache;
    const stemcel::DecodedCache& decodedCache;
    stemcel::PeakJobCoordinator& peakJobs;
    stemcel::ProjectSession& session;
};
} // namespace

// Hoist payload readers so dispatch branches stay readable.
using stemcel::bridge::tryGetNumber;
using stemcel::bridge::tryGetRequiredString;
using stemcel::bridge::tryGetString;
using stemcel::bridge::readOptionalNumber;
using stemcel::bridge::readOptionalBool;
using stemcel::bridge::readOptionalString;
using stemcel::broadcastApplied;

namespace
{
// Each dispatchXxx returns true if it owned `type`. Branch bodies are unchanged
// from the original monolithic switch; only the routing is grouped by domain.

bool dispatchClip(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;
    auto& peakPool = ctx.peakPool;
    const auto& cache = ctx.cache;
    const auto& decodedCache = ctx.decodedCache;
    auto& session = ctx.session;

    if (type == "CLIP_ADD")
    {
        stemcel::log::info("bridge", "recv CLIP_ADD trackId=" + payload.getProperty("trackId", "").toString() +
                                           " clipId=" + payload.getProperty("clipId", "").toString());
        stemcel::handleClipAdd(payload, engine, projectState, bridge, peakPool, cache, decodedCache, ctx.peakJobs);
    }
    else if (type == "CLIP_MOVE")
    {
        stemcel::log::debug("bridge", "recv CLIP_MOVE clipId=" + payload.getProperty("clipId", "").toString() +
                                            " pos=" + payload.getProperty("positionMs", "").toString());
        stemcel::handleClipMove(payload, engine, projectState);
    }
    else if (type == "CLIP_TRIM")
    {
        stemcel::log::debug("bridge", "recv CLIP_TRIM clipId=" + payload.getProperty("clipId", "").toString() +
                                            " start=" + payload.getProperty("startMs", "").toString() +
                                            " in=" + payload.getProperty("inMs", "").toString() +
                                            " dur=" + payload.getProperty("durationMs", "").toString());
        stemcel::handleClipTrim(payload, engine, projectState);
    }
    else if (type == "CLIP_COLOR")
    {
        stemcel::log::debug("bridge", "recv CLIP_COLOR clipId=" + payload.getProperty("clipId", "").toString() +
                                            " idx=" + payload.getProperty("colorIndex", "").toString());
        stemcel::handleClipColor(payload, projectState);
    }
    else if (type == "CLIP_SET_LOCKED")
    {
        stemcel::handleClipSetLocked(payload, projectState);
    }
    else if (type == "CLIP_SET_REVERSED")
    {
        stemcel::handleClipSetReversed(payload, engine, projectState);
    }
    else if (type == "CLIP_SET_BEAT_OFFSET")
    {
        stemcel::handleClipSetBeatOffset(payload, projectState);
    }
    else if (type == "CLIP_SET_BRAKE")
    {
        stemcel::handleClipSetBrake(payload, engine, projectState);
    }
    else if (type == "CLIP_SET_BACKSPIN")
    {
        stemcel::handleClipSetBackspin(payload, engine, projectState);
    }
    else if (type == "CLIP_REMOVE")
    {
        stemcel::log::info("bridge", "recv CLIP_REMOVE clipId=" + payload.getProperty("clipId", "").toString());
        stemcel::handleClipRemove(payload, engine, projectState, bridge);
    }
    else if (type == "CLIP_RENAME")
    {
        stemcel::handleClipRename(payload, projectState);
    }
    else if (type == "CLIP_REBIND")
    {
        stemcel::handleClipRebind(payload, projectState);
    }
    else if (type == "CLIP_SET_WARP")
    {
        stemcel::handleClipSetWarp(payload, engine, projectState, bridge);
    }
    else if (type == "CLIP_SAVE_AS_SAMPLE")
    {
        stemcel::handleClipSaveAsSample(payload, engine, projectState, bridge, peakPool, cache,
                                          session.currentPath);
    }
    else if (type == "CLIP_SLICE_TO_SAMPLES")
    {
        stemcel::handleClipSliceToSamples(payload, engine, projectState, bridge, peakPool, cache,
                                            session.currentPath);
    }
    else if (type == "CLIP_SPLIT_CHANNELS")
    {
        stemcel::handleClipSplitChannels(payload, engine, projectState, bridge, peakPool, cache,
                                           session.currentPath);
    }
    else if (type == "CLIP_SET_ENVELOPE")
    {
        stemcel::log::debug("bridge", "recv CLIP_SET_ENVELOPE clipId=" +
                                            payload.getProperty("clipId", "").toString());
        stemcel::handleClipSetEnvelope(payload, engine, projectState, bridge);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchLibrary(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;
    auto& peakPool = ctx.peakPool;
    const auto& cache = ctx.cache;
    const auto& decodedCache = ctx.decodedCache;
    auto& session = ctx.session;

    if (type == "LIBRARY_ITEM_RELINK")
    {
        stemcel::log::info("bridge", "recv LIBRARY_ITEM_RELINK itemId=" + payload.getProperty("itemId", "").toString() +
                                            " path=" + payload.getProperty("filePath", "").toString());
        stemcel::handleLibraryItemRelink(payload, engine, projectState, bridge, session, peakPool, decodedCache);
    }
    else if (type == "LIBRARY_ITEM_SAVE_AS_SAMPLE")
    {
        stemcel::handleLibraryItemSaveAsSample(payload, engine, projectState, bridge, peakPool, cache,
                                                 session.currentPath);
    }
    else if (type == "LIBRARY_ADD")
    {
        stemcel::handleLibraryAdd(payload, engine, projectState, bridge, peakPool, decodedCache);
    }
    else if (type == "LIBRARY_REMOVE")
    {
        stemcel::handleLibraryRemove(payload, projectState, session);
    }
    else if (type == "LIBRARY_DELETE_ARTIFACTS")
    {
        stemcel::handleLibraryDeleteArtifacts(payload, session, engine);
    }
    else if (type == "LIBRARY_REANALYSE")
    {
        stemcel::handleLibraryReanalyse(payload, engine, projectState, bridge, peakPool, decodedCache);
    }
    else if (type == "LIBRARY_ITEM_SET_AUDIO_TYPE")
    {
        stemcel::handleLibraryItemSetAudioType(payload, projectState);
    }
    else if (type == "LIBRARY_ITEM_SET_COVER_HIDDEN")
    {
        stemcel::handleLibraryItemSetCoverHidden(payload, projectState);
    }
    else if (type == "LIBRARY_ITEM_SET_SECTIONS")
    {
        stemcel::handleLibraryItemSetSections(payload, projectState);
    }
    else if (type == "LIBRARY_ITEM_SET_COVER_OVERRIDE")
    {
        stemcel::handleLibraryItemSetCoverOverride(payload, projectState);
    }
    else if (type == "LIBRARY_ITEM_SET_MANUAL_TEMPO")
    {
        stemcel::handleLibraryItemSetManualTempo(payload, engine, projectState, bridge);
    }
    else if (type == "LIBRARY_ITEM_CORRECT_TEMPO")
    {
        stemcel::handleLibraryItemCorrectTempo(payload, engine, projectState, bridge);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchTransport(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;

    if (type == "TRANSPORT_PLAY")
    {
        stemcel::handleTransportPlay(engine, g_mixdownBusy.load());
    }
    else if (type == "TRANSPORT_PAUSE")
    {
        stemcel::handleTransportPause(engine);
    }
    else if (type == "TRANSPORT_STOP")
    {
        stemcel::handleTransportStop(engine, projectState);
    }
    else if (type == "TRANSPORT_SEEK")
    {
        stemcel::handleTransportSeek(payload, engine, projectState);
    }
    else if (type == "TRANSPORT_SCRUB")
    {
        stemcel::handleTransportScrub(payload, engine, projectState);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchPreview(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;
    const auto& decodedCache = ctx.decodedCache;
    auto& peakPool = ctx.peakPool;

    if (type == "PREVIEW_LOAD")
    {
        stemcel::handlePreviewLoad(payload, engine, projectState, bridge, decodedCache, peakPool);
    }

    else if (type == "PREVIEW_UNLOAD")
    {
        stemcel::handlePreviewUnload(engine, bridge);
    }
    else if (type == "PREVIEW_PLAY")
    {
        stemcel::handlePreviewPlay(engine, bridge);
    }
    else if (type == "PREVIEW_PAUSE")
    {
        stemcel::handlePreviewPause(engine, bridge);
    }
    else if (type == "PREVIEW_STOP")
    {
        stemcel::handlePreviewStop(engine, bridge);
    }
    else if (type == "PREVIEW_SEEK")
    {
        stemcel::handlePreviewSeek(payload, engine);
    }
    else if (type == "PREVIEW_SET_LOOP")
    {
        stemcel::handlePreviewSetLoop(payload, engine);
    }
    else if (type == "PREVIEW_SET_WARP")
    {
        stemcel::handlePreviewSetWarp(payload, engine);
    }
    else if (type == "PREVIEW_SET_ENVELOPE")
    {
        stemcel::handlePreviewSetEnvelope(payload, engine);
    }
    else if (type == "PREVIEW_SET_REVERSED")
    {
        stemcel::handlePreviewSetReversed(payload, engine);
    }
    else if (type == "PREVIEW_SET_METRONOME")
    {
        stemcel::handlePreviewSetMetronome(payload, engine, projectState);
    }
    else if (type == "PREVIEW_SET_BRAKE")
    {
        stemcel::handlePreviewSetBrake(payload, engine, projectState);
    }
    else if (type == "PREVIEW_SET_BACKSPIN")
    {
        stemcel::handlePreviewSetBackspin(payload, engine, projectState);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchScratch(const DispatchContext& ctx)
{
    if (ctx.type == "SCRATCH_SESSION_OPEN")
    {
        handleScratchSessionOpen(ctx.payload, ctx.engine, ctx.projectState, ctx.bridge,
                                 ctx.peakPool, ctx.cache, ctx.session.currentPath);
    }
    else if (ctx.type == "SCRATCH_SESSION_CLOSE")
    {
        handleScratchSessionClose(ctx.payload, ctx.engine, ctx.bridge);
    }
    else if (ctx.type == "SCRATCH_SESSION_CONTROL")
    {
        handleScratchSessionControl(ctx.payload, ctx.engine, ctx.bridge);
    }
    else if (ctx.type == "SCRATCH_BACKING_PREPARE")
    {
        handleScratchBackingPrepare(ctx.payload, ctx.engine, ctx.projectState, ctx.bridge,
                                    ctx.peakPool);
    }
    else if (ctx.type == "SCRATCH_BACKING_CLEAR")
    {
        handleScratchBackingClear(ctx.payload, ctx.engine, ctx.bridge);
    }
    else if (ctx.type == "SCRATCH_PATTERN_SAVE")
    {
        handleScratchPatternSave(ctx.payload, ctx.projectState, ctx.bridge, ctx.session);
    }
    else if (ctx.type == "SCRATCH_SAVE_AS_SAMPLE")
    {
        handleScratchSaveAsSample(ctx.payload, ctx.engine, ctx.projectState, ctx.bridge,
                                  ctx.peakPool, ctx.cache, ctx.session);
    }
    else if (ctx.type == "SCRATCH_PATTERN_DELETE")
    {
        handleScratchPatternDelete(ctx.payload, ctx.projectState, ctx.bridge, ctx.session);
    }
    else if (ctx.type == "SCRATCH_PATTERN_RENAME")
    {
        handleScratchPatternRename(ctx.payload, ctx.projectState, ctx.bridge, ctx.session);
    }
    else if (ctx.type == "SCRATCH_PATTERN_APPLY")
    {
        handleScratchPatternApply(ctx.payload, ctx.projectState, ctx.engine, ctx.bridge, ctx.session);
    }
    else if (ctx.type == "SCRATCH_PATTERN_REMOVE")
    {
        handleScratchPatternRemove(ctx.payload, ctx.projectState, ctx.engine, ctx.bridge, ctx.session);
    }
    else if (ctx.type == "SCRATCH_PATTERN_REPLAY_START")
    {
        handleScratchPatternReplayStart(ctx.payload, ctx.engine, ctx.projectState, ctx.bridge);
    }
    else if (ctx.type == "SCRATCH_PATTERN_REPLAY_STOP")
    {
        handleScratchPatternReplayStop(ctx.payload, ctx.engine, ctx.bridge);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchRecording(const DispatchContext& ctx)
{
    if (ctx.type == "RECORD_INPUTS_REQUEST")
    {
        handleRecordInputsRequest(ctx.payload, ctx.bridge);
    }
    else if (ctx.type == "RECORD_SESSION_OPEN")
    {
        handleRecordSessionOpen(ctx.payload, ctx.engine, ctx.projectState, ctx.bridge, ctx.peakPool,
                                ctx.cache, ctx.session);
    }
    else if (ctx.type == "RECORD_SESSION_CONTROL")
    {
        handleRecordSessionControl(ctx.payload, ctx.projectState, ctx.bridge);
    }
    else if (ctx.type == "RECORD_SESSION_CLOSE")
    {
        handleRecordSessionClose(ctx.payload, ctx.bridge);
    }
    else if (ctx.type == "RECORD_RECORDING_SET_STEREO")
    {
        handleRecordRecordingSetStereo(ctx.payload, ctx.engine, ctx.bridge, ctx.cache);
    }
    else if (ctx.type == "RECORD_CALIBRATE_START")
    {
        handleRecordCalibrateStart(ctx.payload, ctx.engine, ctx.bridge, ctx.peakPool);
    }
    else if (ctx.type == "RECORD_CALIBRATE_CANCEL")
    {
        handleRecordCalibrateCancel(ctx.payload, ctx.bridge);
    }
    else if (ctx.type == "RECORD_RECORDING_COMMIT")
    {
        handleRecordRecordingCommit(ctx.payload, ctx.engine, ctx.projectState, ctx.bridge,
                                    ctx.peakPool, ctx.cache, ctx.decodedCache, ctx.peakJobs,
                                    ctx.session);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchTrack(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;

    if (type == "TRACK_ADD")
    {
        stemcel::log::info("bridge", "recv TRACK_ADD trackId=" + payload.getProperty("trackId", "").toString());
        stemcel::handleTrackAdd(payload, projectState, bridge);
    }
    else if (type == "TRACK_REMOVE")
    {
        stemcel::log::info("bridge", "recv TRACK_REMOVE trackId=" + payload.getProperty("trackId", "").toString());
        stemcel::handleTrackRemove(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_RENAME")
    {
        stemcel::log::info("bridge", "recv TRACK_RENAME trackId=" + payload.getProperty("trackId", "").toString());
        stemcel::handleTrackRename(payload, projectState);
    }
    else if (type == "TRACK_GAIN")
    {
        stemcel::log::debug("bridge", "recv TRACK_GAIN trackId=" + payload.getProperty("trackId", "").toString() +
                                            " gain=" + payload.getProperty("gain", "").toString());
        stemcel::handleTrackGain(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_MUTE")
    {
        stemcel::log::info("bridge", "recv TRACK_MUTE trackId=" + payload.getProperty("trackId", "").toString() +
                                            " muted=" + payload.getProperty("muted", "").toString());
        stemcel::handleTrackMute(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_SOLO")
    {
        stemcel::log::info("bridge", "recv TRACK_SOLO trackId=" + payload.getProperty("trackId", "").toString() +
                                            " soloed=" + payload.getProperty("soloed", "").toString());
        stemcel::handleTrackSolo(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_SET_COLOR")
    {
        const auto trackId = tryGetRequiredString(payload, "trackId").value_or(juce::String{});
        const auto colorVar = tryGetNumber(payload, "colorIndex");
        if (trackId.isNotEmpty() && colorVar.has_value())
        {
            projectState.setTrackColorIndex(trackId, static_cast<int>(*colorVar));
        }
    }
    else if (type == "TRACK_SET_HEIGHT")
    {
        const auto trackId = tryGetRequiredString(payload, "trackId").value_or(juce::String{});
        const auto heightVar = tryGetNumber(payload, "heightPx");
        stemcel::log::debug("bridge", "recv TRACK_SET_HEIGHT trackId=" + trackId +
                                            " heightPx=" + payload.getProperty("heightPx", "").toString());
        if (trackId.isNotEmpty() && heightVar.has_value())
        {
            projectState.setTrackHeightPx(trackId, *heightVar);
        }
    }
    else if (type == "TRACK_SET_AUTOMATION_LANE_VIEW")
    {
        const auto trackId = tryGetRequiredString(payload, "trackId").value_or(juce::String{});
        const auto& lanes = payload.getProperty("lanes", juce::var{});
        if (trackId.isNotEmpty() && lanes.isArray())
        {
            projectState.setTrackAutomationLaneView(trackId, *lanes.getArray());
        }
    }
    else if (type == "TRACK_REORDER")
    {
        const auto trackId = tryGetRequiredString(payload, "trackId").value_or(juce::String{});
        const auto idxVar = tryGetNumber(payload, "newIndex");
        stemcel::log::info("bridge", "recv TRACK_REORDER trackId=" + trackId +
                                           " newIndex=" + payload.getProperty("newIndex", "").toString());
        if (trackId.isNotEmpty() && idxVar.has_value())
        {
            projectState.moveTrack(trackId, static_cast<int>(*idxVar));
        }
    }
    else if (type == "TRACK_SET_SENDS")
    {
        stemcel::log::debug("bridge",
                              "recv TRACK_SET_SENDS trackId=" +
                                  payload.getProperty("trackId", "").toString() +
                                  " rev=" + payload.getProperty("reverbSend", "").toString() +
                                  " dly=" + payload.getProperty("delaySend", "").toString());
        stemcel::handleTrackSetSends(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_SET_TONE")
    {
        stemcel::log::debug("bridge", "recv TRACK_SET_TONE trackId=" +
                                            payload.getProperty("trackId", "").toString());
        stemcel::handleTrackSetTone(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_SET_LEVELER")
    {
        stemcel::log::debug("bridge", "recv TRACK_SET_LEVELER trackId=" +
                                            payload.getProperty("trackId", "").toString() +
                                            " amount=" + payload.getProperty("amount", "").toString());
        stemcel::handleTrackSetLeveler(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_SET_PUNCH")
    {
        stemcel::log::debug("bridge", "recv TRACK_SET_PUNCH trackId=" +
                                            payload.getProperty("trackId", "").toString() +
                                            " amount=" + payload.getProperty("amount", "").toString());
        stemcel::handleTrackSetPunch(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_SET_SATURATION")
    {
        stemcel::log::debug("bridge", "recv TRACK_SET_SATURATION trackId=" +
                                            payload.getProperty("trackId", "").toString());
        stemcel::handleTrackSetSaturation(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_SET_BIT_CRUSHER")
    {
        stemcel::log::debug("bridge", "recv TRACK_SET_BIT_CRUSHER trackId=" +
                                            payload.getProperty("trackId", "").toString());
        stemcel::handleTrackSetBitCrusher(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_SET_PAN")
    {
        stemcel::log::debug("bridge", "recv TRACK_SET_PAN trackId=" +
                                            payload.getProperty("trackId", "").toString() +
                                            " pan=" + payload.getProperty("pan", "").toString());
        stemcel::handleTrackSetPan(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_SET_AUTOMATION")
    {
        stemcel::log::debug("bridge", "recv TRACK_SET_AUTOMATION trackId=" +
                                            payload.getProperty("trackId", "").toString() +
                                            " paramId=" + payload.getProperty("paramId", "").toString());
        stemcel::handleTrackSetAutomation(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_BEAT_REPEAT_ADD")
    {
        stemcel::applyBeatRepeatAdd(payload, projectState);
        stemcel::finishBeatRepeatEdit(engine, projectState, bridge, ctx.session);
    }
    else if (type == "TRACK_BEAT_REPEAT_DELETE")
    {
        stemcel::applyBeatRepeatDelete(payload, projectState);
        stemcel::finishBeatRepeatEdit(engine, projectState, bridge, ctx.session);
    }
    else if (type == "PLUGIN_LIST_REQUEST")
    {
        stemcel::handlePluginListRequest(engine, bridge);
    }
    else if (type == "PLUGIN_SCAN")
    {
        stemcel::handlePluginScan(payload, engine, bridge);
    }
    else if (type == "TRACK_ADD_PLUGIN")
    {
        stemcel::handleTrackAddPlugin(payload, engine, projectState, bridge, ctx.session);
    }
    else if (type == "TRACK_REMOVE_PLUGIN")
    {
        stemcel::handleTrackRemovePlugin(payload, engine, projectState, bridge, ctx.session);
    }
    else if (type == "TRACK_REORDER_PLUGIN")
    {
        stemcel::handleTrackReorderPlugin(payload, engine, projectState, bridge, ctx.session);
    }
    else if (type == "TRACK_SET_PLUGIN_BYPASS")
    {
        stemcel::handleTrackSetPluginBypass(payload, engine, projectState, bridge);
    }
    else if (type == "TRACK_OPEN_PLUGIN_EDITOR")
    {
        stemcel::handleTrackOpenPluginEditor(payload, engine, projectState, bridge);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchProjectFx(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;

    if (type == "PROJECT_SET_REVERB")
    {
        stemcel::log::debug("bridge", "recv PROJECT_SET_REVERB");
        stemcel::handleProjectSetReverb(payload, engine, projectState, bridge);
    }
    else if (type == "PROJECT_SET_DELAY")
    {
        stemcel::log::debug("bridge", "recv PROJECT_SET_DELAY");
        stemcel::handleProjectSetDelay(payload, engine, projectState, bridge);
    }
    else if (type == "PROJECT_SET_MIX_GLUE")
    {
        stemcel::log::debug("bridge", "recv PROJECT_SET_MIX_GLUE");
        stemcel::handleProjectSetMixGlue(payload, engine, projectState, bridge);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchWaveform(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;
    auto& peakPool = ctx.peakPool;
    const auto& cache = ctx.cache;
    const auto& decodedCache = ctx.decodedCache;
    auto& peakJobs = ctx.peakJobs;

    if (type == "WAVEFORM_REQUEST")
    {
        stemcel::log::debug("bridge", "recv WAVEFORM_REQUEST clipId=" + payload.getProperty("clipId", "").toString());
        stemcel::handleWaveformRequest(payload, engine, projectState, bridge, peakPool, cache, decodedCache,
                                         peakJobs);
    }
    else if (type == "CLIP_EDITOR_PEAKS_REQUEST")
    {
        stemcel::log::debug("bridge",
                              "recv CLIP_EDITOR_PEAKS_REQUEST libId=" +
                                  payload.getProperty("libraryItemId", "").toString() +
                                  " ppS=" + payload.getProperty("peaksPerSecond", "").toString());
        stemcel::handleClipEditorPeaksRequest(payload, engine, projectState, bridge, peakPool, cache, decodedCache,
                                                peakJobs);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchProject(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;
    auto& peakPool = ctx.peakPool;
    const auto& decodedCache = ctx.decodedCache;
    auto& session = ctx.session;

    if (type == "PROJECT_NEW")
    {
        stemcel::log::info("bridge", "recv PROJECT_NEW");
        stemcel::handleProjectNew(engine, projectState, bridge, session);
    }
    else if (type == "PROJECT_SAVE")
    {
        stemcel::log::info("bridge", "recv PROJECT_SAVE");
        stemcel::handleProjectSave(payload, engine, projectState, bridge, session, /*isSaveAs*/ false,
                                     peakPool, decodedCache);
    }
    else if (type == "PROJECT_SAVE_AS")
    {
        stemcel::log::info("bridge", "recv PROJECT_SAVE_AS path=" + payload.getProperty("filePath", "").toString());
        stemcel::handleProjectSave(payload, engine, projectState, bridge, session, /*isSaveAs*/ true,
                                     peakPool, decodedCache);
    }
    else if (type == "PROJECT_SAVE_VIEW_STATE")
    {
        stemcel::log::info("bridge", "recv PROJECT_SAVE_VIEW_STATE");
        stemcel::handleProjectSaveViewState(payload, engine, projectState, bridge, session);
    }
    else if (type == "PROJECT_LOAD")
    {
        stemcel::log::info("bridge", "recv PROJECT_LOAD path=" + payload.getProperty("filePath", "").toString());
        stemcel::handleProjectLoad(payload, engine, projectState, bridge, session, peakPool, decodedCache);
    }
    else if (type == "PROJECT_LOAD_RECOVERY")
    {
        stemcel::log::info("bridge", "recv PROJECT_LOAD_RECOVERY autosavePath=" +
                                           payload.getProperty("autosavePath", "").toString());
        stemcel::handleProjectLoadRecovery(payload, engine, projectState, bridge, session, peakPool, decodedCache);
    }
    else if (type == "PROJECT_IMPORT_SOURCE_INSPECT")
    {
        stemcel::handleProjectImportSourceInspect(payload, projectState, bridge, session);
    }
    else if (type == "PROJECT_IMPORT_ASSETS")
    {
        stemcel::handleProjectImportAssets(payload, projectState, bridge, session);
    }
    else if (type == "PROJECT_AUTOSAVE")
    {
        stemcel::log::debug("bridge", "recv PROJECT_AUTOSAVE path=" +
                                            payload.getProperty("filePath", "").toString());
        stemcel::handleProjectAutosave(payload, engine, projectState, bridge);
    }
    else if (type == "PROJECT_RENAME")
    {
        stemcel::log::info("bridge", "recv PROJECT_RENAME name=" + payload.getProperty("name", "").toString());
        stemcel::handleProjectRename(payload, projectState, bridge);
    }
    else if (type == "PROJECT_SET_VIEW")
    {
        stemcel::handleProjectSetView(payload, engine, projectState);
    }
    else if (type == "PROJECT_SET_BPM")
    {
        stemcel::handleProjectSetBpm(payload, engine, projectState, bridge);
    }
    else if (type == "TEMPO_MAP_SET")
    {
        stemcel::applyTempoMapSet(payload, projectState);
    }
    else if (type == "MIX_TRANSITION_SET")
    {
        stemcel::applyMixTransitionSet(payload, projectState);
    }
    else if (type == "MIX_TRANSITION_REMOVE")
    {
        stemcel::applyMixTransitionRemove(payload, projectState);
    }
    else if (type == "PROJECT_SET_LENGTH")
    {
        stemcel::handleProjectSetLength(payload, projectState);
    }
    else if (type == "PROJECT_SET_AUDIO_OUTPUT")
    {
        stemcel::handleProjectSetAudioOutput(payload, projectState);
    }
    else if (type == "PROJECT_SET_TARGET_SAMPLE_RATE")
    {
        stemcel::handleProjectSetTargetSampleRate(payload, projectState);
    }
    else if (type == "PROJECT_SET_EXPORT_SETTINGS")
    {
        stemcel::handleProjectSetExportSettings(payload, projectState);
    }
    else if (type == "PROJECT_SET_MASTER_VOLUME")
    {
        stemcel::handleProjectSetMasterVolume(payload, engine, projectState);
    }
    else if (type == "PROJECT_SET_SAFETY_LIMITER")
    {
        stemcel::handleProjectSetSafetyLimiter(payload, engine, projectState);
    }
    else if (type == "PROJECT_SET_BAR_COUNTER_START")
    {
        stemcel::handleProjectSetBarCounterStart(payload, projectState);
    }
    else if (type == "PROJECT_SET_MIXDOWN_START_BAR")
    {
        stemcel::handleProjectSetMixdownStartBar(payload, projectState);
    }
    else if (type == "PROJECT_SET_METRONOME")
    {
        stemcel::handleProjectSetMetronome(payload, engine, projectState);
    }
    else if (type == "PROJECT_SET_SEED_TEMPO_PREF")
    {
        stemcel::handleSetSeedProjectTempoPref(payload, projectState);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchMarker(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& projectState = ctx.projectState;

    if (type == "PROJECT_MARKER_ADD")
    {
        stemcel::applyMarkerAdd(payload, projectState);
    }
    else if (type == "PROJECT_MARKER_MOVE")
    {
        stemcel::applyMarkerMove(payload, projectState);
    }
    else if (type == "PROJECT_MARKER_REMOVE")
    {
        stemcel::applyMarkerRemove(payload, projectState);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchAudioDevice(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& bridge = ctx.bridge;
    auto& peakPool = ctx.peakPool;

    if (type == "AUDIO_DEVICES_REQUEST")
    {
        stemcel::log::debug("bridge", "recv AUDIO_DEVICES_REQUEST refresh=" +
                                            payload.getProperty("refresh", "false").toString());
        stemcel::handleAudioDevicesRequest(payload, engine, bridge);
    }
    else if (type == "AUDIO_DEVICE_SELECT")
    {
        stemcel::log::info("bridge", "recv AUDIO_DEVICE_SELECT type=" +
                                           payload.getProperty("typeName", "").toString() + " name=" +
                                           payload.getProperty("deviceName", "").toString());
        stemcel::handleAudioDeviceSelect(payload, engine, bridge);
    }
    else if (type == "AUDIO_KEEP_AWAKE_SET")
    {
        stemcel::log::info("bridge", "recv AUDIO_KEEP_AWAKE_SET enabled=" +
                                           payload.getProperty("enabled", false).toString());
        stemcel::handleAudioKeepAwakeSet(payload, engine);
    }
    else if (type == "BRAKE_SETTINGS_SET")
    {
        stemcel::handleSetBrakeSettings(payload, engine);
    }
    else if (type == "BACKSPIN_SETTINGS_SET")
    {
        stemcel::handleSetBackspinSettings(payload, engine);
    }
    else if (type == "SCRATCH_REALISM_SET")
    {
        stemcel::handleScratchRealismSet(payload, engine);
    }
    else if (type == "AUDIO_FILE_PROBE")
    {
        stemcel::handleAudioFileProbe(payload, engine, bridge, peakPool, ctx.decodedCache);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchMidi(const DispatchContext& ctx)
{
    stemcel::setMidiScratchEngine(ctx.engine);
    if (ctx.type == "MIDI_DEVICES_REQUEST")
    {
        stemcel::log::debug("bridge", "recv MIDI_DEVICES_REQUEST");
        stemcel::handleMidiDevicesRequest(ctx.bridge);
        return true;
    }
    if (ctx.type == "MIDI_INPUTS_SET")
    {
        stemcel::handleMidiInputsSet(ctx.payload, ctx.bridge);
        return true;
    }
    if (ctx.type == "MIDI_DECK_SELECTION_SET")
    {
        stemcel::handleMidiDeckSelectionSet(ctx.payload, ctx.bridge);
        return true;
    }
    if (ctx.type == "MIDI_SCRATCH_SETTINGS_SET")
    {
        stemcel::handleMidiScratchSettingsSet(ctx.payload);
        return true;
    }
    return false;
}

bool dispatchMixdown(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;
    auto& peakPool = ctx.peakPool;
    const auto& decodedCache = ctx.decodedCache;

    if (type == "MIXDOWN_START")
    {
        stemcel::handleMixdownStart(payload, engine, projectState, bridge, peakPool, decodedCache,
                                      g_mixdownBusy, g_mixdownCancel);
    }
    else if (type == "MIXDOWN_CANCEL")
    {
        stemcel::handleMixdownCancel(g_mixdownBusy, g_mixdownCancel);
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchUndo(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;
    auto& peakPool = ctx.peakPool;
    const auto& decodedCache = ctx.decodedCache;
    auto& session = ctx.session;

    if (type == "EDIT_UNDO")
    {
        stemcel::log::info("bridge", "recv EDIT_UNDO");
        stemcel::handleEditUndo(engine, projectState, bridge, session, peakPool, decodedCache);
    }
    else if (type == "EDIT_REDO")
    {
        stemcel::log::info("bridge", "recv EDIT_REDO");
        stemcel::handleEditRedo(engine, projectState, bridge, session, peakPool, decodedCache);
    }
    else if (type == "EDIT_GROUP_BEGIN")
    {
        const auto label = stemcel::bridge::readOptionalString(ctx.payload, "label")
                               .value_or(juce::String{});
        stemcel::log::info("bridge", "recv EDIT_GROUP_BEGIN label=" + label);
        stemcel::beginUndoGroup(label, projectState);
    }
    else if (type == "EDIT_GROUP_END")
    {
        stemcel::log::info("bridge", "recv EDIT_GROUP_END");
        stemcel::endUndoGroup();
    }
    else
    {
        return false;
    }
    return true;
}

bool dispatchTransition(const DispatchContext& ctx)
{
    const auto& type = ctx.type;
    const auto& payload = ctx.payload;
    auto& engine = ctx.engine;
    auto& projectState = ctx.projectState;
    auto& bridge = ctx.bridge;
    auto& session = ctx.session;

    if (type == "TRANSITION_CREATE")
    {
        stemcel::log::info("bridge", "recv TRANSITION_CREATE track=" +
                                           payload.getProperty("trackId", "").toString());
        stemcel::applyTransitionCreate(payload, projectState);
        stemcel::finishTransitionEdit(engine, projectState, bridge, session);
    }
    else if (type == "TRANSITION_DELETE")
    {
        stemcel::log::info("bridge", "recv TRANSITION_DELETE id=" +
                                           payload.getProperty("transitionId", "").toString());
        stemcel::applyTransitionDelete(payload, projectState);
        stemcel::finishTransitionEdit(engine, projectState, bridge, session);
    }
    else if (type == "TRANSITION_SET_RECIPE")
    {
        stemcel::log::info("bridge", "recv TRANSITION_SET_RECIPE id=" +
                                           payload.getProperty("transitionId", "").toString());
        stemcel::applyTransitionSetRecipe(payload, projectState);
        stemcel::finishTransitionEdit(engine, projectState, bridge, session);
    }
    else
    {
        return false;
    }
    return true;
}
} // namespace

// Wire-protocol order is fixed as (type, payload).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void dispatchBridgeMessage(const juce::String& type, const juce::var& payload, stemcel::AudioEngine& engine,
                           stemcel::ProjectState& projectState, stemcel::BridgeServer& bridge,
                           juce::ThreadPool& peakPool, const stemcel::PeaksCache& cache,
                           const stemcel::DecodedCache& decodedCache, stemcel::PeakJobCoordinator& peakJobs,
                           stemcel::ProjectSession& session)
{
    // Answer on the message thread so PING proves command-thread responsiveness.
    if (type == "PING")
    {
        auto* p = new juce::DynamicObject();
        p->setProperty("id", payload.getProperty("id", 0));
        bridge.broadcast("PONG", juce::var(p));
        return;
    }

    // Mutations get one undo transaction; high-rate drag streams coalesce by target.
    stemcel::beginUndoTransactionIfNeeded(type, payload, projectState);

    // Counted before the handler runs so the reconcile pass below can see removals the
    // handler made itself. Only for the types that can move transition geometry, so the
    // tree walk stays off the path of every other message.
    const bool mayChangeTransitions = stemcel::transitionGeometryMayHaveChanged(type);
    const int transitionsBeforeDispatch = mayChangeTransitions ? projectState.countTransitions() : 0;

    // Route to the first domain that owns the type. Type strings are unique, so
    // the chaining order only affects readability, not behaviour.
    const DispatchContext ctx{type, payload, engine, projectState, bridge, peakPool,
                              cache, decodedCache, peakJobs, session};
    const bool handled = dispatchClip(ctx) || dispatchLibrary(ctx) || dispatchTransport(ctx) ||
                         dispatchPreview(ctx) || dispatchScratch(ctx) || dispatchRecording(ctx) ||
                         dispatchTrack(ctx) || dispatchProjectFx(ctx) ||
                         dispatchWaveform(ctx) || dispatchProject(ctx) || dispatchMarker(ctx) ||
                         dispatchAudioDevice(ctx) || dispatchMidi(ctx) || dispatchMixdown(ctx) ||
                         dispatchUndo(ctx) || dispatchTransition(ctx);
    if (!handled)
    {
        stemcel::log::warn("bridge", "unhandled message type: " + type);
    }

    // Run after mutation so terminal gesture events fold into the open transaction.
    stemcel::endUndoTransactionIfNeeded(type, payload);

    // Geometry edits can invalidate transition overlaps; reconcile inside the same undo step.
    if (mayChangeTransitions)
    {
        stemcel::reconcileTransitionsAfterGeometryEdit(engine, projectState, bridge, session,
                                                        transitionsBeforeDispatch);
    }

    // Whatever the command was, the engine's tempo-map followers match project state after it.
    stemcel::syncTempoMap(engine, projectState);

    // Mutations and project replacement can change undo/redo menu state.
    if (stemcel::isUndoableEnvelopeType(type) || type == "EDIT_UNDO" || type == "EDIT_REDO" ||
        type == "PROJECT_NEW" || type == "PROJECT_LOAD" || type == "PROJECT_LOAD_RECOVERY")
    {
        stemcel::broadcastEditUndoState(projectState, bridge);
    }
}

} // namespace stemcel
