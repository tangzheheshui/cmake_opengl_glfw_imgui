//
//  HandleTable.h
//  RHI 句柄槽位表（V3-1：句柄表 + 越界/死句柄检查）
//
//  统一后端里手写的「std::vector<T> + h.id-1 索引 + 边界检查」。
//  - id  = 槽下标 + 1，0 表示无效（与 RhiHandle::valid() 约定一致）；
//  - gen = 槽代际，销毁时 +1（当前 id 不复用、槽位保留为失效；
//    gen 为将来「槽位复用」预留的校验位，alive() 一并校验）；
//  - alive(h)：id 在范围内、槽存活、gen 一致，三者都满足才算有效。
//
//  注意：当前语义为「id 不复用」（与 B0 契约一致），alloc 恒追加，
//  所以 id 恒等于 size+1。上层若有「预测下一个 id = size+1」的写法（如
//  Vulkan 纹理上传的延迟回调）不受影响。将来启用槽位复用时再引入空闲链表。
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
    // 分配一个槽位，返回句柄（id 从 1 起 = size+1，gen = 该槽当前代际）。
    RhiHandle alloc(T value) {
        const uint32_t idx = static_cast<uint32_t>(_slots.size());
        _slots.emplace_back();
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
};

} // namespace detail
} // namespace rhi

#endif /* RhiDetailHandleTable_h */
