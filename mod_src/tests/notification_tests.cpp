// Offline integration: real notification driver + battle menu guard, no game
// process, OS input injection, window operations, or real-game logging.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../xinput_proxy.cpp"
#include <cstdio>
#include <cstring>

using namespace AutoPlay;
namespace N = AutoPlay::NativeNotifications;
namespace {
constexpr size_t kSize = 128ull * 1024 * 1024;
uintptr_t image = 0, task = 0, context = 0, queue = 0, notice = 0, ui = 0, node = 0, config = 0;
int calls = 0, accepts = 0;
bool opaqueReturn = false;
bool rejectInput = false;
template<class T> T& At(uintptr_t address) { return *reinterpret_cast<T*>(address); }
bool Check(bool value, const char* label) {
    printf("%s: %s\n", value ? "PASS" : "FAIL", label);
    return value;
}
void Reset() {
    memset(reinterpret_cast<void*>(image), 0, kSize);
    g_Base = image;
    task = image + 0x1000000; context = image + 0x1400000;
    queue = image + 0x1500000; notice = image + 0x1600000;
    ui = image + 0x1700000; node = image + 0x1800000; config = image + 0x1900000;
    At<uintptr_t>(task) = image + RVA::VTABLE_BattleMainTask;
    At<int>(task + 0x14) = 0x15;
    At<uintptr_t>(task + 0x80) = context;
    At<uintptr_t>(image + RVA::PTR_BattleContext) = context;
    At<uintptr_t>(image + N::kQueuePointerRva) = queue;
    At<int>(queue + 0x204) = 1;
    At<uintptr_t>(queue) = notice;
    At<uintptr_t>(notice) = image + N::kVtableRva;
    At<int>(notice + 0x1C) = 1;
    At<int>(notice + 0x2EAC) = -1;
    At<uintptr_t>(notice + 0x2EB0) = ui;
    At<uintptr_t>(ui + 0x60) = node;
    // Literal constants verified in the original executable: fixed-point 12.
    At<int>(node + 0x28) = 0xC0000;
    At<float>(image + N::kAnimationScaleRva) = 1.0f / 65536.0f;
    At<float>(image + N::kAnimationReadyRva) = 12.0f;
    At<uintptr_t>(image + RVA::PTR_KeyConfig) = config;
    At<uint64_t>(config + 0x70) = VK_SPACE;
    At<int>(image + RVA::ADDR_InputDeviceMode) = 2;
    calls = accepts = 0; opaqueReturn = false;
    rejectInput = false; N::ResetDriver();
    g_Enabled = true; g_PulseAction = g_HoldAction = -1;
}
// Models the recovered 09A310 input/countdown contract. It deliberately
// accepts fresh presses only, never the held key used for attack animations.
uint64_t __fastcall Original(void* object) {
    ++calls;
    const uintptr_t p = reinterpret_cast<uintptr_t>(object);
    int& elapsed = At<int>(p + 0x2EA8);
    int& deadline = At<int>(p + 0x2EAC);
    ++elapsed;
    uint64_t vk = At<uint64_t>(config + 0x70);
    if (vk < 1 || vk > 255) vk = At<uint64_t>(config + 0x440);
    if (!rejectInput && deadline == -1 && At<int>(p + 0x1C) == 1 &&
        At<int>(node + 0x28) == 0xC0000 && vk >= 1 && vk <= 255 &&
        At<uint8_t>(image + RVA::ADDR_KeyJustPressed + vk) &&
        At<uint8_t>(image + RVA::ADDR_WindowFocused) &&
        At<int>(image + RVA::ADDR_InputDeviceMode) == 0) {
        deadline = elapsed + 6; ++accepts;
    }
    if (opaqueReturn) return 0x123456789ABCDEF0ull;
    return deadline >= 0 && elapsed >= deadline ? 1 : 0;
}
uint64_t Tick(bool enabled = true) { return N::Drive(&Original, image, reinterpret_cast<void*>(notice), task, enabled); }
bool TestCloseAndResume() {
    Reset();
    At<uint8_t>(image + RVA::ADDR_KeyHeld + VK_SPACE) = 1;
    bool ok = Check(Tick() == 0 && accepts == 1 && calls == 1 && At<int>(notice + 0x2EAC) == 7,
                    "fresh Confirm starts native six-frame close exactly once");
    ok &= Check(At<uint8_t>(image + RVA::ADDR_KeyJustPressed + VK_SPACE) == 0 &&
        At<uint8_t>(image + RVA::ADDR_KeyHeld + VK_SPACE) == 1 &&
        At<uint8_t>(image + RVA::ADDR_WindowFocused) == 0 &&
        At<int>(image + RVA::ADDR_InputDeviceMode) == 2,
        "temporary input restored; held key untouched");
    for (int i = 0; i < 5; ++i) ok &= Tick() == 0;
    ok &= Check(Tick() == 1 && accepts == 1 && calls == 7, "native close completes without repeated Confirm");
    g_PulseAction = ACT_CANCEL; g_HoldAction = ACT_CONFIRM;
    const int oldStuck = g_StateStuckTicks;
    StepBattleDecision(task, context);
    ok &= Check(g_PulseAction == -1 && g_HoldAction == -1 && g_StateStuckTicks == oldStuck,
                "real battle handler suspends input and retries behind modal");
    At<uintptr_t>(queue) = 0; At<int>(queue + 0x200) = 1;
    ok &= Check(!N::BlocksBattlePlanning(image), "native queue removal releases battle planner");
    // Next queued notification must work without a stale pointer/debounce latch.
    notice += 0x4000;
    At<int>(queue + 0x204) = 2; At<uintptr_t>(queue + 8) = notice;
    At<uintptr_t>(notice) = image + N::kVtableRva;
    At<int>(notice + 0x1C) = 1; At<int>(notice + 0x2EAC) = -1;
    At<uintptr_t>(notice + 0x2EB0) = ui;
    Tick();
    ok &= Check(accepts == 2, "back-to-back notification accepted independently");
    return ok;
}
bool TestEligibility() {
    bool ok = true;
    Reset(); Tick(false);
    ok &= Check(accepts == 0 && calls == 1, "F1 off passes original update through untouched");
    Tick(true); ok &= Check(accepts == 1, "enabling F1 behind existing modal permits dismissal");
    Reset(); At<uintptr_t>(notice) = image + 0x98A060; Tick();
    ok &= Check(accepts == 0 && N::BlocksBattlePlanning(image), "choice subclass stays manual and blocks combat inputs");
    Reset(); At<int>(notice + 0x1C) = 0; Tick();
    ok &= Check(accepts == 0, "timed notifications receive no synthetic input");
    Reset(); At<int>(notice + 0x2EAC) = 100; Tick();
    ok &= Check(accepts == 0, "already closing notifications receive no synthetic input");
    Reset(); At<int>(node + 0x28) = 0xBFFFF; Tick();
    ok &= Check(accepts == 0, "wait for native open animation readiness");
    At<int>(node + 0x28) = 0xC0000; Tick(); ok &= Check(accepts == 1, "dismiss once animation becomes ready");
    Reset(); At<int>(task + 0x14) = 0x20; Tick();
    ok &= Check(accepts == 0, "results/hub/unknown states excluded");
    Reset(); At<uintptr_t>(task + 0x80) = context + 8; Tick();
    ok &= Check(accepts == 0, "stale battle context excluded");
    Reset(); At<uintptr_t>(task) = 0; Tick();
    ok &= Check(accepts == 0, "destroyed battle task excluded");
    Reset(); At<uintptr_t>(queue) = notice + 0x4000; At<int>(queue + 0x204) = 2;
    At<uintptr_t>(queue + 8) = notice; Tick();
    ok &= Check(accepts == 0, "background notification cannot consume Confirm");
    Reset(); At<int>(queue + 0x200) = 63; At<int>(queue + 0x204) = 1;
    At<uintptr_t>(queue + 63 * 8) = 0; Tick();
    ok &= Check(accepts == 1, "queue wraparound and null entries handled");
    Reset(); At<int>(queue + 0x200) = 64; Tick();
    ok &= Check(accepts == 0 && N::BlocksBattlePlanning(image), "invalid queue metadata fails closed");
    Reset(); At<uintptr_t>(ui + 0x60) = 1; Tick();
    ok &= Check(accepts == 0, "unreadable UI node skipped without dereference");
    Reset(); At<uint64_t>(config + 0x70) = 0; At<uint64_t>(config + 0x440) = 'Z'; Tick();
    ok &= Check(accepts == 1 && At<uint64_t>(config + 0x70) == 0, "alternate Confirm honored without rebinding");
    Reset(); At<uint64_t>(config + 0x70) = 0; Tick();
    ok &= Check(accepts == 0, "missing Confirm mappings left unchanged");
    Reset(); At<uint8_t>(image + RVA::ADDR_KeyJustPressed + VK_SPACE) = 1;
    At<uint8_t>(image + RVA::ADDR_WindowFocused) = 1; opaqueReturn = true;
    ok &= Check(Tick() == 0x123456789ABCDEF0ull && calls == 1 &&
        At<uint8_t>(image + RVA::ADDR_KeyJustPressed + VK_SPACE) == 1 &&
        At<uint8_t>(image + RVA::ADDR_WindowFocused) == 1,
        "original 64-bit return and genuine user input preserved");
    return ok;
}
bool TestClearFlow() {
    Reset();
    At<int>(task + 0x14) = 0x20;
    bool ok = Check(!N::Ready(image, notice, task), "clear state alone does not authorize notification input");
    At<uintptr_t>(task + N::kClearTaskOffset) = image + N::kClearTaskVtableRva;
    ok &= Check(N::BattlePhase(image, task) == N::Phase::Clear && N::Ready(image, notice, task),
                "verified native clear task authorizes the surrender OK notice");
    g_PulseAction = g_HoldAction = ACT_CONFIRM;
    StepBattleDecision(task, context);
    ok &= Check(g_PulseAction == -1 && g_HoldAction == -1, "clear handler waits while surrender modal owns input");
    Tick();
    for (int i = 0; i < 6; ++i) Tick();
    ok &= Check(accepts == 1 && N::driver.completed, "clear notice completes its native close");
    At<uintptr_t>(queue) = 0; At<int>(queue + 0x200) = 1;
    g_CooldownTicks = 0;
    StepBattleDecision(task, context);
    ok &= Check(g_PulseAction == ACT_CONFIRM && N::driver.object == 0,
                "full battle handler resumes clear/result confirmation after queue removal");
    Reset(); At<int>(task + 0x14) = 0x20;
    At<uintptr_t>(task + N::kClearTaskOffset) = image + N::kClearTaskVtableRva;
    Tick(false); ok &= Check(accepts == 0, "clear notice stays manual when both owners are off");
    Reset(); At<int>(task + 0x14) = 0x20;
    At<uintptr_t>(task + N::kClearTaskOffset) = image + N::kClearTaskVtableRva;
    rejectInput = true;
    for (int i = 0; i < 30; ++i) Tick();
    ok &= Check(calls == 30 && N::driver.attempts == 2 && accepts == 0,
                "rejected native confirmations stop after two attempts without stopping native updates");
    rejectInput = false;
    Tick(); ok &= Check(accepts == 0, "exhausted notice does not retry indefinitely");
    // A replacement popup may reuse the same allocation but starts a new lifetime.
    At<int>(notice + 0x2EA8) = 0;
    Tick(); ok &= Check(accepts == 1 && N::driver.attempts == 1, "new notice lifetime resets the retry limit");
    Reset(); At<int>(node + 0x28) = 1;
    for (int i = 0; i < 10; ++i) Tick();
    ok &= Check(N::driver.attempts == 0, "animation readiness waits do not consume attempts");
    return ok;
}
}
int main() {
    if (tmpfile_s(&g_LogFile) || !g_LogFile) return 2;
    image = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!image) return 2;
    const bool ok = TestCloseAndResume() & TestEligibility() & TestClearFlow();
    VirtualFree(reinterpret_cast<void*>(image), 0, MEM_RELEASE);
    fclose(g_LogFile); g_LogFile = nullptr;
    return ok ? 0 : 1;
}
