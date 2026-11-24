//
//  DeletionQueue.h
//  RHI 延迟资源销毁（V3-0）
//
//  背景：Vulkan 资源被 in-flight command buffer 引用时不能立即 vkDestroy*。
//  destroy* 只把句柄从资源表摘除（标记 dead），实际释放登记到本队列；
//  endFrame 推进帧号，销毁已过 delayFrames 的条目。
//
//  约定：
//  - 单线程（与 device 接口一致），不加锁；
//  - 设备析构前必须先 waitIdle + flush()，否则 lambda 里的 vkDevice 已失效；
//  - GL 后端当前仍即时删除（GL 同步语义；VAO 悬空已由 discardVAOs 处理），
//    后续可复用本类统一。
//
#ifndef RhiDetailDeletionQueue_h
#define RhiDetailDeletionQueue_h

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace rhi {

class DeletionQueue {
public:
    explicit DeletionQueue(uint32_t delayFrames = 2) : _delay(delayFrames) {}

    void enqueue(std::function<void()> fn) {
        _entries.push_back(Entry{_frame + _delay, std::move(fn)});
    }

    // 每帧末尾调用：推进帧号并执行到期条目。
    // 先摘出到期项再执行，允许回调里继续 enqueue。
    void tick() {
        ++_frame;
        std::vector<Entry> due;
        for (auto it = _entries.begin(); it != _entries.end();) {
            if (it->frame <= _frame) {
                due.push_back(std::move(*it));
                it = _entries.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& e : due) e.fn();
    }

    // 设备销毁前强制清空（调用方保证 GPU 已 idle）。
    // 循环处理"回调里又 enqueue"的情况。
    void flush() {
        while (!_entries.empty()) {
            std::vector<Entry> all;
            all.swap(_entries);
            for (auto& e : all) e.fn();
        }
    }

    std::size_t pending() const { return _entries.size(); }
    uint64_t frame() const { return _frame; }

private:
    struct Entry {
        uint64_t frame = 0;
        std::function<void()> fn;
    };
    uint32_t _delay = 2;
    uint64_t _frame = 0;
    std::vector<Entry> _entries;
};

} // namespace rhi

#endif /* RhiDetailDeletionQueue_h */
