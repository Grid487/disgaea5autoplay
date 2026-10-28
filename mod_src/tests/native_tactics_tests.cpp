#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../native_tactics.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace AutoPlay {
void LogMsg(const char*, ...) {}
}

namespace {

using namespace AutoPlay;
using namespace AutoPlay::Combat;
using namespace AutoPlay::NativeTactics;

constexpr size_t kFakeImageSize = 128ull * 1024ull * 1024ull;
constexpr uintptr_t kCharaOffset = 0x04000000;
constexpr int kActor = 5;

uintptr_t g_FakeImage = 0;
int g_MoveRangeCalls = 0;
int g_PathCalls = 0;
bool g_PathSawNativeSentinel = false;
bool g_PathRejects = false;
bool g_PathWritesWrongEndpoint = false;
int g_JumpStat = 1;

size_t NativeIndex(int x, int z) {
    return static_cast<size_t>(x) * 128 + static_cast<size_t>(z);
}

uintptr_t Chara(int index) {
    return g_FakeImage + kCharaOffset + static_cast<uintptr_t>(index) * 0x4000;
}

void SetCost(int x, int z, uint8_t cost) {
    *(uint8_t*)(g_FakeImage + RVA::ADDR_MoveCostGrid + NativeIndex(x, z)) = cost;
}

void SetFlag(int x, int z, uint8_t flag) {
    *(uint8_t*)(g_FakeImage + RVA::ADDR_TileFlagGrid + NativeIndex(x, z)) = flag;
}

void SetHeight(int x, int z, int16_t height) {
    *(int16_t*)(g_FakeImage + NativeTactics::kHeightGridRva + NativeIndex(x, z) * sizeof(int16_t)) = height;
}

int StubFaction(void* unit, uint8_t) {
    const uintptr_t chara = *(uintptr_t*)(reinterpret_cast<uintptr_t>(unit) + 0x240);
    return chara ? *(uint8_t*)(chara + 0x2E32) : -1;
}

bool StubCanMove(void*) { return true; }
bool StubHasAction(void*) { return true; }
int StubMoveStat(void*) { return 6; }
int StubJumpStat(void*) { return g_JumpStat; }
void StubPackedPosition(void*, uint32_t* out) { *out = 0x12345678; }

void StubMoveRange(void*, void*, void* unit, uint32_t, int) {
    ++g_MoveRangeCalls;
    const uintptr_t address = reinterpret_cast<uintptr_t>(unit) + AutoPlay::UNIT_PATH_STATE_OFFSET;
    std::memset(reinterpret_cast<void*>(address), 0x5A, AutoPlay::UNIT_PATH_STATE_SIZE);
}

bool StubComputePath(void* unit, int targetX, int targetZ, uint8_t) {
    ++g_PathCalls;
    const uintptr_t address = reinterpret_cast<uintptr_t>(unit);
    g_PathSawNativeSentinel = g_PathSawNativeSentinel ||
        *(int32_t*)(address + AutoPlay::UNIT_PATH_STATE_OFFSET) == 128;
    if (g_PathRejects) return false;
    *(int16_t*)(address + 0x416) = 1;
    *(int16_t*)(address + 0x41C) = static_cast<int16_t>(
        targetX + (g_PathWritesWrongEndpoint ? 1 : 0));
    *(int16_t*)(address + 0x51C) = static_cast<int16_t>(targetZ);
    return true;
}

bool Expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        return false;
    }
    std::printf("PASS: %s\n", label);
    return true;
}

bool EnsureImage() {
    if (g_FakeImage) return true;
    void* image = VirtualAlloc(nullptr, kFakeImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) {
        std::fprintf(stderr, "VirtualAlloc fake image failed (%lu)\n", GetLastError());
        return false;
    }
    g_FakeImage = reinterpret_cast<uintptr_t>(image);
    AutoPlay::g_Base = g_FakeImage;
    return true;
}

void InitializeUnit(int index, int faction, int gridX, int gridZ, uint8_t characterType = 0) {
    const uintptr_t unit = AutoPlay::GetUnitPtr(index);
    const uintptr_t chara = Chara(index);
    std::memset(reinterpret_cast<void*>(unit), 0, 0x1260);
    std::memset(reinterpret_cast<void*>(chara), 0, 0x4000);
    *(uintptr_t*)(unit + 0x240) = chara;
    *(uint8_t*)(unit + 0x2E8) = 1;
    *(int16_t*)(unit + 0x2EE) = -1;
    *(float*)(unit + 0x08) = static_cast<float>(gridX * 12 + 6);
    *(float*)(unit + 0x10) = static_cast<float>(gridZ * 12 + 6);
    *(uint8_t*)(chara + 0x2E32) = static_cast<uint8_t>(faction);
    *(uint8_t*)(chara + 0x2E2A) = characterType;
    *(int64_t*)(chara + 0x2CC8) = 100;
    *(int64_t*)(chara + 0x2CD8) = 100;
}

bool ResetWorld(int actorX = 10, int actorZ = 10) {
    if (!EnsureImage()) return false;
    const uintptr_t base = g_FakeImage;
    std::memset(reinterpret_cast<void*>(base + RVA::ADDR_BattleUnitArray), 0, 256 * 0x1260);
    std::memset(reinterpret_cast<void*>(base + RVA::ADDR_MapGrid), 0, 0x2000);
    std::memset(reinterpret_cast<void*>(base + RVA::ADDR_PathGrid), 0,
                NativeTactics::kNativeBlockerOffset + 128 * 128);
    std::memset(reinterpret_cast<void*>(base + RVA::ADDR_TileFlagGrid), 0, 128 * 128);
    std::memset(reinterpret_cast<void*>(base + RVA::ADDR_MoveCostGrid), 0xFF, 128 * 128);
    std::memset(reinterpret_cast<void*>(base + NativeTactics::kHeightGridRva), 0,
                128 * 128 * sizeof(int16_t));
    for (int x = 0; x < 128; ++x)
        for (int z = 0; z < 128; ++z) SetHeight(x, z, 1);

    uintptr_t pathGrid = base + RVA::ADDR_PathGrid;
    *(int32_t*)(pathGrid + 0x30) = 128;
    *(int32_t*)(pathGrid + 0x34) = 128;
    *(uint8_t*)(base + RVA::ADDR_MapGrid + 0x1740) = 0;
    InitializeUnit(kActor, 0, actorX, actorZ);

    AutoPlay::g_Base = base;
    AutoPlay::fnGetUnitFaction = &StubFaction;
    AutoPlay::fnCanUnitMove = &StubCanMove;
    AutoPlay::fnHasActionRemaining = &StubHasAction;
    AutoPlay::fnGetUnitMoveStat = &StubMoveStat;
    AutoPlay::fnGetUnitPackedXZ = &StubPackedPosition;
    AutoPlay::fnComputeMoveRange = &StubMoveRange;
    AutoPlay::fnComputePathToTile = &StubComputePath;
    NativeTactics::g_JumpStatOverride = &StubJumpStat;
    g_MoveRangeCalls = 0;
    g_PathCalls = 0;
    g_PathSawNativeSentinel = false;
    g_PathRejects = false;
    g_PathWritesWrongEndpoint = false;
    g_JumpStat = 1;
    std::memset(reinterpret_cast<void*>(AutoPlay::GetUnitPtr(kActor) +
        AutoPlay::UNIT_PATH_STATE_OFFSET), 0xA5, AutoPlay::UNIT_PATH_STATE_SIZE);
    return true;
}

BattleSnapshot Snapshot(int actorX, int actorZ) {
    BattleSnapshot battle{};
    battle.units[kActor].index = kActor;
    battle.units[kActor].faction = Faction::Player;
    battle.units[kActor].alive = true;
    battle.units[kActor].deployed = true;
    battle.units[kActor].canMove = true;
    battle.units[kActor].gridX = actorX;
    battle.units[kActor].gridZ = actorZ;
    return battle;
}

void SetGoal(TacticalPath::CellMask& goals, int x, int z) {
    goals[static_cast<size_t>(TacticalPath::CellIndex(x, z))] = 1;
}

bool PathStateIsOriginal() {
    const uintptr_t path = AutoPlay::GetUnitPtr(kActor) + AutoPlay::UNIT_PATH_STATE_OFFSET;
    for (size_t i = 0; i < AutoPlay::UNIT_PATH_STATE_SIZE; ++i)
        if (*(uint8_t*)(path + i) != 0xA5) return false;
    return true;
}

bool RunToCompletion(MovementJob& job, size_t maxSteps = 1000) {
    size_t steps = 0;
    while (!job.IsComplete() && steps++ < maxSteps) {
        const bool completed = job.Step();
        if (job.LastGraphCellsProcessed() > 256 || job.LastRouteNodesProcessed() > 256 ||
            job.LastEndpointCellsScanned() > 256 || job.LastPathProbes() > 4) return false;
        if (completed && !job.IsComplete()) return false;
    }
    return job.IsComplete();
}

bool TestDetourUsesPathDistanceAndNativeProbeRestoresPathState() {
    if (!ResetWorld(10, 2)) return false;
    for (int z = 0; z <= 4; ++z) SetFlag(12, z, 0xFC); // Direct east route is blocked.
    SetCost(10, 3, 1);
    InitializeUnit(10, 3, 14, 2);
    BattleSnapshot battle = Snapshot(10, 2);
    battle.units[10].index = 10;
    battle.units[10].faction = Faction::Enemy;
    battle.units[10].alive = true;
    battle.units[10].deployed = true;
    battle.units[10].gridX = 14;
    battle.units[10].gridZ = 2;
    TacticalPath::CellMask goals{};
    SetGoal(goals, 13, 2); // Open action position beside the occupied enemy at (14,2).

    MovementJob job{};
    if (!Expect(job.Begin(kActor, battle, goals), "movement job starts with native grids and action-position goals")) return false;
    if (!Expect(g_MoveRangeCalls == 1 && PathStateIsOriginal(),
                "native move-range runs once and its BattleUnit path writes are restored")) return false;
    if (!Expect(RunToCompletion(job), "incremental graph, route field and endpoint scan complete")) return false;

    int targetX = -1, targetZ = -1;
    if (!Expect(job.GetResult(targetX, targetZ), "engine path probe returns a legal movement endpoint")) return false;
    const int currentManhattan = std::abs(10 - 13) + std::abs(2 - 2);
    const int targetManhattan = std::abs(targetX - 13) + std::abs(targetZ - 2);
    return Expect(targetX == 10 && targetZ == 3 && targetManhattan > currentManhattan &&
                  job.RouteDistance(targetX, targetZ) < job.CurrentRouteDistance() &&
                  g_PathSawNativeSentinel && PathStateIsOriginal(),
                  "route distance permits a safe detour that first increases Manhattan distance");
}

bool TestExactNativePathEndpointIsRequired() {
    if (!ResetWorld(10, 10)) return false;
    SetCost(11, 10, 1);
    g_PathWritesWrongEndpoint = true;
    TacticalPath::CellMask goals{};
    SetGoal(goals, 13, 10);
    MovementJob job{};
    if (!Expect(job.Begin(kActor, Snapshot(10, 10), goals), "exact-path test job begins")) return false;
    if (!Expect(RunToCompletion(job), "path-mismatch job reaches a completed safe result")) return false;
    return Expect(!job.Succeeded() && g_PathCalls == 1 && PathStateIsOriginal(),
                  "native path result with a mismatched endpoint is rejected and path state restored");
}

bool TestOccupiedAndBasePanelTilesAreNeverProbed() {
    if (!ResetWorld(10, 10)) return false;
    SetCost(11, 10, 1);
    InitializeUnit(11, 3, 11, 10);
    BattleSnapshot occupiedBattle = Snapshot(10, 10);
    occupiedBattle.units[11].index = 11;
    occupiedBattle.units[11].faction = Faction::Enemy;
    occupiedBattle.units[11].alive = true;
    occupiedBattle.units[11].deployed = true;
    occupiedBattle.units[11].gridX = 11;
    occupiedBattle.units[11].gridZ = 10;
    TacticalPath::CellMask goals{};
    SetGoal(goals, 13, 10);
    MovementJob occupiedJob{};
    if (!Expect(occupiedJob.Begin(kActor, occupiedBattle, goals), "occupied-tile job begins")) return false;
    if (!Expect(RunToCompletion(occupiedJob), "occupied-tile scan completes incrementally")) return false;
    if (!Expect(g_PathCalls == 0 && !occupiedJob.Succeeded(),
                "occupied endpoint is excluded before any native path probe")) return false;

    if (!ResetWorld(10, 10)) return false;
    SetCost(11, 10, 1);
    uintptr_t mapGrid = g_FakeImage + RVA::ADDR_MapGrid;
    *(uint8_t*)(mapGrid + 0x1740) = 1;
    *(float*)(mapGrid + 0x1754) = static_cast<float>(11 * 12 + 6);
    *(float*)(mapGrid + 0x1758) = static_cast<float>(10 * 12 + 6);
    MovementJob basePanelJob{};
    if (!Expect(basePanelJob.Begin(kActor, Snapshot(10, 10), goals), "Base Panel exclusion job begins")) return false;
    if (!Expect(RunToCompletion(basePanelJob), "Base Panel scan completes incrementally")) return false;
    return Expect(g_PathCalls == 0 && !basePanelJob.Succeeded(),
                  "Base Panel endpoint is excluded before any native path probe");
}

bool TestBasePanelActorCanLeaveWithoutStrictRouteProgress() {
    if (!ResetWorld(10, 10)) return false;
    SetCost(10, 9, 1);
    uintptr_t mapGrid = g_FakeImage + RVA::ADDR_MapGrid;
    *(uint8_t*)(mapGrid + 0x1740) = 1;
    *(float*)(mapGrid + 0x1754) = static_cast<float>(10 * 12 + 6);
    *(float*)(mapGrid + 0x1758) = static_cast<float>(10 * 12 + 6);
    TacticalPath::CellMask goals{};
    SetGoal(goals, 10, 10);
    MovementJob job{};
    if (!Expect(job.Begin(kActor, Snapshot(10, 10), goals) && job.OnBasePanel(),
                "Base Panel actor is recognized as the movement exception")) return false;
    if (!Expect(RunToCompletion(job), "Base Panel exception job completes")) return false;
    int x = -1, z = -1;
    return Expect(job.GetResult(x, z) && x == 10 && z == 9 &&
                  job.RouteDistance(x, z) > job.CurrentRouteDistance(),
                  "actor can leave its Base Panel even when route distance does not strictly decrease");
}

bool TestHeightEdgesAndFlyingBypass() {
    if (!ResetWorld(10, 10)) return false;
    for (int z = 0; z < 128; ++z) SetHeight(11, z, 3);
    TacticalPath::CellMask goals{};
    SetGoal(goals, 12, 10);

    MovementJob grounded{};
    if (!Expect(grounded.Begin(kActor, Snapshot(10, 10), goals), "grounded height-edge job begins")) return false;
    if (!Expect(RunToCompletion(grounded), "grounded height-edge search completes")) return false;
    if (!Expect(!grounded.IsFlyingActor() && grounded.JumpStat() == 1 &&
                grounded.CopiedHeight(10, 10) == 1 && grounded.CopiedHeight(11, 10) == 3 &&
                grounded.CopiedHeight(12, 10) == 1,
                "height snapshot uses the native x*128+z int16 grid layout")) return false;
    if (!Expect(grounded.RouteDistance(10, 10) == TacticalPath::kInfiniteDistance,
                "grounded route cannot cross an edge above the jump allowance")) return false;

    if (!ResetWorld(10, 10)) return false;
    for (int z = 0; z < 128; ++z) SetHeight(11, z, 3);
    InitializeUnit(kActor, 0, 10, 10, 2); // Character movement type 2 bypasses height edges.
    MovementJob flying{};
    if (!Expect(flying.Begin(kActor, Snapshot(10, 10), goals), "flying height-edge job begins")) return false;
    if (!Expect(RunToCompletion(flying), "flying height-edge search completes")) return false;
    return Expect(flying.RouteDistance(10, 10) != TacticalPath::kInfiniteDistance,
                  "verified flying type bypasses native height-edge limits");
}

bool TestEnemyExposureBreaksEqualRouteDistance() {
    if (!ResetWorld(0, 0)) return false;
    SetCost(10, 9, 1);
    SetCost(9, 10, 1);
    InitializeUnit(10, 3, 10, 8);
    BattleSnapshot battle = Snapshot(0, 0);
    battle.units[10].index = 10;
    battle.units[10].faction = Faction::Enemy;
    battle.units[10].alive = true;
    battle.units[10].deployed = true;
    battle.units[10].gridX = 10;
    battle.units[10].gridZ = 8;
    TacticalPath::CellMask goals{};
    SetGoal(goals, 10, 10);
    MovementJob job{};
    if (!Expect(job.Begin(kActor, battle, goals), "exposure tie-break job begins")) return false;
    if (!Expect(RunToCompletion(job), "exposure tie-break job completes")) return false;
    int x = -1, z = -1;
    if (!Expect(job.GetResult(x, z), "at least one equal-distance endpoint is accepted")) return false;
    return Expect(job.RouteDistance(x, z) == 1 && x == 9 && z == 10,
                  "among equal route distances, endpoint farther from enemies wins");
}

bool TestPerFrameBudgetsAndSingleRangeQuery() {
    if (!ResetWorld(0, 0)) return false;
    TacticalPath::CellMask goals{};
    SetGoal(goals, 127, 127);
    for (int x = 1; x <= 8; ++x) SetCost(x, 0, 1);
    MovementJob job{};
    if (!Expect(job.Begin(kActor, Snapshot(0, 0), goals), "budget test job begins")) return false;
    bool sawFourProbesInOneStep = false;
    bool budgetsStayedBounded = true;
    size_t steps = 0;
    while (!job.IsComplete() && steps++ < 1000) {
        const int callsBefore = g_PathCalls;
        job.Step(10000, 10000, 10000); // Caller cannot override hard frame budgets.
        const int newCalls = g_PathCalls - callsBefore;
        if (newCalls == 4) sawFourProbesInOneStep = true;
        budgetsStayedBounded = budgetsStayedBounded &&
            job.LastGraphCellsProcessed() <= 256 &&
            job.LastRouteNodesProcessed() <= 256 &&
            job.LastEndpointCellsScanned() <= 256 &&
            job.LastPathProbes() <= 4 && newCalls <= 4;
    }
    if (!Expect(budgetsStayedBounded,
                "every Step stays within graph, BFS, scan and native-probe budgets")) return false;
    int x = -1, z = -1;
    return Expect(job.IsComplete() && job.GetResult(x, z) && x == 8 && z == 0 &&
                  sawFourProbesInOneStep && g_MoveRangeCalls == 1 && g_PathCalls == 8,
                  "incremental job finds the best endpoint with one move-range call and bounded probes");
}

} // namespace

int main() {
    if (!EnsureImage()) return 1;
    const bool passed =
        TestDetourUsesPathDistanceAndNativeProbeRestoresPathState() &&
        TestExactNativePathEndpointIsRequired() &&
        TestOccupiedAndBasePanelTilesAreNeverProbed() &&
        TestBasePanelActorCanLeaveWithoutStrictRouteProgress() &&
        TestHeightEdgesAndFlyingBypass() &&
        TestEnemyExposureBreaksEqualRouteDistance() &&
        TestPerFrameBudgetsAndSingleRangeQuery();
    VirtualFree(reinterpret_cast<void*>(g_FakeImage), 0, MEM_RELEASE);
    AutoPlay::g_Base = 0;
    g_FakeImage = 0;
    return passed ? 0 : 1;
}
