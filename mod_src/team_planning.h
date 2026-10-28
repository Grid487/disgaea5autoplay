#pragma once
#include "native_actions.h"
#include "combat_runtime.h"
#include "planning_budget.h"

namespace AutoPlay::TeamPlanning {

inline uint64_t Fingerprint(uintptr_t context) {
    Planning::QueryScope profile(Planning::QueryKind::Fingerprint);
    uint64_t value = 1469598103934665603ull;
    auto add = [&](uint64_t n) { value = (value ^ n) * 1099511628211ull; };
    auto bytes = [&](uintptr_t p, size_t size) {
        for (size_t n = 0; n < size; ++n) add(*(const uint8_t*)(p + n));
    };
    add(NativeCombat::QueueCount(context));
    for (int i = 0; i < 256; ++i) {
        const auto u = GetUnitPtr(i);
        const auto cd = *(uintptr_t*)(u + 0x240);
        add(cd); add(*(uint8_t*)(u + 0x2E8));
        if (!cd) continue;
        add(*(uint64_t*)(u + 8)); add(*(uint32_t*)(u + 0x10));
        bytes(cd + 0x2CC8, 0x50); // HP/SP/maxima and combat stats.
        add(*(uint8_t*)(cd + 0x2E32));
        add(*(uint32_t*)(u + 0x2EE));
        if (*(uint8_t*)(u + 0x2E8) && *(uint8_t*)(cd + 0x2E32) == 0) {
            const int count = *(int16_t*)(cd + 0x2CC0);
            add(count);
            if (count >= 0 && count <= 512) bytes(cd + 0xCC4, count * 16);
        }
    }
    for (int i = 0, count = NativeCombat::QueueCount(context); i < count; ++i) {
        const auto q = NativeCombat::QueueEntry(i);
        add(*(uint16_t*)q); add(*(uint32_t*)(q + 0x27670));
        add(*(uint64_t*)(q + 0x27680)); add(*(uint64_t*)(q + 0x27690));
        add(*(uint64_t*)(q + 0x276D0));
        const int n = *(int16_t*)(q + 0x27318);
        add(n);
        if (n > 0 && n <= 256)
            for (int j = 0; j < n; ++j) add(*(uint16_t*)(q + 0x1318 + j * 0x260));
    }
    return value;
}

struct Job {
    bool active = false;
    bool complete = false;
    bool requiresQueueExecution = false;
    uint64_t fingerprint = 0;
    Combat::BattleSnapshot state;
    std::vector<int> actors;
    std::vector<int> enemies, healTargets;
    std::vector<NativeSkills::CatalogJob> catalogs;
    std::vector<Combat::ActionCandidate> basics;
    Combat::PendingAction best;
    NativeActions::Preview preview;
    int phase = -1, actor = 0, skill = 0, target = 0, cell = 0, rotation = 0;
    int externalQueue = 0, externalTarget = -1;
    int catalogCheck = 0, evaluationTarget = 0;
    bool evaluating = false;
    Combat::ActionCandidate evaluation;
    Combat::ActionReservation external;
    int shapeId = -1;
    NativeGeometry::ShapeDescriptor shape;
    std::array<uint8_t, 128 * 128 * 4> seen{};
    Planning::Telemetry timing;
};

inline Job current;
inline void Reset(const char* reason = "battle reset") {
    Planning::Report("team", current.timing, reason);
    current = {};
}

inline void Begin(uintptr_t context) {
    Reset();
    current.active = true;
    current.fingerprint = Fingerprint(context);
    current.state = NativeCombat::CaptureSnapshot(context, SmartCombat::accepted);
    for (const auto& u : current.state.units) {
        if (u.alive && u.deployed) {
            if (u.faction == Combat::Faction::Enemy && Combat::ProjectedHp(current.state, u.index) > 0)
                current.enemies.push_back(u.index);
            if (u.faction == Combat::Faction::Player && Combat::ProjectedHp(current.state, u.index) < u.maxHp * .5)
                current.healTargets.push_back(u.index);
        }
        if (!u.alive || !u.deployed || u.faction != Combat::Faction::Player ||
            g_UnitDoneThisTurn[u.index] || NativeCombat::HasQueuedAction(context, u.index) ||
            (!u.canAct && !u.canMove)) continue;
        if (fnIsControllableUnit && !fnIsControllableUnit((void*)GetUnitPtr(u.index), 0)) continue;
        current.actors.push_back(u.index);
        current.catalogs.push_back(NativeSkills::Begin(u.index));
    }
}

inline const NativeSkills::Entry* FindSkill(int actorIndex, int slot) {
    for (size_t a = 0; a < current.actors.size(); ++a)
        if (current.actors[a] == actorIndex)
            for (const auto& s : current.catalogs[a].entries) if (s.slot == slot) return &s;
    return nullptr;
}

inline void Include(const Combat::ActionCandidate& candidate) {
    // All basics are available before skill scoring. Keeping the best skill
    // and one new placement makes each policy invocation bounded by basics.
    auto choices = current.basics;
    if (current.best.valid) choices.push_back(current.best.candidate);
    choices.push_back(candidate);
    current.best = Combat::ChooseBestAction(current.state, choices);
}

inline void NextSkill() {
    ++current.skill;
    current.target = current.cell = current.rotation = 0;
    current.shapeId = -1;
    current.seen.fill(0);
}

inline bool Step(uintptr_t context, bool checkFingerprint = true) {
    auto& j = current;
    if (!j.active) Begin(context);
    if (checkFingerprint && j.fingerprint != Fingerprint(context)) { Reset("changed combat inputs"); Begin(context); return false; }
    if (j.complete) return true;
    if (j.evaluating) {
        bool failed = false;
        if (NativeActions::StepEffects(j.preview, j.evaluation, j.evaluationTarget, failed, 4)) {
            if (!failed) {
                if (j.evaluation.kind == Combat::ActionKind::BasicAttack) j.basics.push_back(j.evaluation);
                else Include(j.evaluation);
            }
            j.evaluating = false;
        }
        return false;
    }
    if (j.phase == -1) {
        // Respect actions already queued by the player as well as our own.
        // Query only a private copy, with four target estimates per frame.
        if (j.externalQueue >= NativeCombat::QueueCount(context)) j.phase = 0;
        else {
            const auto live = NativeCombat::QueueEntry(j.externalQueue);
            const bool tracked = std::any_of(SmartCombat::accepted.begin(), SmartCombat::accepted.end(),
                [&](const NativeCombat::AcceptedReservation& r) { return r.queueIndex == j.externalQueue; });
            if (tracked) { ++j.externalQueue; return false; }
            const auto q = (uintptr_t)j.preview.bytes.data();
            if (j.externalTarget < 0) {
                memcpy((void*)q, (void*)live, NativeCombat::kQueueStride);
                j.external = {};
                j.external.actorIndex = *(int16_t*)q;
                j.external.skillId = *(int32_t*)(q + 0x27670);
                const auto desc = *(uintptr_t*)(q + 0x27678);
                j.external.kind = !desc ? Combat::ActionKind::BasicAttack :
                    *(uint8_t*)(desc + 0x3F6) == 4 ? Combat::ActionKind::HealSkill : Combat::ActionKind::OffensiveSkill;
                const int64_t cost = *(int64_t*)(q + 0x276D0);
                j.external.spCost = cost >= 0 ? (double)cost : 0;
                j.externalTarget = 0;
            }
            const int count = *(int16_t*)(q + 0x27318);
            for (int budget = 0; count > 0 && count <= 256 && j.externalTarget < count && budget < 4;
                ++budget, ++j.externalTarget) {
                Planning::Unit operation(Planning::Resource::Effect);
                if (!operation) return false;
                Combat::EffectEstimate effect;
                if (NativeActions::evaluateEffect && NativeActions::evaluateEffect(q, 0, j.externalTarget,
                    j.external.kind == Combat::ActionKind::HealSkill, effect)) j.external.effects.push_back(effect);
                else {
                    // Resolve an unmodeled queued action before assigning any
                    // other unit. Partial estimates cannot prevent overkill or
                    // duplicate healing reliably.
                    j.requiresQueueExecution = j.complete = true;
                    return true;
                }
            }
            if (count < 1 || count > 256 || j.externalTarget >= count) {
                if (j.external.actorIndex >= 0 && j.external.actorIndex < 256)
                    j.state.reservations.push_back(j.external);
                ++j.externalQueue; j.externalTarget = -1;
            }
            return false;
        }
    }
    if (j.phase == 0) {
        if (j.actor < (int)j.actors.size()) {
            auto& catalog = j.catalogs[j.actor];
            if (!NativeSkills::Step(catalog, 4)) return false;
            auto& actor = j.state.units[j.actors[j.actor]];
            const int stop = std::min((int)catalog.entries.size(), j.catalogCheck + 4);
            for (; j.catalogCheck < stop; ++j.catalogCheck) {
                Planning::Unit operation;
                if (!operation) return false;
                const auto& skill = catalog.entries[j.catalogCheck];
                if (!skill.healing || !skill.currentlyUsable || !skill.affordableFromSp) continue;
                const int id = NativeActions::ConfiguredShape(actor.index, skill);
                NativeGeometry::ShapeDescriptor shape;
                if (id < 0 || !NativeGeometry::ReadShape(id, shape) || !NativeActions::ShapeIsStationary(shape)) continue;
                actor.hasVerifiedHealingSkill = true;
                if (actor.cheapestHealingSp == 0 || skill.spCost < actor.cheapestHealingSp)
                    actor.cheapestHealingSp = (double)skill.spCost;
            }
            if (j.catalogCheck < (int)catalog.entries.size()) return false;
            j.catalogCheck = 0;
            ++j.actor;
            return false;
        }
        j.phase = 1; j.actor = j.target = 0;
    }
    if (j.phase == 1) {
        for (int budget = 0; budget < 16; ++budget) {
            Planning::Unit operation(Planning::Resource::Preview);
            if (!operation) return false;
            if (j.actor >= (int)j.actors.size()) {
                j.best = Combat::ChooseBestAction(j.state, j.basics);
                j.phase = 2; j.actor = j.skill = j.target = 0;
                return false;
            }
            if (j.target >= (int)j.enemies.size()) { ++j.actor; j.target = 0; continue; }
            const auto& target = j.state.units[j.enemies[j.target++]];
            if (!target.alive || !target.deployed || target.faction != Combat::Faction::Enemy ||
                Combat::ProjectedHp(j.state, target.index) <= 0) continue;
            const int actor = j.actors[j.actor];
            const auto& a = j.state.units[actor];
            if (!a.canAct || !NativeActions::getBasicRange || !NativeActions::basicTargetLegal ||
                !NativeActions::basicTargetLegal((void*)GetUnitPtr(actor), (void*)GetUnitPtr(target.index),
                    NativeActions::getBasicRange((void*)GetUnitPtr(actor)), 0, 0)) continue;
            Combat::ActionCandidate candidate;
            if (NativeActions::BuildPreview(j.preview, j.state, actor, nullptr, 1,
                target.gridX, target.gridZ, 0, candidate, false)) {
                j.evaluation = std::move(candidate); j.evaluating = true; j.evaluationTarget = 0;
            }
            return false; // Resumable batch; the scheduler may continue this frame.
        }
        return false;
    }
    for (int budget = 0; budget < 32; ++budget) {
        Planning::Unit operation(Planning::Resource::Preview);
        if (!operation) return false;
        if (j.actor >= (int)j.actors.size()) { j.complete = true; return true; }
        const int actorIndex = j.actors[j.actor];
        const auto& actor = j.state.units[actorIndex];
        const auto& catalog = j.catalogs[j.actor].entries;
        if (!actor.canAct || j.skill >= (int)catalog.size()) {
            ++j.actor; j.skill = -1; NextSkill(); continue;
        }
        const auto& skill = catalog[j.skill];
        if (!skill.currentlyUsable || !skill.affordableFromSp) { NextSkill(); continue; }
        if (j.shapeId < 0) {
            j.shapeId = NativeActions::ConfiguredShape(actorIndex, skill);
            if (j.shapeId < 0 || !NativeGeometry::ReadShape(j.shapeId, j.shape) ||
                !NativeActions::ShapeIsStationary(j.shape)) { NextSkill(); continue; }
        }
        int x = actor.gridX, z = actor.gridZ, rotation = j.rotation;
        bool finishSkill = false;
        if (!skill.range) {
            finishSkill = ++j.rotation >= 4;
        } else {
            const auto& targets = skill.healing ? j.healTargets : j.enemies;
            if (j.target >= (int)targets.size()) { NextSkill(); continue; }
            const auto& target = j.state.units[targets[j.target]];
            const bool wanted = target.alive && target.deployed && (skill.healing ?
                target.faction == Combat::Faction::Player &&
                    Combat::ProjectedHp(j.state, target.index) < target.maxHp * .5 :
                target.faction == Combat::Faction::Enemy && Combat::ProjectedHp(j.state, target.index) > 0);
            if (!wanted) { ++j.target; j.cell = j.rotation = 0; continue; }
            if (j.cell >= j.shape.count) { ++j.target; j.cell = j.rotation = 0; continue; }
            const int sx = j.shape.cells[j.cell].x, sz = j.shape.cells[j.cell].z;
            const int dx = rotation == 0 ? sx : rotation == 1 ? sz : rotation == 2 ? -sx : -sz;
            const int dz = rotation == 0 ? sz : rotation == 1 ? -sx : rotation == 2 ? -sz : sx;
            x = target.gridX - dx; z = target.gridZ - dz;
            if (++j.rotation >= 4) { j.rotation = 0; ++j.cell; }
        }
        const int candidateShape = j.shapeId;
        if (x < 0 || z < 0 || x >= 128 || z >= 128 ||
            abs(x - actor.gridX) + abs(z - actor.gridZ) > skill.range ||
            (skill.rangeFlag == 1 && x != actor.gridX && z != actor.gridZ)) {
            if (finishSkill) NextSkill();
            continue;
        }
        const int seen = (x * 128 + z) * 4 + rotation;
        if (j.seen[seen]) { if (finishSkill) NextSkill(); continue; }
        j.seen[seen] = 1;
        Combat::ActionCandidate candidate;
        if (NativeActions::BuildPreview(j.preview, j.state, actorIndex, &skill,
            candidateShape, x, z, rotation, candidate, false)) {
            j.evaluation = std::move(candidate); j.evaluating = true; j.evaluationTarget = 0;
        }
        if (finishSkill) NextSkill();
        return false;
    }
    return false;
}

inline bool Run(uintptr_t context) {
    if (!Planning::Available()) return false;
    const auto clock = Planning::active ? Planning::active->clock : Planning::SteadyNow;
    const auto setupStart = clock(), cpuStart = Planning::ThreadCpuNow();
    if (!current.active) Begin(context);
    // Check once per frame, rather than once per internal work item.
    if (current.fingerprint != Fingerprint(context)) {
        Reset("changed combat inputs"); Begin(context);
    }
    if (!current.timing.started) { current.timing.start = setupStart; current.timing.started = true; }
    current.timing.cpu += Planning::ThreadCpuNow() - cpuStart;
    current.timing.activeTime += clock() - setupStart;
    return Planning::Run("team", current.timing, [] { return current.complete; },
        [context] {
            Step(context, false);
            current.timing.cursor = {current.phase, current.actor, current.skill, current.target};
            if (current.complete && current.best.valid) current.timing.actor = current.best.candidate.actorIndex;
        });
}

} // namespace AutoPlay::TeamPlanning
