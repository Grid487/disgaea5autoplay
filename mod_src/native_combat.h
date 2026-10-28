#pragma once
#include "autoplay_ai.h"
#include "combat_planner.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace AutoPlay::NativeCombat {

// Proven by native queue setters 2BAFD0, 2BA610 and 2BB960. In particular,
// BattleUnit+2F0 is a carried-unit link, NOT an action-queue index.
constexpr uintptr_t kQueueArray = 0x1E4FF80;
constexpr uintptr_t kQueueStride = 0x27730;
constexpr uintptr_t kQueueSkillId = 0x27670;
constexpr uintptr_t kQueueActorCount = 0x1310;
constexpr uintptr_t kQueueTargetCount = 0x27318;
constexpr uintptr_t kQueueTargets = 0x1318;
constexpr uintptr_t kQueueTargetStride = 0x260;

inline int QueueCount(uintptr_t context) {
    if (!context) return 0;
    const int count = *(int32_t*)(context + 0x950);
    return count >= 0 && count <= 256 ? count : 0;
}

inline uintptr_t QueueEntry(int index) {
    return index >= 0 && index < 256 ? g_Base + kQueueArray + index * kQueueStride : 0;
}

inline bool QueueHasActor(uintptr_t entry, int unitIndex) {
    if (!entry || unitIndex < 0 || unitIndex >= 256) return false;
    const int count = *(int16_t*)(entry + kQueueActorCount);
    if (count < 1 || count > 10) return false;
    for (int i = 0; i < count; ++i)
        if (*(int16_t*)(entry + i * 0x1E8) == unitIndex) return true;
    return false;
}

inline bool HasQueuedAction(uintptr_t context, int unitIndex) {
    for (int i = 0, count = QueueCount(context); i < count; ++i)
        if (QueueHasActor(QueueEntry(i), unitIndex)) return true;
    return false;
}

inline bool QueueMatches(uintptr_t entry, const Combat::ActionCandidate& action) {
    if (!entry || *(int16_t*)entry != action.actorIndex || !QueueHasActor(entry, action.actorIndex)) return false;
    const int skillId = *(int32_t*)(entry + kQueueSkillId);
    const int expectedSkill = action.kind == Combat::ActionKind::BasicAttack ? 0 : action.skillId;
    if (skillId != expectedSkill) return false;
    const int count = *(int16_t*)(entry + kQueueTargetCount);
    if (count <= 0 || count > 256) return false;
    std::array<bool, 256> expected{}, actual{};
    for (const auto& effect : action.effects) {
        if (effect.targetIndex < 0 || effect.targetIndex >= 256) return false;
        expected[effect.targetIndex] = true;
    }
    for (int i = 0; i < count; ++i) {
        const int target = *(int16_t*)(entry + kQueueTargets + i * kQueueTargetStride);
        if (target < 0 || target >= 256) return false;
        actual[target] = true;
    }
    return expected == actual;
}

inline int FindAcceptedAction(uintptr_t context, const Combat::ActionCandidate& action, int firstNewIndex) {
    if (firstNewIndex < 0 || firstNewIndex > 256) return -1;
    for (int i = firstNewIndex, count = QueueCount(context); i < count; ++i)
        if (QueueMatches(QueueEntry(i), action)) return i;
    return -1;
}

inline uint64_t QueueSignature(uintptr_t entry) {
    if (!entry) return 0;
    uint64_t hash = 1469598103934665603ull;
    auto add = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
    add(*(uint16_t*)entry);
    const int actors = *(int16_t*)(entry + kQueueActorCount);
    add(actors);
    if (actors > 0 && actors <= 10)
        for (int i = 0; i < actors; ++i) add(*(uint16_t*)(entry + i * 0x1E8));
    add(*(uint32_t*)(entry + kQueueSkillId));
    add(*(uint64_t*)(entry + 0x27680));
    add(*(uint64_t*)(entry + 0x27690));
    add(*(uint64_t*)(entry + 0x27698));
    add(*(uint64_t*)(entry + 0x276D0));
    const int count = *(int16_t*)(entry + kQueueTargetCount);
    add(count);
    if (count > 0 && count <= 256)
        for (int i = 0; i < count; ++i) add(*(uint16_t*)(entry + kQueueTargets + i * kQueueTargetStride));
    return hash ? hash : 1;
}

inline Combat::UnitSnapshot ReadUnit(int index) {
    Combat::UnitSnapshot out;
    const uintptr_t unit = GetUnitPtr(index);
    if (!unit) return out;
    const uintptr_t chara = *(uintptr_t*)(unit + 0x240);
    if (!chara) return out;
    // 1588C5..1588E1 copies max HP/SP to current HP/SP, corroborated by
    // accessor pair 153360/1533E0. These are game data, never modified here.
    const int64_t hp = *(int64_t*)(chara + 0x2CC8);
    const int64_t maxHp = *(int64_t*)(chara + 0x2CD8);
    const int64_t sp = *(int64_t*)(chara + 0x2CD0);
    const int64_t maxSp = *(int64_t*)(chara + 0x2CE0);
    if (maxHp <= 0 || hp < 0 || maxSp < 0 || sp < 0) return out;
    out.index = index;
    out.hp = (double)hp;
    out.maxHp = (double)maxHp;
    out.sp = (double)sp;
    out.maxSp = (double)maxSp;
    out.alive = hp > 0;
    out.deployed = *(uint8_t*)(unit + 0x2E8) != 0 && *(int16_t*)(unit + 0x2EE) < 0;
    const int faction = fnGetUnitFaction ? fnGetUnitFaction((void*)unit, 1) : -1;
    out.faction = faction == 0 ? Combat::Faction::Player :
        (IsEnemyFaction(faction) ? Combat::Faction::Enemy : Combat::Faction::Other);
    GetUnitGridPos(unit, out.gridX, out.gridZ);
    if (out.alive && out.deployed && out.faction == Combat::Faction::Player) {
        out.canAct = fnHasActionRemaining && fnHasActionRemaining((void*)unit);
        out.canMove = !g_UnitMovedThisTurn[index] && fnCanUnitMove && fnCanUnitMove((void*)unit);
    }
    return out;
}

struct AcceptedReservation {
    int queueIndex = -1;
    uint64_t signature = 0;
    Combat::PendingAction decision;
};

inline Combat::BattleSnapshot CaptureSnapshot(uintptr_t context,
    std::vector<AcceptedReservation>& accepted) {
    Combat::BattleSnapshot out;
    for (int i = 0; i < 256; ++i) out.units[i] = ReadUnit(i);
    // Actual HP/SP already reflect executed actions. Keep projections only
    // while the same action signature remains queued, including after queue
    // compaction. Changed skill configuration/cost invalidates the estimate.
    std::array<bool, 256> matched{};
    accepted.erase(std::remove_if(accepted.begin(), accepted.end(),
        [context, &matched](AcceptedReservation& r) {
            for (int i = 0, count = QueueCount(context); i < count; ++i) {
                const auto entry = QueueEntry(i);
                if (matched[i] || !QueueMatches(entry, r.decision.candidate)) continue;
                const auto signature = QueueSignature(entry);
                if (r.signature && r.signature != signature) continue;
                matched[i] = true;
                r.queueIndex = i; r.signature = signature;
                return false;
            }
            return true;
        }), accepted.end());
    for (const auto& r : accepted) Combat::ReserveAcceptedAction(out, r.decision);
    return out;
}

} // namespace AutoPlay::NativeCombat
