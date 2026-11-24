//
//  HandleTable.h
//  RHI 句柄槽位表（V3-1：句柄表 + 越界/死句柄检查）
//
//  统一后端里手写的「std::vector<T> + h.id-1 索引 + 边界检查」。
//  - id  = 槽下标 + 1，0 表示无效（与 RhiHandle::valid() 约定一致）；
//  - gen = 槽代际，销毁时 +1；分配复用槽位时沿用该槽当前代际，旧句柄因此失效；
//  - alive(h)：id 在范围内、槽存活、gen 一致，三者都满足才算有效。
//
//  V3-13：启用槽位复用（空闲链表）。alloc 优先取最近释放的槽，
//  没有空闲槽才追加。因此 **id 不再等于 size()+1**，上层禁止预测下一个 id
//  （Vulkan 纹理延迟上传回调已改为"句柄占位 + 分配后回填"）。
//
#ifndef RhiDetailHandleTable_h
#define RhiDetailHandleTable_h

#include "../RhiTypes.h"
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace rhi {
namespace detail {

template <class T>
class HandleTable {
public:
    // 分配一个槽位（优先复用最近释放的），返回句柄（gen = 该槽当前代际）。
    RhiHandle alloc(T value) {
        uint32_t idx;
        if (!_freeSlots.empty()) {
            idx = _freeSlots.back();
            _freeSlots.pop_back();
        } else {
            idx = static_cast<uint32_t>(_slots.size());
            _slots.emplace_back();
        }
        Slot& s = _slots[idx];
        s.value = std::move(value);
        s.alive = true;
        RhiHandle h;
        h.id  = idx + 1;
        h.gen = s.gen;
        return h;
    }

    // 销毁槽位：清空资源、标记死亡、gen + 1（旧句柄立即失效）。
    void free(uint32_t id) {
        if (id == 0 || id > _slots.size()) return;
        Slot& s = _slots[id - 1];
        if (!s.alive) return;
        s.value = T{};
        s.alive = false;
        ++s.gen;
        _freeSlots.push_back(id - 1);
    }

    // 按 id 取资源。越界断言（调用前应先 alive() 检查）。
    T& get(uint32_t id) {
        assert(id >= 1 && id <= _slots.size());
        return _slots[id - 1].value;
    }
    const T& get(uint32_t id) const {
        assert(id >= 1 && id <= _slots.size());
        return _slots[id - 1].value;
    }

    // 句柄是否仍有效：id 在范围内、槽存活、代际一致。
    bool alive(RhiHandle h) const {
        return h.id >= 1 && h.id <= _slots.size()
            && _slots[h.id - 1].alive
            && _slots[h.id - 1].gen == h.gen;
    }

    size_t size() const { return _slots.size(); }

    // 调试自检：分配 → 释放 → 再分配应复用同一槽，旧句柄失效、新句柄有效。
    // 只在本表上做一次 alloc/free，不影响业务槽位。
    bool debugSelfCheck() {
        RhiHandle a = alloc(T{});
        const uint32_t aid = a.id;
        free(a.id);
        RhiHandle b = alloc(T{});
        const bool ok = (b.id == aid) && !alive(a) && alive(b);
        free(b.id);
        return ok;
    }

    // 遍历所有存活槽位（析构清理等用），回调可修改资源。
    template <class F>
    void forEach(F&& fn) {
        for (auto& s : _slots) if (s.alive) fn(s.value);
    }

private:
    struct Slot {
        T value{};
        uint32_t gen = 0;
        bool alive = false;
    };
    std::vector<Slot> _slots;
    std::vector<uint32_t> _freeSlots;   // 已释放槽下标（后进先出）
};

} // namespace detail
} // namespace rhi

#endif /* RhiDetailHandleTable_h */
