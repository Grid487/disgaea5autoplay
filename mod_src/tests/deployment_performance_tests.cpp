#define main NativeDeploymentOriginalSuite
#include "native_deployment_tests.cpp"
#undef main

namespace {
bool CompareLargeRoster() {
    if (!ResetWorld()) return false;
    *reinterpret_cast<int16_t*>(Context() + kRosterCountOffset) = 66;
    for (int row = 0; row < 66; ++row) {
        *reinterpret_cast<int16_t*>(Context() + kRosterRowsOffset + row * 2) = static_cast<int16_t>(row);
        SetStats(Roster(row), 1000, 100, 100 + row, 10, 100 + row, 100, 20);
        for (int slot = 0; slot < 24; ++slot)
            SetSkill(Roster(row), slot, 6000 + slot, slot == 0 ? 4 : 2, 2, 5);
    }
    const auto state = EmptySnapshot();
    Job slow;
    slow.Begin(Context(), state);
    int slowFrames = 0;
    while (!slow.IsComplete() && slowFrames < 20000) { ++slowFrames; slow.Step(4); }
    Job fast;
    fast.Begin(Context(), state);
    Planning::Telemetry timing;
    int fastFrames = 0;
    while (!fast.IsComplete() && fastFrames < 20000) {
        ++fastFrames;
        Planning::PlanningBudget budget; Planning::Scope scope(budget);
        Planning::Run("deployment", timing, [&] { return fast.IsComplete(); }, [&] { fast.Step(4); });
        if (budget.used.work > 8192) return false;
    }
    std::printf("DEPLOYMENT {\"roster\":66,\"skillsEach\":24,\"singleBatchFrames\":%d,\"batchedFrames\":%d}\n", slowFrames, fastFrames);
    if (!Expect(fast.IsComplete() && fast.SelectedRoster() == slow.SelectedRoster() &&
                fast.SelectedIsHealer() == slow.SelectedIsHealer() && fast.SelectedIsHealer(),
                "large-roster batching preserves exact healer selection")) return false;
    if (!Expect(fastFrames * 5 < slowFrames, "large roster requires more than 80 percent fewer frames")) return false;

    // A long enemy/skill scoring product must also yield, not run inside one
    // candidate finalization. Compare it against the unchanged reference formula.
    Job scoring;
    scoring.Begin(Context(), state);
    scoring.availableRows.resize(1);
    scoring.enemies.assign(32, {500, 800});
    int frames = 0;
    while (!scoring.IsComplete() && frames++ < 20000) {
        Planning::PlanningBudget b; b.limits.work = 16;
        Planning::Scope s(b);
        Planning::Run("deployment", timing, [&] { return scoring.IsComplete(); }, [&] { scoring.Step(4); });
        if (b.used.work > 16) return false;
    }
    std::vector<SkillAttack> attacks;
    for (int slot = 1; slot < 24; ++slot) {
        NativeSkills::DescriptorInfo info;
        const auto d = Descriptor(6000 + slot);
        NativeSkills::ReadSupportedDescriptor(d, info);
        attacks.push_back({d, info, 0});
    }
    const double reference = DealerScore(Roster(0), scoring.enemies, attacks);
    return Expect(scoring.IsComplete() && frames > 2 && scoring.selectedOffenseScore == reference,
                  "enemy-by-skill deployment scoring yields and matches the original formula");
}
}
int main() { return CompareLargeRoster() ? 0 : 1; }
