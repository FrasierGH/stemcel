// Backend-side deletion of a removed library item's generated files (stems, samples,
// split channels, recordings and baked scratches). Every path is confined to the
// project's own artifact trees, and a per-source folder is pruned once its last file is
// gone — so a user's original imported source can never be removed.

#include "TestRegistry.h"

#include "AudioEngine.h"
#include "LibraryCommands.h"
#include "ProjectSession.h"

#include <juce_core/juce_core.h>

namespace stemcel::tests
{
namespace
{

// Build a `{ paths: [...] }` envelope payload for handleLibraryDeleteArtifacts.
juce::var pathsPayload(const juce::StringArray& paths)
{
    juce::Array<juce::var> arr;
    for (const auto& p : paths) arr.add(p);
    auto* obj = new juce::DynamicObject();
    obj->setProperty("paths", juce::var(arr));
    return juce::var(obj);
}

void writeStub(const juce::File& f)
{
    f.getParentDirectory().createDirectory();
    const auto created = f.create();
    require(created.wasOk(), "failed to create stub file");
    require(f.existsAsFile(), "stub file should exist after creation");
}

stemcel::ProjectSession sessionFor(const juce::File& projectDir)
{
    stemcel::ProjectSession session;
    // The artifact roots derive from the project FILE's parent directory.
    session.currentPath = projectDir.getChildFile("proj.stemcel").getFullPathName();
    return session;
}

void testDeleteArtifactsPrunesEmptiedFolder()
{
    // Each test owns its engine (no preview loaded, so releaseReadersForFile is a no-op):
    // a shared static would outlive the harness's JUCE initialiser, which crashes on macOS.
    stemcel::AudioEngine engine;
    const auto projectDir = makeTempDir("cleanup-prune");
    const auto session = sessionFor(projectDir);

    const auto stemsRoot = projectDir.getChildFile("stems");
    const auto sourceDir = stemsRoot.getChildFile("Song-stems");
    const auto vocals = sourceDir.getChildFile("vocals.wav");
    const auto drums = sourceDir.getChildFile("drums.wav");
    writeStub(vocals);
    writeStub(drums);

    // First delete: the file is gone but the folder is kept (still holds a stem).
    stemcel::handleLibraryDeleteArtifacts(pathsPayload({vocals.getFullPathName()}), session, engine);
    require(! vocals.existsAsFile(), "vocals.wav should be deleted");
    require(sourceDir.isDirectory(), "per-source folder kept while it still holds a stem");

    // Last delete: the file is gone AND the now-empty folder is pruned; root survives.
    stemcel::handleLibraryDeleteArtifacts(pathsPayload({drums.getFullPathName()}), session, engine);
    require(! drums.existsAsFile(), "drums.wav should be deleted");
    require(! sourceDir.exists(), "emptied per-source folder should be pruned");
    require(stemsRoot.isDirectory(), "the stems root itself must never be removed");

    projectDir.deleteRecursively();
}

void testDeleteArtifactsPrunesWhenAllFilesGoInOneCall()
{
    // Each test owns its engine (no preview loaded, so releaseReadersForFile is a no-op):
    // a shared static would outlive the harness's JUCE initialiser, which crashes on macOS.
    stemcel::AudioEngine engine;
    const auto projectDir = makeTempDir("cleanup-batch");
    const auto session = sessionFor(projectDir);

    const auto samplesRoot = projectDir.getChildFile("samples");
    const auto sourceDir = samplesRoot.getChildFile("My Song");
    const auto a = sourceDir.getChildFile("My Song-sample-001.wav");
    const auto b = sourceDir.getChildFile("My Song-sample-002.wav");
    writeStub(a);
    writeStub(b);

    stemcel::handleLibraryDeleteArtifacts(pathsPayload({a.getFullPathName(), b.getFullPathName()}), session, engine);
    require(! sourceDir.exists(), "folder pruned when its last files are deleted together");
    require(samplesRoot.isDirectory(), "the samples root itself must never be removed");

    projectDir.deleteRecursively();
}

void testDeleteArtifactsKeepsFolderWithForeignFile()
{
    // Each test owns its engine (no preview loaded, so releaseReadersForFile is a no-op):
    // a shared static would outlive the harness's JUCE initialiser, which crashes on macOS.
    stemcel::AudioEngine engine;
    const auto projectDir = makeTempDir("cleanup-foreign");
    const auto session = sessionFor(projectDir);

    const auto sourceDir = projectDir.getChildFile("samples").getChildFile("Mixed");
    const auto ours = sourceDir.getChildFile("ours-sample-001.wav");
    const auto foreign = sourceDir.getChildFile("user-notes.txt");
    writeStub(ours);
    writeStub(foreign);

    stemcel::handleLibraryDeleteArtifacts(pathsPayload({ours.getFullPathName()}), session, engine);
    require(! ours.existsAsFile(), "our sample WAV should be deleted");
    require(foreign.existsAsFile(), "an unrelated file must be preserved");
    require(sourceDir.isDirectory(), "folder kept while it still holds a foreign file");

    projectDir.deleteRecursively();
}

void testDeleteArtifactsRemovesReadOnlyFolder()
{
    // Each test owns its engine (no preview loaded, so releaseReadersForFile is a no-op):
    // a shared static would outlive the harness's JUCE initialiser, which crashes on macOS.
    stemcel::AudioEngine engine;
    const auto projectDir = makeTempDir("cleanup-readonly");
    const auto session = sessionFor(projectDir);

    // A sync client (OneDrive) stamps the per-source folder READ-ONLY, which makes
    // Windows deny RemoveDirectory. The handler must clear that before removing it.
    const auto sourceDir = projectDir.getChildFile("samples").getChildFile("Synced Song");
    const auto wav = sourceDir.getChildFile("Synced Song-sample-001.wav");
    writeStub(wav);
    sourceDir.setReadOnly(true, /*applyRecursively*/ false);

    stemcel::handleLibraryDeleteArtifacts(pathsPayload({wav.getFullPathName()}), session, engine);
    require(! wav.existsAsFile(), "the sample WAV should be deleted");
    require(! sourceDir.exists(), "a read-only per-source folder must still be removed");

    projectDir.deleteRecursively();
}

void testDeleteArtifactsRefusesOutsideRoots()
{
    // Each test owns its engine (no preview loaded, so releaseReadersForFile is a no-op):
    // a shared static would outlive the harness's JUCE initialiser, which crashes on macOS.
    stemcel::AudioEngine engine;
    const auto projectDir = makeTempDir("cleanup-outside");
    const auto session = sessionFor(projectDir);

    // A user's original import living beside the project, NOT under stems/samples.
    const auto original = projectDir.getChildFile("original.wav");
    writeStub(original);

    stemcel::handleLibraryDeleteArtifacts(pathsPayload({original.getFullPathName()}), session, engine);
    require(original.existsAsFile(), "a path outside the artifact roots must never be deleted");

    projectDir.deleteRecursively();
}

void testDeleteArtifactsPrunesChannelsFolder()
{
    // Each test owns its engine (no preview loaded, so releaseReadersForFile is a no-op):
    // a shared static would outlive the harness's JUCE initialiser, which crashes on macOS.
    stemcel::AudioEngine engine;
    const auto projectDir = makeTempDir("cleanup-channels");
    const auto session = sessionFor(projectDir);

    // Split-channel WAVs live under a per-source folder of the `channels` root and are
    // owned artifacts (reused 'stem' kind), so removing their library items must delete
    // them and prune the emptied folder — exactly like stems/samples.
    const auto channelsRoot = projectDir.getChildFile("channels");
    const auto sourceDir = channelsRoot.getChildFile("Song");
    const auto left = sourceDir.getChildFile("Song (L)-sample-001.wav");
    const auto right = sourceDir.getChildFile("Song (R)-sample-001.wav");
    writeStub(left);
    writeStub(right);

    stemcel::handleLibraryDeleteArtifacts(
        pathsPayload({left.getFullPathName(), right.getFullPathName()}), session, engine);
    require(! left.existsAsFile(), "split-channel WAV (L) should be deleted");
    require(! right.existsAsFile(), "split-channel WAV (R) should be deleted");
    require(! sourceDir.exists(), "emptied channels per-source folder should be pruned");
    require(channelsRoot.isDirectory(), "the channels root itself must never be removed");

    projectDir.deleteRecursively();
}

// A recording is a sample-kind library item whose WAV sits directly under the
// `recordings` root (ADR 0030) rather than in a per-source folder. Removing the item
// must still take the file with it — the root itself is never removed.
void testDeleteArtifactsDeletesRecording()
{
    // Each test owns its engine (no preview loaded, so releaseReadersForFile is a no-op):
    // a shared static would outlive the harness's JUCE initialiser, which crashes on macOS.
    stemcel::AudioEngine engine;
    const auto projectDir = makeTempDir("cleanup-recording");
    const auto session = sessionFor(projectDir);

    const auto recordingsRoot = projectDir.getChildFile("recordings");
    const auto take = recordingsRoot.getChildFile("Take 1-recording-001.wav");
    writeStub(take);

    stemcel::handleLibraryDeleteArtifacts(pathsPayload({take.getFullPathName()}), session, engine);
    require(! take.existsAsFile(), "a recording WAV should be deleted with its library item");
    require(recordingsRoot.isDirectory(), "the recordings root itself must never be removed");

    projectDir.deleteRecursively();
}

// A baked scratch is a sample-kind item in `scratches/<patternId>/`. Removing it must
// delete the bake, but `source.wav` — the snapshot that lets the pattern be re-edited
// after its original source is gone — belongs to the pattern, not the bake, and must
// survive along with its folder.
void testDeleteArtifactsDeletesBakedScratchButKeepsItsSource()
{
    // Each test owns its engine (no preview loaded, so releaseReadersForFile is a no-op):
    // a shared static would outlive the harness's JUCE initialiser, which crashes on macOS.
    stemcel::AudioEngine engine;
    const auto projectDir = makeTempDir("cleanup-scratch");
    const auto session = sessionFor(projectDir);

    const auto scratchesRoot = projectDir.getChildFile("scratches");
    const auto patternDir = scratchesRoot.getChildFile("pattern-1");
    const auto bake = patternDir.getChildFile("Scratch-sample-001.wav");
    const auto source = patternDir.getChildFile("source.wav");
    writeStub(bake);
    writeStub(source);

    stemcel::handleLibraryDeleteArtifacts(pathsPayload({bake.getFullPathName()}), session, engine);
    require(! bake.existsAsFile(), "a baked scratch WAV should be deleted with its library item");
    require(source.existsAsFile(), "the re-editable scratch source snapshot must be preserved");
    require(patternDir.isDirectory(), "the pattern folder is kept while it still holds source.wav");

    projectDir.deleteRecursively();
}

// With nothing else left in the pattern folder, it is pruned like any other per-source
// folder — the scratches root itself is never removed.
void testDeleteArtifactsPrunesEmptiedScratchFolder()
{
    // Each test owns its engine (no preview loaded, so releaseReadersForFile is a no-op):
    // a shared static would outlive the harness's JUCE initialiser, which crashes on macOS.
    stemcel::AudioEngine engine;
    const auto projectDir = makeTempDir("cleanup-scratch-prune");
    const auto session = sessionFor(projectDir);

    const auto scratchesRoot = projectDir.getChildFile("scratches");
    const auto patternDir = scratchesRoot.getChildFile("pattern-2");
    const auto bake = patternDir.getChildFile("Scratch-sample-001.wav");
    writeStub(bake);

    stemcel::handleLibraryDeleteArtifacts(pathsPayload({bake.getFullPathName()}), session, engine);
    require(! bake.existsAsFile(), "the baked scratch WAV should be deleted");
    require(! patternDir.exists(), "an emptied scratch pattern folder should be pruned");
    require(scratchesRoot.isDirectory(), "the scratches root itself must never be removed");

    projectDir.deleteRecursively();
}

} // namespace

void addLibraryCleanupTests(std::vector<TestCase>& tests)
{
    tests.push_back({"delete artifacts prunes an emptied per-source folder", testDeleteArtifactsPrunesEmptiedFolder});
    tests.push_back({"delete artifacts prunes when all files go in one call",
                     testDeleteArtifactsPrunesWhenAllFilesGoInOneCall});
    tests.push_back({"delete artifacts keeps a folder holding a foreign file",
                     testDeleteArtifactsKeepsFolderWithForeignFile});
    tests.push_back({"delete artifacts removes a read-only (synced) folder",
                     testDeleteArtifactsRemovesReadOnlyFolder});
    tests.push_back(
        {"delete artifacts refuses paths outside the artifact roots", testDeleteArtifactsRefusesOutsideRoots});
    tests.push_back(
        {"delete artifacts prunes an emptied channels folder", testDeleteArtifactsPrunesChannelsFolder});
    tests.push_back({"delete artifacts deletes a recording WAV", testDeleteArtifactsDeletesRecording});
    tests.push_back({"delete artifacts deletes a baked scratch but keeps its source snapshot",
                     testDeleteArtifactsDeletesBakedScratchButKeepsItsSource});
    tests.push_back({"delete artifacts prunes an emptied scratch pattern folder",
                     testDeleteArtifactsPrunesEmptiedScratchFolder});
}

} // namespace stemcel::tests
