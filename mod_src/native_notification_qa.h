#pragma once

// Development-only native smoke test. Never compiled into the normal DLL.
// 2B3BB0's original caller at 7C1DA6..7C1DDC supplies style1, input1,
// deadline-1, sound37h, scale1, and the A17C34 default-position pointer.
// It constructs/enqueues the same 98A0B0 notification used by clear rewards.
// Only the text is synthetic; this does not award skills, prisoners or items.
namespace AutoPlay::NativeNotificationQA {
inline bool combatQueued = false, clearQueued = false;
inline ULONGLONG holdUntil = 0;
using CreateNotice = void(__fastcall*)(int, int, int, int, int, float,
                                       uintptr_t, const void*, const char*, const char*);
inline void MaybeQueue(uintptr_t base, uintptr_t task, bool f1) {
    if (!f1) return;
    uintptr_t foreground = 0;
    if (!NativeNotifications::Foreground(base, foreground) || foreground) return;
    int state = -1;
    const auto phase = NativeNotifications::BattlePhase(base, task, &state);
    const char* text = nullptr;
    if (!combatQueued && phase == NativeNotifications::Phase::Combat && state == 2) {
        combatQueued = true;
        text = "Native autoplay test: combat notification";
    } else if (combatQueued && !clearQueued && phase == NativeNotifications::Phase::Clear) {
        clearQueued = true;
        text = "Native autoplay test: clear notification";
    }
    if (!text) return;
    // Five seconds for visual/memory observation; original UI updates keep
    // running. Only automation's confirm is delayed, never the game thread.
    holdUntil = GetTickCount64() + 5000;
    reinterpret_cast<CreateNotice>(base + 0x2B3BB0)(1, 1, -1, 0, 0x37, 1.0f,
        0, reinterpret_cast<const void*>(base + 0xA17C34), text, nullptr);
    LogMsg("[NativeQA] Queued synthetic-text native OK notice phase=%s state=0x%02X; hold=5s",
           NativeNotifications::PhaseName(phase), state);
}
inline bool AllowConfirm() { return GetTickCount64() >= holdUntil; }
} // namespace AutoPlay::NativeNotificationQA
