#define main SmartRuntimeOriginalSuite
#include "smart_runtime_tests.cpp"
#undef main
#include "planning_benchmark.h"

namespace {
uint64_t g_TestNow = 0;
uint64_t FrozenClock() { return g_TestNow; }

bool TestBudgetAndDeadline() {
    {
        Planning::PlanningBudget budget(FrozenClock); Planning::Scope scope(budget);
        for (int i = 0; i < 8; ++i) {
            Planning::Unit unit(Planning::Resource::Preview);
            if (!unit || !Planning::Preview()) return false;
        }
        Planning::Unit ninth(Planning::Resource::Preview);
        if (!Expect(!ninth && budget.used.previews == 8, "shared preview cap stops before a ninth native query")) return false;
    }
    {
        Planning::PlanningBudget budget(FrozenClock); Planning::Scope scope(budget);
        for (int i = 0; i < 32; ++i) if (!Planning::Effect()) return false;
        if (!Expect(!Planning::Effect() && budget.used.effects == 32, "shared effect cap stops at 32")) return false;
    }
    {
        Planning::PlanningBudget budget(FrozenClock); Planning::Scope scope(budget);
        for (int i = 0; i < 4; ++i) if (!Planning::Path()) return false;
        if (!Expect(!Planning::Path() && budget.used.paths == 4, "shared path cap stops at four")) return false;
    }
    {
        Planning::PlanningBudget budget(FrozenClock); Planning::Scope scope(budget);
        for (int i = 0; i < 8192; ++i) if (!Planning::Work()) return false;
        if (!Expect(!Planning::Work() && budget.used.work == 8192, "shared lightweight work cap stops at 8192")) return false;
    }
    Planning::PlanningBudget deadline(FrozenClock); Planning::Scope scope(deadline);
    if (!Planning::Work()) return false;
    g_TestNow += 1000000;
    if (!Expect(!Planning::Work() && deadline.used.work == 1, "injected deadline yields after the first admitted unit")) return false;
    {
        Planning::PlanningBudget setup(FrozenClock); Planning::Scope inner(setup);
        g_TestNow += 2000000;
        {
            Planning::Unit candidate(Planning::Resource::Preview);
            if (!candidate) return false;
            g_TestNow += 2000000;
            Planning::Unit nativeCall(Planning::Resource::Preview);
            if (!Expect(nativeCall && Planning::Preview(), "admitted candidate finishes native validation after a deadline crossing")) return false;
        }
        Planning::Unit next(Planning::Resource::Effect);
        if (!Expect(!next && setup.used.work == 1 && setup.used.previews == 1,
                    "slow setup guarantees one atomic unit then yields without spending another effect")) return false;
    }
    if (!Expect(Planning::active == &deadline, "nested scopes restore the shared frame budget")) return false;
    {
        Planning::PlanningBudget empty(FrozenClock); empty.limits.previews = 0;
        Planning::Scope inner(empty);
        g_TestNow += 2000000;
        Planning::Unit invalid(Planning::Resource::Preview);
        if (!Expect(!invalid && empty.used.work == 0 && empty.used.previews == 0,
                    "first-unit progress guarantee cannot bypass a hard resource cap")) return false;
    }
    {
        Planning::PlanningBudget noOp(FrozenClock); noOp.limits.work = 8;
        Planning::Scope inner(noOp);
        Planning::Telemetry timing;
        int passes = 0;
        Planning::Run("no-op", timing, [] { return false; }, [&] { ++passes; });
        if (!Expect(passes == 8 && noOp.yield == Planning::YieldReason::PassLimit,
                    "scheduler-only transitions remain bounded without consuming real work admissions")) return false;
    }
    return true;
}

bool TestCacheReuseAndInvalidation() {
    if (!ResetWorld(800)) return false;
    InstallUnit(kEnemy, 30, 30, 3, 1000, 1000, 100, 100, false, false);
    for (int frame = 0; frame < 1000 && !TeamPlanning::current.complete; ++frame) {
        Planning::PlanningBudget b; Planning::Scope s(b); TeamPlanning::Run(BattleContext());
    }
    if (!Expect(TeamPlanning::current.complete && !TeamPlanning::current.best.valid,
                "full search records that no immediate team action exists")) return false;
    const auto original = TeamPlanning::current.fingerprint;
    FinishSmartUnit(kActor);
    if (!Expect(TeamPlanning::current.complete && TeamPlanning::current.fingerprint == original,
                "finishing a unit retains the unchanged complete search and catalogs")) return false;
    const int calls = g_NativePreviewCalls;
    {
        Planning::PlanningBudget b; Planning::Scope s(b);
        if (!TeamPlanning::Run(BattleContext())) return false;
    }
    if (!Expect(g_NativePreviewCalls == calls && TeamPlanning::current.timing.reuses > 0,
                "cache reuse does not repeat native previews")) return false;
    const auto changed = [&](uintptr_t address, const char* reason) {
        const uint8_t old = *reinterpret_cast<uint8_t*>(address);
        *reinterpret_cast<uint8_t*>(address) ^= 1;
        const bool different = TeamPlanning::Fingerprint(BattleContext()) != original;
        *reinterpret_cast<uint8_t*>(address) = old;
        return Expect(different, reason);
    };
    if (!changed(Chara(kActor) + 0xCCE, "saved skill-area changes invalidate cached planning") ||
        !changed(Chara(kActor) + 0xCC9, "learned skill-power changes invalidate cached planning") ||
        !changed(Chara(kActor) + 0x2CD0, "SP changes invalidate cached planning") ||
        !changed(Chara(kEnemy) + 0x2CC8, "target HP changes invalidate cached planning") ||
        !changed(Chara(kEnemy) + 0x2CF8, "combat stat changes invalidate cached planning") ||
        !changed(Unit(kEnemy) + 8, "target position changes invalidate cached planning")) return false;
    *reinterpret_cast<uint8_t*>(Chara(kActor) + 0xCCE) = 1;
    Planning::PlanningBudget b(FrozenClock); Planning::Scope s(b);
    TeamPlanning::Run(BattleContext());
    return Expect(TeamPlanning::current.fingerprint != original, "changed configuration rebuilds the plan before reuse");
}

bool TestSharedBudgetAcrossPhases() {
    BenchmarkWorld(1, 12, 3, false);
    Planning::PlanningBudget budget(FrozenClock);
    Planning::Scope scope(budget);
    TeamPlanning::Run(BattleContext());
    const auto used = budget.used;
    if (!Expect(used.previews == 8 && !TeamPlanning::current.complete,
                "team planning yields at the shared eight-preview limit")) return false;
    Planning::Telemetry timing;
    int otherCalls = 0;
    const bool completed = Planning::Run("second phase", timing, [] { return false; }, [&] { ++otherCalls; });
    return Expect(!completed && otherCalls == 0 && budget.used.previews == 8,
                  "a second phase cannot reset or exceed the same frame budget");
}

bool TestMovementAnimationIsPending() {
    if (!ResetWorld()) return false;
    BeginPendingMove(kActor, 11, 10);
    g_PendingMoveConfirmSent = true;
    *reinterpret_cast<int32_t*>(MainTask() + 0x2C) = 0;
    FinishPendingMoveIfObserved(MainTask(), BattleContext(), 0x06);
    if (!Expect(g_PendingMove && !g_UnitMovedThisTurn[kActor], "state06 preserves movement pending without reporting rejection")) return false;
    *reinterpret_cast<int32_t*>(MainTask() + 0x11E0 + 0x14) = -1;
    FinishPendingMoveIfObserved(MainTask(), BattleContext(), 0x02);
    if (!Expect(g_PendingMove, "unstable map substate still waits for movement resolution")) return false;
    *reinterpret_cast<float*>(Unit(kActor) + 8) = 11 * 12.f + 6.f;
    *reinterpret_cast<int32_t*>(MainTask() + 0x11E0 + 0x14) = 0;
    FinishPendingMoveIfObserved(MainTask(), BattleContext(), 0x02);
    return Expect(!g_PendingMove && g_UnitMovedThisTurn[kActor], "arrival after movement animation records exactly one accepted move");
}
}

int main() {
    if (!OpenIsolatedTestLog()) return 1;
    const bool passed = TestBudgetAndDeadline() && TestSharedBudgetAcrossPhases() && TestCacheReuseAndInvalidation() &&
        TestMovementAnimationIsPending() && Benchmarks();
    return passed ? 0 : 1;
}
