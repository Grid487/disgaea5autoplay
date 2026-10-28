#include "../combat_planner.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace AutoPlay::Combat;

namespace {

UnitSnapshot Player(int index, double hp = 100.0, double sp = 100.0) {
    UnitSnapshot unit{};
    unit.index = index;
    unit.faction = Faction::Player;
    unit.alive = true;
    unit.deployed = true;
    unit.canAct = true;
    unit.canMove = true;
    unit.canDeploy = true;
    unit.hp = hp;
    unit.maxHp = 100.0;
    unit.sp = sp;
    unit.maxSp = 100.0;
    return unit;
}

UnitSnapshot Enemy(int index, double hp = 500.0) {
    UnitSnapshot unit{};
    unit.index = index;
    unit.faction = Faction::Enemy;
    unit.alive = true;
    unit.deployed = true;
    unit.hp = hp;
    unit.maxHp = hp;
    return unit;
}

ActionCandidate Attack(int actor, int target, double damage, ActionKind kind = ActionKind::BasicAttack,
                       double sp = 0.0, int menuIndex = -1, int skillId = -1) {
    ActionCandidate action{};
    action.kind = kind;
    action.actorIndex = actor;
    action.targetIndex = target;
    action.legal = true;
    action.verified = true;
    action.spCost = sp;
    action.menuIndex = menuIndex;
    action.skillId = skillId;
    action.effects.push_back({target, damage, 0.0});
    return action;
}

ActionCandidate Heal(int actor, int target, double amount, double sp = 10.0) {
    ActionCandidate action{};
    action.kind = ActionKind::HealSkill;
    action.actorIndex = actor;
    action.skillId = 100 + target;
    action.menuIndex = 2;
    action.spCost = sp;
    action.legal = true;
    action.verified = true;
    action.effects.push_back({target, 0.0, amount});
    return action;
}

BattleSnapshot BattleWithUnits() {
    BattleSnapshot battle{};
    battle.units[0] = Player(0);
    battle.units[1] = Player(1);
    battle.units[10] = Enemy(10);
    battle.units[11] = Enemy(11);
    return battle;
}

void TestEmergencyHealingOutranksOffense() {
    BattleSnapshot battle = BattleWithUnits();
    battle.units[1].hp = 20.0;
    battle.units[0].hasVerifiedHealingSkill = true;
    battle.units[0].cheapestHealingSp = 10.0;
    const auto heal = Heal(0, 1, 40.0, 40.0); // Emergency healing can spend the SP reserve.
    const auto attack = Attack(0, 10, 200.0);
    const PendingAction chosen = ChooseBestAction(battle, {attack, heal});
    assert(chosen.valid);
    assert(chosen.candidate.kind == ActionKind::HealSkill);
    assert(chosen.reason == DecisionReason::EmergencyHeal);
}

void TestNormalHealingAndOverheal() {
    BattleSnapshot battle = BattleWithUnits();
    battle.units[1].hp = 40.0;
    const auto useful = Heal(0, 1, 30.0);
    const auto attack = Attack(0, 10, 25.0);
    const PendingAction chosen = ChooseBestAction(battle, {attack, useful});
    assert(chosen.valid && chosen.reason == DecisionReason::UsefulHeal);

    battle.units[1].hp = 80.0;
    const PendingAction overheal = ChooseBestAction(battle, {Heal(0, 1, 50.0), attack});
    assert(overheal.valid && overheal.candidate.kind == ActionKind::BasicAttack);

    battle.units[1].hp = 40.0;
    const PendingAction tooSmall = ChooseBestAction(battle, {Heal(0, 1, 10.0), attack});
    assert(tooSmall.valid && tooSmall.candidate.kind == ActionKind::BasicAttack);
}

void TestSpReserveAndVerifiedLearnedSkills() {
    BattleSnapshot battle = BattleWithUnits();
    battle.units[0].sp = 50.0;
    const auto basic = Attack(0, 10, 40.0);
    const auto expensive = Attack(0, 10, 100.0, ActionKind::OffensiveSkill, 31.0, 1, 200);
    assert(ChooseBestAction(battle, {basic, expensive}).candidate.kind == ActionKind::BasicAttack);

    SkillDescriptor descriptor{};
    descriptor.skillId = 200;
    descriptor.menuIndex = 1;
    descriptor.spCost = 10.0;
    descriptor.learned = true;
    descriptor.currentlyUsable = true;
    descriptor.affordable = true;
    descriptor.targetRulesVerified = true;
    descriptor.effectRulesVerified = true;
    assert(IsVerifiedUsableSkill(descriptor, 10.0));
    descriptor.learned = false;
    assert(!IsVerifiedUsableSkill(descriptor, 100.0));
    descriptor.learned = true;
    descriptor.effectRulesVerified = false;
    assert(!IsVerifiedUsableSkill(descriptor, 100.0));
}

void TestAccuracyAndResistanceAreAppliedInExpectedDamage() {
    // Candidates carry final expected damage after the adapter's accuracy and
    // elemental resistance evaluation, so the planner must not use raw power.
    BattleSnapshot battle = BattleWithUnits();
    const auto basic = Attack(0, 10, 100.0);
    const auto resistedLowAccuracy = Attack(0, 10, 55.0, ActionKind::OffensiveSkill, 5.0, 0, 201);
    const PendingAction chosen = ChooseBestAction(battle, {resistedLowAccuracy, basic});
    assert(chosen.valid && chosen.candidate.kind == ActionKind::BasicAttack);

    const auto favorableElement = Attack(0, 10, 130.0, ActionKind::OffensiveSkill, 5.0, 1, 202);
    const PendingAction favorable = ChooseBestAction(battle, {basic, favorableElement});
    assert(favorable.valid && favorable.candidate.skillId == 202);
    assert(favorable.reason == DecisionReason::DamageGain);
}

void TestAreaDamageRequiresUsefulSafePlacement() {
    BattleSnapshot battle = BattleWithUnits();
    const auto basic = Attack(0, 10, 100.0);
    ActionCandidate area{};
    area.kind = ActionKind::OffensiveSkill;
    area.actorIndex = 0;
    area.skillId = 203;
    area.menuIndex = 0;
    area.spCost = 5.0;
    area.legal = true;
    area.verified = true;
    area.effects = {{10, 75.0, 0.0}, {11, 75.0, 0.0}};
    PendingAction chosen = ChooseBestAction(battle, {basic, area});
    assert(chosen.valid && chosen.candidate.skillId == 203);
    assert(chosen.reason == DecisionReason::MultiTarget);

    area.effects.push_back({1, 1.0, 0.0}); // An ally in the selected pattern makes it unsafe.
    chosen = ChooseBestAction(battle, {basic, area});
    assert(chosen.valid && chosen.candidate.kind == ActionKind::BasicAttack);
}

void TestQueuedDamageAvoidsOverkill() {
    BattleSnapshot battle = BattleWithUnits();
    battle.units[10].hp = 80.0;
    ActionReservation queued{};
    queued.actorIndex = 1;
    queued.effects = {{10, 80.0, 0.0}};
    battle.reservations.push_back(queued);
    assert(ProjectedHp(battle, 10) == 0.0);

    const PendingAction chosen = ChooseBestAction(battle, {
        Attack(0, 10, 80.0), Attack(0, 11, 80.0)});
    assert(chosen.valid && chosen.candidate.targetIndex == 11);
}

void TestQueuedHealingAndSpReservations() {
    BattleSnapshot battle = BattleWithUnits();
    battle.units[1].hp = 40.0;
    ActionReservation queued{};
    queued.actorIndex = 1;
    queued.spCost = 10.0;
    queued.effects = {{1, 0.0, 20.0}};
    battle.reservations.push_back(queued);
    assert(ProjectedHp(battle, 1) == 60.0);
    assert(ReservedSp(battle, 1) == 10.0);
    const PendingAction chosen = ChooseBestAction(battle, {Heal(0, 1, 30.0), Attack(0, 10, 10.0)});
    assert(chosen.valid && chosen.candidate.kind == ActionKind::BasicAttack);

    // Revival is out of scope: a later queued heal cannot undo a lethal hit.
    ActionReservation lethal{};
    lethal.actorIndex = 1;
    lethal.effects = {{1, 60.0, 0.0}};
    ActionReservation attemptedRevival{};
    attemptedRevival.actorIndex = 0;
    attemptedRevival.effects = {{1, 0.0, 80.0}};
    battle.units[1].hp = 60.0;
    battle.reservations = {lethal, attemptedRevival};
    assert(ProjectedHp(battle, 1) == 0.0);
    assert(EffectiveHealing(battle, Heal(0, 1, 80.0)) == 0.0);
    const auto noRevival = ChooseBestAction(battle, {Heal(0, 1, 80.0), Attack(0, 10, 10.0)});
    assert(noRevival.valid && noRevival.candidate.kind == ActionKind::BasicAttack);
}

void TestDeploymentIncludesAffordableHealer() {
    BattleSnapshot battle{};
    battle.units[0] = Player(0);
    battle.units[1] = Player(1);
    battle.units[1].deployed = false;
    battle.units[1].hasVerifiedHealingSkill = true;
    battle.units[1].cheapestHealingSp = 10.0;
    battle.units[1].healingScore = 30.0;
    battle.units[2] = Player(2);
    battle.units[2].deployed = false;
    battle.units[2].offenseScore = 20.0;
    battle.units[3] = Player(3);
    battle.units[3].deployed = false;
    battle.units[3].offenseScore = 80.0;
    battle.units[4] = Player(4);
    battle.units[4].deployed = false;
    battle.units[4].hasVerifiedHealingSkill = true;
    battle.units[4].cheapestHealingSp = 11.0;
    battle.units[4].healingScore = 100.0;
    battle.units[4].sp = 10.0; // Not affordable; cannot displace healer 1.

    const std::vector<int> selected = SelectDeployment(battle, {4, 2, 1, 3}, 3);
    assert(selected.size() == 3);
    assert(selected[0] == 0); // Preserve deployed unit.
    assert(selected[1] == 1); // Affordable healer.
    assert(selected[2] == 3); // Strongest remaining dealer.
}

void TestMoveProgressAllowsSafeSidestep() {
    const std::vector<MoveTileCandidate> tiles = {
        {0, 0, 4, 5, true},
        {1, 0, 4, 2, false}, // Equal path progress, safer tile around an obstacle.
        {0, 1, 5, 4, false}
    };
    MoveTileCandidate selected{};
    assert(ChooseMoveTile(tiles, selected));
    assert(selected.gridX == 1 && selected.gridZ == 0);

    const std::vector<MoveTileCandidate> noProgress = {
        {0, 0, 3, 2, true}, {1, 0, 4, 1, false}
    };
    assert(!ChooseMoveTile(noProgress, selected));
}

void TestStageFinisherMaySpendReserveAndTieOrderIsStable() {
    BattleSnapshot battle = BattleWithUnits();
    battle.units[0].sp = 25.0;
    battle.units[10].hp = 20.0;
    battle.units[11].hp = 20.0;
    const auto finisher = Attack(0, 10, 20.0, ActionKind::OffensiveSkill, 20.0, 3, 204);
    ActionCandidate area = finisher;
    area.skillId = 205;
    area.menuIndex = 1;
    area.effects = {{10, 20.0, 0.0}, {11, 20.0, 0.0}};
    battle.units[12] = Enemy(12, 500.0);
    battle.units[12].deployed = false; // Off-map reinforcements are not a stage target.
    const PendingAction chosen = ChooseBestAction(battle, {finisher, area});
    assert(chosen.valid && chosen.reason == DecisionReason::StageFinisher);
    assert(chosen.candidate.skillId == 205);

    battle.units[10].hp = 500.0;
    battle.units[11].hp = 500.0;
    battle.units[0].sp = 100.0;
    const auto laterSkill = Attack(0, 10, 160.0, ActionKind::OffensiveSkill, 5.0, 2, 299);
    const auto earlierSkill = Attack(0, 10, 160.0, ActionKind::OffensiveSkill, 5.0, 1, 298);
    const PendingAction stable = ChooseBestAction(battle, {laterSkill, earlierSkill});
    assert(stable.valid && stable.candidate.menuIndex == 1);
}

} // namespace

int main() {
    TestEmergencyHealingOutranksOffense();
    TestNormalHealingAndOverheal();
    TestSpReserveAndVerifiedLearnedSkills();
    TestAccuracyAndResistanceAreAppliedInExpectedDamage();
    TestAreaDamageRequiresUsefulSafePlacement();
    TestQueuedDamageAvoidsOverkill();
    TestQueuedHealingAndSpReservations();
    TestDeploymentIncludesAffordableHealer();
    TestMoveProgressAllowsSafeSidestep();
    TestStageFinisherMaySpendReserveAndTieOrderIsStable();
    std::cout << "Combat planner regressions passed\n";
    return 0;
}
