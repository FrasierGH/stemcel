#include "CrashHandler.h"

#ifdef _WIN32

#include <atomic>
#include <cstring>
#include <windows.h>

namespace stemcel::crash
{
namespace
{
// State the fault handler reads at crash time. The path is pre-rendered at
// install time so the handler itself does no heap/CRT string work while the
// process is already in a faulted state.
char g_crashLogPath[1024] = {0};
std::atomic<const char*> g_phase{"startup"};

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info)
{
    if (g_crashLogPath[0] == '\0')
    {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Win32 file I/O only — avoid the CRT/heap in a crash context.
    const HANDLE file = CreateFileA(g_crashLogPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        const EXCEPTION_RECORD* rec = info != nullptr ? info->ExceptionRecord : nullptr;
        const void* address = rec != nullptr ? rec->ExceptionAddress : nullptr;
        const DWORD code = rec != nullptr ? rec->ExceptionCode : 0;

        // Resolve the faulting module (which DLL/exe the crash address is in).
        char modulePath[MAX_PATH] = "unknown";
        HMODULE module = nullptr;
        if (address != nullptr &&
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(address), &module) &&
            module != nullptr)
        {
            GetModuleFileNameA(module, modulePath, MAX_PATH);
        }

        const char* phase = g_phase.load(std::memory_order_relaxed);

        // wsprintfA (not the CRT) formats without touching the heap. %I64X carries
        // the 64-bit fault address; the access-violation read/write flag and target
        // address (params 0 and 1) pinpoint a null / bad-pointer dereference.
        char buffer[2048];
        const auto* params = rec != nullptr ? rec->ExceptionInformation : nullptr;
        const int length = wsprintfA(
            buffer,
            "Stemcel backend crash\r\n"
            "phase=%s\r\n"
            "exceptionCode=0x%08X\r\n"
            "faultAddress=0x%I64X\r\n"
            "module=%s\r\n"
            "accessType=%I64u accessAddress=0x%I64X\r\n",
            phase != nullptr ? phase : "?", code,
            static_cast<unsigned __int64>(reinterpret_cast<UINT_PTR>(address)), modulePath,
            params != nullptr ? static_cast<unsigned __int64>(params[0]) : 0ULL,
            params != nullptr ? static_cast<unsigned __int64>(params[1]) : 0ULL);

        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }

    // We have logged what we can; let the process terminate. The backend cannot
    // function past a hard fault, and continuing risks corrupt state.
    return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace

void install(const juce::String& diagDir)
{
    if (diagDir.isEmpty())
    {
        return;
    }
    juce::File dir(diagDir);
    dir.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%dT%H-%M-%S");
    const auto path = dir.getChildFile("backend-crash-" + stamp + ".log").getFullPathName();
    // Copy into the static buffer so the fault handler needs no allocation.
    strncpy_s(g_crashLogPath, sizeof(g_crashLogPath), path.toRawUTF8(), _TRUNCATE);
    SetUnhandledExceptionFilter(&unhandledFilter);
}

void setPhase(const char* phase)
{
    g_phase.store(phase, std::memory_order_relaxed);
}

} // namespace stemcel::crash

#else

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

namespace stemcel::crash
{
namespace
{
// Same pre-rendered state as the Windows handler: the signal handler below may only
// make async-signal-safe calls, so nothing in it formats through stdio or the heap.
char g_crashLogPath[1024] = {0};
std::atomic<const char*> g_phase{"startup"};

// A dedicated signal stack, so a stack overflow can still be reported.
alignas(16) char g_altStack[1 << 17];

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

void writeText(int fd, const char* text)
{
    if (text != nullptr) (void)::write(fd, text, std::strlen(text));
}

void writeHex(int fd, std::uintptr_t value)
{
    constexpr int kDigits = static_cast<int>(sizeof(value) * 2);
    char buffer[2 + kDigits];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < kDigits; ++i)
        buffer[2 + i] = "0123456789ABCDEF"[(value >> ((kDigits - 1 - i) * 4)) & 0xF];
    (void)::write(fd, buffer, sizeof(buffer));
}

void writeDecimal(int fd, int value)
{
    char buffer[16];
    int pos = static_cast<int>(sizeof(buffer));
    const bool negative = value < 0;
    auto magnitude = negative ? -static_cast<long long>(value) : static_cast<long long>(value);
    do
    {
        buffer[--pos] = static_cast<char>('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude > 0 && pos > 1);
    if (negative) buffer[--pos] = '-';
    (void)::write(fd, buffer + pos, sizeof(buffer) - static_cast<size_t>(pos));
}

const char* signalName(int sig)
{
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "?";
    }
}

void faultHandler(int sig, siginfo_t* info, void* /*context*/)
{
    const int fd = ::open(g_crashLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        const char* phase = g_phase.load(std::memory_order_relaxed);
        writeText(fd, "Stemcel backend crash\nphase=");
        writeText(fd, phase != nullptr ? phase : "?");
        writeText(fd, "\nsignal=");
        writeText(fd, signalName(sig));
        writeText(fd, " (");
        writeDecimal(fd, sig);
        writeText(fd, ") code=");
        writeDecimal(fd, info != nullptr ? info->si_code : 0);
        // For SIGSEGV/SIGBUS this is the address that was accessed, which pinpoints a null or
        // bad-pointer dereference the same way the Windows report's accessAddress does.
        writeText(fd, "\naccessAddress=");
        writeHex(fd, reinterpret_cast<std::uintptr_t>(info != nullptr ? info->si_addr : nullptr));
        // backtrace_symbols_fd writes straight to the descriptor without allocating, and
        // names the image of every frame, which is what identifies a faulting plugin.
        writeText(fd, "\nbacktrace:\n");
        void* frames[64];
        const int count = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, count, fd);
        ::fsync(fd);
        ::close(fd);
    }

    // SA_RESETHAND restored the default action, so re-raising terminates the process with
    // the original signal: the backend cannot function past a hard fault.
    ::raise(sig);
}
} // namespace

void install(const juce::String& diagDir)
{
    if (diagDir.isEmpty())
    {
        return;
    }
    juce::File dir(diagDir);
    dir.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%dT%H-%M-%S");
    const auto path = dir.getChildFile("backend-crash-" + stamp + ".log").getFullPathName();
    std::strncpy(g_crashLogPath, path.toRawUTF8(), sizeof(g_crashLogPath) - 1);
    g_crashLogPath[sizeof(g_crashLogPath) - 1] = '\0';

    stack_t altStack{};
    altStack.ss_sp = g_altStack;
    altStack.ss_size = sizeof(g_altStack);
    (void)::sigaltstack(&altStack, nullptr);

    struct sigaction action{};
    action.sa_sigaction = &faultHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (const int sig : kFatalSignals)
    {
        (void)::sigaction(sig, &action, nullptr);
    }
}

void setPhase(const char* phase)
{
    g_phase.store(phase, std::memory_order_relaxed);
}

} // namespace stemcel::crash

#endif
