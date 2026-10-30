#pragma once

#include "Log.h"

#include <juce_core/juce_core.h>
#include <onnxruntime_cxx_api.h>

namespace stemcel
{

// Route ONNX Runtime's internal logging into Stemcel's structured backend.log instead of the
// backend process stderr. By default ORT writes its logs (e.g. a Run() kernel failure) straight to
// stderr, which Electron's backend supervisor force-forwards as an alarming "WARN [backend-err]"
// line — even when the fault is one we recover from. The most common example is a DirectML GPU
// out-of-memory (E_OUTOFMEMORY / 8007000E) on a large transformer MatMul: both the RoFormer packs
// and OnnxStemSeparator catch that Ort::Exception and transparently retry the job on the CPU
// provider, so the separation still succeeds. Keeping ORT's own log off stderr removes the false
// alarm; genuinely unrecoverable failures still surface to the user via the thrown
// StemSeparationError (logged with context by the stem engine).
//
// Called from ORT's internal (possibly worker) threads; stemcel::log::write is thread-safe. Marked
// noexcept because it is invoked across a C ABI boundary where throwing would be undefined behaviour.
inline void ORT_API_CALL ortLogToStemcel(void* /*param*/, OrtLoggingLevel severity,
                                           const char* category, const char* /*logid*/,
                                           const char* codeLocation, const char* message) noexcept
{
    juce::String line;
    if (category != nullptr && category[0] != '\0') line << '[' << category << "] ";
    if (codeLocation != nullptr && codeLocation[0] != '\0') line << codeLocation << ' ';
    if (message != nullptr) line << message;

    // ORT reports recoverable DirectML faults (GPU OOM / device reset) at ERROR before we catch the
    // exception and retry on the CPU, so log routed ORT errors at INFO here rather than WARN/ERROR —
    // an actually-fatal separation failure is reported separately as a StemSeparationError.
    switch (severity)
    {
    case ORT_LOGGING_LEVEL_FATAL:
        stemcel::log::warn("onnx", line);
        break;
    case ORT_LOGGING_LEVEL_ERROR:
        stemcel::log::info("onnx", line);
        break;
    default:
        stemcel::log::debug("onnx", line);
        break;
    }
}

// An Ort::Env whose logging is forwarded to stemcel::log (see ortLogToStemcel) rather than
// stderr. The level stays at ERROR so only genuine ORT errors are captured; recoverable ones are
// downgraded inside the callback.
inline Ort::Env makeOrtEnv(const char* logId)
{
    return {ORT_LOGGING_LEVEL_ERROR, logId, &ortLogToStemcel, nullptr};
}

} // namespace stemcel
