#pragma once
#include "native_skills.h"
#include "native_geometry.h"
#include "planning_budget.h"

namespace AutoPlay::NativeActions {

// The targeting task (60E100) builds this same private preview. None of these
// calls appends to the battle queue or confirms an action.
using FnInit = void(__fastcall*)(void*);
using FnActor = void(__fastcall*)(void*, void*, void*);
using FnLearned = void(__fastcall*)(void*, void*, int, uint32_t);
using FnDescriptor = void(__fastcall*)(void*, void*, uint32_t);
using FnTarget = void(__fastcall*)(void*, float, float);
using FnTargets = uint32_t(__fastcall*)(void*, uint8_t, uintptr_t);
using FnBasicRange = int(__fastcall*)(void*);
using FnBasicLegal = uint8_t(__fastcall*)(void*, void*, int, uint8_t, uint8_t);
using FnBasicReach = uint8_t(__fastcall*)(void*, int, int, void*, int, int, int);
using FnLineWeapon = uint8_t(__fastcall*)(void*);
using EffectEvaluator = bool(*)(uintptr_t, int, int, bool, Combat::EffectEstimate&);
inline FnInit initializeQueue = nullptr;
inline FnActor setActor = nullptr;
inline FnLearned setLearned = nullptr;
inline FnDescriptor setDescriptor = nullptr;
inline FnTarget setTarget = nullptr;
inline FnTargets evaluateTargets = nullptr;
inline FnBasicRange getBasicRange = nullptr;
inline FnBasicLegal basicTargetLegal = nullptr;
inline FnBasicReach basicReachFrom = nullptr;
inline FnLineWeapon isLineWeapon = nullptr;
inline EffectEvaluator evaluateEffect = nullptr;

inline void Initialize(uintptr_t base) {
    initializeQueue = (FnInit)(base + 0x2BB6C0);
    setActor = (FnActor)(base + 0x2DBA70);
    setLearned = (FnLearned)(base + 0x2BA060);
    setDescriptor = (FnDescriptor)(base + 0x2BA610);
    setTarget = (FnTarget)(base + 0x2B9EF0);
    evaluateTargets = (FnTargets)(base + 0x2DBD30);
    getBasicRange = (FnBasicRange)(base + 0x2DB740);
    basicTargetLegal = (FnBasicLegal)(base + 0x4CE990);
    basicReachFrom = (FnBasicReach)(base + 0x153F30);
    isLineWeapon = (FnLineWeapon)(base + 0x1532E0);
}

struct Preview { alignas(16) std::array<uint8_t, NativeCombat::kQueueStride> bytes{}; };

inline int ConfiguredShape(int actorIndex, const NativeSkills::Entry& skill) {
    if (skill.shapeId) return skill.shapeId;
    const uintptr_t u = GetUnitPtr(actorIndex);
    const uintptr_t cd = u ? *(uintptr_t*)(u + 0x240) : 0;
    if (!cd || skill.slot < 0 || skill.slot >= 512) return -1;
    // The native skill record stores the player's selected generic spell area.
    const int area = *(uint8_t*)(cd + 0xCCE + skill.slot * 16);
    return area >= 1 && area <= std::min(9, skill.maxShapeChoices) ? area : -1;
}

inline bool ShapeIsStationary(const NativeGeometry::ShapeDescriptor& shape) {
    if (!shape.count) return false;
    for (int i = 0; i < shape.count; ++i)
        if (shape.cells[i].kind != 1 && shape.cells[i].kind != 3) return false;
    return true;
}

inline bool AreaSafe(const Combat::BattleSnapshot& state, int actor,
    const NativeGeometry::ShapeDescriptor& shape, int x, int z, int rotation, bool healing) {
    NativeGeometry::RotatedHitTiles tiles;
    if (!NativeGeometry::CollectRotatedHitTiles(shape, x, z, rotation, tiles)) return false;
    for (const auto& u : state.units) {
        if (!u.alive || !u.deployed) continue;
        for (size_t i = 0; i < tiles.count; ++i) {
            if (u.gridX != tiles.tiles[i].x || u.gridZ != tiles.tiles[i].z) continue;
            if (healing ? u.faction != Combat::Faction::Player :
                (u.index == actor || u.faction != Combat::Faction::Enemy)) return false;
        }
    }
    return true;
}

inline bool BuildPreview(Preview& preview, const Combat::BattleSnapshot& state,
    int actorIndex, const NativeSkills::Entry* skill, int shapeId,
    int x, int z, int rotation, Combat::ActionCandidate& out, bool calculateEffects = true) {
    Planning::Unit operation(Planning::Resource::Preview);
    if (!operation) return false;
    if (!initializeQueue || !setActor || !setLearned || !setDescriptor ||
        !setTarget || !evaluateTargets || !evaluateEffect || actorIndex < 0 || actorIndex >= 256 ||
        x < 0 || z < 0 || x >= 128 || z >= 128 || rotation < 0 || rotation > 3) return false;
    const auto& actor = state.units[actorIndex];
    if (!actor.alive || !actor.deployed || !actor.canAct) return false;
    const uintptr_t unit = GetUnitPtr(actorIndex);
    const uintptr_t cd = unit ? *(uintptr_t*)(unit + 0x240) : 0;
    if (!cd || *(int16_t*)(unit + 0x2F4) != actorIndex || *(int16_t*)(unit + 0x2F0) >= 0) return false;
    NativeGeometry::ShapeDescriptor shape;
    if (!NativeGeometry::ReadShape(shapeId, shape) || !ShapeIsStationary(shape) ||
        !AreaSafe(state, actorIndex, shape, x, z, rotation, skill && skill->healing)) return false;
    uintptr_t basicTarget = 0;
    int attackRange = 0;
    if (!skill) {
        if (!getBasicRange || !basicTargetLegal || !isLineWeapon) return false;
        attackRange = getBasicRange((void*)unit);
        if (attackRange < 1 || attackRange > 255) return false;
        for (const auto& target : state.units)
            if (target.alive && target.deployed && target.faction == Combat::Faction::Enemy &&
                target.gridX == x && target.gridZ == z) {
                basicTarget = GetUnitPtr(target.index); break;
            }
        if (!basicTarget || !basicTargetLegal((void*)unit, (void*)basicTarget, attackRange, 0, 0)) return false;
    }
    int64_t cost = 0;
    if (skill) {
        if (!skill->learned || !skill->currentlyUsable || !skill->affordableFromSp ||
            ConfiguredShape(actorIndex, *skill) != shapeId) return false;
        cost = NativeSkills::fnEffectiveCost((void*)skill->descriptor, (void*)cd, (void*)unit,
            skill->slot, skill->shapeId ? 1 : shapeId);
        if (cost < 0 || cost > *(int64_t*)(cd + 0x2CD0) ||
            !NativeSkills::fnCanUseSkill((void*)unit, (void*)skill->descriptor, cost)) return false;
    }
    auto q = (uintptr_t)preview.bytes.data();
    if (!Planning::Preview()) return false; // Actual native preview, not cheap rejections.
    initializeQueue((void*)q);
    setActor((void*)q, (void*)unit, skill ? (void*)skill->descriptor : nullptr);
    if (*(int16_t*)(q + NativeCombat::kQueueActorCount) != 1) return false;
    if (skill) setLearned((void*)q, (void*)cd, skill->slot, shapeId);
    else setDescriptor((void*)q, nullptr, shapeId);
    *(int32_t*)(q + 0x27694) = rotation;
    *(int32_t*)(q + 0x27698) = skill ? skill->range : attackRange;
    *(int32_t*)(q + 0x2769C) = skill ? skill->rangeFlag : isLineWeapon((void*)unit);
    *(int16_t*)(q + 0x276A0) = (int16_t)actor.gridX;
    *(int16_t*)(q + 0x276A2) = (int16_t)actor.gridZ;
    if (skill) {
        const auto row = cd + 0xCC4 + skill->slot * 16;
        *(int16_t*)(q + 0x27680) = *(int8_t*)(row + 5);
        *(int16_t*)(q + 0x27682) = *(int8_t*)(row + 6);
    }
    *(int64_t*)(q + 0x276D0) = cost;
    setTarget((void*)q, x * 12.f + 6.f, z * 12.f + 6.f);
    // For a basic attack, native 2DA750 requires the selected BattleUnit as
    // the third argument; skill tile targeting deliberately passes null.
    if (evaluateTargets((void*)q, 0, basicTarget) != 0) return false;
    const int count = *(int16_t*)(q + NativeCombat::kQueueTargetCount);
    if (count <= 0 || count > 256) return false;
    out = {};
    out.actorIndex = actorIndex;
    out.kind = !skill ? Combat::ActionKind::BasicAttack : skill->healing ?
        Combat::ActionKind::HealSkill : Combat::ActionKind::OffensiveSkill;
    out.skillId = skill ? skill->skillId : 0;
    out.menuIndex = skill ? skill->slot : -1;
    out.shapeId = shapeId;
    out.rotation = rotation;
    out.targetX = x;
    out.targetZ = z;
    out.spCost = (double)cost;
    for (int i = 0; i < count; ++i) {
        Combat::EffectEstimate effect;
        const int target = *(int16_t*)(q + 0x1318 + i * 0x260);
        if (target < 0 || target >= 256 || !state.units[target].alive || !state.units[target].deployed ||
            (skill && skill->healing ? state.units[target].faction != Combat::Faction::Player :
                state.units[target].faction != Combat::Faction::Enemy)) return false;
        effect.targetIndex = target;
        if (calculateEffects) {
            Planning::Unit estimate(Planning::Resource::Effect);
            if (!estimate || !evaluateEffect(q, 0, i, skill && skill->healing, effect) || effect.targetIndex != target)
                return false;
        }
        out.effects.push_back(effect);
    }
    out.targetIndex = out.effects.front().targetIndex;
    out.legal = true;
    out.verified = calculateEffects;
    return true;
}

// Evaluate a prepared private preview over several update frames. The target
// set and native geometry are fixed; a changed battle fingerprint discards it.
inline bool StepEffects(Preview& preview, Combat::ActionCandidate& action,
    int& nextTarget, bool& failed, int budget = 4) {
    failed = false;
    const uintptr_t q = (uintptr_t)preview.bytes.data();
    const int end = std::min((int)action.effects.size(), nextTarget + std::max(0, std::min(4, budget)));
    for (; nextTarget < end; ++nextTarget) {
        Planning::Unit operation(Planning::Resource::Effect);
        if (!operation) return false;
        Combat::EffectEstimate effect;
        if (!evaluateEffect || !evaluateEffect(q, 0, nextTarget,
            action.kind == Combat::ActionKind::HealSkill, effect) ||
            effect.targetIndex != action.effects[nextTarget].targetIndex) { failed = true; return true; }
        action.effects[nextTarget] = effect;
    }
    action.verified = nextTarget >= (int)action.effects.size();
    return action.verified;
}

} // namespace AutoPlay::NativeActions
