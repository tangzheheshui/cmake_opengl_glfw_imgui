//
//  DeletionQueue.h
//  RHI 内部值对象（GL/VK 共用）—— 句柄制前提之一：帧在飞时延迟回收
//
//  V3-0（docs/RHI_V3_PLAN.md）：destroy* 不立即销毁 GPU 对象，改为摘句柄 +
//  登记回调（dueFrame = 当前帧 + 帧滞后期）；设备在每帧 beginFrame（GPU 同步点
//  之后）批量执行到期回调，析构前 flushAll() 兜底。
//  纯 CPU 数据结构，无虚函数，不进契约层。
//
#ifndef RhiDeletionQueue_h
#define RhiDeletionQueue_h

#include <cstdint>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace rhi {
namespace detail {

class DeletionQueue {
public:
    // 帧滞后期：资源登记后至少经过 kLagFrames 帧才真正销毁。
    // 当前两后端 frame-in-flight = 1（单 fence），登记帧的提交在下一帧
    // beginFrame 的 fence 等待后必然完成 → lag=1 即安全；E2 提升到 fif=2
    // 时改为 2。
    static constexpr uint64_t kLagFrames = 1;

    // 登记一个到期销毁回调。dueFrame 通常为 currentFrame + kLagFrames。
    void push(uint64_t dueFrame, std::function<void()> fn) {
        _items.push_back(Item{dueFrame, std::move(fn)});
    }

    // 执行所有 dueFrame <= currentFrame 的回调（保持登记顺序）。
    void flush(uint64_t currentFrame) {
        if (_items.empty()) return;
        std::vector<Item> remain;
        remain.reserve(_items.size());
        for (Item& it : _items) {
            if (it.dueFrame <= currentFrame) {
                if (it.fn) it.fn();
            } else {
                remain.push_back(std::move(it));
            }
        }
        _items.swap(remain);
    }

    // 无条件执行全部回调（设备析构 / waitIdle 之后）。
    void flushAll() {
        for (Item& it : _items) {
            if (it.fn) it.fn();
        }
        _items.clear();
    }

    bool     empty() const { return _items.empty(); }
    uint32_t size()  const { return static_cast<uint32_t>(_items.size()); }

private:
    struct Item {
        uint64_t             dueFrame = 0;
        std::function<void()> fn;
    };
    std::vector<Item> _items;
};

} // namespace detail
} // namespace rhi

#endif /* RhiDeletionQueue_h */
