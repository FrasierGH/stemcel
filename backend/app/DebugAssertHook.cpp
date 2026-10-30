// Debug builds on Windows: a CRT assertion (e.g. an empty std::optional dereferenced)
// normally opens a modal "Debug Assertion Failed" box, which blocks the message loop with
// no trace of where it came from. This hook writes the assertion and a symbolized stack to
// the diagnostics log instead, then lets the default report run.
#include "DebugAssertHook.h"

#include "Log.h"

#if JUCE_WINDOWS && JUCE_DEBUG
  #include <windows.h>
  #include <crtdbg.h>
  #include <dbghelp.h>
  #pragma comment(lib, "dbghelp.lib")
#endif

namespace stemcel::native
{

#if JUCE_WINDOWS && JUCE_DEBUG
namespace
{
juce::String stackTrace()
{
    void* frames[48] = {};
    const USHORT count = CaptureStackBackTrace(2, 48, frames, nullptr);
    const HANDLE process = GetCurrentProcess();
    static bool initialised = false;
    if (!initialised)
    {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        initialised = true;
    }
    juce::String out;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    for (USHORT i = 0; i < count; ++i)
    {
        const auto address = reinterpret_cast<DWORD64>(frames[i]);
        juce::String line = "  #" + juce::String(i) + " ";
        if (SymFromAddr(process, address, nullptr, symbol)) line << symbol->Name;
        else line << juce::String::toHexString(static_cast<juce::int64>(address));
        IMAGEHLP_LINE64 source = {};
        source.SizeOfStruct = sizeof(source);
        DWORD displacement = 0;
        if (SymGetLineFromAddr64(process, address, &displacement, &source))
            line << "  (" << juce::File(source.FileName).getFileName() << ":" << static_cast<int>(source.LineNumber) << ")";
        out << line << "\n";
    }
    return out;
}

int reportHook(int type, char* message, int* returnValue)
{
    if (type == _CRT_ASSERT || type == _CRT_ERROR)
    {
        log::error("assert", juce::String(message != nullptr ? message : "CRT assertion") + "\n" + stackTrace());
        log::shutdown(); // flush before the report box (or abort) takes over
    }
    if (returnValue != nullptr) *returnValue = 0;
    return FALSE; // continue with the default report
}
} // namespace

void installDebugAssertHook()
{
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, reportHook);
}
#else
void installDebugAssertHook() {}
#endif

} // namespace stemcel::native
