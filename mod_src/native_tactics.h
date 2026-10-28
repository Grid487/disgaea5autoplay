#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "native_combat.h"
#include "tactical_path.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace AutoPlay::NativeTactics {

constexpr uintptr_t kJumpStatRva = 0x00157ED0;
constexpr uintptr_t kHeightGridRva = 0x00B82910;
constexpr uintptr_t kNativeBlockerOffset = 0x4040;
constexpr size_t kGridCellCount = TacticalPath::kGridCellCount;

using JumpStatFunction = int(__fastcall*)(void* unit);

// Offline tests install a stub here. Production leaves it null and calls the
// verified native getter at RVA 0x157ED0 after checking the code page.
inline JumpStatFunction g_JumpStatOverride = nullptr;

enum class JobPhase : uint8_t { Idle, BuildGraph, Route, ProbeEndpoints, Complete, Failed };

class MovementJob {
public:
    bool Begin(int actorIndex, const Combat::BattleSnapshot& snapshot,
               const TacticalPath::CellMask& usefulGoalMask,
               const TacticalPath::CellMask* reservedMask = nullptr) {
        Reset();
        phase_ = JobPhase::Failed;
        if (actorIndex < 0 || actorIndex >= Combat::kMaxUnits || !AutoPlay::g_Base) return false;
        const Combat::UnitSnapshot& actorState = snapshot.units[static_cast<size_t>(actorIndex)];
        if (actorState.index != actorIndex || !actorState.alive || !actorState.deployed ||
            !actorState.canMove || actorState.faction != Combat::Faction::Player ||
            !TacticalPath::InBounds(actorState.gridX, actorState.gridZ)) return false;

        actorIndex_ = actorIndex;
        actorUnit_ = AutoPlay::GetUnitPtr(actorIndex);
        actorCell_ = TacticalPath::CellIndex(actorState.gridX, actorState.gridZ);
        if (!actorUnit_ || !NativeRangesReady()) return false;

        const uintptr_t chara = *(uintptr_t*)(actorUnit_ + 0x240);
        if (!chara || !ReadableRange(chara, 0x2E2B)) return false;
        isFlying_ = *(uint8_t*)(chara + 0x2E2A) == 2;
        const int jump = QueryJumpStat(reinterpret_cast<void*>(actorUnit_));
        if (jump < 0) return false;
        jumpStat_ = jump;

        gridWidth_ = *(int32_t*)(pathGrid_ + 0x30);
        gridHeight_ = *(int32_t*)(pathGrid_ + 0x34);
        if (gridWidth_ <= 0 || gridWidth_ > TacticalPath::kGridWidth ||
            gridHeight_ <= 0 || gridHeight_ > TacticalPath::kGridHeight) return false;

        moveStat_ = AutoPlay::fnGetUnitMoveStat
            ? AutoPlay::fnGetUnitMoveStat(reinterpret_cast<void*>(actorUnit_)) : 6;
        if (moveStat_ <= 0) return false;

        SavePathState();
        if (!PopulateMoveRangeOnce()) {
            RestorePathState();
            return false;
        }
        RestorePathState();
        if (!CopyMovementInputs(usefulGoalMask, reservedMask, snapshot)) return false;
        phase_ = JobPhase::BuildGraph;
        return true;
    }

    // Each call is bounded: <=256 graph cells, <=256 reverse-BFS nodes and
    // <=4 native endpoint path probes, regardless of caller-supplied budgets.
    bool Step(size_t graphBudget = 256, size_t routeBudget = 256,
              size_t pathProbeBudget = 4) {
        lastGraphCells_ = 0;
        lastRouteNodes_ = 0;
        lastEndpointCells_ = 0;
        lastPathProbes_ = 0;
        if (phase_ == JobPhase::Idle || phase_ == JobPhase::Failed || phase_ == JobPhase::Complete)
            return phase_ == JobPhase::Complete;

        graphBudget = std::min<size_t>(graphBudget, 256);
        routeBudget = std::min<size_t>(routeBudget, 256);
        pathProbeBudget = std::min<size_t>(pathProbeBudget, 4);

        if (phase_ == JobPhase::BuildGraph) {
            lastGraphCells_ = BuildGraph(graphBudget);
            if (graphCursor_ >= static_cast<size_t>(gridWidth_ * gridHeight_)) {
                if (!route_.Start(passable_, edges_, goals_, excluded_, actorCell_)) {
                    phase_ = JobPhase::Failed;
                    return false;
                }
                phase_ = JobPhase::Route;
            }
        }

        if (phase_ == JobPhase::Route) {
            lastRouteNodes_ = route_.Step(routeBudget);
            if (route_.IsComplete()) phase_ = JobPhase::ProbeEndpoints;
        }

        if (phase_ == JobPhase::ProbeEndpoints) {
            ProbeEndpointCells(graphBudget, pathProbeBudget);
            if (probeCursor_ >= static_cast<size_t>(gridWidth_ * gridHeight_)) {
                phase_ = JobPhase::Complete;
                if (bestValid_) routeMemory_.goalSource = bestGoalSource_;
            }
        }
        return phase_ == JobPhase::Complete;
    }

    bool IsComplete() const { return phase_ == JobPhase::Complete || phase_ == JobPhase::Failed; }
    bool Succeeded() const { return phase_ == JobPhase::Complete && bestValid_; }
    bool GetResult(int& x, int& z) const {
        if (!Succeeded()) return false;
        x = bestX_;
        z = bestZ_;
        return true;
    }
    JobPhase Phase() const { return phase_; }
    uint16_t RouteDistance(int x, int z) const { return route_.Distance(x, z); }
    uint16_t CurrentRouteDistance() const { return route_.Distance(actorCell_); }
    size_t LastGraphCellsProcessed() const { return lastGraphCells_; }
    size_t LastRouteNodesProcessed() const { return lastRouteNodes_; }
    size_t LastEndpointCellsScanned() const { return lastEndpointCells_; }
    size_t LastPathProbes() const { return lastPathProbes_; }
    int MoveRangeCalls() const { return moveRangeCalls_; }
    bool OnBasePanel() const { return onBasePanel_; }
    bool IsFlyingActor() const { return isFlying_; }
    int JumpStat() const { return jumpStat_; }
    int16_t CopiedHeight(int x, int z) const {
        return TacticalPath::InBounds(x, z) ? heights_[NativeGridIndex(x, z)] : 0;
    }

private:
    static bool ReadableRange(uintptr_t address, size_t byteCount) {
        if (address == 0 || byteCount == 0 ||
            address > (std::numeric_limits<uintptr_t>::max)() - byteCount) return false;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info))
            return false;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) return false;
        const DWORD protection = info.Protect & 0xFF;
        const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable) return false;
        const uintptr_t start = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (info.RegionSize > (std::numeric_limits<uintptr_t>::max)() - start) return false;
        return address >= start && address + byteCount <= start + info.RegionSize;
    }

    static bool IsExecutableRange(uintptr_t address, size_t byteCount) {
        if (!ReadableRange(address, byteCount)) return false;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info))
            return false;
        const DWORD protection = info.Protect & 0xFF;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    }

    static int QueryJumpStat(void* unit) {
        if (g_JumpStatOverride) return g_JumpStatOverride(unit);
        if (!AutoPlay::g_Base || AutoPlay::g_Base >
            (std::numeric_limits<uintptr_t>::max)() - kJumpStatRva) return -1;
        const uintptr_t target = AutoPlay::g_Base + kJumpStatRva;
        if (!IsExecutableRange(target, 1)) return -1;
        return reinterpret_cast<JumpStatFunction>(target)(unit);
    }

    bool NativeRangesReady() {
        const uintptr_t base = AutoPlay::g_Base;
        if (!ReadableRange(actorUnit_, AutoPlay::UNIT_PATH_STATE_OFFSET +
                           AutoPlay::UNIT_PATH_STATE_SIZE)) return false;
        if (base > (std::numeric_limits<uintptr_t>::max)() - RVA::ADDR_PathGrid) return false;
        pathGrid_ = base + RVA::ADDR_PathGrid;
        if (!ReadableRange(pathGrid_ + 0x30, 8) ||
            !ReadableRange(pathGrid_ + kNativeBlockerOffset, kGridCellCount) ||
            !ReadableRange(base + RVA::ADDR_TileFlagGrid, kGridCellCount) ||
            !ReadableRange(base + RVA::ADDR_MoveCostGrid, kGridCellCount) ||
            !ReadableRange(base + kHeightGridRva, kGridCellCount * sizeof(int16_t))) return false;
        return AutoPlay::fnGetUnitPackedXZ && AutoPlay::fnComputeMoveRange &&
            AutoPlay::fnComputePathToTile;
    }

    bool CopyMovementInputs(const TacticalPath::CellMask& usefulGoalMask,
                            const TacticalPath::CellMask* reservedMask,
                            const Combat::BattleSnapshot& snapshot) {
        goals_ = usefulGoalMask;
        excluded_.fill(0);
        if (reservedMask) {
            for (size_t i = 0; i < kGridCellCount; ++i)
                excluded_[i] = (*reservedMask)[i] != 0 ? 1 : 0;
        }
        for (int index = 0; index < Combat::kMaxUnits; ++index) {
            const Combat::UnitSnapshot& unit = snapshot.units[static_cast<size_t>(index)];
            if (unit.index != index || !unit.alive || !unit.deployed || index == actorIndex_ ||
                !TacticalPath::InBounds(unit.gridX, unit.gridZ)) continue;
            const int cell = TacticalPath::CellIndex(unit.gridX, unit.gridZ);
            excluded_[static_cast<size_t>(cell)] = 1;
        }

        float bpWX = 0.0f, bpWY = 0.0f, bpWZ = 0.0f;
        int bpGX = -1, bpGZ = -1;
        if (ReadableRange(AutoPlay::g_Base + RVA::ADDR_MapGrid + 0x1740, 0x1C) &&
            AutoPlay::GetBasePanelPosition(bpWX, bpWY, bpWZ, bpGX, bpGZ) &&
            TacticalPath::InBounds(bpGX, bpGZ)) {
            basePanelCell_ = TacticalPath::CellIndex(bpGX, bpGZ);
            excluded_[static_cast<size_t>(basePanelCell_)] = 1;
        }
        onBasePanel_ = actorCell_ == basePanelCell_ && basePanelCell_ >= 0;

        const uintptr_t base = AutoPlay::g_Base;
        const uintptr_t tileFlags = base + RVA::ADDR_TileFlagGrid;
        const uintptr_t moveCosts = base + RVA::ADDR_MoveCostGrid;
        const uintptr_t heights = base + kHeightGridRva;
        const uintptr_t blockers = pathGrid_ + kNativeBlockerOffset;
        std::memcpy(tileFlags_.data(), reinterpret_cast<const void*>(tileFlags), kGridCellCount);
        std::memcpy(moveCosts_.data(), reinterpret_cast<const void*>(moveCosts), kGridCellCount);
        std::memcpy(heights_.data(), reinterpret_cast<const void*>(heights),
                    kGridCellCount * sizeof(int16_t));
        std::memcpy(blockers_.data(), reinterpret_cast<const void*>(blockers), kGridCellCount);

        enemyCount_ = 0;
        for (int index = 0; index < Combat::kMaxUnits; ++index) {
            const Combat::UnitSnapshot& unit = snapshot.units[static_cast<size_t>(index)];
            if (unit.index != index || !unit.alive || !unit.deployed ||
                unit.faction != Combat::Faction::Enemy ||
                !TacticalPath::InBounds(unit.gridX, unit.gridZ)) continue;
            enemyX_[static_cast<size_t>(enemyCount_)] = static_cast<int16_t>(unit.gridX);
            enemyZ_[static_cast<size_t>(enemyCount_)] = static_cast<int16_t>(unit.gridZ);
            ++enemyCount_;
        }
        return true;
    }

    bool PopulateMoveRangeOnce() {
        if (!actorUnit_ || !AutoPlay::fnGetUnitPackedXZ || !AutoPlay::fnComputeMoveRange) return false;
        uint32_t packedPosition = 0;
        AutoPlay::fnGetUnitPackedXZ(reinterpret_cast<void*>(actorUnit_), &packedPosition);
        AutoPlay::fnComputeMoveRange(nullptr, reinterpret_cast<void*>(pathGrid_),
                                     reinterpret_cast<void*>(actorUnit_), packedPosition, 0);
        ++moveRangeCalls_;

        // The native range routine is permitted to touch path state. Copy the
        // resulting grids after the call in CopyMovementInputs, but restore the
        // live BattleUnit path block before returning from this frame.
        return true;
    }

    void SavePathState() {
        std::memcpy(pathState_.data(), reinterpret_cast<const void*>(actorUnit_ +
            AutoPlay::UNIT_PATH_STATE_OFFSET), pathState_.size());
        pathStateSaved_ = true;
    }

    void RestorePathState() {
        if (pathStateSaved_ && actorUnit_)
            std::memcpy(reinterpret_cast<void*>(actorUnit_ + AutoPlay::UNIT_PATH_STATE_OFFSET),
                        pathState_.data(), pathState_.size());
    }

    size_t BuildGraph(size_t budget) {
        size_t processed = 0;
        while (processed < budget && graphCursor_ < static_cast<size_t>(gridWidth_ * gridHeight_)) {
            Planning::Unit operation;
            if (!operation) break;
            const int cell = TacticalPath::CellIndex(static_cast<int>(graphCursor_ % gridWidth_),
                                                   static_cast<int>(graphCursor_ / gridWidth_));
            const int x = TacticalPath::CellX(cell);
            const int z = TacticalPath::CellZ(cell);
            const size_t nativeIndex = NativeGridIndex(x, z);
            const bool inMap = x < gridWidth_ && z < gridHeight_;
            const bool traversable = inMap && tileFlags_[nativeIndex] < 0xFC &&
                heights_[nativeIndex] != 0 && blockers_[nativeIndex] != 1 &&
                (cell == actorCell_ || !excluded_[static_cast<size_t>(cell)]);
            passable_[static_cast<size_t>(cell)] = traversable ? 1 : 0;
            edges_[static_cast<size_t>(cell)] = traversable ? BuildOutgoingEdges(x, z, nativeIndex) : 0;
            ++graphCursor_;
            ++processed;
        }
        return processed;
    }

    static size_t NativeGridIndex(int x, int z) {
        // The executable's 128x128 maps are indexed x*128+z. TacticalPath's
        // public CellIndex is z*128+x, so the conversion stays explicit.
        return static_cast<size_t>(x) * TacticalPath::kGridWidth + static_cast<size_t>(z);
    }

    bool RawTraversable(int x, int z) const {
        if (!TacticalPath::InBounds(x, z) || x >= gridWidth_ || z >= gridHeight_) return false;
        const int cell = TacticalPath::CellIndex(x, z);
        const size_t nativeIndex = NativeGridIndex(x, z);
        return tileFlags_[nativeIndex] < 0xFC && heights_[nativeIndex] != 0 &&
            blockers_[nativeIndex] != 1 && (cell == actorCell_ || !excluded_[static_cast<size_t>(cell)]);
    }

    uint8_t BuildOutgoingEdges(int x, int z, size_t currentIndex) const {
        uint8_t edges = 0;
        const int currentHeight = heights_[currentIndex];
        const int nextX[4] = {x, x + 1, x, x - 1};
        const int nextZ[4] = {z - 1, z, z + 1, z};
        const uint8_t direction[4] = {TacticalPath::North, TacticalPath::East,
                                      TacticalPath::South, TacticalPath::West};
        for (int i = 0; i < 4; ++i) {
            const int nx = nextX[i];
            const int nz = nextZ[i];
            if (!RawTraversable(nx, nz)) continue;
            const size_t neighborIndex = NativeGridIndex(nx, nz);
            const int neighborHeight = heights_[neighborIndex];
            if (isFlying_ || currentHeight - neighborHeight <= jumpStat_)
                edges |= direction[i];
        }
        return edges;
    }

    int ExposureDistance(int x, int z) const {
        int nearestEnemy = 256;
        for (int i = 0; i < enemyCount_; ++i) {
            const int distance = std::abs(x - enemyX_[static_cast<size_t>(i)]) +
                std::abs(z - enemyZ_[static_cast<size_t>(i)]);
            if (distance < nearestEnemy) nearestEnemy = distance;
        }
        return nearestEnemy;
    }

    bool IsBetterResult(uint16_t distance, int exposure, int goalSource, int x, int z) const {
        if (!bestValid_ || distance < bestDistance_) return true;
        if (distance > bestDistance_) return false;
        if (exposure != bestExposure_) return exposure > bestExposure_;
        const bool candidateUsesLockedGoal = goalSource == routeMemory_.goalSource;
        const bool bestUsesLockedGoal = bestGoalSource_ == routeMemory_.goalSource;
        if (candidateUsesLockedGoal != bestUsesLockedGoal) return candidateUsesLockedGoal;
        return x < bestX_ || (x == bestX_ && z < bestZ_);
    }

    bool CheapCandidateCheck(int cell) const {
        if (cell == actorCell_ || !passable_[static_cast<size_t>(cell)] ||
            excluded_[static_cast<size_t>(cell)]) return false;
        const int x = TacticalPath::CellX(cell);
        const int z = TacticalPath::CellZ(cell);
        const size_t nativeIndex = NativeGridIndex(x, z);
        const uint8_t cost = moveCosts_[nativeIndex];
        if (cost == 0 || cost >= 0x80 || cost > moveStat_ || tileFlags_[nativeIndex] >= 0xFC)
            return false;
        const uint16_t routeDistance = route_.Distance(cell);
        const uint16_t currentDistance = route_.Distance(actorCell_);
        if (routeDistance == TacticalPath::kInfiniteDistance) return false;
        if (!onBasePanel_ && routeDistance >= currentDistance) return false;
        if (bestValid_ && routeDistance > bestDistance_) return false;

        const int exposure = ExposureDistance(x, z);
        const int goalSource = route_.GoalSource(cell);
        return IsBetterResult(routeDistance, exposure, goalSource, x, z);
    }

    bool ProbeExactPath(int x, int z) {
        if (!AutoPlay::fnComputePathToTile || !actorUnit_) return false;
        RestorePathState();
        *(int32_t*)(actorUnit_ + AutoPlay::UNIT_PATH_STATE_OFFSET) = 128;
        ++lastPathProbes_;
        const bool computed = AutoPlay::fnComputePathToTile(reinterpret_cast<void*>(actorUnit_), x, z, 0);
        const bool endsAtTarget = computed && AutoPlay::ComputedPathEndsAtTile(actorUnit_, x, z);
        RestorePathState();
        return endsAtTarget;
    }

    void ProbeEndpointCells(size_t scanBudget, size_t probeBudget) {
        size_t scanned = 0;
        while (probeCursor_ < static_cast<size_t>(gridWidth_ * gridHeight_) && scanned < scanBudget) {
            const int cell = TacticalPath::CellIndex(static_cast<int>(probeCursor_ % gridWidth_),
                                                   static_cast<int>(probeCursor_ / gridWidth_));
            const bool needsPath = CheapCandidateCheck(cell);
            if (needsPath && lastPathProbes_ >= probeBudget) break;
            Planning::Unit operation(needsPath ? Planning::Resource::Path : Planning::Resource::Work);
            if (!operation) break;
            if (needsPath) {
                if (lastPathProbes_ >= probeBudget) break;
                ++probeCursor_;
                ++scanned;
                const int x = TacticalPath::CellX(cell);
                const int z = TacticalPath::CellZ(cell);
                if (ProbeExactPath(x, z)) {
                    bestValid_ = true;
                    bestX_ = x;
                    bestZ_ = z;
                    bestDistance_ = route_.Distance(cell);
                    bestExposure_ = ExposureDistance(x, z);
                    bestGoalSource_ = route_.GoalSource(cell);
                }
            } else {
                ++probeCursor_;
                ++scanned;
            }
        }
        lastEndpointCells_ = scanned;
    }

    void Reset() {
        phase_ = JobPhase::Idle;
        actorIndex_ = -1;
        actorUnit_ = 0;
        actorCell_ = -1;
        pathGrid_ = 0;
        basePanelCell_ = -1;
        gridWidth_ = gridHeight_ = 0;
        moveStat_ = 0;
        jumpStat_ = 0;
        isFlying_ = false;
        onBasePanel_ = false;
        pathStateSaved_ = false;
        pathState_.fill(0);
        enemyCount_ = 0;
        // The next BuildGraph pass overwrites every passable/edge entry,
        // CopyMovementInputs overwrites masks and native snapshots, and
        // IncrementalPathField::Start resets its fixed search arrays.
        graphCursor_ = 0;
        probeCursor_ = 0;
        passable_.fill(0);
        edges_.fill(0);
        moveRangeCalls_ = 0;
        bestValid_ = false;
        bestX_ = bestZ_ = -1;
        bestDistance_ = TacticalPath::kInfiniteDistance;
        bestExposure_ = -1;
        bestGoalSource_ = -1;
        lastGraphCells_ = lastRouteNodes_ = lastEndpointCells_ = lastPathProbes_ = 0;
    }

    JobPhase phase_ = JobPhase::Idle;
    int actorIndex_ = -1;
    uintptr_t actorUnit_ = 0;
    int actorCell_ = -1;
    uintptr_t pathGrid_ = 0;
    int basePanelCell_ = -1;
    int gridWidth_ = 0;
    int gridHeight_ = 0;
    int moveStat_ = 0;
    int jumpStat_ = 0;
    bool isFlying_ = false;
    bool onBasePanel_ = false;
    bool pathStateSaved_ = false;
    std::array<uint8_t, AutoPlay::UNIT_PATH_STATE_SIZE> pathState_{};
    std::array<uint8_t, kGridCellCount> tileFlags_{};
    std::array<uint8_t, kGridCellCount> moveCosts_{};
    std::array<int16_t, kGridCellCount> heights_{};
    std::array<uint8_t, kGridCellCount> blockers_{};
    TacticalPath::CellMask passable_{};
    TacticalPath::EdgeMask edges_{};
    TacticalPath::CellMask excluded_{};
    TacticalPath::CellMask goals_{};
    std::array<int16_t, Combat::kMaxUnits> enemyX_{};
    std::array<int16_t, Combat::kMaxUnits> enemyZ_{};
    int enemyCount_ = 0;
    TacticalPath::IncrementalPathField route_{};
    size_t graphCursor_ = 0;
    size_t probeCursor_ = 0;
    int moveRangeCalls_ = 0;
    bool bestValid_ = false;
    int bestX_ = -1;
    int bestZ_ = -1;
    uint16_t bestDistance_ = TacticalPath::kInfiniteDistance;
    int bestExposure_ = -1;
    int bestGoalSource_ = -1;
    TacticalPath::RouteGoalMemory routeMemory_{};
    size_t lastGraphCells_ = 0;
    size_t lastRouteNodes_ = 0;
    size_t lastEndpointCells_ = 0;
    size_t lastPathProbes_ = 0;
};

} // namespace AutoPlay::NativeTactics
