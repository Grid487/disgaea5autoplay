#pragma once
#include "native_geometry.h"

namespace AutoPlay::NativeNotifications {

// Original-file contracts: 0997C0/09A310, 2B44B0/2B4640 and 224570.
// The choice-dialog subclass has a DIFFERENT vtable (98A060).
constexpr uintptr_t kVtableRva = 0x98A0B0;
constexpr uintptr_t kUpdateRva = 0x09A310;
constexpr uintptr_t kQueuePointerRva = 0xB828C0;
constexpr uintptr_t kAnimationScaleRva = 0x990E80;
constexpr uintptr_t kAnimationReadyRva = 0x9913E4;
using Update = uint64_t (__fastcall*)(void*);
constexpr uintptr_t kClearTaskOffset = 0x8A4A8;
constexpr uintptr_t kClearTaskVtableRva = 0x98CD18;
enum class Phase { Other, Combat, Clear };
enum class Owner { None, CombatAutoplay, StageRepeat };
inline const char* PhaseName(Phase phase) {
    return phase == Phase::Combat ? "combat" : phase == Phase::Clear ? "clear" : "other";
}
inline const char* OwnerName(Owner owner) {
    return owner == Owner::CombatAutoplay ? "F1" : owner == Owner::StageRepeat ? "F2" : "none";
}

template<class T> inline bool Read(uintptr_t address, T& value) {
    if (!NativeGeometry::IsReadableRange(address, sizeof(T))) return false;
    memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
}

// A null manager is normal outside gameplay. Invalid queue metadata blocks
// automation rather than permitting commands behind an unclassified modal.
inline bool Foreground(uintptr_t base, uintptr_t& object) {
    object = 0;
    uintptr_t manager = 0;
    if (!Read(base + kQueuePointerRva, manager)) return false;
    if (!manager) return true;
    int head = 0, tail = 0;
    if (!NativeGeometry::IsReadableRange(manager, 0x208) ||
        !Read(manager + 0x200, head) || !Read(manager + 0x204, tail) ||
        head < 0 || head >= 64 || tail < 0 || tail >= 64) return false;
    for (int i = head; i != tail; i = (i + 1) % 64) {
        if (!Read(manager + static_cast<uintptr_t>(i) * 8, object)) return false;
        if (object) return true;
    }
    return true;
}

inline bool BlocksBattlePlanning(uintptr_t base) {
    uintptr_t foreground = 0;
    return !Foreground(base, foreground) || foreground != 0;
}

inline Phase BattlePhase(uintptr_t base, uintptr_t task, int* observedState = nullptr) {
    uintptr_t vtable = 0, context = 0, globalContext = 0;
    int state = -1;
    if (!task || !Read(task, vtable) || vtable != base + RVA::VTABLE_BattleMainTask ||
        !Read(task + 0x80, context) || !context ||
        !Read(base + RVA::PTR_BattleContext, globalContext) || context != globalContext ||
        !Read(task + 0x14, state)) return Phase::Other;
    if (observedState) *observedState = state;
    switch (state) {
    case 0x02: case 0x03: case 0x05: case 0x06: case 0x08: case 0x09:
    case 0x0C: case 0x0E: case 0x10: case 0x11: case 0x12:
    case 0x15: case 0x1A: case 0x1B: case 0x1C: return Phase::Combat;
    case 0x20:
        // 4A28E0 drives this exact result task through 861E60. Its
        // 61E2E0/61DF20 updates explicitly wait for the notification queue.
        if (Read(task + kClearTaskOffset, vtable) && vtable == base + kClearTaskVtableRva)
            return Phase::Clear;
        return Phase::Other;
    default: return Phase::Other; // Hub, exits, unknown/other result states stay manual.
    }
}

inline bool IsActiveBattle(uintptr_t base, uintptr_t task) {
    return BattlePhase(base, task) != Phase::Other;
}

inline const char* Eligibility(uintptr_t base, uintptr_t object, uintptr_t battleTask) {
    uintptr_t foreground = 0, vtable = 0, ui = 0, node = 0;
    int inputMode = 0, deadline = 0, progress = 0;
    float scale = 0, ready = 0;
    if (!IsActiveBattle(base, battleTask)) return "unverified battle/clear context";
    if (!Foreground(base, foreground)) return "invalid notification queue";
    if (!object || foreground != object) return "not foreground notification";
    if (!Read(object, vtable) || vtable != base + kVtableRva) return "unsupported dialog class";
    if (!Read(object + 0x1C, inputMode) || inputMode != 1) return "not a single-OK input notice";
    if (!Read(object + 0x2EAC, deadline)) return "unreadable close state";
    if (deadline != -1) return "native close pending";
    if (!Read(object + 0x2EB0, ui) || !ui || !Read(ui + 0x60, node) || !node ||
        !Read(node + 0x28, progress) || !Read(base + kAnimationScaleRva, scale) ||
        !Read(base + kAnimationReadyRva, ready) || !(scale > 0) || !(ready > 0))
        return "unreadable animation state";
    if (static_cast<float>(progress) * scale != ready) return "waiting for open animation";
    return nullptr;
}

inline bool Ready(uintptr_t base, uintptr_t object, uintptr_t battleTask) {
    return Eligibility(base, object, battleTask) == nullptr;
}

struct DriverState {
    uintptr_t object = 0, ui = 0;
    int lastElapsed = -1, attempts = 0, retryAfter = -1;
    bool accepted = false, completed = false;
    ULONGLONG lastReport = 0;
    const char* lastReason = nullptr;
};
inline DriverState driver;
inline void ResetDriver() { driver = {}; }

inline void ObserveQueue(uintptr_t base) {
    uintptr_t foreground = 0;
    if (driver.object && Foreground(base, foreground) && foreground != driver.object) {
        if (driver.accepted)
            LogMsg("[Notification] Observed queue removal object=0x%llX", static_cast<unsigned long long>(driver.object));
        ResetDriver();
    }
}

// Only the original notification update sees this fresh Confirm edge. Do not
// modify saved key bindings, held/repeat tables, or OS input/window focus.
class ScopedConfirm {
    uint8_t* key_ = nullptr;
    uint8_t* focus_ = nullptr;
    int32_t* device_ = nullptr;
    uint8_t savedKey_ = 0, savedFocus_ = 0;
    int32_t savedDevice_ = 0;
public:
    explicit ScopedConfirm(uintptr_t base, bool enabled = true) {
        if (!enabled) return;
        uintptr_t config = 0;
        uint64_t vk = 0;
        if (!Read(base + RVA::PTR_KeyConfig, config) || !config) return;
        if (!Read(config + 0x70, vk)) return;
        if (vk < 1 || vk > 0xFF) {
            if (!Read(config + 0x440, vk) || vk < 1 || vk > 0xFF) return;
        }
        const uintptr_t key = base + RVA::ADDR_KeyJustPressed + static_cast<uintptr_t>(vk);
        if (!Read(key, savedKey_) || !Read(base + RVA::ADDR_WindowFocused, savedFocus_) ||
            !Read(base + RVA::ADDR_InputDeviceMode, savedDevice_)) return;
        key_ = reinterpret_cast<uint8_t*>(key);
        focus_ = reinterpret_cast<uint8_t*>(base + RVA::ADDR_WindowFocused);
        device_ = reinterpret_cast<int32_t*>(base + RVA::ADDR_InputDeviceMode);
        *key_ = 1;
        *focus_ = 1;
        *device_ = 0;
    }
    ~ScopedConfirm() {
        if (!key_) return;
        *key_ = savedKey_;
        *focus_ = savedFocus_;
        *device_ = savedDevice_;
    }
    bool active() const { return key_ != nullptr; }
    ScopedConfirm(const ScopedConfirm&) = delete;
    ScopedConfirm& operator=(const ScopedConfirm&) = delete;
};

inline uint64_t Drive(Update original, uintptr_t base, void* notification,
                     uintptr_t battleTask, bool enabled, Owner owner = Owner::CombatAutoplay,
                     bool repeatEnabled = false) {
    const uintptr_t object = reinterpret_cast<uintptr_t>(notification);
    ObserveQueue(base);
    uintptr_t foreground = 0, ui = 0, vtable = 0;
    int elapsed = -1, state = -1;
    const Phase phase = BattlePhase(base, battleTask, &state);
    Read(object + 0x2EB0, ui); Read(object + 0x2EA8, elapsed); Read(object, vtable);
    const bool tracked = Foreground(base, foreground) && foreground == object && object;
    if (tracked && (driver.object != object || driver.ui != ui || elapsed < driver.lastElapsed)) {
        driver = {}; driver.object = object; driver.ui = ui;
    }
    const char* reason = !enabled ? "autoplay owner disabled" : Eligibility(base, object, battleTask);
    if (!reason && tracked && driver.attempts >= 2) reason = "two rejected attempts exhausted";
    if (!reason && tracked && elapsed < driver.retryAfter) reason = "waiting before retry";
    if (tracked) {
        driver.lastElapsed = elapsed;
        const ULONGLONG now = GetTickCount64();
        if (!driver.lastReason || now - driver.lastReport >= 1000) {
            LogMsg("[Notification] object=0x%llX typeRva=0x%llX phase=%s state=0x%02X F1=%d F2=%d owner=%s attempts=%d reason=%s",
                static_cast<unsigned long long>(object), static_cast<unsigned long long>(vtable - base),
                PhaseName(phase), state, g_Enabled ? 1 : 0, repeatEnabled ? 1 : 0,
                enabled ? OwnerName(owner) : "none", driver.attempts, reason ? reason : "ready");
            driver.lastReport = now; driver.lastReason = reason ? reason : "ready";
        }
    }
    if (reason) {
        const uint64_t result = original(notification);
        if (tracked && driver.accepted && !driver.completed && result == 1) {
            driver.completed = true;
            LogMsg("[Notification] Native close complete object=0x%llX", static_cast<unsigned long long>(object));
        }
        return result;
    }
    uint64_t result = 0;
    bool injected = false;
    {
        ScopedConfirm confirm(base);
        injected = confirm.active();
        if (injected && tracked) { ++driver.attempts; driver.retryAfter = elapsed + 6; }
        result = original(notification); // Exactly once; engine owns close animation/removal.
    }
    int deadline = -1;
    if (injected && Read(object + 0x2EAC, deadline) && deadline >= 0) {
        if (tracked) driver.accepted = true;
        LogMsg("[Notification] Accepted fresh Confirm object=0x%llX phase=%s owner=%s closeFrame=%d",
               static_cast<unsigned long long>(object), PhaseName(phase), OwnerName(owner), deadline);
    } else if (injected) {
        LogMsg("[Notification] Native confirm rejected object=0x%llX attempt=%d",
               static_cast<unsigned long long>(object), driver.attempts);
    }
    return result;
}

} // namespace AutoPlay::NativeNotifications
