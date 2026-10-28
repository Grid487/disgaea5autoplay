#pragma once

#include "native_effects.h"
#include "native_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace AutoPlay::NativeDeployment {

constexpr uintptr_t kRosterBaseRva = 0x5207DB8;
constexpr uintptr_t kRosterTotalCountRva = 0x5574A9C;
constexpr size_t kRosterStride = 0x4750;
constexpr size_t kRosterCountOffset = 0x968;
constexpr size_t kRosterRowsOffset = 0x96A;
constexpr size_t kRosterFlagsOffset = 0xA6C;
constexpr size_t kTaskBattleContextOffset = 0x108;
constexpr size_t kTaskRosterCursorOffset = 0x1C0;
constexpr int kMaxRosterRows = 128;
constexpr int kMaxRosterOrdinal = 0x1000;

struct Row {
    int menuRow = -1;
    int rosterOrdinal = -1;
    uintptr_t charaData = 0;
    uint32_t rowFlags = 0;
};

struct Candidate {
    Row row{};
    double healingScore = 0.0;
    int64_t cheapestHealingSp = std::numeric_limits<int64_t>::max();
    double offenseScore = 0.0;
    double hitTieBreak = 0.0;
    bool living = false;
    bool hasAffordableHealingSkill = false;
};

struct SkillAttack {
    uintptr_t descriptor = 0;
    NativeSkills::DescriptorInfo info{};
    int skillRank = 0;
};

struct ActiveCandidate {
    bool active = false;
    Row row{};
    Candidate candidate{};
    uintptr_t charaData = 0;
    int64_t currentSp = 0;
    int rowCount = 0;
    int nextSkill = 0;
    size_t scoreEnemy = 0, scoreAttack = 0;
    double scoreSum = 0.0, scoreBest = 0.0;
    bool scoreEnemyStarted = false;
    std::array<uint32_t, NativeSkills::kMaxSkillRows> rows{};
    std::vector<SkillAttack> attacks;
};

inline uintptr_t RosterCharaData(int ordinal) {
    if (!AutoPlay::g_Base || ordinal < 0 || ordinal >= kMaxRosterOrdinal) return 0;
    const uintptr_t offset = kRosterBaseRva + static_cast<uintptr_t>(ordinal) * kRosterStride;
    if (AutoPlay::g_Base > (std::numeric_limits<uintptr_t>::max)() - offset) return 0;
    return AutoPlay::g_Base + offset;
}

inline int RosterTotalCount() {
    if (!AutoPlay::g_Base) return 0;
    const int count = *reinterpret_cast<const int16_t*>(AutoPlay::g_Base + kRosterTotalCountRva);
    return count > 0 && count <= kMaxRosterOrdinal ? count : 0;
}

inline double ReadStat(uintptr_t charaData, int selector) {
    if (!charaData || selector < 0 || selector > 10) return 0.0;
    if (NativeEffects::fnGetStat) {
        const int64_t value = NativeEffects::fnGetStat(reinterpret_cast<void*>(charaData), selector);
        if (NativeEffects::IsFiniteNativeValue(value) && value >= 0)
            return static_cast<double>(value);
    }
    const auto value = [charaData](size_t offset) {
        return static_cast<double>(*reinterpret_cast<const int64_t*>(charaData + offset));
    };
    switch (selector) {
    case 0: case 1: return value(0x2CE8);
    case 2: return value(0x2CF0);
    case 3: return value(0x2CF8);
    case 4: return value(0x2D00);
    case 5: return value(0x2D08);
    case 6: return (value(0x2D08) + value(0x2CE8)) / 2.0;
    case 7: return (value(0x2D10) + value(0x2CE8)) / 2.0;
    case 8: return (value(0x2D10) + value(0x2D08)) / 2.0;
    case 9: return (value(0x2CF8) + value(0x2CE8)) / 2.0;
    case 10: return (value(0x2D08) + value(0x2D00)) / 2.0;
    default: return 0.0;
    }
}

inline bool ReadEffectPower(uintptr_t descriptor, int rank, int& power) {
    if (!descriptor || rank < 0 || rank > 9) return false;
    if (NativeEffects::fnEffectPower) {
        power = NativeEffects::fnEffectPower(reinterpret_cast<void*>(descriptor), rank, 0);
        return power >= -100 && power <= 100000;
    }
    // This is the formula in RVA 0x1D7A20 for a primary effect of type 1.
    if (*reinterpret_cast<const uint8_t*>(descriptor + 0x3E0) != 1) return false;
    const int base = *reinterpret_cast<const int16_t*>(descriptor + 0x3E6);
    const int level = *reinterpret_cast<const int16_t*>(descriptor + 0x420 + rank * 2);
    power = base + level - 100;
    return power >= -100 && power <= 100000;
}

inline int64_t ReadRosterSkillCost(uintptr_t charaData, uintptr_t descriptor,
                                   uint32_t slot, int configuredShapeId) {
    if (!descriptor || !charaData || slot >= NativeSkills::kMaxSkillRows || configuredShapeId < 1)
        return -1;
    if (!NativeSkills::fnEffectiveCost) return -1;
    // RVA 0x1D7550 passes CharaData and BattleUnit through to 0x1D7600. Its
    // 0x1F4830 unit-dependent aura access is guarded by `unit != nullptr` and
    // the deployed-unit predicate. With a null BattleUnit, learned-row,
    // passive, enhancement, and area cost terms remain available without an
    // invented actor object or a game write.
    const bool fixedShape = *reinterpret_cast<const uint16_t*>(descriptor + 0x438) != 0;
    const uint32_t configuredAreaCount = fixedShape ? 1u : static_cast<uint32_t>(configuredShapeId);
    return NativeSkills::fnEffectiveCost(reinterpret_cast<void*>(descriptor),
        reinterpret_cast<void*>(charaData), nullptr, slot, configuredAreaCount);
}

inline bool SafeStationaryShape(uintptr_t charaData, uintptr_t descriptor,
                                int slot, int maxShapeChoices, int& shapeId) {
    shapeId = -1;
    if (!charaData || !descriptor || slot < 0 || slot >= static_cast<int>(NativeSkills::kMaxSkillRows))
        return false;
    shapeId = *reinterpret_cast<const uint16_t*>(descriptor + 0x438);
    if (shapeId == 0) {
        const int area = *reinterpret_cast<const uint8_t*>(charaData + 0xCCE +
            static_cast<uintptr_t>(slot) * NativeSkills::kSkillRowStride);
        if (area < 1 || area > 9 || area > maxShapeChoices) return false;
        shapeId = area;
    }
    NativeGeometry::ShapeDescriptor shape{};
    if (!NativeGeometry::ReadShape(static_cast<uint32_t>(shapeId), shape) || shape.count == 0)
        return false;
    for (uint32_t i = 0; i < shape.count; ++i) {
        const int8_t kind = shape.cells[i].kind;
        if (kind != static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit) &&
            kind != static_cast<int8_t>(NativeGeometry::ShapeCellKind::HitWithAlternateRule))
            return false;
    }
    return true;
}

struct EnemyProfile {
    double def = 0.0;
    double res = 0.0;
};

inline std::vector<EnemyProfile> ReadEnemies(const Combat::BattleSnapshot& snapshot) {
    std::vector<EnemyProfile> enemies;
    for (int i = 0; i < Combat::kMaxUnits; ++i) {
        const auto& unit = snapshot.units[static_cast<size_t>(i)];
        if (unit.index != i || !unit.alive || !unit.deployed || unit.faction != Combat::Faction::Enemy)
            continue;
        const uintptr_t battleUnit = AutoPlay::GetUnitPtr(i);
        const uintptr_t charaData = battleUnit ?
            *reinterpret_cast<const uintptr_t*>(battleUnit + 0x240) : 0;
        if (!charaData) continue;
        enemies.push_back({ReadStat(charaData, 2), ReadStat(charaData, 4)});
    }
    return enemies;
}

inline double DealerScore(uintptr_t charaData, const std::vector<EnemyProfile>& enemies,
                          const std::vector<SkillAttack>& attacks) {
    const double atk = ReadStat(charaData, 0);
    const double intelligence = ReadStat(charaData, 3);
    double score = 0.0;
    if (enemies.empty()) {
        score = std::max(atk, intelligence);
    } else {
        for (const auto& enemy : enemies) {
            double best = std::max(std::max(0.0, atk - enemy.def / 2.0),
                                   std::max(0.0, intelligence - enemy.res / 2.0));
            for (const auto& attack : attacks) {
                const uintptr_t descriptor = attack.descriptor;
                const NativeSkills::DescriptorInfo& info = attack.info;
                int selector = info.formula == 2 ? 0 : 3;
                if (NativeEffects::fnAttackSelector) {
                    const uint32_t nativeSelector = NativeEffects::fnAttackSelector(
                        reinterpret_cast<void*>(charaData), reinterpret_cast<void*>(descriptor), 0);
                    if (nativeSelector <= 10) selector = static_cast<int>(nativeSelector);
                }
                const double offensive = ReadStat(charaData, selector);
                const double defense = info.formula == 2 ? enemy.def : enemy.res;
                int effectPower = 0;
                if (!ReadEffectPower(descriptor, attack.skillRank, effectPower)) continue;
                const double powerMultiplier = std::max(0.0, 100.0 + effectPower) / 100.0;
                const double expected = std::max(0.0, offensive * powerMultiplier - defense / 2.0);
                best = std::max(best, expected);
            }
            score += best;
        }
        score /= static_cast<double>(enemies.size());
    }
    return std::isfinite(score) && score >= 0.0 ? score : 0.0;
}

inline bool IsUsableHealingEntry(const NativeSkills::CatalogJob& catalog,
                                 const NativeSkills::Entry& entry) {
    if (!entry.healing || !entry.currentlyUsable || !entry.affordableFromSp ||
        entry.spCost < 0 || !entry.learned) return false;
    int shapeId = -1;
    const int maxChoices = NativeSkills::fnMaxShapeChoices
        ? std::max(1, NativeSkills::fnMaxShapeChoices(reinterpret_cast<void*>(catalog.charaData),
                                                       reinterpret_cast<void*>(entry.descriptor))) : 1;
    return SafeStationaryShape(catalog.charaData, entry.descriptor, entry.slot, maxChoices, shapeId);
}

struct Job {
    uintptr_t context = 0;
    bool started = false;
    bool complete = false;
    bool selectedIsHealer = false;
    bool deployedHealerFound = false;
    int deployedCount = 0;
    int selectedRow = -1;
    int selectedRoster = -1;
    uintptr_t selectedCharaData = 0;
    double selectedHealingScore = 0.0;
    int64_t selectedHealingCost = std::numeric_limits<int64_t>::max();
    double selectedOffenseScore = 0.0;
    double selectedHitTieBreak = 0.0;
    std::vector<Row> availableRows;
    std::vector<Candidate> candidates;
    ActiveCandidate activeCandidate{};
    std::vector<int> deployedPlayers;
    size_t deployedScanIndex = 0;
    NativeSkills::CatalogJob deployedCatalog{};
    bool deployedCatalogActive = false;
    bool deployedCatalogComplete = false;
    size_t deployedEntryIndex = 0;
    std::vector<EnemyProfile> enemies;
    size_t nextRosterRow = 0;

    void Begin(uintptr_t battleContext, const Combat::BattleSnapshot& snapshot) {
        *this = Job{};
        started = true;
        context = battleContext;
        if (!context || !AutoPlay::g_Base) { complete = true; return; }

        const int count = *reinterpret_cast<const int16_t*>(context + kRosterCountOffset);
        if (count < 0 || count > kMaxRosterRows) { complete = true; return; }
        availableRows.reserve(static_cast<size_t>(count));
        for (int row = 0; row < count; ++row) {
            const int ordinal = *reinterpret_cast<const int16_t*>(context + kRosterRowsOffset + row * 2);
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(context + kRosterFlagsOffset + row * 4);
            if (ordinal < 0 || ordinal >= kMaxRosterOrdinal || ordinal >= RosterTotalCount() || flags != 0)
                continue;
            availableRows.push_back({row, ordinal, RosterCharaData(ordinal), flags});
        }

        for (int i = 0; i < Combat::kMaxUnits; ++i) {
            const auto& unit = snapshot.units[static_cast<size_t>(i)];
            if (unit.index != i || unit.faction != Combat::Faction::Player || !unit.deployed) continue;
            ++deployedCount;
            if (unit.alive && unit.hasVerifiedHealingSkill &&
                std::isfinite(unit.cheapestHealingSp) && unit.cheapestHealingSp >= 0.0 &&
                unit.sp >= unit.cheapestHealingSp)
                deployedHealerFound = true;
            if (unit.alive) deployedPlayers.push_back(i);
        }
        enemies = ReadEnemies(snapshot);
        if (deployedCount >= Combat::kMaxDeployments || availableRows.empty()) {
            complete = true;
            return;
        }
        if (deployedHealerFound) deployedPlayers.clear();
    }

    bool StartRosterCandidate(const Row& row) {
        activeCandidate = {};
        const uintptr_t charaData = row.charaData;
        if (!charaData) return false;
        const int64_t hp = *reinterpret_cast<const int64_t*>(charaData + 0x2CC8);
        const int64_t maxHp = *reinterpret_cast<const int64_t*>(charaData + 0x2CD8);
        const int64_t currentSp = *reinterpret_cast<const int64_t*>(charaData + 0x2CD0);
        const int64_t maxSp = *reinterpret_cast<const int64_t*>(charaData + 0x2CE0);
        if (hp <= 0 || maxHp <= 0 || currentSp < 0 || maxSp < 0) return false;
        const int storedSkills = *reinterpret_cast<const int16_t*>(charaData + 0x2CC0);
        if (storedSkills < 0 || storedSkills > (int)NativeSkills::kMaxSkillRows) return false;

        activeCandidate.active = true;
        activeCandidate.row = row;
        activeCandidate.candidate.row = row;
        activeCandidate.candidate.living = true;
        activeCandidate.charaData = charaData;
        activeCandidate.currentSp = currentSp;
        if (NativeSkills::fnEnumerateSkills) {
            Planning::QueryScope profile(Planning::QueryKind::Enumerate);
            activeCandidate.rowCount = NativeSkills::fnEnumerateSkills(
                reinterpret_cast<void*>(charaData), activeCandidate.rows.data());
            if (activeCandidate.rowCount < 0 ||
                activeCandidate.rowCount > static_cast<int>(activeCandidate.rows.size()))
                activeCandidate.rowCount = 0;
        }
        activeCandidate.attacks.reserve(static_cast<size_t>(activeCandidate.rowCount));
        return true;
    }

    void EvaluateRosterSkill(int rowIndex) {
        Planning::QueryScope profile(Planning::QueryKind::RosterSkill);
        if (!activeCandidate.active || rowIndex < 0 || rowIndex >= activeCandidate.rowCount ||
            !NativeSkills::fnSkillIndex) return;
        const uint32_t slot = activeCandidate.rows[static_cast<size_t>(rowIndex)];
        if (slot >= NativeSkills::kMaxSkillRows) return;
        const uintptr_t charaData = activeCandidate.charaData;
        const uintptr_t charaRow = charaData + 0xCC4 +
            static_cast<uintptr_t>(slot) * NativeSkills::kSkillRowStride;
        const uint32_t skillId = *reinterpret_cast<const uint32_t*>(charaRow);
        if (!skillId || skillId == 0xFFFFFFFFu) return;
        const uintptr_t descriptor = NativeSkills::ResolveDescriptor(
            reinterpret_cast<const void*>(charaRow), skillId);
        NativeSkills::DescriptorInfo info{};
        if (!descriptor || !NativeSkills::ReadSupportedDescriptor(descriptor, info) ||
            info.skillId != static_cast<int>(skillId)) return;

        const int shapeChoices = NativeSkills::fnMaxShapeChoices
            ? std::max(1, NativeSkills::fnMaxShapeChoices(reinterpret_cast<void*>(charaData),
                                                           reinterpret_cast<void*>(descriptor))) : 1;
        int shapeId = -1;
        if (!SafeStationaryShape(charaData, descriptor, static_cast<int>(slot), shapeChoices, shapeId))
            return;

        if (info.healing) {
            const int64_t effectiveCost = ReadRosterSkillCost(charaData, descriptor, slot, shapeId);
            if (effectiveCost < 0 || effectiveCost > activeCandidate.currentSp) return;
            // NativeActions copies row+6 to queue+0x27682; NativeEffects then
            // passes that queue field to RVA 0x1D7A20 as the skill-rank input.
            const int skillRank = *reinterpret_cast<const int8_t*>(charaRow + 6);
            int power = 0;
            if (!ReadEffectPower(descriptor, skillRank, power)) return;
            const double healingStat = ReadStat(charaData, 4); // RES selector.
            const double effective = healingStat * std::max(0.0, 100.0 + power) / 100.0;
            Candidate& candidate = activeCandidate.candidate;
            if (!std::isfinite(effective) || effective <= 0.0) return;
            if (!candidate.hasAffordableHealingSkill || effective > candidate.healingScore ||
                (effective == candidate.healingScore && effectiveCost < candidate.cheapestHealingSp)) {
                candidate.hasAffordableHealingSkill = true;
                candidate.healingScore = effective;
                candidate.cheapestHealingSp = effectiveCost;
            }
        } else {
            const int skillRank = *reinterpret_cast<const int8_t*>(charaRow + 6);
            if (skillRank < 0 || skillRank > 9) return;
            activeCandidate.attacks.push_back({descriptor, info, skillRank});
        }
    }

    void FinishActiveCandidate() {
        if (!activeCandidate.active) return;
        activeCandidate.candidate.offenseScore = enemies.empty() ?
            std::max(ReadStat(activeCandidate.charaData, 0), ReadStat(activeCandidate.charaData, 3)) :
            activeCandidate.scoreSum / static_cast<double>(enemies.size());
        if (!std::isfinite(activeCandidate.candidate.offenseScore) || activeCandidate.candidate.offenseScore < 0)
            activeCandidate.candidate.offenseScore = 0;
        activeCandidate.candidate.hitTieBreak = ReadStat(activeCandidate.charaData, 5);
        candidates.push_back(std::move(activeCandidate.candidate));
        activeCandidate = {};
    }

    bool StepOffenseScore() {
        Planning::QueryScope profile(Planning::QueryKind::RosterScore);
        auto& a = activeCandidate;
        if (a.scoreEnemy >= enemies.size()) return true;
        Planning::Unit operation;
        if (!operation) return false;
        const auto& enemy = enemies[a.scoreEnemy];
        if (!a.scoreEnemyStarted) {
            a.scoreBest = std::max(std::max(0.0, ReadStat(a.charaData, 0) - enemy.def / 2.0),
                                   std::max(0.0, ReadStat(a.charaData, 3) - enemy.res / 2.0));
            a.scoreEnemyStarted = true;
        }
        if (a.scoreAttack < a.attacks.size()) {
            const auto& attack = a.attacks[a.scoreAttack++];
            int selector = attack.info.formula == 2 ? 0 : 3;
            if (NativeEffects::fnAttackSelector) {
                const auto native = NativeEffects::fnAttackSelector((void*)a.charaData, (void*)attack.descriptor, 0);
                if (native <= 10) selector = static_cast<int>(native);
            }
            int power = 0;
            if (ReadEffectPower(attack.descriptor, attack.skillRank, power)) {
                const double defense = attack.info.formula == 2 ? enemy.def : enemy.res;
                const double value = ReadStat(a.charaData, selector) * std::max(0.0, 100.0 + power) / 100.0;
                a.scoreBest = std::max(a.scoreBest, std::max(0.0, value - defense / 2.0));
            }
            return false;
        }
        a.scoreSum += a.scoreBest;
        ++a.scoreEnemy;
        a.scoreAttack = 0;
        a.scoreEnemyStarted = false;
        return a.scoreEnemy >= enemies.size();
    }

    void FinalizeSelection() {
        const Candidate* bestHealer = nullptr;
        const Candidate* bestDealer = nullptr;
        for (const Candidate& candidate : candidates) {
            if (!candidate.living) continue;
            if (candidate.hasAffordableHealingSkill &&
                (!bestHealer || candidate.healingScore > bestHealer->healingScore ||
                 (candidate.healingScore == bestHealer->healingScore &&
                  (candidate.cheapestHealingSp < bestHealer->cheapestHealingSp ||
                   (candidate.cheapestHealingSp == bestHealer->cheapestHealingSp &&
                    candidate.row.rosterOrdinal < bestHealer->row.rosterOrdinal)))))
                bestHealer = &candidate;
            if (!bestDealer || candidate.offenseScore > bestDealer->offenseScore ||
                (candidate.offenseScore == bestDealer->offenseScore &&
                 (candidate.hitTieBreak > bestDealer->hitTieBreak ||
                  (candidate.hitTieBreak == bestDealer->hitTieBreak &&
                   candidate.row.rosterOrdinal < bestDealer->row.rosterOrdinal))))
                bestDealer = &candidate;
        }
        const Candidate* selected = !deployedHealerFound && bestHealer ? bestHealer : bestDealer;
        if (!selected) return;
        selectedRow = selected->row.menuRow;
        selectedRoster = selected->row.rosterOrdinal;
        selectedCharaData = selected->row.charaData;
        selectedIsHealer = selected == bestHealer && !deployedHealerFound;
        selectedHealingScore = selected->healingScore;
        selectedHealingCost = selected->cheapestHealingSp;
        selectedOffenseScore = selected->offenseScore;
        selectedHitTieBreak = selected->hitTieBreak;
    }

    bool Step(int maxSlots = 4) {
        if (!started || complete) return complete;
        if (maxSlots <= 0) return false;

        if (deployedScanIndex < deployedPlayers.size()) {
            if (!deployedCatalogActive) {
                deployedCatalog = NativeSkills::Begin(deployedPlayers[deployedScanIndex]);
                deployedCatalogActive = true;
                deployedCatalogComplete = false;
                deployedEntryIndex = 0;
            }
            if (!deployedCatalogComplete) {
                deployedCatalogComplete = NativeSkills::Step(deployedCatalog, maxSlots);
                return false;
            }

            const size_t stop = std::min(deployedCatalog.entries.size(),
                deployedEntryIndex + static_cast<size_t>(maxSlots));
            for (; deployedEntryIndex < stop; ++deployedEntryIndex) {
                Planning::Unit operation;
                if (!operation) return false;
                if (!IsUsableHealingEntry(deployedCatalog,
                        deployedCatalog.entries[deployedEntryIndex])) continue;
                deployedHealerFound = true;
                deployedPlayers.clear();
                deployedScanIndex = 0;
                deployedCatalogActive = false;
                deployedCatalogComplete = false;
                deployedEntryIndex = 0;
                return false;
            }
            if (deployedEntryIndex >= deployedCatalog.entries.size()) {
                ++deployedScanIndex;
                deployedCatalog = {};
                deployedCatalogActive = false;
                deployedCatalogComplete = false;
                deployedEntryIndex = 0;
            }
            return false;
        }

        if (!activeCandidate.active) {
            Planning::Unit operation;
            if (!operation) return false;
            if (nextRosterRow >= availableRows.size()) {
                FinalizeSelection();
                complete = true;
                return true;
            }
            const Row row = availableRows[nextRosterRow++];
            if (!StartRosterCandidate(row)) return false;
        }

        int budget = maxSlots;
        while (budget > 0 && activeCandidate.nextSkill < activeCandidate.rowCount) {
            Planning::Unit operation;
            if (!operation) return false;
            EvaluateRosterSkill(activeCandidate.nextSkill++);
            --budget;
        }
        if (activeCandidate.nextSkill >= activeCandidate.rowCount && StepOffenseScore()) FinishActiveCandidate();
        if (!activeCandidate.active && nextRosterRow >= availableRows.size()) {
            FinalizeSelection();
            complete = true;
        }
        return complete;
    }

    bool IsComplete() const { return complete; }
    int SelectedRow() const { return complete ? selectedRow : -1; }
    int SelectedRoster() const { return complete ? selectedRoster : -1; }
    bool SelectedIsHealer() const { return complete && selectedIsHealer; }

    bool SelectCursor(uintptr_t task, uintptr_t currentContext) const {
        if (!complete || selectedRow < 0 || selectedRoster < 0 || !task || !currentContext ||
            currentContext != context || !AutoPlay::g_Base) return false;
        if (*reinterpret_cast<const uintptr_t*>(task + kTaskBattleContextOffset) != currentContext)
            return false;
        const int currentCount = *reinterpret_cast<const int16_t*>(currentContext + kRosterCountOffset);
        if (currentCount < 0 || currentCount > kMaxRosterRows || selectedRow >= currentCount) return false;
        const int ordinal = *reinterpret_cast<const int16_t*>(currentContext + kRosterRowsOffset + selectedRow * 2);
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(currentContext + kRosterFlagsOffset + selectedRow * 4);
        if (ordinal != selectedRoster || flags != 0 || ordinal < 0 || ordinal >= kMaxRosterOrdinal ||
            ordinal >= RosterTotalCount())
            return false;
        const uintptr_t charaData = RosterCharaData(ordinal);
        if (!charaData || charaData != selectedCharaData) return false;
        if (*reinterpret_cast<const int64_t*>(charaData + 0x2CC8) <= 0 ||
            *reinterpret_cast<const int64_t*>(charaData + 0x2CD8) <= 0) return false;
        if (selectedIsHealer && *reinterpret_cast<const int64_t*>(charaData + 0x2CD0) < selectedHealingCost)
            return false;
        *reinterpret_cast<int32_t*>(task + kTaskRosterCursorOffset) = selectedRow;
        return true;
    }
};

} // namespace AutoPlay::NativeDeployment
