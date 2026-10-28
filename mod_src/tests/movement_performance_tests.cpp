#define main NativeMovementOriginalSuite
#include "native_tactics_tests.cpp"
#undef main

namespace {
uint64_t movementClockNow = 0;
uint64_t MovementClock() { return movementClockNow; }
bool CompareMovementScheduling() {
    if (!ResetWorld(10, 2)) return false;
    *(int32_t*)(g_FakeImage + RVA::ADDR_PathGrid + 0x30) = 32;
    *(int32_t*)(g_FakeImage + RVA::ADDR_PathGrid + 0x34) = 24;
    for (int z = 0; z <= 4; ++z) SetFlag(12, z, 0xFC);
    SetCost(10, 3, 1);
    TacticalPath::CellMask goals{};
    SetGoal(goals, 13, 2);
    const auto state = Snapshot(10, 2);
    MovementJob slow;
    if (!slow.Begin(kActor, state, goals)) return false;
    int slowFrames = 0;
    while (!slow.IsComplete() && slowFrames < 10000) { ++slowFrames; slow.Step(1, 1, 1); }
    int slowX = -1, slowZ = -1;
    if (!slow.GetResult(slowX, slowZ)) return false;
    MovementJob fast;
    if (!fast.Begin(kActor, state, goals)) return false;
    Planning::Telemetry timing;
    int fastFrames = 0;
    while (!fast.IsComplete() && fastFrames < 10000) {
        ++fastFrames;
        Planning::PlanningBudget budget;
        Planning::Scope scope(budget);
        const int calls = g_PathCalls;
        Planning::Run("movement", timing, [&] { return fast.IsComplete(); }, [&] { fast.Step(); });
        if (budget.used.paths > 4 || g_PathCalls - calls != static_cast<int>(budget.used.paths) ||
            budget.used.work > 8192) return false;
    }
    int x = -1, z = -1;
    if (!Expect(fast.GetResult(x, z) && x == slowX && z == slowZ && x == 10 && z == 3,
                "shared-budget movement preserves obstacle detour and exact endpoint across batch sizes")) return false;
    if (!Expect(PathStateIsOriginal(), "batched path probes restore the entire original path state")) return false;
    if (!Expect(fastFrames * 5 < slowFrames, "batched graph and route processing removes frame-boundary delays")) return false;
    std::printf("MOVEMENT {\"singleWorkFrames\":%d,\"batchedFrames\":%d,\"width\":32,\"height\":24}\n", slowFrames, fastFrames);

    // Reusing the same job with a smaller map cannot retain old graph cells.
    *(int32_t*)(g_FakeImage + RVA::ADDR_PathGrid + 0x30) = 12;
    if (!fast.Begin(kActor, state, goals) || !RunToCompletion(fast)) return false;
    return Expect(!fast.GetResult(x, z), "smaller map rebuild clears passability outside verified dimensions");
}

bool TestExpiredSetupStillMakesRouteProgress() {
    if (!ResetWorld(10, 2)) return false;
    *(int32_t*)(g_FakeImage + RVA::ADDR_PathGrid + 0x30) = 16;
    *(int32_t*)(g_FakeImage + RVA::ADDR_PathGrid + 0x34) = 8;
    for (int z = 0; z <= 4; ++z) SetFlag(12, z, 0xFC);
    SetCost(10, 3, 1);
    TacticalPath::CellMask goals{}; SetGoal(goals, 13, 2);
    MovementJob job;
    if (!job.Begin(kActor, Snapshot(10, 2), goals)) return false;
    Planning::Telemetry timing;
    int frames = 0;
    while (!job.IsComplete() && frames++ < 1000) {
        Planning::PlanningBudget budget(MovementClock); Planning::Scope scope(budget);
        movementClockNow += 2000000; // Model slow setup/fingerprint before Run.
        Planning::Run("movement", timing, [&] { return job.IsComplete(); }, [&] { job.Step(); });
        if (budget.used.work > 1 || budget.used.paths > 1) return false;
    }
    int x = -1, z = -1;
    return Expect(job.GetResult(x, z) && x == 10 && z == 3 && frames > 1 && PathStateIsOriginal(),
                  "movement resumes one admitted unit per expired frame and preserves the exact legal detour");
}
}
int main() { return CompareMovementScheduling() && TestExpiredSetupStillMakesRouteProgress() ? 0 : 1; }
