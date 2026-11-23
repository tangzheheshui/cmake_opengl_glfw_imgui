//
//  CrashBacktrace.h —— 调试设施：abort/段错误时打印 backtrace 到 stderr
//
//  背景：MoltenVK 内部 MVKAssert 静默 abort（无日志）时，崩溃报告的符号化
//  经常缺失中间帧；进程内 backtrace 是最快的定位手段。
//  用法：RHI_DEBUG_LOG=1 时 app 在启动时安装（RHI_DEBUG_LOG 同时关闭 stdout
//  缓冲，保证 abort 前的日志不丢）。
//
#ifndef ENGINE_DEBUG_CRASH_BACKTRACE_H
#define ENGINE_DEBUG_CRASH_BACKTRACE_H

namespace engine {
namespace debug {

/// 安装 SIGABRT/SIGSEGV/SIGBUS handler（POSIX；Windows 为 no-op）。
/// handler 打印 backtrace 到 stderr 后恢复默认处置（便于系统崩溃报告生成）。
void installCrashBacktrace();

} // namespace debug
} // namespace engine

#endif // ENGINE_DEBUG_CRASH_BACKTRACE_H
