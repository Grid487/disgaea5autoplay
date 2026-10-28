#pragma once

// Incremental, conservative generation of tiles from which a unit can perform
// at least one currently useful basic attack or configured skill. This file
// only reads native data; movement legality and final action legality remain
// owned by NativeTactics and NativeActions respectively.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "native_actions.h"
#include "tactical_path.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <intrin.h>
#include <vector>

namespace AutoPlay::NativeActionPositions {

// BattleUnit+0x10A4/+0x11A4 is an occupancy footprint, not attack offsets.
// The verified table has room for exactly 0x100 / 8 = 32 signed x/z pairs.
constexpr size_t kMaxFootprintCells = 32;
constexpr size_t kMaxShapeCacheEntries = NativeSkills::kMaxSkillRows;
constexpr size_t kGridCellCount = TacticalPath::kGridCellCount;
constexpr uintptr_t kHeightGridRva = 0x00B66A30;
constexpr size_t kBattleUnitFootprintCountOffset = 0x11A4;
constexpr size_t kBattleUnitFootprintCellsOffset = 0x10A4;
constexpr size_t kDescriptorMinHeightOffset = 0x43A;
constexpr size_t kDescriptorMaxHeightOffset = 0x43B;

struct FootprintCell { int32_t x = 0; int32_t z = 0; };

// Tests can supply immutable data readers. Production leaves these null and
// uses the recovered game layouts and safe NativeGeometry range checks.
using UnitFootprintReader = bool(*)(int unitIndex, std::vector<FootprintCell>& out);
using BasicRangeReader = int(*)(int actorIndex);
using BasicReachReader = bool(*)(int actorIndex, int sourceX, int sourceZ,
                                 int targetIndex, int targetX, int targetZ, int range);
using LineWeaponReader = bool(*)(int actorIndex);
using BasicVerticalReader = bool(*)(int sourceX, int sourceZ, int targetIndex,
                                    int targetX, int targetZ, int range);
using ConfiguredShapeReader = int(*)(int actorIndex, const NativeSkills::Entry& skill);
using HeightReader = bool(*)(int x, int z, int16_t& outHeight);
using HeightLimitsReader = bool(*)(const NativeSkills::Entry& skill,
                                    uint8_t& maxBelow, uint8_t& maxAbove);

inline UnitFootprintReader g_UnitFootprintReaderOverride = nullptr;
inline BasicRangeReader g_BasicRangeReaderOverride = nullptr;
inline BasicReachReader g_BasicReachReaderOverride = nullptr;
inline LineWeaponReader g_LineWeaponReaderOverride = nullptr;
inline BasicVerticalReader g_BasicVerticalReaderOverride = nullptr;
inline ConfiguredShapeReader g_ConfiguredShapeReaderOverride = nullptr;
inline HeightReader g_HeightReaderOverride = nullptr;
inline HeightLimitsReader g_HeightLimitsReaderOverride = nullptr;

inline bool ReadNativeUnitFootprint(int unitIndex, std::vector<FootprintCell>& out) {
    out.clear();
    if (unitIndex < 0 || unitIndex >= Combat::kMaxUnits || !AutoPlay::g_Base) return false;
    const uintptr_t unit = AutoPlay::GetUnitPtr(unitIndex);
    if (!NativeGeometry::IsReadableRange(unit, kBattleUnitFootprintCountOffset + sizeof(int16_t))) return false;

    int16_t count = 0;
    std::memcpy(&count, reinterpret_cast<const void*>(unit + kBattleUnitFootprintCountOffset), sizeof(count));
    if (count == 0) return true;
    if (count < 0) return false;
    if (count > static_cast<int16_t>(kMaxFootprintCells)) return false;
    const size_t byteCount = static_cast<size_t>(count) * sizeof(uint64_t);
    const uintptr_t cells = unit + kBattleUnitFootprintCellsOffset;
    if (!NativeGeometry::IsReadableRange(cells, byteCount)) return false;

    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        uint64_t packed = 0;
        std::memcpy(&packed, reinterpret_cast<const void*>(cells + static_cast<uintptr_t>(i) * 8), sizeof(packed));
        out.push_back({static_cast<int32_t>(static_cast<uint32_t>(packed)),
                       static_cast<int32_t>(static_cast<uint32_t>(packed >> 32))});
    }
    return true;
}

inline bool ReadUnitFootprint(int unitIndex, std::vector<FootprintCell>& out) {
    return g_UnitFootprintReaderOverride
        ? g_UnitFootprintReaderOverride(unitIndex, out)
        : ReadNativeUnitFootprint(unitIndex, out);
}

inline int ReadBasicRange(int actorIndex) {
    if (g_BasicRangeReaderOverride) return g_BasicRangeReaderOverride(actorIndex);
    if (!NativeActions::getBasicRange || actorIndex < 0 || actorIndex >= Combat::kMaxUnits) return -1;
    const uintptr_t actor = AutoPlay::GetUnitPtr(actorIndex);
    if (!NativeGeometry::IsReadableRange(actor, kBattleUnitFootprintCountOffset + sizeof(int16_t))) return -1;
    return NativeActions::getBasicRange(reinterpret_cast<void*>(actor));
}

inline bool BasicReachFrom(int actorIndex, int sourceX, int sourceZ,
                           int targetIndex, int targetX, int targetZ, int range) {
    if (g_BasicReachReaderOverride)
        return g_BasicReachReaderOverride(actorIndex, sourceX, sourceZ, targetIndex, targetX, targetZ, range);
    if (!NativeActions::basicReachFrom || actorIndex < 0 || actorIndex >= Combat::kMaxUnits ||
        targetIndex < 0 || targetIndex >= Combat::kMaxUnits) return false;
    const uintptr_t actor = AutoPlay::GetUnitPtr(actorIndex);
    const uintptr_t target = AutoPlay::GetUnitPtr(targetIndex);
    if (!NativeGeometry::IsReadableRange(actor, kBattleUnitFootprintCountOffset + sizeof(int16_t)) ||
        !NativeGeometry::IsReadableRange(target, kBattleUnitFootprintCountOffset + sizeof(int16_t))) return false;
    return NativeActions::basicReachFrom(reinterpret_cast<void*>(actor), sourceX, sourceZ,
        reinterpret_cast<void*>(target), targetX, targetZ, range) != 0;
}

inline bool IsLineWeapon(int actorIndex) {
    if (g_LineWeaponReaderOverride) return g_LineWeaponReaderOverride(actorIndex);
    if (!NativeActions::isLineWeapon || actorIndex < 0 || actorIndex >= Combat::kMaxUnits) return false;
    const uintptr_t actor = AutoPlay::GetUnitPtr(actorIndex);
    if (!NativeGeometry::IsReadableRange(actor, kBattleUnitFootprintCountOffset + sizeof(int16_t))) return false;
    return NativeActions::isLineWeapon(reinterpret_cast<void*>(actor)) != 0;
}

inline bool ReadNativeHeight(int x, int z, int16_t& outHeight) {
    outHeight = 0;
    if (!TacticalPath::InBounds(x, z) || !AutoPlay::g_Base ||
        AutoPlay::g_Base > (std::numeric_limits<uintptr_t>::max)() - kHeightGridRva) return false;
    const uintptr_t grid = AutoPlay::g_Base + kHeightGridRva;
    const uintptr_t cell = grid + (static_cast<uintptr_t>(x) * 128u + static_cast<uintptr_t>(z)) * sizeof(int16_t);
    if (!NativeGeometry::IsReadableRange(cell, sizeof(int16_t))) return false;
    std::memcpy(&outHeight, reinterpret_cast<const void*>(cell), sizeof(outHeight));
    return true;
}

inline bool ReadNativeHeightLimits(const NativeSkills::Entry& skill,
                                   uint8_t& maxBelow, uint8_t& maxAbove) {
    maxBelow = 0;
    maxAbove = 0;
    if (!skill.descriptor ||
        !NativeGeometry::IsReadableRange(skill.descriptor + kDescriptorMinHeightOffset, 2)) return false;
    std::memcpy(&maxBelow, reinterpret_cast<const void*>(skill.descriptor + kDescriptorMinHeightOffset), 1);
    std::memcpy(&maxAbove, reinterpret_cast<const void*>(skill.descriptor + kDescriptorMaxHeightOffset), 1);
    return true;
}

inline int ResolveConfiguredShape(int actorIndex, const NativeSkills::Entry& skill) {
    return g_ConfiguredShapeReaderOverride
        ? g_ConfiguredShapeReaderOverride(actorIndex, skill)
        : NativeActions::ConfiguredShape(actorIndex, skill);
}

inline bool ReadHeight(int x, int z, int16_t& outHeight) {
    return g_HeightReaderOverride ? g_HeightReaderOverride(x, z, outHeight)
                                  : ReadNativeHeight(x, z, outHeight);
}

inline bool BasicVerticalLegal(int sourceX, int sourceZ, int targetIndex,
                               int targetX, int targetZ, int range) {
    if (g_BasicVerticalReaderOverride)
        return g_BasicVerticalReaderOverride(sourceX, sourceZ, targetIndex, targetX, targetZ, range);
    if (targetIndex < 0 || targetIndex >= Combat::kMaxUnits) return false;
    int16_t sourceY = 0;
    if (!ReadHeight(sourceX, sourceZ, sourceY)) return false;
    const uintptr_t target = AutoPlay::GetUnitPtr(targetIndex);
    if (!NativeGeometry::IsReadableRange(target + 0x0C, sizeof(float))) return false;
    float targetY = 0.0f;
    std::memcpy(&targetY, reinterpret_cast<const void*>(target + 0x0C), sizeof(targetY));
    return std::isfinite(targetY) &&
        std::fabs(targetY - static_cast<float>(sourceY)) <= static_cast<float>((range + 1) * 6);
}

inline bool ReadHeightLimits(const NativeSkills::Entry& skill,
                             uint8_t& maxBelow, uint8_t& maxAbove) {
    return g_HeightLimitsReaderOverride
        ? g_HeightLimitsReaderOverride(skill, maxBelow, maxAbove)
        : ReadNativeHeightLimits(skill, maxBelow, maxAbove);
}

inline bool RotateOffset(int x, int z, int rotation, int& rotatedX, int& rotatedZ) {
    switch (rotation) {
    case 0: rotatedX = x;  rotatedZ = z;  return true;
    case 1: rotatedX = z;  rotatedZ = -x; return true;
    case 2: rotatedX = -x; rotatedZ = -z; return true;
    case 3: rotatedX = -z; rotatedZ = x;  return true;
    default: return false;
    }
}

enum class JobStage : uint8_t {
    Idle,
    BasicAttacks,
    SkillStart,
    ShapeCacheSearch,
    ShapeRecordSearch,
    ShapeCellCopy,
    ShapeCheck,
    Seeds,
    ValidateArea,
    SourceTiles,
    Complete,
    Failed
};

class GoalJob {
public:
    // Copies the compact planning inputs. Native lookup and per-target work is
    // deferred to Step so the caller can spread it over successive frames.
    bool Begin(int actorIndex, const Combat::BattleSnapshot& snapshot,
               const std::vector<NativeSkills::Entry>& entries) {
        Reset();
        stage_ = JobStage::Failed;
        if (actorIndex < 0 || actorIndex >= Combat::kMaxUnits) return false;
        const Combat::UnitSnapshot& actor = snapshot.units[static_cast<size_t>(actorIndex)];
        if (actor.index != actorIndex || !actor.alive || !actor.deployed ||
            actor.faction != Combat::Faction::Player ||
            !TacticalPath::InBounds(actor.gridX, actor.gridZ)) return false;

        actorIndex_ = actorIndex;
        actorX_ = actor.gridX;
        actorZ_ = actor.gridZ;
        battle_ = snapshot;
        entries_ = entries;
        shapeCache_.clear();
        shapeCache_.reserve(std::min(entries_.size(), kMaxShapeCacheEntries));
        BuildOccupancy();

        std::vector<FootprintCell> footprint;
        if (ReadUnitFootprint(actorIndex, footprint) && footprint.size() <= kMaxFootprintCells) {
            actorFootprintCount_ = static_cast<uint16_t>(footprint.size());
            for (size_t i = 0; i < footprint.size(); ++i) actorFootprint_[i] = footprint[i];
        }
        basicRange_ = ReadBasicRange(actorIndex);
        if (basicRange_ < 0 || basicRange_ > 127) basicRange_ = -1;
        basicLineWeapon_ = IsLineWeapon(actorIndex);

        BuildDesiredTargetLists();
        stage_ = JobStage::BasicAttacks;
        return true;
    }

    // One work unit processes at most one basic source tile, table record, shape
    // cell, target/offset/rotation seed, area cell, or potential source tile.
    // The budget is capped so an accidental large caller value stays bounded.
    bool Step(size_t budget = 256) {
        lastWork_ = 0;
        budget = std::min<size_t>(budget, 256);
        while (lastWork_ < budget && stage_ != JobStage::Complete && stage_ != JobStage::Failed) {
            Planning::Unit operation;
            if (!operation) break;
            ProcessOne();
            ++lastWork_;
        }
        return stage_ == JobStage::Complete;
    }

    bool IsComplete() const { return stage_ == JobStage::Complete || stage_ == JobStage::Failed; }
    bool Succeeded() const { return stage_ == JobStage::Complete; }
    bool Failed() const { return stage_ == JobStage::Failed; }
    JobStage Stage() const { return stage_; }
    const TacticalPath::CellMask& Goals() const { return goals_; }
    size_t LastWorkUnits() const { return lastWork_; }
    size_t ProcessedBasicPairs() const { return processedBasicPairs_; }
    size_t ProcessedShapeRecords() const { return processedShapeRecords_; }
    size_t ProcessedSeeds() const { return processedSeeds_; }

private:
    struct ShapeCacheEntry {
        uint32_t id = 0;
        bool valid = false;
        NativeGeometry::ShapeDescriptor shape{};
    };

    void Reset() {
        stage_ = JobStage::Idle;
        actorIndex_ = -1;
        actorX_ = actorZ_ = 0;
        battle_ = Combat::BattleSnapshot{};
        entries_.clear();
        goals_.fill(0);
        for (auto& column : goalColumns_) column.fill(0);
        actorFootprint_.fill({});
        actorFootprintCount_ = 0;
        targetFootprint_.fill({});
        targetFootprintCount_ = 0;
        basicTargetCursor_ = 0;
        basicRange_ = -1;
        basicLineWeapon_ = false;
        basicTargetPrepared_ = false;
        basicMinX_ = basicMaxX_ = basicMinZ_ = basicMaxZ_ = 0;
        basicSourceX_ = basicSourceZ_ = 0;
        basicLineX_.fill(0);
        basicLineZ_.fill(0);
        enemyTargetCount_ = 0;
        healTargetCount_ = 0;
        enemyTargets_.fill(-1);
        healTargets_.fill(-1);
        friendlyMask_.fill(0);
        enemyMask_.fill(0);
        unknownOrOtherMask_.fill(0);
        currentAreaStamp_.fill(0);
        areaGeneration_ = 0;
        skillCursor_ = 0;
        cacheCursor_ = 0;
        tableRecordCursor_ = 0;
        shapeCellCursor_ = 0;
        shapeCheckCursor_ = 0;
        targetCursor_ = 0;
        shapeCellIndex_ = 0;
        rotation_ = 0;
        areaCellCursor_ = 0;
        areaSafe_ = true;
        sourceActive_ = false;
        sourceDx_ = sourceDz_ = 0;
        sourceRange_ = 0;
        currentSkill_ = NativeSkills::Entry{};
        currentShape_ = NativeGeometry::ShapeDescriptor{};
        currentShapeId_ = 0;
        currentShapeFound_ = false;
        currentSkillValid_ = false;
        currentMinHeight_ = currentMaxHeight_ = 0;
        tableHeader_ = 0;
        tableRecords_ = 0;
        tableCells_ = 0;
        tableRecordCount_ = 0;
        tableCellCount_ = 0;
        tableRecord_ = NativeGeometry::ShapeRecord{};
        centerX_ = centerZ_ = 0;
        targetUnitIndex_ = -1;
        lastWork_ = 0;
        processedBasicPairs_ = 0;
        processedShapeRecords_ = 0;
        processedSeeds_ = 0;
    }

    void BuildOccupancy() {
        for (int i = 0; i < Combat::kMaxUnits; ++i) {
            const Combat::UnitSnapshot& unit = battle_.units[static_cast<size_t>(i)];
            if (unit.index != i || !unit.alive || !unit.deployed ||
                !TacticalPath::InBounds(unit.gridX, unit.gridZ) || unit.index == actorIndex_) continue;
            const size_t cell = static_cast<size_t>(TacticalPath::CellIndex(unit.gridX, unit.gridZ));
            switch (unit.faction) {
            case Combat::Faction::Player: friendlyMask_[cell] = 1; break;
            case Combat::Faction::Enemy: enemyMask_[cell] = 1; break;
            default: unknownOrOtherMask_[cell] = 1; break;
            }
        }
    }

    void BuildDesiredTargetLists() {
        for (int i = 0; i < Combat::kMaxUnits; ++i) {
            const Combat::UnitSnapshot& unit = battle_.units[static_cast<size_t>(i)];
            if (unit.index != i || !unit.alive || !unit.deployed ||
                !TacticalPath::InBounds(unit.gridX, unit.gridZ)) continue;
            if (unit.faction == Combat::Faction::Enemy && Combat::ProjectedHp(battle_, i) > 0.0)
                enemyTargets_[enemyTargetCount_++] = i;
            if (unit.faction == Combat::Faction::Player &&
                Combat::IsFinitePositive(unit.maxHp) &&
                Combat::ProjectedHp(battle_, i) > 0.0 &&
                Combat::ProjectedHp(battle_, i) < unit.maxHp * Combat::kOrdinaryHealHpFraction)
                healTargets_[healTargetCount_++] = i;
        }
    }

    void ProcessOne() {
        switch (stage_) {
        case JobStage::BasicAttacks: ProcessBasicSource(); break;
        case JobStage::SkillStart: ProcessSkillStart(); break;
        case JobStage::ShapeCacheSearch: ProcessCacheEntry(); break;
        case JobStage::ShapeRecordSearch: ProcessShapeRecord(); break;
        case JobStage::ShapeCellCopy: ProcessShapeCellCopy(); break;
        case JobStage::ShapeCheck: ProcessShapeCellCheck(); break;
        case JobStage::Seeds: ProcessSeed(); break;
        case JobStage::ValidateArea: ProcessAreaCell(); break;
        case JobStage::SourceTiles: ProcessSourceTile(); break;
        default: stage_ = JobStage::Failed; break;
        }
    }

    void ProcessBasicSource() {
        if (basicRange_ < 0 || actorFootprintCount_ == 0 || basicTargetCursor_ >= enemyTargetCount_) {
            stage_ = JobStage::SkillStart;
            return;
        }
        if (!basicTargetPrepared_) {
            PrepareBasicTarget();
            return;
        }
        if (basicSourceX_ > basicMaxX_) {
            ++basicTargetCursor_;
            basicTargetPrepared_ = false;
            return;
        }
        const int targetIndex = enemyTargets_[basicTargetCursor_];
        const Combat::UnitSnapshot& target = battle_.units[static_cast<size_t>(targetIndex)];
        const int x = basicSourceX_;
        const int z = basicSourceZ_;
        ++processedBasicPairs_;
        const size_t cell = static_cast<size_t>(TacticalPath::CellIndex(x, z));
        bool legal = !friendlyMask_[cell] && !enemyMask_[cell] && !unknownOrOtherMask_[cell];
        if (legal && basicLineWeapon_)
            legal = basicLineX_[static_cast<size_t>(x)] || basicLineZ_[static_cast<size_t>(z)];
        if (legal)
            legal = BasicReachFrom(actorIndex_, x, z, targetIndex, target.gridX, target.gridZ, basicRange_);
        if (legal)
            legal = BasicVerticalLegal(x, z, targetIndex, target.gridX, target.gridZ, basicRange_);
        if (legal) SetGoal(x, z);

        if (++basicSourceZ_ > basicMaxZ_) {
            basicSourceZ_ = basicMinZ_;
            ++basicSourceX_;
        }
    }

    void PrepareBasicTarget() {
        if (basicTargetCursor_ >= enemyTargetCount_) {
            stage_ = JobStage::SkillStart;
            return;
        }
        const int targetIndex = enemyTargets_[basicTargetCursor_];
        const auto& target = battle_.units[static_cast<size_t>(targetIndex)];
        std::vector<FootprintCell> footprint;
        if (!ReadUnitFootprint(targetIndex, footprint) || footprint.empty() ||
            footprint.size() > kMaxFootprintCells) {
            ++basicTargetCursor_;
            return;
        }
        targetFootprintCount_ = static_cast<uint16_t>(footprint.size());
        for (size_t i = 0; i < footprint.size(); ++i) targetFootprint_[i] = footprint[i];
        for (size_t i = 0; i < targetFootprintCount_; ++i) {
            const int64_t occupiedX = static_cast<int64_t>(target.gridX) + targetFootprint_[i].x;
            const int64_t occupiedZ = static_cast<int64_t>(target.gridZ) + targetFootprint_[i].z;
            if (occupiedX >= 0 && occupiedX < TacticalPath::kGridWidth &&
                occupiedZ >= 0 && occupiedZ < TacticalPath::kGridHeight)
                enemyMask_[static_cast<size_t>(TacticalPath::CellIndex(
                    static_cast<int>(occupiedX), static_cast<int>(occupiedZ)))] = 1;
        }

        int32_t actorMinX = actorFootprint_[0].x, actorMaxX = actorFootprint_[0].x;
        int32_t actorMinZ = actorFootprint_[0].z, actorMaxZ = actorFootprint_[0].z;
        for (size_t i = 1; i < actorFootprintCount_; ++i) {
            actorMinX = std::min(actorMinX, actorFootprint_[i].x);
            actorMaxX = std::max(actorMaxX, actorFootprint_[i].x);
            actorMinZ = std::min(actorMinZ, actorFootprint_[i].z);
            actorMaxZ = std::max(actorMaxZ, actorFootprint_[i].z);
        }
        int32_t targetMinX = targetFootprint_[0].x, targetMaxX = targetFootprint_[0].x;
        int32_t targetMinZ = targetFootprint_[0].z, targetMaxZ = targetFootprint_[0].z;
        for (size_t i = 1; i < targetFootprintCount_; ++i) {
            targetMinX = std::min(targetMinX, targetFootprint_[i].x);
            targetMaxX = std::max(targetMaxX, targetFootprint_[i].x);
            targetMinZ = std::min(targetMinZ, targetFootprint_[i].z);
            targetMaxZ = std::max(targetMaxZ, targetFootprint_[i].z);
        }
        const int64_t r = basicRange_;
        const int64_t left = static_cast<int64_t>(target.gridX) + targetMinX - actorMaxX - r;
        const int64_t right = static_cast<int64_t>(target.gridX) + targetMaxX - actorMinX + r;
        const int64_t top = static_cast<int64_t>(target.gridZ) + targetMinZ - actorMaxZ - r;
        const int64_t bottom = static_cast<int64_t>(target.gridZ) + targetMaxZ - actorMinZ + r;
        if (right < 0 || left >= TacticalPath::kGridWidth ||
            bottom < 0 || top >= TacticalPath::kGridHeight) {
            ++basicTargetCursor_;
            return;
        }
        basicMinX_ = static_cast<int>(std::clamp<int64_t>(left, 0, TacticalPath::kGridWidth - 1));
        basicMaxX_ = static_cast<int>(std::clamp<int64_t>(right, 0, TacticalPath::kGridWidth - 1));
        basicMinZ_ = static_cast<int>(std::clamp<int64_t>(top, 0, TacticalPath::kGridHeight - 1));
        basicMaxZ_ = static_cast<int>(std::clamp<int64_t>(bottom, 0, TacticalPath::kGridHeight - 1));
        basicSourceX_ = basicMinX_;
        basicSourceZ_ = basicMinZ_;
        basicLineX_.fill(0);
        basicLineZ_.fill(0);
        if (basicLineWeapon_) {
            for (size_t a = 0; a < actorFootprintCount_; ++a) {
                for (size_t t = 0; t < targetFootprintCount_; ++t) {
                    const int64_t sx = static_cast<int64_t>(target.gridX) + targetFootprint_[t].x - actorFootprint_[a].x;
                    const int64_t sz = static_cast<int64_t>(target.gridZ) + targetFootprint_[t].z - actorFootprint_[a].z;
                    if (sx >= 0 && sx < TacticalPath::kGridWidth) basicLineX_[static_cast<size_t>(sx)] = 1;
                    if (sz >= 0 && sz < TacticalPath::kGridHeight) basicLineZ_[static_cast<size_t>(sz)] = 1;
                }
            }
        }
        basicTargetPrepared_ = basicMinX_ <= basicMaxX_ && basicMinZ_ <= basicMaxZ_;
        if (!basicTargetPrepared_) ++basicTargetCursor_;
    }

    void ProcessSkillStart() {
        if (skillCursor_ >= entries_.size()) {
            stage_ = JobStage::Complete;
            return;
        }
        currentSkill_ = entries_[skillCursor_];
        currentSkillValid_ = currentSkill_.slot >= 0 && currentSkill_.skillId >= 0 &&
            currentSkill_.descriptor != 0 && currentSkill_.learned &&
            currentSkill_.currentlyUsable && currentSkill_.affordableFromSp &&
            currentSkill_.rangeFlag <= 1 &&
            (currentSkill_.healing ? healTargetCount_ > 0 : enemyTargetCount_ > 0);
        if (!currentSkillValid_) {
            ++skillCursor_;
            return;
        }

        const int resolvedShape = ResolveConfiguredShape(actorIndex_, currentSkill_);
        if (resolvedShape < 0) {
            ++skillCursor_;
            return;
        }
        currentShapeId_ = static_cast<uint32_t>(resolvedShape);
        if (!ReadHeightLimits(currentSkill_, currentMinHeight_, currentMaxHeight_)) {
            ++skillCursor_;
            return;
        }

        cacheCursor_ = 0;
        stage_ = JobStage::ShapeCacheSearch;
    }

    void ProcessCacheEntry() {
        if (cacheCursor_ < shapeCache_.size()) {
            const ShapeCacheEntry& cached = shapeCache_[cacheCursor_++];
            if (cached.id != currentShapeId_) return;
            if (!cached.valid) {
                ++skillCursor_;
                stage_ = JobStage::SkillStart;
                return;
            }
            currentShape_ = cached.shape;
            PrepareShapeSeeds();
            return;
        }
        if (!BeginShapeTableSearch()) {
            CacheCurrentShape(false);
            ++skillCursor_;
            stage_ = JobStage::SkillStart;
            return;
        }
        if (stage_ != JobStage::ShapeCellCopy && stage_ != JobStage::ShapeCheck)
            stage_ = JobStage::ShapeRecordSearch;
    }

    bool BeginShapeTableSearch() {
        if (NativeGeometry::ShapeCacheReady()) {
            if (!NativeGeometry::ReadShape(currentShapeId_, currentShape_)) return false;
            shapeCheckCursor_ = 0;
            stage_ = JobStage::ShapeCheck;
            return true;
        }
        if (!AutoPlay::g_Base ||
            !NativeGeometry::ReadGlobalPointer(AutoPlay::g_Base, NativeGeometry::kShapeHeaderPointerRva,
                                               tableHeader_) ||
            !NativeGeometry::ReadGlobalPointer(AutoPlay::g_Base, NativeGeometry::kShapeRecordsPointerRva,
                                               tableRecords_) ||
            !NativeGeometry::ReadGlobalPointer(AutoPlay::g_Base, NativeGeometry::kShapeCellsPointerRva,
                                               tableCells_) ||
            !NativeGeometry::IsReadableRange(tableHeader_, sizeof(NativeGeometry::ShapeTableHeader), true)) return false;

        NativeGeometry::ShapeTableHeader header{};
        std::memcpy(&header, reinterpret_cast<const void*>(tableHeader_), sizeof(header));
        if (header.recordCount == 0 || header.recordCount > NativeGeometry::kMaxShapeRecords ||
            header.cellCount > NativeGeometry::kMaxShapeTableCells) return false;
        const uint64_t recordBytes = static_cast<uint64_t>(header.recordCount) * sizeof(NativeGeometry::ShapeRecord);
        if (recordBytes > (std::numeric_limits<size_t>::max)() ||
            !NativeGeometry::IsReadableRange(tableRecords_, static_cast<size_t>(recordBytes), true)) return false;
        tableRecordCount_ = header.recordCount;
        tableCellCount_ = header.cellCount;
        tableRecordCursor_ = 0;
        currentShapeFound_ = currentShapeId_ == 0;
        if (currentShapeFound_) {
            tableRecordCursor_ = 0;
            return BeginShapeCellCopyAt(0);
        }
        return true;
    }

    void ProcessShapeRecord() {
        if (tableRecordCursor_ >= tableRecordCount_) {
            CacheCurrentShape(false);
            ++skillCursor_;
            stage_ = JobStage::SkillStart;
            return;
        }
        NativeGeometry::ShapeRecord record{};
        const uintptr_t address = tableRecords_ + static_cast<uintptr_t>(tableRecordCursor_) *
            sizeof(NativeGeometry::ShapeRecord);
        std::memcpy(&record, reinterpret_cast<const void*>(address), sizeof(record));
        ++processedShapeRecords_;
        ++tableRecordCursor_;
        if (record.id != static_cast<uint16_t>(currentShapeId_)) return;
        tableRecord_ = record;
        if (!BeginShapeCellCopy(record)) {
            CacheCurrentShape(false);
            ++skillCursor_;
            stage_ = JobStage::SkillStart;
            return;
        }
        stage_ = JobStage::ShapeCellCopy;
    }

    bool BeginShapeCellCopyAt(uint32_t index) {
        if (index >= tableRecordCount_) return false;
        NativeGeometry::ShapeRecord record{};
        std::memcpy(&record, reinterpret_cast<const void*>(tableRecords_ +
            static_cast<uintptr_t>(index) * sizeof(NativeGeometry::ShapeRecord)), sizeof(record));
        tableRecord_ = record;
        if (!BeginShapeCellCopy(record)) return false;
        stage_ = JobStage::ShapeCellCopy;
        return true;
    }

    bool BeginShapeCellCopy(const NativeGeometry::ShapeRecord& record) {
        const uint64_t endCell = static_cast<uint64_t>(record.firstCell) + record.cellCount;
        if (endCell > tableCellCount_ || record.cellCount == 0 ||
            record.cellCount > NativeGeometry::kMaxShapeCells) return false;
        const uintptr_t byteOffset = static_cast<uintptr_t>(record.firstCell) * sizeof(NativeGeometry::ShapeCell);
        if (byteOffset > (std::numeric_limits<uintptr_t>::max)() - tableCells_) return false;
        const size_t bytes = static_cast<size_t>(record.cellCount) * sizeof(NativeGeometry::ShapeCell);
        if (!NativeGeometry::IsReadableRange(tableCells_ + byteOffset, bytes, true)) return false;
        currentShape_ = NativeGeometry::ShapeDescriptor{};
        currentShape_.id = currentShapeId_;
        currentShape_.count = record.cellCount;
        currentShape_.flag = record.flags != 0;
        shapeCellCursor_ = 0;
        return true;
    }

    void ProcessShapeCellCopy() {
        if (shapeCellCursor_ >= tableRecord_.cellCount) {
            shapeCheckCursor_ = 0;
            stage_ = JobStage::ShapeCheck;
            return;
        }
        const uintptr_t offset = (static_cast<uintptr_t>(tableRecord_.firstCell) + shapeCellCursor_) *
            sizeof(NativeGeometry::ShapeCell);
        std::memcpy(&currentShape_.cells[shapeCellCursor_],
                    reinterpret_cast<const void*>(tableCells_ + offset), sizeof(NativeGeometry::ShapeCell));
        const auto& cell = currentShape_.cells[shapeCellCursor_];
        const uint32_t distance = static_cast<uint32_t>((cell.x < 0 ? -cell.x : cell.x) +
                                                        (cell.z < 0 ? -cell.z : cell.z));
        currentShape_.farthest = std::max(currentShape_.farthest, distance);
        ++shapeCellCursor_;
    }

    void ProcessShapeCellCheck() {
        if (shapeCheckCursor_ >= currentShape_.count) {
            CacheCurrentShape(true);
            PrepareShapeSeeds();
            return;
        }
        const int8_t kind = currentShape_.cells[shapeCheckCursor_++].kind;
        // Configured offensive/healing shapes with movement or unknown cells
        // are skipped instead of guessing how those cells affect legality.
        if (!NativeGeometry::IsHitKind(kind)) {
            CacheCurrentShape(false);
            ++skillCursor_;
            stage_ = JobStage::SkillStart;
        }
    }

    void CacheCurrentShape(bool valid) {
        if (shapeCache_.size() >= kMaxShapeCacheEntries) return;
        shapeCache_.push_back({currentShapeId_, valid,
            valid ? currentShape_ : NativeGeometry::ShapeDescriptor{}});
    }

    void PrepareShapeSeeds() {
        targetCursor_ = 0;
        shapeCellIndex_ = 0;
        rotation_ = 0;
        stage_ = JobStage::Seeds;
    }

    bool GetCurrentTarget(int& index) const {
        const auto& targets = currentSkill_.healing ? healTargets_ : enemyTargets_;
        const size_t count = currentSkill_.healing ? healTargetCount_ : enemyTargetCount_;
        if (targetCursor_ >= count) return false;
        index = targets[targetCursor_];
        return true;
    }

    void AdvanceSeed() {
        rotation_ = 0;
        if (++shapeCellIndex_ >= currentShape_.count) {
            shapeCellIndex_ = 0;
            ++targetCursor_;
        }
        const size_t targetCount = currentSkill_.healing ? healTargetCount_ : enemyTargetCount_;
        if (targetCursor_ >= targetCount) {
            ++skillCursor_;
            stage_ = JobStage::SkillStart;
        } else {
            stage_ = JobStage::Seeds;
        }
    }

    void ProcessSeed() {
        int targetIndex = -1;
        if (!GetCurrentTarget(targetIndex)) {
            ++skillCursor_;
            stage_ = JobStage::SkillStart;
            return;
        }
        const Combat::UnitSnapshot& target = battle_.units[static_cast<size_t>(targetIndex)];
        if (shapeCellIndex_ >= currentShape_.count) {
            AdvanceSeed();
            return;
        }
        int dx = 0, dz = 0;
        if (!RotateOffset(currentShape_.cells[shapeCellIndex_].x,
                          currentShape_.cells[shapeCellIndex_].z, rotation_, dx, dz)) {
            AdvanceSeed();
            return;
        }
        centerX_ = target.gridX - dx;
        centerZ_ = target.gridZ - dz;
        targetUnitIndex_ = targetIndex;
        ++processedSeeds_;
        if (!TacticalPath::InBounds(centerX_, centerZ_)) {
            AdvanceRotation();
            return;
        }
        areaCellCursor_ = 0;
        areaSafe_ = true;
        if (++areaGeneration_ == 0) {
            currentAreaStamp_.fill(0);
            areaGeneration_ = 1;
        }
        stage_ = JobStage::ValidateArea;
    }

    void AdvanceRotation() {
        if (++rotation_ >= 4) AdvanceSeed();
        else stage_ = JobStage::Seeds;
    }

    void ProcessAreaCell() {
        if (!areaSafe_) {
            AdvanceRotation();
            return;
        }
        if (areaCellCursor_ >= currentShape_.count) {
            BeginSourceEnumeration();
            return;
        }
        const NativeGeometry::ShapeCell& cell = currentShape_.cells[areaCellCursor_++];
        int dx = 0, dz = 0;
        if (!RotateOffset(cell.x, cell.z, rotation_, dx, dz)) {
            areaSafe_ = false;
            return;
        }
        const int hitX = centerX_ + dx;
        const int hitZ = centerZ_ + dz;
        if (!TacticalPath::InBounds(hitX, hitZ)) {
            areaSafe_ = false;
            return;
        }
        const size_t cellIndex = static_cast<size_t>(TacticalPath::CellIndex(hitX, hitZ));
        currentAreaStamp_[cellIndex] = areaGeneration_;
        if (currentSkill_.healing) {
            if (enemyMask_[cellIndex] || unknownOrOtherMask_[cellIndex]) areaSafe_ = false;
        } else if (friendlyMask_[cellIndex] || unknownOrOtherMask_[cellIndex]) {
            areaSafe_ = false;
        }
    }

    void BeginSourceEnumeration() {
        sourceRange_ = std::min<int>(currentSkill_.range, 127);
        sourceDx_ = std::max(-sourceRange_, -centerX_);
        sourceMaxDx_ = std::min(sourceRange_, 127-centerX_);
        sourceDz_ = std::max(-(sourceRange_ - std::abs(sourceDx_)), -centerZ_);
        sourceActive_ = true;
        stage_ = JobStage::SourceTiles;
    }

    void AdvanceSource() {
        const int reach = std::min(sourceRange_ - std::abs(sourceDx_), 127-centerZ_);
        if (++sourceDz_ > reach) {
            AdvanceSourceColumn();
        }
    }

    void AdvanceSourceColumn() {
        if (++sourceDx_ > sourceMaxDx_) {
            sourceActive_ = false; AdvanceRotation(); return;
        }
        sourceDz_ = std::max(-(sourceRange_ - std::abs(sourceDx_)), -centerZ_);
    }

    bool SelectUnprovenSource() {
        const int x = centerX_ + sourceDx_;
        const int reach = sourceRange_ - std::abs(sourceDx_);
        const int first = std::max(centerZ_ + sourceDz_, std::max(0, centerZ_-reach));
        const int last = std::min(127, centerZ_+reach);
        if (first > last || x < 0 || x > 127) return false;
        // At most two 64-bit words. This skips already-proven union members
        // and out-of-map portions without scanning every tile of every seed.
        for (int word=first/64; word<=last/64; ++word) {
            const int lo = word==first/64 ? first%64 : 0;
            const int hi = word==last/64 ? last%64 : 63;
            uint64_t mask = ~goalColumns_[x][word] & (~uint64_t{0} << lo);
            if (hi < 63) mask &= (uint64_t{1} << (hi+1))-1;
            if (currentSkill_.rangeFlag == 1) {
                const uint64_t center = centerZ_/64==word ? uint64_t{1} << (centerZ_%64) : 0;
                mask &= sourceDx_ == 0 ? ~center : center;
            }
            unsigned long bit = 0;
            if (mask && _BitScanForward64(&bit, mask)) {
                sourceDz_ = word*64+static_cast<int>(bit)-centerZ_; return true;
            }
        }
        return false;
    }

    void ProcessSourceTile() {
        Planning::QueryScope profile(Planning::QueryKind::SourceTile);
        if (!sourceActive_) {
            AdvanceRotation();
            return;
        }
        if (!SelectUnprovenSource()) { AdvanceSourceColumn(); return; }
        const int sx = centerX_ + sourceDx_;
        const int sz = centerZ_ + sourceDz_;
        // This job builds a union of useful movement positions. Once any
        // verified action proves a tile useful, later placements cannot add
        // anything there. Keep enumerating placements, skip redundant probes.
        if (TacticalPath::InBounds(sx, sz) && goals_[static_cast<size_t>(TacticalPath::CellIndex(sx, sz))]) {
            AdvanceSource();
            return;
        }
        const int dx = std::abs(sourceDx_);
        const int dz = std::abs(sourceDz_);
        const int manhattan = dx + dz;
        bool legal = TacticalPath::InBounds(sx, sz) && manhattan <= currentSkill_.range;
        if (legal && currentSkill_.rangeFlag == 1) {
            const bool sameX = sx == centerX_;
            const bool sameZ = sz == centerZ_;
            legal = sameX != sameZ; // Native range flag 1 permits one shared axis, not the same cell.
        }
        if (legal && !currentSkill_.healing &&
            currentAreaStamp_[static_cast<size_t>(TacticalPath::CellIndex(sx, sz))] == areaGeneration_)
            legal = false;
        if (legal) {
            int16_t sourceHeight = 0;
            int16_t targetHeight = 0;
            legal = ReadHeight(sx, sz, sourceHeight) &&
                ReadHeight(battle_.units[static_cast<size_t>(targetUnitIndex_)].gridX,
                           battle_.units[static_cast<size_t>(targetUnitIndex_)].gridZ, targetHeight);
            if (legal) {
                const int delta = static_cast<int>(targetHeight) - static_cast<int>(sourceHeight);
                legal = delta >= -static_cast<int>(currentMinHeight_) &&
                    delta <= static_cast<int>(currentMaxHeight_);
            }
        }
        if (legal) SetGoal(sx, sz);
        AdvanceSource();
    }

    void SetGoal(int x, int z) {
        if (!TacticalPath::InBounds(x, z)) return;
        const int cell = TacticalPath::CellIndex(x, z);
        goals_[static_cast<size_t>(cell)] = 1;
        goalColumns_[x][z/64] |= uint64_t{1} << (z%64);
    }

private:
    JobStage stage_ = JobStage::Idle;
    int actorIndex_ = -1;
    int actorX_ = 0;
    int actorZ_ = 0;
    Combat::BattleSnapshot battle_{};
    std::vector<NativeSkills::Entry> entries_;
    TacticalPath::CellMask goals_{};
    std::array<std::array<uint64_t, 2>, 128> goalColumns_{};
    std::array<FootprintCell, kMaxFootprintCells> actorFootprint_{};
    uint16_t actorFootprintCount_ = 0;
    std::array<FootprintCell, kMaxFootprintCells> targetFootprint_{};
    uint16_t targetFootprintCount_ = 0;
    std::array<int, Combat::kMaxUnits> enemyTargets_{};
    std::array<int, Combat::kMaxUnits> healTargets_{};
    size_t enemyTargetCount_ = 0;
    size_t healTargetCount_ = 0;
    std::array<uint8_t, kGridCellCount> friendlyMask_{};
    std::array<uint8_t, kGridCellCount> enemyMask_{};
    std::array<uint8_t, kGridCellCount> unknownOrOtherMask_{};
    std::array<uint32_t, kGridCellCount> currentAreaStamp_{};
    uint32_t areaGeneration_ = 0;
    size_t basicTargetCursor_ = 0;
    int basicRange_ = -1;
    bool basicLineWeapon_ = false;
    bool basicTargetPrepared_ = false;
    int basicMinX_ = 0;
    int basicMaxX_ = -1;
    int basicMinZ_ = 0;
    int basicMaxZ_ = -1;
    int basicSourceX_ = 0;
    int basicSourceZ_ = 0;
    std::array<uint8_t, TacticalPath::kGridWidth> basicLineX_{};
    std::array<uint8_t, TacticalPath::kGridHeight> basicLineZ_{};
    std::vector<ShapeCacheEntry> shapeCache_;
    size_t skillCursor_ = 0;
    size_t cacheCursor_ = 0;
    uintptr_t tableHeader_ = 0;
    uintptr_t tableRecords_ = 0;
    uintptr_t tableCells_ = 0;
    uint32_t tableRecordCount_ = 0;
    uint32_t tableCellCount_ = 0;
    uint32_t tableRecordCursor_ = 0;
    NativeGeometry::ShapeRecord tableRecord_{};
    NativeSkills::Entry currentSkill_{};
    NativeGeometry::ShapeDescriptor currentShape_{};
    uint32_t currentShapeId_ = 0;
    bool currentShapeFound_ = false;
    bool currentSkillValid_ = false;
    uint8_t currentMinHeight_ = 0;
    uint8_t currentMaxHeight_ = 0;
    size_t shapeCellCursor_ = 0;
    size_t shapeCheckCursor_ = 0;
    size_t targetCursor_ = 0;
    size_t shapeCellIndex_ = 0;
    int rotation_ = 0;
    size_t areaCellCursor_ = 0;
    bool areaSafe_ = true;
    bool sourceActive_ = false;
    int sourceRange_ = 0;
    int sourceDx_ = 0;
    int sourceMaxDx_ = 0;
    int sourceDz_ = 0;
    int centerX_ = 0;
    int centerZ_ = 0;
    int targetUnitIndex_ = -1;
    size_t lastWork_ = 0;
    size_t processedBasicPairs_ = 0;
    size_t processedShapeRecords_ = 0;
    size_t processedSeeds_ = 0;
};

} // namespace AutoPlay::NativeActionPositions
