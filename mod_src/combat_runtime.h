#pragma once
#include "native_combat.h"

namespace AutoPlay::SmartCombat {

static Combat::PendingAction pending;
static std::vector<NativeCombat::AcceptedReservation> accepted;
static uint8_t failedAttempts[256]{};
static bool confirmationSent = false;
static bool executeEmergency = false;
static int firstNewQueueIndex = 0;
static int confirmationFrames = 0;

inline const char* ReasonName(Combat::DecisionReason reason) {
    switch (reason) {
    case Combat::DecisionReason::EmergencyHeal: return "emergency heal";
    case Combat::DecisionReason::UsefulHeal: return "useful heal";
    case Combat::DecisionReason::StageFinisher: return "stage finisher";
    case Combat::DecisionReason::ExtraKill: return "extra kill";
    case Combat::DecisionReason::MultiTarget: return "safe area attack";
    case Combat::DecisionReason::ReachAdvantage: return "reach advantage";
    case Combat::DecisionReason::DamageGain: return "damage gain";
    case Combat::DecisionReason::BasicDamage: return "efficient basic attack";
    case Combat::DecisionReason::TacticalMove: return "tactical movement";
    default: return "none";
    }
}

inline void ClearPending() {
    pending = {};
    confirmationSent = false;
    confirmationFrames = 0;
}

inline void Reset() {
    ClearPending();
    accepted.clear();
    executeEmergency = false;
    memset(failedAttempts, 0, sizeof(failedAttempts));
}

inline bool Begin(const Combat::PendingAction& decision, uintptr_t context) {
    if (!decision.valid || decision.candidate.actorIndex < 0 || decision.candidate.actorIndex >= 256 ||
        failedAttempts[decision.candidate.actorIndex] >= 2 ||
        NativeCombat::HasQueuedAction(context, decision.candidate.actorIndex)) return false;
    ClearPending();
    pending = decision;
    firstNewQueueIndex = NativeCombat::QueueCount(context);
    return true;
}

inline void Fail(const char* reason) {
    if (pending.valid) {
        const int actor = pending.candidate.actorIndex;
        if (actor >= 0 && actor < 256) {
            ++failedAttempts[actor];
            if (failedAttempts[actor] >= 2) g_UnitDoneThisTurn[actor] = true;
        }
        LogMsg("[Combat] Rejected actor=%d skill=%d reason=%s attempt=%u",
            actor, pending.candidate.skillId, reason,
            actor >= 0 && actor < 256 ? failedAttempts[actor] : 0);
    }
    ClearPending();
}

inline void ConfirmSent(uintptr_t context) {
    if (!pending.valid) return;
    firstNewQueueIndex = NativeCombat::QueueCount(context);
    confirmationSent = true;
    confirmationFrames = 0;
}

inline bool ObserveQueue(uintptr_t context) {
    if (!pending.valid || !confirmationSent) return false;
    const int index = NativeCombat::FindAcceptedAction(context, pending.candidate, firstNewQueueIndex);
    if (index >= 0) {
        const auto& a = pending.candidate;
        NativeCombat::AcceptedReservation reservation;
        reservation.queueIndex = index;
        reservation.signature = NativeCombat::QueueSignature(NativeCombat::QueueEntry(index));
        reservation.decision = pending;
        accepted.push_back(reservation);
        g_UnitDoneThisTurn[a.actorIndex] = true;
        executeEmergency = pending.reason == Combat::DecisionReason::EmergencyHeal;
        char targetIds[1280]{};
        size_t used = 0;
        for (const auto& effect : a.effects) {
            const int wrote = snprintf(targetIds + used, sizeof(targetIds) - used,
                "%s%d", used ? "," : "", effect.targetIndex);
            if (wrote < 0 || (size_t)wrote >= sizeof(targetIds) - used) break;
            used += (size_t)wrote;
        }
        LogMsg("[Combat] Accepted actor=%d skill=%d targets=[%s] count=%u damage=%.0f heal=%.0f SP=%.0f reason=%s queue=%d",
            a.actorIndex, a.kind == Combat::ActionKind::BasicAttack ? 0 : a.skillId,
            targetIds, (unsigned)a.effects.size(), pending.effectiveDamage, pending.effectiveHealing,
            a.spCost, ReasonName(pending.reason), index);
        ClearPending();
        return true;
    }
    if (++confirmationFrames > 90) Fail("queue acceptance timeout");
    return false;
}

inline bool RevalidateCommon(uintptr_t context, const Combat::ActionCandidate& a) {
    const auto actor = NativeCombat::ReadUnit(a.actorIndex);
    if (!actor.alive || !actor.deployed || !actor.canAct || actor.sp < a.spCost ||
        NativeCombat::HasQueuedAction(context, a.actorIndex)) return false;
    for (const auto& e : a.effects) {
        const auto target = NativeCombat::ReadUnit(e.targetIndex);
        if (!target.alive || !target.deployed) return false;
        if (a.kind != Combat::ActionKind::HealSkill && target.faction != Combat::Faction::Enemy) return false;
        if (a.kind == Combat::ActionKind::HealSkill && target.faction != Combat::Faction::Player) return false;
    }
    return true;
}

} // namespace AutoPlay::SmartCombat
