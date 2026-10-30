#pragma once

#include <juce_core/juce_core.h>

#include <onnxruntime_cxx_api.h>

#include <string>

namespace stemcel
{

// ONNX Runtime takes model paths as ORTCHAR_T, which is wchar_t on Windows and UTF-8 char
// everywhere else. Keep the returned string alive for as long as the pointer is used.
inline std::basic_string<ORTCHAR_T> toOrtPath(const juce::String& path)
{
#ifdef _WIN32
    return path.toWideCharPointer();
#else
    return path.toStdString();
#endif
}

} // namespace stemcel
