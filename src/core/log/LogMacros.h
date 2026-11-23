//
//  LogMacros.h
//  引擎日志 —— 唯一映射点
//
//  原则：引擎代码禁止直接调用 printf / fprintf / std::cout，
//        一律使用下方 ENGINE_LOG_* 宏。
//
//  换项目时快速切换日志系统的两种方式：
//    方式 A（推荐）：编译定义覆盖
//      target_compile_definitions(my_target PRIVATE
//          ENGINE_LOG_INFO=MyLog_Info
//          ENGINE_LOG_ERROR=MyLog_Error ...)
//    方式 B：在 include 本头文件之前 #define 覆盖默认实现
//
//  宏签名统一为 printf 风格：ENGINE_LOG_XXX(fmt, ...)
//
#ifndef EngineLogMacros_h
#define EngineLogMacros_h

#include <cstdio>

// ---- 默认实现（printf）——换项目时覆盖以下任一宏即可 ----
#ifndef ENGINE_LOG_INFO
#define ENGINE_LOG_INFO(fmt, ...)  std::printf("[INFO ] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef ENGINE_LOG_WARN
#define ENGINE_LOG_WARN(fmt, ...)  std::printf("[WARN ] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef ENGINE_LOG_ERROR
#define ENGINE_LOG_ERROR(fmt, ...) \
    do { std::fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); } while (0)
#endif

// 一次性告警（同一位置只打印一次，用于"暂不支持"类提示）
#ifndef ENGINE_LOG_WARN_ONCE
#define ENGINE_LOG_WARN_ONCE(fmt, ...)                             \
    do {                                                           \
        static bool _logged_once = false;                          \
        if (!_logged_once) {                                       \
            _logged_once = true;                                   \
            ENGINE_LOG_WARN(fmt, ##__VA_ARGS__);                   \
        }                                                          \
    } while (0)
#endif

#endif /* EngineLogMacros_h */
