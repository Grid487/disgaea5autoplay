// Offline state-machine checks. This includes the implementation so tests can
// exercise internal helpers without invoking DllMain or a real game process.
#include "../xinput_proxy.cpp"

#include <cstdio>
#include <cstring>

namespace {

constexpr size_t kImageSize = 0x08000000;
constexpr uintptr_t kTaskOffset = 0x06000000;
constexpr uintptr_t kContextOffset = 0x07800000;
constexpr int kPlayer = 16;
constexpr int kEnemy = 0;
constexpr int kOtherPlayer = 17;
constexpr int kTargetX = 11;
constexpr int kTargetZ = 10;

bool g_HasCandidate = true;
uintptr_t MainTask() { return AutoPlay::g_Base + kTaskOffset; }
uintptr_t BattleContext() { return AutoPlay::g_Base + kContextOffset; }
uintptr_t Unit(int index) { return AutoPlay::GetUnitPtr(index); }

void StubComputeMoveRange(void*, void*, void* unit, uint32_t, int) {
    std::memset(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(unit) + 0x416), 0x5A,
                AutoPlay::UNIT_PATH_STATE_SIZE);
    std::memset(reinterpret_cast<void*>(AutoPlay::g_Base + RVA::ADDR_MoveCostGrid), 0xFF, 128 * 128);
    std::memset(reinterpret_cast<void*>(AutoPlay::g_Base + RVA::ADDR_TileFlagGrid), 0xFC, 128 * 128);
    if (g_HasCandidate) {
        const size_t tile = static_cast<size_t>(kTargetX) * 128 + kTargetZ;
        *(uint8_t*)(AutoPlay::g_Base + RVA::ADDR_MoveCostGrid + tile) = 1;
        *(uint8_t*)(AutoPlay::g_Base + RVA::ADDR_TileFlagGrid + tile) = 0;
    }
}

bool StubComputePath(void* unit, int x, int z, uint8_t) {
    const auto address = reinterpret_cast<uintptr_t>(unit);
    *(int16_t*)(address + 0x416) = 1;
    *(int16_t*)(address + 0x41C) = static_cast<int16_t>(x);
    *(int16_t*)(address + 0x51C) = static_cast<int16_t>(z);
    return true;
}

void StubGetPackedXZ(void*, uint32_t* out) { *out = 0; }
int StubMoveStat(void*) { return 6; }
int StubMapObject(void*) { return 0; }
bool StubControllable(void* unit, int) {
    const auto address = reinterpret_cast<uintptr_t>(unit);
    const auto chara = *(uintptr_t*)(address + 0x240);
    return chara && *(uint8_t*)(chara + 0x2E32) == 0;
}
int StubFaction(void* unit, uint8_t) {
    const auto address = reinterpret_cast<uintptr_t>(unit);
    const auto chara = *(uintptr_t*)(address + 0x240);
    return chara ? *(uint8_t*)(chara + 0x2E32) : -1;
}
bool StubCanMove(void*) { return true; }
bool StubHasAction(void*) { return true; }
bool StubCanAttack(void*, int, uint8_t) { return false; }
float StubTileHeight(float, float) { return 0.0f; }
void StubOriginalExecute(void*, int) {}

void InitializeUnit(int index, int gridX, int gridZ, uint8_t faction, bool deployed = true) {
    const uintptr_t unit = Unit(index);
    const uintptr_t chara = AutoPlay::g_Base + 0x03F00000 + static_cast<uintptr_t>(index) * 0x4000;
    std::memset(reinterpret_cast<void*>(unit), 0, 0x1260);
    std::memset(reinterpret_cast<void*>(chara), 0, 0x4000);
    *(float*)(unit + 0x08) = static_cast<float>(gridX * 12 + 6);
    *(float*)(unit + 0x10) = static_cast<float>(gridZ * 12 + 6);
    *(uintptr_t*)(unit + 0x240) = chara;
    *(uint8_t*)(unit + 0x2E8) = deployed ? 1 : 0;
    *(int16_t*)(unit + 0x2EE) = -1;
    *(uint8_t*)(chara + 0x2E32) = faction;
    *(int64_t*)(chara + 0x2CC8) = deployed ? 100 : 0;
}

bool ResetWorld() {
    if (!AutoPlay::g_Base) {
        auto memory = VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!memory) {
            std::fprintf(stderr, "VirtualAlloc failed (%lu)\n", GetLastError());
            return false;
        }
        AutoPlay::g_Base = reinterpret_cast<uintptr_t>(memory);
    }

    std::memset(reinterpret_cast<void*>(AutoPlay::g_Base + RVA::ADDR_BattleUnitArray), 0,
                256 * 0x1260);
    std::memset(reinterpret_cast<void*>(MainTask()), 0, 0x00220000);
    std::memset(reinterpret_cast<void*>(BattleContext()), 0, 0x1000);
    std::memset(reinterpret_cast<void*>(AutoPlay::g_Base + RVA::ADDR_MapGrid + 0x1700), 0, 0x100);
    std::memset(reinterpret_cast<void*>(AutoPlay::g_Base + RVA::ADDR_MoveCostGrid), 0, 128 * 128);
    std::memset(reinterpret_cast<void*>(AutoPlay::g_Base + RVA::ADDR_TileFlagGrid), 0, 128 * 128);
    *(uintptr_t*)(AutoPlay::g_Base + RVA::PTR_BattleContext) = BattleContext();
    *(int32_t*)(BattleContext() + 0xB8) = kPlayer;

    InitializeUnit(kPlayer, 10, 10, 0);
    InitializeUnit(kEnemy, 14, 10, 3);
    InitializeUnit(kOtherPlayer, 0, 0, 0, false);

    AutoPlay::fnComputeMoveRange = &StubComputeMoveRange;
    AutoPlay::fnComputePathToTile = &StubComputePath;
    AutoPlay::fnGetUnitPackedXZ = &StubGetPackedXZ;
    AutoPlay::fnGetUnitMoveStat = &StubMoveStat;
    AutoPlay::fnIsMapObject = &StubMapObject;
    AutoPlay::fnIsControllableUnit = &StubControllable;
    AutoPlay::fnGetUnitFaction = &StubFaction;
    AutoPlay::fnCanUnitMove = &StubCanMove;
    AutoPlay::fnHasActionRemaining = &StubHasAction;
    AutoPlay::fnCanUnitAttack = &StubCanAttack;
    AutoPlay::fnGetTileHeight = &StubTileHeight;
    AutoPlay::fnUpdateCursorHover = nullptr;
    AutoPlay::orig_BattleExecute = &StubOriginalExecute;

    AutoPlay::ResetAutoPlayTurnTracking();
    AutoPlay::g_LastBattleState = -1;
    AutoPlay::g_StateStuckTicks = 0;
    AutoPlay::g_CooldownTicks = 0;
    AutoPlay::g_PulseAction = -1;
    AutoPlay::g_HoldAction = -1;
    AutoPlay::g_DeployCountThisStage = 0;
    AutoPlay::g_Enabled = true;
    g_HasCandidate = true;
    return true;
}

bool Check(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        return false;
    }
    std::printf("PASS: %s\n", label);
    return true;
}

bool TestMoveConfirmWaitsForAcceptance() {
    if (!ResetWorld()) return false;
    const uintptr_t task = MainTask();
    *(int32_t*)(task + 0x14) = 0x05;
    AutoPlay::StepBattleDecision(task, BattleContext());
    AutoPlay::g_CooldownTicks = 0;
    AutoPlay::StepBattleDecision(task, BattleContext());
    return Check(AutoPlay::g_PendingMoveConfirmSent && AutoPlay::g_PulseAction == ACT_CONFIRM &&
                 !AutoPlay::g_UnitMovedThisTurn[kPlayer],
                 "move confirm stays pending until engine accepts it");
}

bool TestMoveAcceptedByPositionChange() {
    if (!ResetWorld()) return false;
    const uintptr_t task = MainTask();
    AutoPlay::BeginPendingMove(kPlayer, kTargetX, kTargetZ);
    AutoPlay::g_PendingMoveConfirmSent = true;
    *(int32_t*)(task + 0x14) = 0x02;
    *(int32_t*)(task + 0x11E0 + 0x14) = 0;
    *(float*)(Unit(kPlayer) + 0x08) = static_cast<float>(kTargetX * 12 + 6);
    AutoPlay::FinishPendingMoveIfObserved(task, BattleContext(), 0x02);
    return Check(AutoPlay::g_UnitMovedThisTurn[kPlayer] && !AutoPlay::g_PendingMove,
                 "move is marked after destination exit and unit position changes");
}

bool TestMoveAcceptedByNewTarget() {
    if (!ResetWorld()) return false;
    const uintptr_t task = MainTask();
    AutoPlay::BeginPendingMove(kPlayer, kTargetX, kTargetZ);
    AutoPlay::g_PendingMoveConfirmSent = true;
    *(int32_t*)(task + 0x14) = 0x02;
    *(int32_t*)(task + 0x11E0 + 0x14) = 0;
    *(float*)(Unit(kPlayer) + 0x34) = static_cast<float>(kTargetX * 12 + 6);
    *(float*)(Unit(kPlayer) + 0x3C) = static_cast<float>(kTargetZ * 12 + 6);
    AutoPlay::FinishPendingMoveIfObserved(task, BattleContext(), 0x02);
    return Check(AutoPlay::g_UnitMovedThisTurn[kPlayer] && !AutoPlay::g_PendingMove,
                 "accepted target is recognized after destination state exits");
}

bool TestRejectedMoveIsNotMarked() {
    if (!ResetWorld()) return false;
    const uintptr_t task = MainTask();
    AutoPlay::BeginPendingMove(kPlayer, kTargetX, kTargetZ);
    AutoPlay::g_PendingMoveConfirmSent = true;
    AutoPlay::g_MoveAttemptsThisTurn[kPlayer] = 1;
    *(int32_t*)(task + 0x14) = 0x02;
    *(int32_t*)(task + 0x11E0 + 0x14) = 0;
    AutoPlay::FinishPendingMoveIfObserved(task, BattleContext(), 0x02);
    return Check(!AutoPlay::g_UnitMovedThisTurn[kPlayer] && !AutoPlay::g_UnitDoneThisTurn[kPlayer] &&
                 !AutoPlay::g_PendingMove,
                 "leaving destination selection without acceptance is not recorded as movement");
}

bool TestInvalidSelectedUnitCancels() {
    if (!ResetWorld()) return false;
    AutoPlay::g_PendingMove = false;
    AutoPlay::g_PulseAction = -1;
    AutoPlay::DriveMoveDestination(MainTask(), BattleContext(), 256);
    return Check(AutoPlay::g_PulseAction == ACT_CANCEL && !AutoPlay::g_PendingMove,
                 "selected unit index 256 cancels safely");
}

bool TestStaleTargetDoesNotCountAsAccepted() {
    if (!ResetWorld()) return false;
    const uintptr_t task = MainTask();
    *(float*)(Unit(kPlayer) + 0x34) = static_cast<float>(kTargetX * 12 + 6);
    *(float*)(Unit(kPlayer) + 0x3C) = static_cast<float>(kTargetZ * 12 + 6);
    AutoPlay::BeginPendingMove(kPlayer, kTargetX, kTargetZ);
    AutoPlay::g_PendingMoveConfirmSent = true;
    *(int32_t*)(task + 0x14) = 0x02;
    *(int32_t*)(task + 0x11E0 + 0x14) = 0;
    AutoPlay::FinishPendingMoveIfObserved(task, BattleContext(), 0x02);
    return Check(!AutoPlay::g_UnitMovedThisTurn[kPlayer] && !AutoPlay::g_PendingMove,
                 "stale matching destination fields do not count as a new accepted move");
}

bool TestRepeatedTimeoutStopsUnit() {
    if (!ResetWorld()) return false;
    *(int32_t*)(MainTask() + 0x14) = 0x05;
    AutoPlay::BeginPendingMove(kPlayer, kTargetX, kTargetZ);
    AutoPlay::g_PendingMoveConfirmSent = true;
    AutoPlay::g_MoveAttemptsThisTurn[kPlayer] = 2;
    AutoPlay::g_StateStuckTicks = 26;
    AutoPlay::DriveMoveDestination(MainTask(), BattleContext(), kPlayer);
    return Check(AutoPlay::g_UnitDoneThisTurn[kPlayer] && !AutoPlay::g_UnitMovedThisTurn[kPlayer] &&
                 !AutoPlay::g_PendingMove && AutoPlay::g_PulseAction == ACT_CANCEL &&
                 !AutoPlay::g_BasePanelExhaustedThisTurn,
                 "second rejected move timeout stops the unit without exhausting deployment");
}

bool TestOrdinaryNoPathDoesNotExhaustPanel() {
    if (!ResetWorld()) return false;
    g_HasCandidate = false;
    AutoPlay::DriveMoveDestination(MainTask(), BattleContext(), kPlayer);
    return Check(AutoPlay::g_BasePanelExhaustedThisTurn == false &&
                 AutoPlay::g_UnitDoneThisTurn[kPlayer] && AutoPlay::g_PulseAction == ACT_CANCEL,
                 "ordinary unit no-path result does not exhaust the Base Panel");
}

bool TestDeploymentCountsOnlyObservedIncrease() {
    if (!ResetWorld()) return false;
    AutoPlay::g_DeployCountThisStage = 3;
    AutoPlay::g_LastBattleState = 0x0C;
    AutoPlay::g_StateStuckTicks = 3;
    *(int32_t*)(MainTask() + 0x14) = 0x0C;
    AutoPlay::StepBattleDecision(MainTask(), BattleContext());
    if (!AutoPlay::g_PendingDeployment || AutoPlay::g_DeployCountThisStage != 3 ||
        AutoPlay::g_PulseAction != ACT_CONFIRM) return Check(false, "dispatch confirmation does not count as deployment");

    InitializeUnit(kOtherPlayer, 12, 12, 0, true);
    AutoPlay::g_CooldownTicks = 0;
    *(int32_t*)(MainTask() + 0x14) = 0x05;
    AutoPlay::StepBattleDecision(MainTask(), BattleContext());
    return Check(!AutoPlay::g_PendingDeployment && AutoPlay::g_DeployCountThisStage == 4,
                 "stage deployment count increases after deployed-unit count rises");
}

bool TestStageClearAdvances() {
    if (!ResetWorld()) return false;
    AutoPlay::g_LastBattleState = 0x20;
    *(int32_t*)(MainTask() + 0x14) = 0x20;
    AutoPlay::StepBattleDecision(MainTask(), BattleContext());
    return Check(AutoPlay::g_PulseAction == ACT_CONFIRM,
                 "stage-clear state pulses confirm to advance");
}

bool TestMapSubstateChangeResetsStuckCounter() {
    if (!ResetWorld()) return false;
    AutoPlay::g_LastBattleState = 0x02;
    AutoPlay::g_LastSubState = -1;
    AutoPlay::g_StateStuckTicks = 26;
    *(int32_t*)(MainTask() + 0x14) = 0x02;
    *(int32_t*)(MainTask() + 0x11E0 + 0x14) = 0;
    AutoPlay::StepBattleDecision(MainTask(), BattleContext());
    return Check(AutoPlay::g_StateStuckTicks == 0,
                 "map-cursor substate change clears the stale stuck-state timeout");
}

} // namespace

int main() {
    if (tmpfile_s(&AutoPlay::g_LogFile) != 0 || !AutoPlay::g_LogFile) {
        std::fprintf(stderr, "Unable to open temporary log stream\n");
        return 2;
    }
    bool ok = true;
    ok = TestMoveConfirmWaitsForAcceptance() && ok;
    ok = TestMoveAcceptedByPositionChange() && ok;
    ok = TestMoveAcceptedByNewTarget() && ok;
    ok = TestRejectedMoveIsNotMarked() && ok;
    ok = TestInvalidSelectedUnitCancels() && ok;
    ok = TestStaleTargetDoesNotCountAsAccepted() && ok;
    ok = TestRepeatedTimeoutStopsUnit() && ok;
    ok = TestOrdinaryNoPathDoesNotExhaustPanel() && ok;
    ok = TestDeploymentCountsOnlyObservedIncrease() && ok;
    ok = TestStageClearAdvances() && ok;
    ok = TestMapSubstateChangeResetsStuckCounter() && ok;

    if (AutoPlay::g_LogFile) std::fclose(AutoPlay::g_LogFile);
    if (AutoPlay::g_Base) VirtualFree(reinterpret_cast<void*>(AutoPlay::g_Base), 0, MEM_RELEASE);
    if (!ok) return 1;
    std::puts("All offline state regression checks passed.");
    return 0;
}
