#pragma once
#include "native_notifications.h"
#include <cstddef>

namespace AutoPlay::NativeScenes {
// 13CCF0 builds this registry; the battle row is scene5, with update4AEFE0
// (861E60 / Execute4). Scene8's base update is4A0150. Post/draw is separate.
constexpr uintptr_t kManagerPointerRva = 0xBDFD08;
constexpr uintptr_t kBattleMainPointerRva = 0x7B58E68;
constexpr uintptr_t kBattleUpdateRva = 0x4AEFE0;
constexpr uintptr_t kBaseUpdateRva = 0x4A0150;
struct Row {
    int32_t id;
    uint32_t padding;
    uintptr_t enter, update, post, cleanup;
};
static_assert(sizeof(Row) == 0x28 && offsetof(Row, update) == 0x10, "verified scene callback layout");
struct Registry {
    uintptr_t manager = 0;
    int desired = -1, current = -1, count = 0;
};
using Update = uint64_t (__fastcall*)();
inline bool Settled(const Registry& registry,int scene) {
    // Scene manager consumes requested transitions by writing -1. Keeping the
    // current scene ID here would reject the normal idle hub forever.
    return registry.current==scene && (registry.desired==-1 || registry.desired==scene);
}
inline bool ReadRegistry(uintptr_t base, Registry& out) {
    out = {};
    return NativeNotifications::Read(base + kManagerPointerRva, out.manager) && out.manager &&
        NativeNotifications::Read(out.manager, out.desired) &&
        NativeNotifications::Read(out.manager + 4, out.current) &&
        NativeNotifications::Read(out.manager + 8, out.count) &&
        out.count >= 1 && out.count <= 64 &&
        NativeGeometry::IsReadableRange(out.manager + 0x10, static_cast<size_t>(out.count) * sizeof(Row));
}
inline uintptr_t FindUpdateSlot(uintptr_t base, int scene, uintptr_t expected, const Registry& registry) {
    uintptr_t found = 0;
    for (int i = 0; i < registry.count; ++i) {
        const uintptr_t address = registry.manager + 0x10 + static_cast<uintptr_t>(i) * sizeof(Row);
        Row row{};
        if (!NativeNotifications::Read(address, row)) return 0;
        if (row.id != scene) continue;
        // Duplicate IDs or a changed callback contract cannot be hooked.
        if (found || row.update != base + expected) return 0;
        found = address + offsetof(Row, update);
    }
    return found;
}
inline bool InstallUpdate(uintptr_t base, int scene, uintptr_t expectedRva, Update replacement, Update& original) {
    if (!replacement || original) return false;
    Registry registry;
    if (!ReadRegistry(base, registry)) return false;
    const uintptr_t slot = FindUpdateSlot(base, scene, expectedRva, registry);
    if (!slot || slot % alignof(uintptr_t)) return false;
    DWORD saved = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(uintptr_t), PAGE_READWRITE, &saved)) return false;
    const auto expected = reinterpret_cast<void*>(base + expectedRva);
    original = reinterpret_cast<Update>(expected); // Publish before the callback slot.
    const auto replaced = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(replacement), expected);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(uintptr_t), saved, &ignored);
    if (replaced != expected) { original = nullptr; return false; }
    return true;
}
} // namespace AutoPlay::NativeScenes
