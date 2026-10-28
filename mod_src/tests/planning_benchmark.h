// Shared by the current and archived v5.1 benchmark executables. Runtime
// fixtures install only stubs and allocated memory; this never attaches to a game.
#pragma once
#include <chrono>

namespace {
int __fastcall EnumerateBenchmarkSkills(void* cd, uint32_t* rows) {
    const int count = *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(cd) + 0x2CC0);
    for (int i = 0; i < count; ++i) rows[i] = static_cast<uint32_t>(i);
    return count;
}

void BenchmarkWorld(int actors, int skills, int enemies, bool area) {
    ResetWorld(800, 1000);
    NativeSkills::fnEnumerateSkills = &EnumerateBenchmarkSkills;
    if (area) InstallAoeShapeTable();
    if (area) {
        g_ForceSkillTargets = true;
        g_ForcedTargetCount = enemies;
        for (int i = 0; i < enemies; ++i) g_ForcedTargets[i] = i;
    }
    for (int i = 0; i < enemies; ++i)
        InstallUnit(i, 14 + i % 2, 10 + i / 2, 3, 1000, 1000, 100, 100, false, false);
    for (int a = 0; a < actors; ++a) {
        const int index = kActor + a;
        InstallUnit(index, 10 + a % 2, 10 + a / 2, 0, 1000, 1000, 1000, 1000, true, false);
        const uintptr_t cd = Chara(index);
        *reinterpret_cast<int16_t*>(cd + 0x2CC0) = static_cast<int16_t>(skills);
        for (int s = 0; s < skills; ++s) {
            const int id = 64 + s;
            const uintptr_t d = SkillTable() + id * NativeSkills::kDescriptorStride;
            std::memcpy(reinterpret_cast<void*>(d), reinterpret_cast<const void*>(Descriptor()), NativeSkills::kDescriptorStride);
            *reinterpret_cast<uint32_t*>(d) = id;
            *reinterpret_cast<uint8_t*>(d + 0x3F6) = 2;
            *reinterpret_cast<uint8_t*>(d + 0x435) = 6;
            *reinterpret_cast<uint16_t*>(d + 6) = 3;
            const uintptr_t row = cd + 0xCC4 + s * 16;
            std::memset(reinterpret_cast<void*>(row), 0, 16);
            *reinterpret_cast<uint32_t*>(row) = id;
            *reinterpret_cast<uint8_t*>(row + 10) = kAreaShape;
        }
    }
    TeamPlanning::Reset();
}

struct BenchmarkResult {
    int frames = 0, previews = 0, effects = 0;
    double milliseconds = 0;
    Combat::PendingAction decision;
};

BenchmarkResult RunBenchmark(int previewCap = 8, bool batched = true) {
    BenchmarkResult result;
    const auto start = std::chrono::steady_clock::now();
    while (!TeamPlanning::current.complete && result.frames < 200000) {
        ++result.frames;
#ifdef D5_BASELINE
        TeamPlanning::Step(BattleContext());
#else
        if (batched) {
            Planning::PlanningBudget budget;
            budget.limits.previews = previewCap;
            Planning::Scope scope(budget);
            TeamPlanning::Run(BattleContext());
            if (budget.used.previews > budget.limits.previews || budget.used.effects > 32 ||
                budget.used.paths > 4 || budget.used.work > 8192) std::abort();
        } else TeamPlanning::Step(BattleContext());
#endif
    }
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    result.previews = g_NativePreviewCalls;
    result.effects = g_EffectCallbackCalls;
    result.decision = TeamPlanning::current.best;
    return result;
}

void PrintBenchmark(const char* scenario, const BenchmarkResult& result) {
    const auto& d = result.decision;
    const auto& a = d.candidate;
    std::printf("BENCH {\"scenario\":\"%s\",\"frames\":%d,\"previews\":%d,\"effects\":%d,\"cpuMs\":%.3f,"
        "\"decision\":\"%d/%d/%d/%d/%d/%d/%d/%d/%.0f/%.0f\"}\n", scenario, result.frames,
        result.previews, result.effects, result.milliseconds, d.valid, a.actorIndex, a.skillId,
        static_cast<int>(a.kind), a.targetX, a.targetZ, a.rotation, static_cast<int>(d.reason),
        d.effectiveDamage, d.effectiveHealing);
}

bool SameDecision(const BenchmarkResult& a, const BenchmarkResult& b) {
    const auto& x = a.decision;
    const auto& y = b.decision;
    if (a.previews != b.previews || a.effects != b.effects || x.valid != y.valid || x.reason != y.reason ||
        x.effectiveDamage != y.effectiveDamage || x.effectiveHealing != y.effectiveHealing) return false;
    const auto& p = x.candidate; const auto& q = y.candidate;
    if (p.actorIndex != q.actorIndex || p.skillId != q.skillId || p.kind != q.kind ||
        p.menuIndex != q.menuIndex || p.targetX != q.targetX || p.targetZ != q.targetZ ||
        p.rotation != q.rotation || p.spCost != q.spCost || p.effects.size() != q.effects.size()) return false;
    for (size_t i = 0; i < p.effects.size(); ++i)
        if (p.effects[i].targetIndex != q.effects[i].targetIndex ||
            p.effects[i].expectedDamage != q.effects[i].expectedDamage ||
            p.effects[i].expectedHealing != q.effects[i].expectedHealing) return false;
    return true;
}

bool Benchmarks() {
    struct Scenario { const char* name; int actors, skills, enemies; bool area; };
    const Scenario scenarios[] = {{"warm", 1, 4, 3, false}, {"many_skills", 3, 24, 6, false},
                                  {"clustered", 2, 6, 6, true}};
    for (const auto& s : scenarios) {
        BenchmarkWorld(s.actors, s.skills, s.enemies, s.area);
        const auto result = RunBenchmark();
        PrintBenchmark(s.name, result);
        if (!TeamPlanning::current.complete || !result.decision.valid) return false;
#ifndef D5_BASELINE
        BenchmarkWorld(s.actors, s.skills, s.enemies, s.area);
        const auto slow = RunBenchmark(1, false);
        if (!Expect(SameDecision(result, slow), "batched and single-step exhaustive searches produce identical decisions and query counts")) return false;
        BenchmarkWorld(s.actors, s.skills, s.enemies, s.area);
        const auto reducedBatch = RunBenchmark(2);
        if (!Expect(SameDecision(result, reducedBatch), "two-preview and eight-preview budgets preserve exact decision and effects")) return false;
        if (!Expect(result.frames * 5 < slow.frames, "batch scheduling reduces required frames by more than 80 percent")) return false;
#endif
    }
    return true;
}
}
