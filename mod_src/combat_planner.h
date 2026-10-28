#pragma once

// Pure combat policy. The native adapter owns all game-memory reads/writes and
// supplies only legal actions whose effects it has safely evaluated.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>
#include "party_roster.h"

namespace AutoPlay::Combat {

constexpr int kMaxUnits = 256;
constexpr int kMaxDeployments = Party::kAutomaticLimit; // Compatibility alias for the default party.
constexpr double kEmergencyHpFraction = 0.25;
constexpr double kOrdinaryHealHpFraction = 0.50;
constexpr double kMinimumUsefulHealFraction = 0.20;
constexpr double kDefaultSpReserveFraction = 0.20;
constexpr double kSkillImprovementFraction = 0.25;

enum class Faction : uint8_t { Unknown, Player, Enemy, Other };
enum class ActionKind : uint8_t { BasicAttack, OffensiveSkill, HealSkill, Move };
enum class DecisionReason : uint8_t {
    None,
    EmergencyHeal,
    UsefulHeal,
    StageFinisher,
    ExtraKill,
    MultiTarget,
    ReachAdvantage,
    DamageGain,
    BasicDamage,
    TacticalMove
};

struct UnitSnapshot {
    int index = -1;
    Faction faction = Faction::Unknown;
    bool alive = false;
    bool deployed = false;
    bool canAct = false;
    bool canMove = false;
    bool canDeploy = false;
    bool hasVerifiedHealingSkill = false;
    double hp = 0.0;
    double maxHp = 0.0;
    double sp = 0.0;
    double maxSp = 0.0;
    double cheapestHealingSp = 0.0;
    double healingScore = 0.0;
    double offenseScore = 0.0;
    int gridX = 0;
    int gridZ = 0;
};

struct SkillDescriptor {
    int skillId = -1;       // Native ID; do not resolve policy by localized name.
    int menuIndex = -1;
    double spCost = 0.0;
    bool learned = false;
    bool currentlyUsable = false;
    bool affordable = false;
    bool targetRulesVerified = false;
    bool effectRulesVerified = false;
};

inline bool IsVerifiedUsableSkill(const SkillDescriptor& skill, double availableSp) {
    return skill.skillId >= 0 && skill.menuIndex >= 0 && skill.learned &&
        skill.currentlyUsable && skill.affordable &&
        skill.targetRulesVerified && skill.effectRulesVerified &&
        std::isfinite(skill.spCost) && skill.spCost >= 0.0 &&
        std::isfinite(availableSp) && availableSp >= skill.spCost;
}

struct EffectEstimate {
    int targetIndex = -1;
    double expectedDamage = 0.0; // Accuracy and resistance already applied.
    double expectedHealing = 0.0;
};

struct ActionCandidate {
    ActionKind kind = ActionKind::BasicAttack;
    int actorIndex = -1;
    int skillId = -1;
    int menuIndex = -1;
    int targetIndex = -1;
    int shapeId = 1;
    int rotation = 0;
    int targetX = -1;
    int targetZ = -1;
    double spCost = 0.0;
    bool legal = false;
    bool verified = false;
    bool basicCouldHitSameTargets = true;
    bool reachAdvantage = false;
    std::vector<EffectEstimate> effects;
};

struct ActionReservation {
    int actorIndex = -1;
    ActionKind kind = ActionKind::BasicAttack;
    int skillId = -1;
    double spCost = 0.0;
    std::vector<EffectEstimate> effects;
};

struct BattleSnapshot {
    std::array<UnitSnapshot, kMaxUnits> units{};
    std::vector<ActionReservation> reservations;
};

struct PendingAction {
    bool valid = false;
    ActionCandidate candidate{};
    DecisionReason reason = DecisionReason::None;
    int killedEnemies = 0;
    double effectiveDamage = 0.0;
    double effectiveHealing = 0.0;
};

struct MoveTileCandidate {
    int gridX = 0;
    int gridZ = 0;
    int distanceToUsefulPosition = -1; // Engine/adapter-computed path distance.
    int threat = -1;                   // Unknown threat is rejected conservatively.
    bool currentTile = false;
};

inline bool IsFinitePositive(double value) {
    return std::isfinite(value) && value > 0.0;
}

inline const UnitSnapshot* GetUnit(const BattleSnapshot& battle, int index) {
    if (index < 0 || index >= kMaxUnits) return nullptr;
    const UnitSnapshot& unit = battle.units[static_cast<size_t>(index)];
    return unit.alive && unit.index == index ? &unit : nullptr;
}

inline double ProjectedHp(const BattleSnapshot& battle, int targetIndex) {
    const UnitSnapshot* unit = GetUnit(battle, targetIndex);
    if (!unit || !IsFinitePositive(unit->maxHp)) return 0.0;
    double hp = std::max(0.0, std::min(unit->hp, unit->maxHp));
    bool defeated = hp <= 0.0;
    // The adapter records accepted queue entries in engine order. Apply their
    // effects in that order so a heal cannot revive an ally already killed by
    // an earlier queued attack and an earlier heal can prevent over-healing.
    for (const ActionReservation& reservation : battle.reservations) {
        for (const EffectEstimate& effect : reservation.effects) {
            if (effect.targetIndex != targetIndex) continue;
            if (IsFinitePositive(effect.expectedDamage)) {
                hp = std::max(0.0, hp - effect.expectedDamage);
                defeated = defeated || hp <= 0.0;
            }
            if (!defeated && IsFinitePositive(effect.expectedHealing))
                hp = std::min(unit->maxHp, hp + effect.expectedHealing);
        }
    }
    return hp;
}

inline double ReservedSp(const BattleSnapshot& battle, int actorIndex) {
    double reserved = 0.0;
    for (const ActionReservation& reservation : battle.reservations) {
        if (reservation.actorIndex == actorIndex && IsFinitePositive(reservation.spCost))
            reserved += reservation.spCost;
    }
    return reserved;
}

inline double EffectiveDamage(const BattleSnapshot& battle, const ActionCandidate& action) {
    const UnitSnapshot* actor = GetUnit(battle, action.actorIndex);
    if (!actor || actor->faction != Faction::Player) return 0.0;
    double total = 0.0;
    std::array<double, kMaxUnits> seen{};
    for (const EffectEstimate& effect : action.effects) {
        const UnitSnapshot* target = GetUnit(battle, effect.targetIndex);
        if (!target || target->faction != Faction::Enemy ||
            !IsFinitePositive(effect.expectedDamage)) continue;
        if (effect.expectedHealing > 0.0) return 0.0;
        const double before = ProjectedHp(battle, effect.targetIndex) - seen[static_cast<size_t>(effect.targetIndex)];
        const double dealt = std::min(std::max(0.0, before), effect.expectedDamage);
        seen[static_cast<size_t>(effect.targetIndex)] += dealt;
        total += dealt;
    }
    return total;
}

inline int ProjectedKills(const BattleSnapshot& battle, const ActionCandidate& action) {
    const UnitSnapshot* actor = GetUnit(battle, action.actorIndex);
    if (!actor || actor->faction != Faction::Player) return 0;
    std::array<double, kMaxUnits> damage{};
    for (const EffectEstimate& effect : action.effects) {
        const UnitSnapshot* target = GetUnit(battle, effect.targetIndex);
        if (!target || target->faction != Faction::Enemy ||
            !IsFinitePositive(effect.expectedDamage)) continue;
        damage[static_cast<size_t>(effect.targetIndex)] += effect.expectedDamage;
    }
    int kills = 0;
    for (int index = 0; index < kMaxUnits; ++index) {
        if (damage[static_cast<size_t>(index)] <= 0.0) continue;
        if (ProjectedHp(battle, index) > 0.0 &&
            damage[static_cast<size_t>(index)] >= ProjectedHp(battle, index)) ++kills;
    }
    return kills;
}

inline double EffectiveHealing(const BattleSnapshot& battle, const ActionCandidate& action) {
    const UnitSnapshot* actor = GetUnit(battle, action.actorIndex);
    if (!actor || actor->faction != Faction::Player) return 0.0;
    double total = 0.0;
    std::array<double, kMaxUnits> seen{};
    for (const EffectEstimate& effect : action.effects) {
        const UnitSnapshot* target = GetUnit(battle, effect.targetIndex);
        if (!target || target->faction != Faction::Player ||
            !IsFinitePositive(effect.expectedHealing)) continue;
        if (effect.expectedDamage > 0.0) return 0.0;
        const double projected = ProjectedHp(battle, effect.targetIndex);
        if (projected <= 0.0) continue; // Healing cannot revive a queued casualty.
        const double missing = std::max(0.0, target->maxHp - projected - seen[static_cast<size_t>(effect.targetIndex)]);
        const double restored = std::min(missing, effect.expectedHealing);
        seen[static_cast<size_t>(effect.targetIndex)] += restored;
        total += restored;
    }
    return total;
}

inline bool CausesFriendlyFire(const BattleSnapshot& battle, const ActionCandidate& action) {
    const UnitSnapshot* actor = GetUnit(battle, action.actorIndex);
    if (!actor || actor->faction != Faction::Player) return true;
    for (const EffectEstimate& effect : action.effects) {
        const UnitSnapshot* target = GetUnit(battle, effect.targetIndex);
        if (!target) continue;
        if (target->faction == Faction::Player && IsFinitePositive(effect.expectedDamage)) return true;
        if (target->faction == Faction::Enemy && IsFinitePositive(effect.expectedHealing)) return true;
    }
    return false;
}

inline bool IsStageFinisher(const BattleSnapshot& battle, const ActionCandidate& action) {
    bool foundEnemy = false;
    for (const UnitSnapshot& enemy : battle.units) {
        if (!enemy.alive || !enemy.deployed || enemy.faction != Faction::Enemy || !IsFinitePositive(enemy.maxHp)) continue;
        foundEnemy = true;
        double remaining = ProjectedHp(battle, enemy.index);
        for (const EffectEstimate& effect : action.effects) {
            if (effect.targetIndex == enemy.index && IsFinitePositive(effect.expectedDamage))
                remaining = std::max(0.0, remaining - effect.expectedDamage);
        }
        if (remaining > 0.0) return false;
    }
    return foundEnemy;
}

inline double SpReserve(const UnitSnapshot& actor) {
    const double baseline = IsFinitePositive(actor.maxSp)
        ? actor.maxSp * kDefaultSpReserveFraction : 0.0;
    return actor.hasVerifiedHealingSkill && IsFinitePositive(actor.cheapestHealingSp)
        ? std::max(baseline, actor.cheapestHealingSp) : baseline;
}

inline bool WithinSpBudget(const BattleSnapshot& battle, const ActionCandidate& action,
                           bool emergencyHeal, bool stageFinisher) {
    const UnitSnapshot* actor = GetUnit(battle, action.actorIndex);
    if (!actor || !std::isfinite(action.spCost) || action.spCost < 0.0) return false;
    const double available = std::max(0.0, actor->sp - ReservedSp(battle, action.actorIndex));
    if (action.spCost > available) return false;
    if (emergencyHeal || stageFinisher) return true;
    return available - action.spCost + 1e-9 >= SpReserve(*actor);
}

struct ScoredAction {
    PendingAction decision{};
    double urgency = 0.0;
    double coverage = 0.0;
};

inline DecisionReason OffensiveReason(const BattleSnapshot& battle,
                                       const ActionCandidate& candidate,
                                       int kills, double damage,
                                       const std::vector<ActionCandidate>& all) {
    if (candidate.kind == ActionKind::BasicAttack) return DecisionReason::BasicDamage;
    if (candidate.reachAdvantage || !candidate.basicCouldHitSameTargets)
        return DecisionReason::ReachAdvantage;

    int maxBasicKills = 0;
    std::array<double, kMaxUnits> bestBasicTargetDamage{};
    std::array<bool, kMaxUnits> hitEnemies{};
    std::array<bool, kMaxUnits> basicCanKill{};
    std::array<bool, kMaxUnits> candidateCanKill{};
    for (const ActionCandidate& other : all) {
        if (!other.legal || !other.verified || other.actorIndex != candidate.actorIndex ||
            other.kind != ActionKind::BasicAttack || CausesFriendlyFire(battle, other)) continue;
        maxBasicKills = std::max(maxBasicKills, ProjectedKills(battle, other));
        for (const EffectEstimate& effect : other.effects) {
            if (effect.targetIndex >= 0 && effect.targetIndex < kMaxUnits &&
                IsFinitePositive(effect.expectedDamage)) {
                bestBasicTargetDamage[static_cast<size_t>(effect.targetIndex)] = std::max(
                    bestBasicTargetDamage[static_cast<size_t>(effect.targetIndex)], effect.expectedDamage);
            }
            if (effect.targetIndex >= 0 && effect.targetIndex < kMaxUnits &&
                effect.expectedDamage >= ProjectedHp(battle, effect.targetIndex))
                basicCanKill[static_cast<size_t>(effect.targetIndex)] = true;
        }
    }
    for (const EffectEstimate& effect : candidate.effects) {
        if (effect.targetIndex < 0 || effect.targetIndex >= kMaxUnits || effect.expectedDamage <= 0.0) continue;
        const UnitSnapshot* target = GetUnit(battle, effect.targetIndex);
        if (!target || target->faction != Faction::Enemy) continue;
        hitEnemies[static_cast<size_t>(effect.targetIndex)] = true;
        if (effect.expectedDamage >= ProjectedHp(battle, effect.targetIndex))
            candidateCanKill[static_cast<size_t>(effect.targetIndex)] = true;
    }
    int enemyCount = 0;
    bool additionalKill = false;
    double bestBasicDamage = 0.0;
    bool damageGain = false;
    for (int i = 0; i < kMaxUnits; ++i) {
        if (hitEnemies[static_cast<size_t>(i)]) {
            ++enemyCount;
            bestBasicDamage = std::max(bestBasicDamage, bestBasicTargetDamage[static_cast<size_t>(i)]);
            for (const EffectEstimate& effect : candidate.effects) {
                if (effect.targetIndex == i &&
                    effect.expectedDamage >= bestBasicTargetDamage[static_cast<size_t>(i)] *
                        (1.0 + kSkillImprovementFraction) &&
                    IsFinitePositive(effect.expectedDamage)) damageGain = true;
            }
        }
        if (candidateCanKill[static_cast<size_t>(i)]) {
            if (!basicCanKill[static_cast<size_t>(i)]) additionalKill = true;
        }
    }
    if (kills > maxBasicKills || additionalKill)
        return DecisionReason::ExtraKill;
    // A multi-target skill must have meaningful total effect relative to the
    // best ordinary attack; a tiny AoE tick should not waste SP.
    if (enemyCount >= 2 && damage >= bestBasicDamage * 0.75) return DecisionReason::MultiTarget;
    if (bestBasicDamage <= 0.0 && damage > 0.0) return DecisionReason::ReachAdvantage;
    if (damageGain) return DecisionReason::DamageGain;
    return DecisionReason::None;
}

inline int ActionPriority(DecisionReason reason) {
    switch (reason) {
    case DecisionReason::EmergencyHeal: return 7;
    case DecisionReason::StageFinisher: return 6;
    case DecisionReason::UsefulHeal: return 5;
    case DecisionReason::ExtraKill: return 4;
    case DecisionReason::MultiTarget: return 3;
    case DecisionReason::ReachAdvantage: return 2;
    case DecisionReason::DamageGain: return 1;
    case DecisionReason::BasicDamage: return 0;
    default: return -1;
    }
}

inline bool IsBetter(const ScoredAction& lhs, const ScoredAction& rhs) {
    const PendingAction& a = lhs.decision;
    const PendingAction& b = rhs.decision;
    if (!b.valid) return a.valid;
    const bool lhsEmergency = a.reason == DecisionReason::EmergencyHeal;
    const bool rhsEmergency = b.reason == DecisionReason::EmergencyHeal;
    if (lhsEmergency != rhsEmergency) return lhsEmergency;
    const bool lhsFinisher = a.reason == DecisionReason::StageFinisher;
    const bool rhsFinisher = b.reason == DecisionReason::StageFinisher;
    if (lhsFinisher != rhsFinisher) return lhsFinisher;
    const bool lhsHeal = a.reason == DecisionReason::UsefulHeal;
    const bool rhsHeal = b.reason == DecisionReason::UsefulHeal;
    if (lhsHeal != rhsHeal) return lhsHeal;
    if (a.killedEnemies != b.killedEnemies) return a.killedEnemies > b.killedEnemies;
    if (ActionPriority(a.reason) != ActionPriority(b.reason))
        return ActionPriority(a.reason) > ActionPriority(b.reason);
    if (lhs.urgency != rhs.urgency) return lhs.urgency > rhs.urgency;
    if (lhs.coverage != rhs.coverage) return lhs.coverage > rhs.coverage;
    if (a.effectiveDamage != b.effectiveDamage) return a.effectiveDamage > b.effectiveDamage;
    if (a.effectiveHealing != b.effectiveHealing) return a.effectiveHealing > b.effectiveHealing;
    if (a.candidate.spCost != b.candidate.spCost) return a.candidate.spCost < b.candidate.spCost;
    if (a.candidate.actorIndex != b.candidate.actorIndex) return a.candidate.actorIndex < b.candidate.actorIndex;
    if (a.candidate.menuIndex != b.candidate.menuIndex) return a.candidate.menuIndex < b.candidate.menuIndex;
    if (a.candidate.skillId != b.candidate.skillId) return a.candidate.skillId < b.candidate.skillId;
    if (a.candidate.targetIndex != b.candidate.targetIndex) return a.candidate.targetIndex < b.candidate.targetIndex;
    if (a.candidate.targetX != b.candidate.targetX) return a.candidate.targetX < b.candidate.targetX;
    return a.candidate.targetZ < b.candidate.targetZ;
}

inline PendingAction ChooseBestAction(const BattleSnapshot& battle,
                                     const std::vector<ActionCandidate>& candidates) {
    ScoredAction best{};
    for (const ActionCandidate& action : candidates) {
        if (!action.legal || !action.verified || action.actorIndex < 0 || action.actorIndex >= kMaxUnits)
            continue;
        const UnitSnapshot* actor = GetUnit(battle, action.actorIndex);
        if (!actor || actor->faction != Faction::Player || !actor->deployed || !actor->canAct ||
            CausesFriendlyFire(battle, action)) continue;
        if (action.kind == ActionKind::Move) continue; // Movement uses ChooseMoveTile.
        if ((action.kind == ActionKind::OffensiveSkill || action.kind == ActionKind::HealSkill) &&
            (action.skillId < 0 || action.menuIndex < 0)) continue;

        ScoredAction scored{};
        scored.decision.valid = true;
        scored.decision.candidate = action;
        const int kills = ProjectedKills(battle, action);
        const double damage = EffectiveDamage(battle, action);
        const double healing = EffectiveHealing(battle, action);
        scored.decision.killedEnemies = kills;
        scored.decision.effectiveDamage = damage;
        scored.decision.effectiveHealing = healing;
        scored.coverage = static_cast<double>(std::count_if(action.effects.begin(), action.effects.end(),
            [&](const EffectEstimate& effect) {
                const UnitSnapshot* target = GetUnit(battle, effect.targetIndex);
                return target && target->faction == Faction::Enemy && IsFinitePositive(effect.expectedDamage);
            }));

        bool emergency = false;
        if (action.kind == ActionKind::HealSkill && healing > 0.0) {
            double mostCriticalRatio = 1.0;
            bool reachesEmergency = false;
            bool hasUsefulNormalTarget = false;
            for (const EffectEstimate& effect : action.effects) {
                const UnitSnapshot* target = GetUnit(battle, effect.targetIndex);
                if (!target || target->faction != Faction::Player ||
                    !IsFinitePositive(effect.expectedHealing) || !IsFinitePositive(target->maxHp)) continue;
                const double projectedHp = ProjectedHp(battle, effect.targetIndex);
                if (projectedHp <= 0.0) continue;
                const double ratio = projectedHp / target->maxHp;
                mostCriticalRatio = std::min(mostCriticalRatio, ratio);
                reachesEmergency = reachesEmergency || ratio <= kEmergencyHpFraction;
                const bool normalThreshold = ratio < kOrdinaryHealHpFraction;
                const bool usefulAmount = std::min(target->maxHp - projectedHp, effect.expectedHealing) >=
                    target->maxHp * kMinimumUsefulHealFraction;
                hasUsefulNormalTarget = hasUsefulNormalTarget || (normalThreshold && usefulAmount);
            }
            emergency = reachesEmergency && healing > 0.0;
            if (!emergency && !hasUsefulNormalTarget) continue;
            scored.decision.reason = emergency ? DecisionReason::EmergencyHeal : DecisionReason::UsefulHeal;
            scored.urgency = emergency ? (kEmergencyHpFraction - mostCriticalRatio) : (kOrdinaryHealHpFraction - mostCriticalRatio);
            if (!WithinSpBudget(battle, action, emergency, false)) continue;
        } else {
            if (!(damage > 0.0)) continue;
            const DecisionReason reason = OffensiveReason(battle, action, kills, damage, candidates);
            if (action.kind == ActionKind::OffensiveSkill && reason == DecisionReason::None) continue;
            const bool finisher = IsStageFinisher(battle, action);
            if (!WithinSpBudget(battle, action, false, finisher)) continue;
            scored.decision.reason = finisher ? DecisionReason::StageFinisher : reason;
        }
        if (IsBetter(scored, best)) best = scored;
    }
    return best.decision;
}

inline bool ReserveAcceptedAction(BattleSnapshot& battle, const PendingAction& accepted) {
    if (!accepted.valid || accepted.candidate.actorIndex < 0 || accepted.candidate.actorIndex >= kMaxUnits)
        return false;
    ActionReservation reservation{};
    reservation.actorIndex = accepted.candidate.actorIndex;
    reservation.kind = accepted.candidate.kind;
    reservation.skillId = accepted.candidate.skillId;
    reservation.spCost = accepted.candidate.spCost;
    reservation.effects = accepted.candidate.effects;
    battle.reservations.push_back(std::move(reservation));
    return true;
}

inline void ClearReservations(BattleSnapshot& battle) { battle.reservations.clear(); }

inline std::vector<int> SelectDeployment(const BattleSnapshot& battle,
                                         const std::vector<int>& rosterOrder,
                                         int maxSlots = kMaxDeployments) {
    std::vector<int> selected;
    std::array<bool, kMaxUnits> included{};
    for (int index = 0; index < kMaxUnits; ++index) {
        const UnitSnapshot* unit = GetUnit(battle, index);
        if (unit && unit->faction == Faction::Player && unit->deployed && unit->alive) {
            selected.push_back(index);
            included[static_cast<size_t>(index)] = true;
        }
    }
    if (maxSlots < 0) maxSlots = 0;
    maxSlots = std::min(maxSlots, Party::kMaximumMembers);
    maxSlots = std::max(maxSlots, static_cast<int>(selected.size())); // Never remove deployed units.
    int bestHealer = -1;
    for (int index : rosterOrder) {
        const UnitSnapshot* unit = GetUnit(battle, index);
        if (!unit || unit->faction != Faction::Player || unit->deployed || !unit->canDeploy ||
            !unit->hasVerifiedHealingSkill || !IsFinitePositive(unit->healingScore) ||
            !std::isfinite(unit->cheapestHealingSp) || unit->cheapestHealingSp < 0.0 ||
            unit->sp < unit->cheapestHealingSp) continue;
        const UnitSnapshot* currentBest = bestHealer < 0 ? nullptr :
            &battle.units[static_cast<size_t>(bestHealer)];
        if (!currentBest || unit->healingScore > currentBest->healingScore ||
            (unit->healingScore == currentBest->healingScore &&
             unit->cheapestHealingSp < currentBest->cheapestHealingSp))
            bestHealer = index;
    }
    bool alreadyHasHealer = false;
    for (int index : selected) {
        const UnitSnapshot* unit = GetUnit(battle, index);
        if (unit && unit->hasVerifiedHealingSkill &&
            unit->sp >= unit->cheapestHealingSp &&
            std::isfinite(unit->cheapestHealingSp) && unit->cheapestHealingSp >= 0.0)
            alreadyHasHealer = true;
    }
    if (!alreadyHasHealer && bestHealer >= 0 && static_cast<int>(selected.size()) < maxSlots) {
        selected.push_back(bestHealer);
        included[static_cast<size_t>(bestHealer)] = true;
    }
    std::vector<int> dealers;
    for (int index : rosterOrder) {
        const UnitSnapshot* unit = GetUnit(battle, index);
        if (!unit || unit->faction != Faction::Player || unit->deployed || !unit->canDeploy ||
            included[static_cast<size_t>(index)]) continue;
        dealers.push_back(index);
    }
    std::stable_sort(dealers.begin(), dealers.end(), [&](int lhs, int rhs) {
        const double left = battle.units[static_cast<size_t>(lhs)].offenseScore;
        const double right = battle.units[static_cast<size_t>(rhs)].offenseScore;
        return left > right;
    });
    for (int index : dealers) {
        if (static_cast<int>(selected.size()) >= maxSlots) break;
        selected.push_back(index);
    }
    return selected;
}

inline bool ChooseMoveTile(const std::vector<MoveTileCandidate>& tiles, MoveTileCandidate& out) {
    const MoveTileCandidate* current = nullptr;
    const MoveTileCandidate* best = nullptr;
    for (const MoveTileCandidate& tile : tiles) {
        if (tile.distanceToUsefulPosition < 0 || tile.threat < 0) continue;
        if (tile.currentTile) {
            if (!current || tile.threat < current->threat) current = &tile;
            continue;
        }
        if (!best || tile.distanceToUsefulPosition < best->distanceToUsefulPosition ||
            (tile.distanceToUsefulPosition == best->distanceToUsefulPosition && tile.threat < best->threat) ||
            (tile.distanceToUsefulPosition == best->distanceToUsefulPosition && tile.threat == best->threat &&
             (tile.gridX < best->gridX || (tile.gridX == best->gridX && tile.gridZ < best->gridZ))))
            best = &tile;
    }
    if (!best) return false;
    if (current && (best->distanceToUsefulPosition > current->distanceToUsefulPosition ||
        (best->distanceToUsefulPosition == current->distanceToUsefulPosition && best->threat >= current->threat)))
        return false;
    out = *best;
    return true;
}

} // namespace AutoPlay::Combat
