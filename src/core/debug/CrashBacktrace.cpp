//
//  CrashBacktrace.cpp —— 见头文件说明
//

#include "debug/CrashBacktrace.h"

#if !defined(_WIN32)
#include <execinfo.h>
#include <csignal>
#include <unistd.h>
#endif

namespace engine {
namespace debug {

#if !defined(_WIN32)

static void crashSignalHandler(int sig) {
    // backtrace_symbols_fd 是 async-signal-safe 推荐用法（fd 直写，不走 stdio）
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::write(STDERR_FILENO, "\n==== CRASH BACKTRACE (signal ", 30);
    const char sigCh = static_cast<char>('0' + sig % 10);
    ::write(STDERR_FILENO, &sigCh, 1);
    ::write(STDERR_FILENO, ") ====\n", 7);
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
    // 恢复默认处置并重发，让系统崩溃报告（.ips）也正常生成
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

void installCrashBacktrace() {
    ::signal(SIGABRT, crashSignalHandler);
    ::signal(SIGSEGV, crashSignalHandler);
    ::signal(SIGBUS,  crashSignalHandler);
}

#else

void installCrashBacktrace() {}

#endif

} // namespace debug
} // namespace engine
