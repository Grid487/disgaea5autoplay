// Exercise the real deployment handler, not only NativeDeployment::Job.
// VirtualQuery still validates local test buffers; its simulated cost changes
// an injected clock, without sleeping or attaching to the user's game.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../planning_budget.h"
#include <windows.h>
#include <algorithm>
#include <string>

namespace {
uint64_t testNow = 0, preliminaryCost = 0, insideQueryCost = 0, setupCost = 0, enumerationCost = 0;
uint64_t firstBudgetStart = 0;
AutoPlay::Planning::Counts peak;
SIZE_T WINAPI TimedVirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size) {
    testNow += AutoPlay::Planning::active ? insideQueryCost : preliminaryCost;
    return ::VirtualQuery(address, info, size);
}
uint64_t TestClock() {
    if (!AutoPlay::Planning::active && firstBudgetStart == 0) firstBudgetStart = testNow;
    if (const auto* b = AutoPlay::Planning::active) {
        peak.work = (std::max)(peak.work, b->used.work);
        peak.previews = (std::max)(peak.previews, b->used.previews);
        peak.effects = (std::max)(peak.effects, b->used.effects);
        peak.paths = (std::max)(peak.paths, b->used.paths);
    }
    return testNow;
}
}
#define VirtualQuery TimedVirtualQuery
#define main OriginalRuntimeSuite
#include "smart_runtime_tests.cpp"
#undef main
#undef VirtualQuery

namespace {
int __fastcall TimedEnumeration(void* cd, uint32_t* rows) {
    testNow += enumerationCost;
    const int count = *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(cd) + 0x2CC0);
    for (int i = 0; i < count; ++i) rows[i] = static_cast<uint32_t>(i);
    return count;
}
int __fastcall TimedFaction(void* unit, uint8_t mode) {
    if (Planning::active && Planning::active->used.work == 0) testNow += setupCost;
    return StubFaction(unit, mode);
}

bool PrepareDispatch(int rows = 2, int skills = 1) {
    preliminaryCost = insideQueryCost = setupCost = enumerationCost = 0;
    testNow = firstBudgetStart = 0;
    peak = {};
    if (!ResetWorld(800) || !OpenIsolatedTestLog()) return false;
    const auto context = BattleContext(), mainTask = MainTask();
    *reinterpret_cast<int16_t*>(g_Image + NativeDeployment::kRosterTotalCountRva) = static_cast<int16_t>(rows);
    *reinterpret_cast<int16_t*>(context + NativeDeployment::kRosterCountOffset) = static_cast<int16_t>(rows);
    for (int r = 0; r < rows; ++r) {
        const auto cd = NativeDeployment::RosterCharaData(r);
        std::memcpy(reinterpret_cast<void*>(cd), reinterpret_cast<void*>(Chara(kActor)), 0x4000);
        *reinterpret_cast<int16_t*>(context + NativeDeployment::kRosterRowsOffset + r * 2) = static_cast<int16_t>(r);
        *reinterpret_cast<int64_t*>(cd + 0x2D00) = 300 + r * 100;
        *reinterpret_cast<int64_t*>(cd + 0x2CE8) = 100 + r * 100;
        *reinterpret_cast<int16_t*>(cd + 0x2CC0) = static_cast<int16_t>(skills);
        for (int s = 0; s < skills; ++s) {
            const auto row = cd + 0xCC4 + s * 16;
            std::memset(reinterpret_cast<void*>(row), 0, 16);
            *reinterpret_cast<uint32_t*>(row) = kSkillId;
            *reinterpret_cast<uint8_t*>(row + 10) = kAreaShape;
        }
    }
    *reinterpret_cast<uint8_t*>(Unit(kActor) + 0x2E8) = 0;
    *reinterpret_cast<uint8_t*>(Unit(kAlly) + 0x2E8) = 0;
    *reinterpret_cast<uintptr_t*>(mainTask + 0x141510 + NativeDeployment::kTaskBattleContextOffset) = context;
    *reinterpret_cast<int32_t*>(mainTask + 0x14) = 0x0C;
    *reinterpret_cast<int32_t*>(mainTask + 0x2C) = 0;
    *reinterpret_cast<int32_t*>(mainTask + 0x141510 + 0x14) = 0;
    *reinterpret_cast<int32_t*>(mainTask + 0x141510 + 0x2C) = 0;
    g_LastBattleState = 0x0C;
    g_StateStuckTicks = 3;
    NativeSkills::fnEnumerateSkills = &TimedEnumeration;
    fnGetUnitFaction = &TimedFaction;
    return true;
}

int DispatchFrames(int limit = 500) {
    for (int frame = 1; frame <= limit; ++frame) {
        g_PulseAction = -1;
        StepBattleDecision(MainTask(), BattleContext(), &TestClock);
        if (g_PendingDeployment) return frame;
    }
    return 0;
}

bool TestSlowPrechecksDispatch() {
    if (!PrepareDispatch()) return false;
    const int fastFrames = DispatchFrames();
    const int expected = g_DeploymentJob.SelectedRoster();
    if (!Expect(fastFrames > 0 && expected == 1 && g_PulseAction == ACT_CONFIRM,
                "full deployment handler dispatches the strongest verified healer")) return false;
    if (!PrepareDispatch()) return false;
    preliminaryCost = 5000000;
    const int slowFrames = DispatchFrames();
    if (!Expect(slowFrames > 0 && g_DeploymentJob.SelectedRoster() == expected &&
                g_PulseAction == ACT_CONFIRM && firstBudgetStart >= 5000000,
                "preliminary checks exceeding one millisecond cannot starve the full dispatch handler")) return false;
    std::printf("STARVATION precheckFastFrames=%d precheckSlowFrames=%d selected=%d\n", fastFrames, slowFrames, expected);
    return true;
}

bool TestSlowSetupResumesAndToggleResets() {
    if (!PrepareDispatch(2, 8)) return false;
    preliminaryCost = 3000000;
    setupCost = 3000000;
    insideQueryCost = 2000000;
    enumerationCost = 2000000;
    g_PulseAction = -1;
    StepBattleDecision(MainTask(), BattleContext(), &TestClock);
    if (!Expect(g_DeploymentPlanning && g_DeploymentJob.nextRosterRow == 1 &&
                g_DeploymentJob.activeCandidate.active && !g_PendingDeployment,
                "slow snapshot/setup still admits a bounded roster operation on the first eligible frame")) return false;
    for (int reset = 0; reset < 3; ++reset) {
        // Same planning reset used by F1, without touching windows or audio.
        g_Enabled = false; ResetAutoPlayTurnTracking();
        g_Enabled = true; ResetAutoPlayTurnTracking();
        g_CooldownTicks = 0; g_StateStuckTicks = 3;
        g_PulseAction = -1;
        StepBattleDecision(MainTask(), BattleContext(), &TestClock);
        if (!g_DeploymentPlanning || g_PendingDeployment || g_DeploymentJob.nextRosterRow != 1) return false;
    }
    const auto before = g_DeploymentJob.activeCandidate.nextSkill;
    const int frames = DispatchFrames();
    if (!Expect(frames > 1 && g_DeploymentJob.SelectedRoster() == 1 &&
                g_DeploymentJob.SelectedIsHealer() && g_PulseAction == ACT_CONFIRM &&
                !g_BasePanelExhaustedThisTurn && before == 0,
                "slow work resumes after repeated F1-equivalent resets and confirms the same healer")) return false;
    return Expect(peak.work <= 8192 && peak.previews <= 8 && peak.effects <= 32 && peak.paths <= 4,
                  "full-handler progress guarantee preserves all shared hard resource limits");
}

bool TestCombatSelectionAndConfirmationAfterOverruns() {
    preliminaryCost = insideQueryCost = setupCost = enumerationCost = 0;
    testNow = firstBudgetStart = 0;
    if (!ResetWorld(800)) return false;
    InstallUnit(kAlly, 9, 10, 0, 800, 1000, 50, 100, false, false);
    InstallUnit(kEnemy, 11, 10, 3, 500, 1000, 50, 100, false, false);
    *reinterpret_cast<uint8_t*>(g_Image + RVA::ADDR_MapGrid + 0x1740) = 0;
    *reinterpret_cast<int32_t*>(MainTask() + 0x14) = 0x02;
    *reinterpret_cast<int32_t*>(MainTask() + 0x2C) = 0;
    *reinterpret_cast<int32_t*>(MainTask() + 0x11E0 + 0x14) = 0;
    NativeSkills::fnEnumerateSkills = &TimedEnumeration;
    fnGetUnitFaction = &TimedFaction;
    preliminaryCost = setupCost = enumerationCost = 2000000;
    insideQueryCost = 200000;
    bool selected = false;
    for (int frame = 0; frame < 100 && !selected; ++frame) {
        g_PulseAction = -1;
        StepBattleDecision(MainTask(), BattleContext(), &TestClock);
        selected = SmartCombat::pending.valid && g_PulseAction == ACT_CONFIRM;
    }
    if (!Expect(selected && SmartCombat::pending.candidate.kind == Combat::ActionKind::BasicAttack &&
                SmartCombat::pending.candidate.actorIndex == kActor && g_NativePreviewCalls == 1 &&
                g_EffectCallbackCalls == 1,
                "slow map checks and setup preserve the complete combat decision without duplicate or lost previews")) return false;
    *reinterpret_cast<int32_t*>(MainTask() + 0x14) = 0x03;
    g_CooldownTicks = 0;
    bool commandSent = false;
    for (int frame = 0; frame < 20 && !commandSent; ++frame) {
        g_PulseAction = -1;
        StepBattleDecision(MainTask(), BattleContext(), &TestClock);
        commandSent = g_PulseAction == ACT_CONFIRM;
    }
    if (!commandSent) return false;
    *reinterpret_cast<int32_t*>(MainTask() + 0x14) = 0x08;
    *reinterpret_cast<int32_t*>(BattleContext() + 0x94C) = 1;
    *reinterpret_cast<uint16_t*>(BattleContext() + 0x74C) = kEnemy;
    g_CooldownTicks = 0;
    for (int frame = 0; frame < 30 && !SmartCombat::confirmationSent; ++frame)
        StepBattleDecision(MainTask(), BattleContext(), &TestClock);
    if (!Expect(SmartCombat::confirmationSent && g_PulseAction == ACT_CONFIRM &&
                NativeCombat::QueueCount(BattleContext()) == 0 && SmartCombat::accepted.empty(),
                "confirmation completes after validation crosses the deadline and still awaits engine queue acceptance")) return false;
    const auto action = SmartCombat::pending.candidate;
    InstallAcceptedQueue(action);
    return Expect(SmartCombat::ObserveQueue(BattleContext()) && SmartCombat::accepted.size() == 1,
                  "the engine queue entry remains the only acceptance signal after budget overruns");
}

int startReports = 0, progressReports = 0;
uint64_t lastReportTime = 0;
bool reportingTooOften = false;
void CaptureReport(const char*, const Planning::Telemetry&, const char* reason) {
    if (std::strcmp(reason, "start") == 0) { ++startReports; lastReportTime = testNow; }
    if (std::strcmp(reason, "progress") == 0) {
        if (testNow - lastReportTime < 1000000000ull) reportingTooOften = true;
        ++progressReports; lastReportTime = testNow;
    }
}
bool TestRateLimitedProgress() {
    Planning::Telemetry timing;
    startReports = progressReports = 0; reportingTooOften = false;
    const auto originalReporter = Planning::reporter;
    Planning::reporter = &CaptureReport;
    for (int frame = 0; frame < 25; ++frame) {
        testNow += 100000000;
        Planning::PlanningBudget budget(TestClock); budget.limits.work = 1;
        Planning::Scope scope(budget);
        Planning::Run("deployment", timing, [] { return false; }, [&] {
            Planning::Unit operation;
            if (operation) ++timing.cursor[0];
        });
    }
    Planning::reporter = originalReporter;
    return Expect(startReports == 1 && progressReports == 2 && !reportingTooOften && timing.cursor[0] == 25,
                  "unfinished planning reports start and cursor progress at most once per second");
}
}

int main() {
    if (!OpenIsolatedTestLog()) return 1;
    const bool passed = TestSlowPrechecksDispatch() && TestSlowSetupResumesAndToggleResets() &&
        TestCombatSelectionAndConfirmationAfterOverruns() && TestRateLimitedProgress();
    return passed ? 0 : 1;
}
