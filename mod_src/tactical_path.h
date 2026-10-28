#pragma once
#include "planning_budget.h"

// Incremental reverse route field for the battle planner. The adapter supplies
// already-verified passability and directed movement edges; this file has no
// native-memory access and performs no heap allocation.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace AutoPlay::TacticalPath {

constexpr int kGridWidth = 128;
constexpr int kGridHeight = 128;
constexpr int kGridCellCount = kGridWidth * kGridHeight;
constexpr uint16_t kInfiniteDistance = std::numeric_limits<uint16_t>::max();
constexpr uint16_t kInvalidCell = std::numeric_limits<uint16_t>::max();

using CellMask = std::array<uint8_t, kGridCellCount>;
using EdgeMask = std::array<uint8_t, kGridCellCount>;

enum Direction : uint8_t {
    North = 1u << 0,
    East  = 1u << 1,
    South = 1u << 2,
    West  = 1u << 3,
    AllDirections = North | East | South | West
};

inline bool InBounds(int x, int z) {
    return x >= 0 && x < kGridWidth && z >= 0 && z < kGridHeight;
}

inline int CellIndex(int x, int z) {
    return InBounds(x, z) ? z * kGridWidth + x : -1;
}

inline int CellX(int cell) { return cell >= 0 && cell < kGridCellCount ? cell % kGridWidth : -1; }
inline int CellZ(int cell) { return cell >= 0 && cell < kGridCellCount ? cell / kGridWidth : -1; }

struct EndpointCandidate {
    int cell = -1;
    int threat = -1; // Unknown threat is rejected conservatively.
    bool engineLegal = false;
    bool occupied = false;
    bool basePanel = false;
    bool reserved = false;
};

struct RouteGoalMemory {
    int goalSource = -1;
};

struct EndpointChoice {
    bool valid = false;
    int cell = -1;
    int goalSource = -1;
    uint16_t distance = kInfiniteDistance;
    int threat = -1;
};

class IncrementalPathField {
public:
    // edgeMask[cell] uses Direction bits to describe legal outgoing edges
    // from that cell. excluded marks occupied, BasePanel and reserved cells.
    // The actor cell is allowed as the search start even if excluded.
    bool Start(const CellMask& passable, const EdgeMask& edgeMask,
               const CellMask& terminalGoals, const CellMask& excluded,
               int actorCell) {
        Reset();
        if (!IsCell(actorCell)) return false;

        passable_ = passable;
        edgeMask_ = edgeMask;
        excluded_ = excluded;
        actorCell_ = actorCell;
        active_ = true;

        for (int cell = 0; cell < kGridCellCount; ++cell) {
            if (!terminalGoals[static_cast<size_t>(cell)] || !CanTraverse(cell)) continue;
            distance_[static_cast<size_t>(cell)] = 0;
            goalSource_[static_cast<size_t>(cell)] = static_cast<uint16_t>(cell);
            queue_[queueTail_++] = static_cast<uint16_t>(cell);
        }
        if (queueTail_ == 0) complete_ = true;
        return true;
    }

    // Expands no more than maxNodes queue entries. Call from successive frames
    // until IsComplete() is true; a zero budget never advances the search.
    size_t Step(size_t maxNodes = 256) {
        if (!active_ || complete_ || maxNodes == 0) return 0;
        size_t processed = 0;
        while (processed < maxNodes && queueHead_ < queueTail_) {
            Planning::Unit operation;
            if (!operation) break;
            const uint16_t current = queue_[queueHead_++];
            const int x = CellX(current);
            const int z = CellZ(current);
            AddPredecessor(current, x, z - 1, South);
            AddPredecessor(current, x + 1, z, West);
            AddPredecessor(current, x, z + 1, North);
            AddPredecessor(current, x - 1, z, East);
            ++processed;
        }
        if (queueHead_ >= queueTail_) complete_ = true;
        return processed;
    }

    uint16_t Distance(int cell) const {
        if (!active_ || !IsCell(cell)) return kInfiniteDistance;
        return distance_[static_cast<size_t>(cell)];
    }

    uint16_t Distance(int x, int z) const { return Distance(CellIndex(x, z)); }

    int GoalSource(int cell) const {
        if (!active_ || !IsCell(cell)) return -1;
        const uint16_t source = goalSource_[static_cast<size_t>(cell)];
        return source == kInvalidCell ? -1 : static_cast<int>(source);
    }

    bool IsComplete() const { return active_ && complete_; }
    bool IsActive() const { return active_; }
    int ActorCell() const { return actorCell_; }

    bool IsTraversable(int cell) const { return active_ && IsCell(cell) && CanTraverse(cell); }

private:
    static bool IsCell(int cell) { return cell >= 0 && cell < kGridCellCount; }

    bool CanTraverse(int cell) const {
        if (!passable_[static_cast<size_t>(cell)]) return false;
        return cell == actorCell_ || !excluded_[static_cast<size_t>(cell)];
    }

    void AddPredecessor(uint16_t current, int x, int z, uint8_t predecessorDirection) {
        const int predecessor = CellIndex(x, z);
        if (predecessor < 0 || !CanTraverse(predecessor)) return;
        const size_t predecessorSlot = static_cast<size_t>(predecessor);
        if ((edgeMask_[predecessorSlot] & predecessorDirection) == 0 ||
            distance_[predecessorSlot] != kInfiniteDistance) return;

        const uint16_t nextDistance = static_cast<uint16_t>(distance_[current] + 1);
        distance_[predecessorSlot] = nextDistance;
        goalSource_[predecessorSlot] = goalSource_[current];
        queue_[queueTail_++] = static_cast<uint16_t>(predecessor);
    }

    void Reset() {
        passable_.fill(0);
        edgeMask_.fill(0);
        excluded_.fill(0);
        distance_.fill(kInfiniteDistance);
        goalSource_.fill(kInvalidCell);
        queue_.fill(kInvalidCell);
        queueHead_ = 0;
        queueTail_ = 0;
        actorCell_ = -1;
        active_ = false;
        complete_ = false;
    }

    CellMask passable_{};
    EdgeMask edgeMask_{};
    CellMask excluded_{};
    std::array<uint16_t, kGridCellCount> distance_{};
    std::array<uint16_t, kGridCellCount> goalSource_{};
    std::array<uint16_t, kGridCellCount> queue_{};
    size_t queueHead_ = 0;
    size_t queueTail_ = 0;
    int actorCell_ = -1;
    bool active_ = false;
    bool complete_ = false;
};

inline bool ChooseEndpoint(const IncrementalPathField& field,
                           const EndpointCandidate* candidates, size_t count,
                           const RouteGoalMemory& memory, EndpointChoice& out) {
    out = EndpointChoice{};
    // Partial reverse-search distances are useful for progress display, but
    // endpoint selection waits for completion so it cannot prefer the first
    // route discovered over a shorter route that has not been expanded yet.
    if (!field.IsComplete() || !candidates) return false;

    for (size_t i = 0; i < count; ++i) {
        const EndpointCandidate& candidate = candidates[i];
        if (!candidate.engineLegal || candidate.threat < 0 ||
            candidate.occupied || candidate.basePanel || candidate.reserved ||
            !field.IsTraversable(candidate.cell)) continue;
        const uint16_t distance = field.Distance(candidate.cell);
        if (distance == kInfiniteDistance) continue;
        const int source = field.GoalSource(candidate.cell);
        if (source < 0) continue;

        bool better = !out.valid;
        if (out.valid && distance != out.distance) better = distance < out.distance;
        else if (out.valid && candidate.threat != out.threat) better = candidate.threat < out.threat;
        else if (out.valid) {
            const bool matchesLock = source == memory.goalSource;
            const bool currentMatchesLock = out.goalSource == memory.goalSource;
            if (matchesLock != currentMatchesLock) better = matchesLock;
            else {
                const int candidateX = CellX(candidate.cell);
                const int bestX = CellX(out.cell);
                const int candidateZ = CellZ(candidate.cell);
                const int bestZ = CellZ(out.cell);
                better = candidateX < bestX || (candidateX == bestX && candidateZ < bestZ);
            }
        }

        if (better) {
            out.valid = true;
            out.cell = candidate.cell;
            out.goalSource = source;
            out.distance = distance;
            out.threat = candidate.threat;
        }
    }
    return out.valid;
}

inline bool ChooseEndpoint(const IncrementalPathField& field,
                           const EndpointCandidate* candidates, size_t count,
                           RouteGoalMemory& memory, EndpointChoice& out) {
    if (!ChooseEndpoint(field, candidates, count,
                        static_cast<const RouteGoalMemory&>(memory), out)) return false;
    memory.goalSource = out.goalSource;
    return true;
}

} // namespace AutoPlay::TacticalPath
