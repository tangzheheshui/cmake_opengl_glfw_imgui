//
//  Result.h
//  RHI 返回值类型（V3-1 基座）
//
//  资源创建/查询路径用 Result 表达失败，替代 abort / 静默返回空句柄。
//  当前先落类型，后续逐接口迁移；不改变现有行为。
//
#ifndef RhiResult_h
#define RhiResult_h

#include <utility>

namespace rhi {

enum class RhiResult : int {
    Ok = 0,
    InvalidArg,
    Unsupported,
    OutOfMemory,
    DeviceLost,
};

template <class T = void>
struct Result {
    RhiResult code = RhiResult::Ok;
    T         value{};

    explicit operator bool() const { return code == RhiResult::Ok; }

    static Result ok(T v) {
        Result r;
        r.code = RhiResult::Ok;
        r.value = std::move(v);
        return r;
    }
    static Result err(RhiResult c) {
        Result r;
        r.code = c;
        return r;
    }
};

template <>
struct Result<void> {
    RhiResult code = RhiResult::Ok;

    explicit operator bool() const { return code == RhiResult::Ok; }

    static Result ok() { return Result{}; }
    static Result err(RhiResult c) {
        Result r;
        r.code = c;
        return r;
    }
};

} // namespace rhi

#endif /* RhiResult_h */
