#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../combat_runtime.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace AutoPlay {
void LogMsg(const char*, ...) {}
}

namespace {

using namespace AutoPlay;
using namespace AutoPlay::Combat;
using namespace AutoPlay::NativeCombat;
using namespace AutoPlay::SmartCombat;

constexpr size_t kFakeImageSize = 128ull * 1024ull * 1024ull;
constexpr uintptr_t kContextOffset = 0x01000000;
constexpr uintptr_t kCharaOffset = 0x04000000;
constexpr int kActor = 5;
constexpr int kTargetA = 10;
constexpr int kTargetB = 11;
constexpr int kTargetC = 12;

uintptr_t g_FakeImage = 0;

uintptr_t Context() { return g_FakeImage + kContextOffset; }
uintptr_t Chara(int index) { return g_FakeImage + kCharaOffset + static_cast<uintptr_t>(index) * 0x4000; }

int StubFaction(void* unit, uint8_t) {
    const uintptr_t chara = *(uintptr_t*)(reinterpret_cast<uintptr_t>(unit) + 0x240);
    return chara ? *(uint8_t*)(chara + 0x2E32) : -1;
}

bool StubCanMove(void*) { return true; }
bool StubHasAction(void*) { return true; }

bool Expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        return false;
    }
    std::printf("PASS: %s\n", label);
    return true;
}

bool EnsureFakeImage() {
    if (g_FakeImage) return true;
    void* image = VirtualAlloc(nullptr, kFakeImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) {
        std::fprintf(stderr, "VirtualAlloc fake image failed (%lu)\n", GetLastError());
        return false;
    }
    g_FakeImage = reinterpret_cast<uintptr_t>(image);
    return true;
}

void InitializeUnit(int index, int faction, int64_t hp, int64_t maxHp,
                    int64_t sp, int64_t maxSp, bool deployed = true) {
    const uintptr_t unit = GetUnitPtr(index);
    const uintptr_t chara = Chara(index);
    std::memset(reinterpret_cast<void*>(unit), 0, 0x1260);
    std::memset(reinterpret_cast<void*>(chara), 0, 0x4000);
    *(uintptr_t*)(unit + 0x240) = chara;
    *(uint8_t*)(unit + 0x2E8) = deployed ? 1 : 0;
    *(int16_t*)(unit + 0x2EE) = -1;
    *(float*)(unit + 0x08) = static_cast<float>(index * 12 + 6);
    *(float*)(unit + 0x10) = static_cast<float>(index * 12 + 6);
    *(uint8_t*)(chara + 0x2E32) = static_cast<uint8_t>(faction);
    *(int64_t*)(chara + 0x2CC8) = hp;
    *(int64_t*)(chara + 0x2CD8) = maxHp;
    *(int64_t*)(chara + 0x2CD0) = sp;
    *(int64_t*)(chara + 0x2CE0) = maxSp;
}

void SetQueueCount(int count) { *(int32_t*)(Context() + 0x950) = count; }

uintptr_t WriteQueueEntry(int queueIndex, int actor, int skillId,
                          const int* targets, int targetCount) {
    const uintptr_t entry = QueueEntry(queueIndex);
    std::memset(reinterpret_cast<void*>(entry), 0, kQueueStride);
    *(int16_t*)entry = static_cast<int16_t>(actor);
    *(int16_t*)(entry + kQueueActorCount) = 1;
    *(int16_t*)(entry + 0 * 0x1E8) = static_cast<int16_t>(actor);
    *(int32_t*)(entry + kQueueSkillId) = skillId;
    *(int16_t*)(entry + kQueueTargetCount) = static_cast<int16_t>(targetCount);
    for (int i = 0; i < targetCount; ++i)
        *(int16_t*)(entry + kQueueTargets + i * kQueueTargetStride) =
            static_cast<int16_t>(targets[i]);
    return entry;
}

ActionCandidate MakeCandidate(ActionKind kind, int actor, int skillId,
                              std::initializer_list<int> targets) {
    ActionCandidate action{};
    action.kind = kind;
    action.actorIndex = actor;
    action.skillId = skillId;
    action.menuIndex = skillId == 0 ? -1 : 1;
    action.legal = true;
    action.verified = true;
    for (int target : targets) action.effects.push_back({target, 10.0, 0.0});
    return action;
}

PendingAction MakeDecision(ActionKind kind, int actor, int skillId,
                           DecisionReason reason, std::initializer_list<int> targets) {
    PendingAction decision{};
    decision.valid = true;
    decision.reason = reason;
    decision.candidate = MakeCandidate(kind, actor, skillId, targets);
    decision.effectiveDamage = 10.0;
    return decision;
}

bool ResetWorld() {
    if (!EnsureFakeImage()) return false;
    std::memset(reinterpret_cast<void*>(g_FakeImage + RVA::ADDR_BattleUnitArray), 0,
                256 * 0x1260);
    std::memset(reinterpret_cast<void*>(g_FakeImage + kQueueArray), 0, 3 * kQueueStride);
    SetQueueCount(0);
    AutoPlay::g_Base = g_FakeImage;
    AutoPlay::fnGetUnitFaction = &StubFaction;
    AutoPlay::fnCanUnitMove = &StubCanMove;
    AutoPlay::fnHasActionRemaining = &StubHasAction;
    std::memset(AutoPlay::g_UnitDoneThisTurn, 0, sizeof(AutoPlay::g_UnitDoneThisTurn));
    std::memset(AutoPlay::g_UnitMovedThisTurn, 0, sizeof(AutoPlay::g_UnitMovedThisTurn));
    Reset();
    return true;
}

bool TestReadUnitUsesCurrentAndMaximumHpSpOffsets() {
    if (!ResetWorld()) return false;
    InitializeUnit(kActor, 0, 73, 129, 31, 77);
    const UnitSnapshot unit = ReadUnit(kActor);
    return Expect(unit.index == kActor && unit.alive && unit.deployed && unit.canAct && unit.canMove &&
                  unit.faction == Faction::Player && unit.hp == 73.0 && unit.maxHp == 129.0 &&
                  unit.sp == 31.0 && unit.maxSp == 77.0,
                  "ReadUnit reads HP, max HP, SP and max SP from their verified fields");
}

bool TestQueueMatchRequiresActorSkillAndExactTargetSet() {
    if (!ResetWorld()) return false;
    const ActionCandidate action = MakeCandidate(ActionKind::OffensiveSkill, kActor, 321,
                                                 {kTargetA, kTargetB});
    const int exactTargets[] = {kTargetA, kTargetB};
    const int missingTarget[] = {kTargetA};
    const int extraTarget[] = {kTargetA, kTargetB, kTargetC};
    const uintptr_t entry = WriteQueueEntry(0, kActor, 321, exactTargets, 2);

    if (!Expect(QueueMatches(entry, action), "matching actor, skill and complete target set is accepted")) return false;
    WriteQueueEntry(0, kActor + 1, 321, exactTargets, 2);
    if (!Expect(!QueueMatches(QueueEntry(0), action), "queue action with wrong actor is rejected")) return false;
    WriteQueueEntry(0, kActor, 322, exactTargets, 2);
    if (!Expect(!QueueMatches(QueueEntry(0), action), "queue action with wrong skill ID is rejected")) return false;
    WriteQueueEntry(0, kActor, 321, missingTarget, 1);
    if (!Expect(!QueueMatches(QueueEntry(0), action), "queue action missing an expected target is rejected")) return false;
    WriteQueueEntry(0, kActor, 321, extraTarget, 3);
    return Expect(!QueueMatches(QueueEntry(0), action), "queue action with an extra target is rejected");
}

bool TestQueueStartIndexExcludesPriorEntry() {
    if (!ResetWorld()) return false;
    const PendingAction decision = MakeDecision(ActionKind::OffensiveSkill, kActor, 321,
                                                DecisionReason::DamageGain, {kTargetA});
    if (!Expect(Begin(decision, Context()), "planning begins before the confirmation input")) return false;

    const int target[] = {kTargetA};
    WriteQueueEntry(0, kActor, 321, target, 1); // Stale entry appears before confirm is sent.
    SetQueueCount(1);
    ConfirmSent(Context());
    if (!Expect(firstNewQueueIndex == 1, "confirmation snapshots the first new queue index")) return false;

    WriteQueueEntry(1, kActor, 321, target, 1);
    SetQueueCount(2);
    if (!Expect(FindAcceptedAction(Context(), decision.candidate, firstNewQueueIndex) == 1,
                "queue scan excludes a stale matching entry before the confirmation boundary")) return false;
    if (!Expect(ObserveQueue(Context()), "matching new queue entry is observed")) return false;
    return Expect(accepted.size() == 1 && accepted[0].queueIndex == 1,
                  "reservation records the newly queued entry, not the stale entry");
}

bool TestCarriedChildLinkIsNotAnActionQueueEntry() {
    if (!ResetWorld()) return false;
    InitializeUnit(kActor, 0, 100, 100, 50, 50);
    InitializeUnit(kActor + 1, 0, 50, 50, 25, 25);
    *(uintptr_t*)(GetUnitPtr(kActor) + 0x2F0) = GetUnitPtr(kActor + 1);
    const int target[] = {kTargetA};
    WriteQueueEntry(0, kActor, 0, target, 1);
    SetQueueCount(1);
    return Expect(HasQueuedAction(Context(), kActor) && !HasQueuedAction(Context(), kActor + 1),
                  "BattleUnit+0x2F0 carried-child link does not count as a queued action");
}

bool TestAcceptedReservationExpiresWithQueueEntry() {
    if (!ResetWorld()) return false;
    InitializeUnit(kActor, 0, 100, 120, 60, 80);
    InitializeUnit(kTargetA, 3, 90, 100, 15, 20);
    const int target[] = {kTargetA};
    WriteQueueEntry(0, kActor, 321, target, 1);
    SetQueueCount(1);

    NativeCombat::AcceptedReservation reservation{};
    reservation.queueIndex = 0;
    reservation.decision = MakeDecision(ActionKind::OffensiveSkill, kActor, 321,
                                        DecisionReason::DamageGain, {kTargetA});
    accepted.push_back(reservation);
    BattleSnapshot snapshot = CaptureSnapshot(Context(), accepted);
    if (!Expect(accepted.size() == 1 && snapshot.reservations.size() == 1 &&
                snapshot.reservations[0].actorIndex == kActor,
                "matching queued entry keeps its action reservation")) return false;

    WriteQueueEntry(0, kActor, 322, target, 1); // Queue slot was reused by a different skill.
    snapshot = CaptureSnapshot(Context(), accepted);
    if (!Expect(accepted.empty() && snapshot.reservations.empty(),
                "reservation clears when its queue slot no longer matches")) return false;

    reservation.decision = MakeDecision(ActionKind::OffensiveSkill, kActor, 321,
                                        DecisionReason::DamageGain, {kTargetA});
    accepted.push_back(reservation);
    WriteQueueEntry(0, kActor, 321, target, 1);
    snapshot = CaptureSnapshot(Context(), accepted);
    if (!Expect(accepted.size() == 1 && snapshot.reservations.size() == 1,
                "reservation can persist while a matching action is still queued")) return false;
    SetQueueCount(0); // The engine executed and removed the entry.
    snapshot = CaptureSnapshot(Context(), accepted);
    return Expect(accepted.empty() && snapshot.reservations.empty(),
                  "reservation clears after execution removes its queue entry");
}

bool TestConfirmSentAloneDoesNotAcceptOrReserve() {
    if (!ResetWorld()) return false;
    const PendingAction decision = MakeDecision(ActionKind::HealSkill, kActor, 401,
        DecisionReason::EmergencyHeal, {kTargetA});
    if (!Expect(Begin(decision, Context()), "emergency healing decision begins")) return false;
    ConfirmSent(Context());
    if (!Expect(confirmationSent && pending.valid, "confirm input remains pending until queue observation")) return false;
    if (!Expect(!AutoPlay::g_UnitDoneThisTurn[kActor] && accepted.empty() && !executeEmergency,
                "ConfirmSent alone does not mark done, reserve an action, or trigger emergency execution")) return false;
    if (!Expect(!ObserveQueue(Context()), "no queue entry means action is not accepted")) return false;
    return Expect(!AutoPlay::g_UnitDoneThisTurn[kActor] && accepted.empty() && !executeEmergency,
                  "unobserved confirmation still has no completion side effects");
}

bool TestReservationFollowsCompactionAndRejectsChangedCost() {
    if (!ResetWorld()) return false;
    const int targets[] = {kTargetA};
    WriteQueueEntry(1, kActor, 321, targets, 1);
    SetQueueCount(2);
    AcceptedReservation r;
    r.queueIndex = 1;
    r.decision = MakeDecision(ActionKind::OffensiveSkill, kActor, 321,
        DecisionReason::DamageGain, {kTargetA});
    r.signature = QueueSignature(QueueEntry(1));
    accepted.push_back(r);
    std::memcpy((void*)QueueEntry(0), (void*)QueueEntry(1), kQueueStride);
    SetQueueCount(1);
    auto snapshot = CaptureSnapshot(Context(), accepted);
    if (!Expect(accepted.size() == 1 && accepted[0].queueIndex == 0 && snapshot.reservations.size() == 1,
        "accepted reservation follows an identical action after queue compaction")) return false;
    *(int64_t*)(QueueEntry(0) + 0x276D0) = 99;
    snapshot = CaptureSnapshot(Context(), accepted);
    return Expect(accepted.empty() && snapshot.reservations.empty(),
        "changed native cost invalidates the old reservation signature");
}

bool TestAcceptedEmergencySetsExecuteFlag() {
    if (!ResetWorld()) return false;
    const PendingAction decision = MakeDecision(ActionKind::HealSkill, kActor, 401,
        DecisionReason::EmergencyHeal, {kTargetA});
    if (!Expect(Begin(decision, Context()), "emergency decision begins")) return false;
    ConfirmSent(Context());
    const int target[] = {kTargetA};
    WriteQueueEntry(0, kActor, 401, target, 1);
    SetQueueCount(1);
    if (!Expect(ObserveQueue(Context()), "matching healing action enters the queue")) return false;
    return Expect(executeEmergency && accepted.size() == 1 &&
                  AutoPlay::g_UnitDoneThisTurn[kActor] && !pending.valid,
                  "accepted emergency heal triggers execution and records its reservation");
}

bool TestRejectedConfirmationsStopAfterTwoAttempts() {
    if (!ResetWorld()) return false;
    const PendingAction decision = MakeDecision(ActionKind::BasicAttack, kActor, 0,
        DecisionReason::BasicDamage, {kTargetA});
    if (!Expect(Begin(decision, Context()), "first confirmation attempt begins")) return false;
    Fail("test rejection one");
    if (!Expect(failedAttempts[kActor] == 1 && !AutoPlay::g_UnitDoneThisTurn[kActor],
                "first rejection allows one retry")) return false;
    if (!Expect(Begin(decision, Context()), "second confirmation attempt begins")) return false;
    Fail("test rejection two");
    if (!Expect(failedAttempts[kActor] == 2 && AutoPlay::g_UnitDoneThisTurn[kActor],
                "second rejection marks the actor done")) return false;
    return Expect(!Begin(decision, Context()), "third confirmation attempt is blocked");
}

} // namespace

int main() {
    if (!EnsureFakeImage()) return 1;
    AutoPlay::g_Base = g_FakeImage;
    const bool passed =
        TestReadUnitUsesCurrentAndMaximumHpSpOffsets() &&
        TestQueueMatchRequiresActorSkillAndExactTargetSet() &&
        TestQueueStartIndexExcludesPriorEntry() &&
        TestCarriedChildLinkIsNotAnActionQueueEntry() &&
        TestAcceptedReservationExpiresWithQueueEntry() &&
        TestReservationFollowsCompactionAndRejectsChangedCost() &&
        TestConfirmSentAloneDoesNotAcceptOrReserve() &&
        TestAcceptedEmergencySetsExecuteFlag() &&
        TestRejectedConfirmationsStopAfterTwoAttempts();
    VirtualFree(reinterpret_cast<void*>(g_FakeImage), 0, MEM_RELEASE);
    g_FakeImage = 0;
    return passed ? 0 : 1;
}
