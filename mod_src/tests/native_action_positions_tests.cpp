#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../native_action_positions.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace AutoPlay;
using namespace AutoPlay::NativeActionPositions;

constexpr size_t kFakeImageSize = 128ull * 1024ull * 1024ull;
constexpr uintptr_t kHeaderOffset = 0x01000000;
constexpr uintptr_t kRecordsOffset = 0x01001000;
constexpr uintptr_t kCellsOffset = 0x01100000;
constexpr uintptr_t kDescriptorOffset = 0x02000000;

uintptr_t g_Image = 0;
std::array<std::vector<FootprintCell>, Combat::kMaxUnits> g_Footprints;
std::array<int16_t, 128 * 128> g_Heights{};
uint8_t g_MaxBelow = 8;
uint8_t g_MaxAbove = 8;
int g_BasicRange = 1;
bool g_LineWeapon = false;

bool Expect(bool value, const char* label) {
    if (!value) std::fprintf(stderr, "FAIL: %s\n", label);
    else std::printf("PASS: %s\n", label);
    return value;
}

bool EnsureImage() {
    if (g_Image) return true;
    void* image = VirtualAlloc(nullptr, kFakeImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) {
        std::fprintf(stderr, "VirtualAlloc fake image failed (%lu)\n", GetLastError());
        return false;
    }
    g_Image = reinterpret_cast<uintptr_t>(image);
    AutoPlay::g_Base = g_Image;
    return true;
}

void ResetImage() {
    if (!EnsureImage()) return;
    std::memset(reinterpret_cast<void*>(g_Image + kHeaderOffset), 0, 0x110000);
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeHeaderPointerRva) = g_Image + kHeaderOffset;
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeRecordsPointerRva) = g_Image + kRecordsOffset;
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeCellsPointerRva) = g_Image + kCellsOffset;
    for (auto& footprint : g_Footprints) footprint.clear();
    g_Heights.fill(0);
    g_MaxBelow = 8;
    g_MaxAbove = 8;
    g_BasicRange = 1;
    g_LineWeapon = false;
    AutoPlay::g_Base = g_Image;
}

bool StubFootprint(int index, std::vector<FootprintCell>& out) {
    out = index >= 0 && index < Combat::kMaxUnits ? g_Footprints[static_cast<size_t>(index)] :
        std::vector<FootprintCell>{};
    if (out.empty()) out.push_back({0, 0});
    return true;
}

int StubBasicRange(int) { return g_BasicRange; }

bool StubBasicReach(int actorIndex, int sourceX, int sourceZ, int targetIndex,
                    int targetX, int targetZ, int range) {
    std::vector<FootprintCell> actor, target;
    if (!StubFootprint(actorIndex, actor) || !StubFootprint(targetIndex, target)) return false;
    for (const auto& a : actor) for (const auto& t : target) {
        const int dx = std::abs((sourceX + a.x) - (targetX + t.x));
        const int dz = std::abs((sourceZ + a.z) - (targetZ + t.z));
        if (dx + dz <= range) return true;
    }
    return false;
}

bool StubLineWeapon(int) { return g_LineWeapon; }

int StubConfiguredShape(int, const NativeSkills::Entry& skill) {
    return skill.shapeId;
}

bool StubHeight(int x, int z, int16_t& height) {
    if (!TacticalPath::InBounds(x, z)) return false;
    height = g_Heights[static_cast<size_t>(TacticalPath::CellIndex(x, z))];
    return true;
}

bool StubBasicVertical(int sourceX, int sourceZ, int, int targetX, int targetZ, int range) {
    int16_t sourceY = 0, targetY = 0;
    if (!StubHeight(sourceX, sourceZ, sourceY) || !StubHeight(targetX, targetZ, targetY)) return false;
    return std::abs(static_cast<int>(targetY) - static_cast<int>(sourceY)) <= (range + 1) * 6;
}

bool StubHeightLimits(const NativeSkills::Entry&, uint8_t& below, uint8_t& above) {
    below = g_MaxBelow;
    above = g_MaxAbove;
    return true;
}

void InstallReaders() {
    g_UnitFootprintReaderOverride = &StubFootprint;
    g_BasicRangeReaderOverride = &StubBasicRange;
    g_BasicReachReaderOverride = &StubBasicReach;
    g_LineWeaponReaderOverride = &StubLineWeapon;
    g_BasicVerticalReaderOverride = &StubBasicVertical;
    g_ConfiguredShapeReaderOverride = &StubConfiguredShape;
    g_HeightReaderOverride = &StubHeight;
    g_HeightLimitsReaderOverride = &StubHeightLimits;
}

void SetUnit(Combat::BattleSnapshot& state, int index, Combat::Faction faction,
             int x, int z, double hp = 100.0, double maxHp = 100.0) {
    auto& unit = state.units[static_cast<size_t>(index)];
    unit = {};
    unit.index = index;
    unit.faction = faction;
    unit.alive = true;
    unit.deployed = true;
    unit.canAct = true;
    unit.canMove = true;
    unit.hp = hp;
    unit.maxHp = maxHp;
    unit.sp = 100;
    unit.maxSp = 100;
    unit.gridX = x;
    unit.gridZ = z;
}

NativeSkills::Entry MakeSkill(int id, int shapeId, uint8_t range,
                              bool healing = false, uint8_t rangeFlag = 0) {
    NativeSkills::Entry skill{};
    skill.slot = id;
    skill.skillId = id;
    skill.descriptor = g_Image + kDescriptorOffset + static_cast<uintptr_t>(id) * 0x500;
    skill.spCost = 3;
    skill.range = range;
    skill.rangeFlag = rangeFlag;
    skill.shapeId = static_cast<uint16_t>(shapeId);
    skill.learned = true;
    skill.currentlyUsable = true;
    skill.affordableFromSp = true;
    skill.healing = healing;
    return skill;
}

void SetShapeTable(const std::vector<NativeGeometry::ShapeRecord>& records,
                   const std::vector<NativeGeometry::ShapeCell>& cells) {
    auto* header = reinterpret_cast<NativeGeometry::ShapeTableHeader*>(g_Image + kHeaderOffset);
    auto* nativeRecords = reinterpret_cast<NativeGeometry::ShapeRecord*>(g_Image + kRecordsOffset);
    auto* nativeCells = reinterpret_cast<NativeGeometry::ShapeCell*>(g_Image + kCellsOffset);
    *header = {static_cast<uint16_t>(records.size()), static_cast<uint16_t>(cells.size())};
    std::copy(records.begin(), records.end(), nativeRecords);
    std::copy(cells.begin(), cells.end(), nativeCells);
}

bool RunToCompletion(GoalJob& job, size_t budget = 256) {
    for (size_t frame = 0; frame < 200000 && !job.IsComplete(); ++frame) {
        job.Step(budget);
        if (job.LastWorkUnits() > std::min<size_t>(budget, 256)) return false;
    }
    return job.IsComplete() && job.Succeeded();
}

bool TestBasicAdjacentPositionsAndQueuedDeadEnemy() {
    ResetImage();
    Combat::BattleSnapshot state{};
    SetUnit(state, 1, Combat::Faction::Player, 10, 10);
    SetUnit(state, 20, Combat::Faction::Enemy, 30, 40);

    GoalJob alive{};
    if (!Expect(alive.Begin(1, state, {}), "basic-position job begins for a deployed actor")) return false;
    if (!Expect(RunToCompletion(alive), "basic-position job completes with native range and reach readers")) return false;
    const auto& goals = alive.Goals();
    if (!Expect(goals[static_cast<size_t>(TacticalPath::CellIndex(29, 40))] != 0 &&
                goals[static_cast<size_t>(TacticalPath::CellIndex(31, 40))] != 0 &&
                goals[static_cast<size_t>(TacticalPath::CellIndex(30, 39))] != 0 &&
                goals[static_cast<size_t>(TacticalPath::CellIndex(30, 41))] != 0,
                "range-one basic attack generates all four adjacent source tiles")) return false;
    if (!Expect(goals[static_cast<size_t>(TacticalPath::CellIndex(30, 40))] == 0,
                "basic attack does not use the enemy-occupied tile as its source")) return false;

    state.reservations.push_back({1, Combat::ActionKind::BasicAttack, -1, 0.0, {{20, 100.0, 0.0}}});
    GoalJob dead{};
    if (!Expect(dead.Begin(1, state, {}), "basic-position job accepts queued reservations")) return false;
    if (!Expect(RunToCompletion(dead), "job completes when all targets are projected dead")) return false;
    return Expect(std::all_of(dead.Goals().begin(), dead.Goals().end(), [](uint8_t v) { return v == 0; }),
                  "queued lethal damage prevents movement goals for an expected-dead enemy");
}

bool TestBasicReachUsesBothOccupancyFootprints() {
    ResetImage();
    g_Footprints[1] = {{0, 0}, {1, 0}};
    g_Footprints[20] = {{0, 0}, {0, 1}};
    Combat::BattleSnapshot state{};
    SetUnit(state, 1, Combat::Faction::Player, 10, 10);
    SetUnit(state, 20, Combat::Faction::Enemy, 30, 40);
    GoalJob job{};
    if (!Expect(job.Begin(1, state, {}), "multi-cell basic footprint job begins")) return false;
    if (!Expect(RunToCompletion(job), "multi-cell basic footprint job completes")) return false;
    if (!Expect(job.Goals()[static_cast<size_t>(TacticalPath::CellIndex(28, 40))] != 0,
                "native reach semantics account for the actor occupancy footprint")) return false;
    if (!Expect(job.Goals()[static_cast<size_t>(TacticalPath::CellIndex(29, 42))] != 0,
                "native reach semantics account for the target occupancy footprint")) return false;
    if (!Expect(job.Goals()[static_cast<size_t>(TacticalPath::CellIndex(30, 40))] == 0 &&
                job.Goals()[static_cast<size_t>(TacticalPath::CellIndex(30, 41))] == 0,
                "movement goals exclude target footprint cells")) return false;

    GoalJob bounded{};
    if (!Expect(bounded.Begin(1, state, {}), "bounded basic enumeration job begins")) return false;
    size_t maxWork = 0;
    size_t maxCandidates = 0;
    for (size_t frame = 0; frame < 200000 && !bounded.IsComplete(); ++frame) {
        const size_t before = bounded.ProcessedBasicPairs();
        bounded.Step(1);
        maxWork = std::max(maxWork, bounded.LastWorkUnits());
        maxCandidates = std::max(maxCandidates, bounded.ProcessedBasicPairs() - before);
    }
    return Expect(bounded.Succeeded() && maxWork <= 1 && maxCandidates <= 1,
                  "source-tile evaluation respects one-unit incremental step budgets");
}

bool TestNativeOccupancyFootprintMemoryBounds() {
    ResetImage();
    const uintptr_t unit = AutoPlay::GetUnitPtr(1);
    *reinterpret_cast<int16_t*>(unit + kBattleUnitFootprintCountOffset) = 2;
    const uint64_t first = (static_cast<uint64_t>(static_cast<uint32_t>(-3)) << 32) |
        static_cast<uint32_t>(4);
    const uint64_t second = (static_cast<uint64_t>(static_cast<uint32_t>(7)) << 32) |
        static_cast<uint32_t>(-8);
    std::memcpy(reinterpret_cast<void*>(unit + kBattleUnitFootprintCellsOffset), &first, sizeof(first));
    std::memcpy(reinterpret_cast<void*>(unit + kBattleUnitFootprintCellsOffset + 8), &second, sizeof(second));
    std::vector<FootprintCell> offsets;
    if (!Expect(ReadNativeUnitFootprint(1, offsets), "native reader reads the verified occupancy footprint table")) return false;
    if (!Expect(offsets.size() == 2 && offsets[0].x == 4 && offsets[0].z == -3 &&
                offsets[1].x == -8 && offsets[1].z == 7,
                "native occupancy footprint decodes packed signed x/z values in order")) return false;

    *reinterpret_cast<int16_t*>(unit + kBattleUnitFootprintCountOffset) = 33;
    offsets.clear();
    if (!Expect(!ReadNativeUnitFootprint(1, offsets), "native reader rejects count beyond its 32-cell storage")) return false;
    return Expect(offsets.empty(), "invalid occupancy footprint count produces no partial cells");
}

bool TestBasicLineAndVerticalLegality() {
    ResetImage();
    Combat::BattleSnapshot state{};
    SetUnit(state, 1, Combat::Faction::Player, 10, 10);
    SetUnit(state, 20, Combat::Faction::Enemy, 30, 40);
    g_BasicRange = 2;
    g_LineWeapon = true;
    GoalJob line{};
    if (!Expect(line.Begin(1, state, {}), "line-weapon goal job begins")) return false;
    if (!Expect(RunToCompletion(line), "line-weapon goal job completes")) return false;
    if (!Expect(line.Goals()[static_cast<size_t>(TacticalPath::CellIndex(28, 40))] != 0,
                "line weapon includes a two-tile source on the target row")) return false;
    if (!Expect(line.Goals()[static_cast<size_t>(TacticalPath::CellIndex(29, 39))] == 0,
                "line weapon excludes a diagonal source with no shared footprint axis")) return false;

    g_LineWeapon = false;
    g_BasicRange = 1;
    g_Heights[static_cast<size_t>(TacticalPath::CellIndex(30, 40))] = 13;
    GoalJob highTarget{};
    if (!Expect(highTarget.Begin(1, state, {}), "height-limited basic goal job begins")) return false;
    if (!Expect(RunToCompletion(highTarget), "height-limited basic goal job completes")) return false;
    return Expect(highTarget.Goals()[static_cast<size_t>(TacticalPath::CellIndex(29, 40))] == 0,
                  "basic source is rejected when world-Y difference exceeds native range tolerance");
}

bool TestSkillRotationOrthogonalRangeAndHeight() {
    ResetImage();
    SetShapeTable({{77, 0, 1, 0}, {78, 1, 1, 0}}, {
        {1, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)}});
    Combat::BattleSnapshot state{};
    SetUnit(state, 1, Combat::Faction::Player, 5, 5);
    SetUnit(state, 20, Combat::Faction::Enemy, 40, 40);
    auto skill = MakeSkill(1, 77, 0, false, 0);
    GoalJob job{};
    if (!Expect(job.Begin(1, state, {skill}), "configured skill position job begins")) return false;
    if (!Expect(RunToCompletion(job), "configured skill position job completes")) return false;
    const auto& goals = job.Goals();
    if (!Expect(goals[static_cast<size_t>(TacticalPath::CellIndex(39, 40))] != 0,
                "native shape cell offset produces the correct unrotated cast site")) return false;
    if (!Expect(goals[static_cast<size_t>(TacticalPath::CellIndex(40, 41))] != 0,
                "verified quarter-turn shape rotation produces the corresponding cast site")) return false;

    auto orthogonal = MakeSkill(5, 78, 2, false, 1);
    GoalJob rangeJob{};
    if (!Expect(rangeJob.Begin(1, state, {orthogonal}), "orthogonal-range job begins")) return false;
    if (!Expect(RunToCompletion(rangeJob), "orthogonal-range job completes")) return false;
    const auto& rangeGoals = rangeJob.Goals();
    if (!Expect(rangeGoals[static_cast<size_t>(TacticalPath::CellIndex(38, 40))] != 0,
                "orthogonal range includes a legal two-tile horizontal source position")) return false;
    if (!Expect(rangeGoals[static_cast<size_t>(TacticalPath::CellIndex(40, 38))] != 0,
                "orthogonal range includes a legal two-tile vertical source position")) return false;
    if (!Expect(rangeGoals[static_cast<size_t>(TacticalPath::CellIndex(39, 39))] == 0,
                "range flag 1 excludes diagonal source positions")) return false;

    g_MaxAbove = 3;
    g_BasicRange = 0; // isolate the skill's descriptor height rule from basic goals
    g_Heights[static_cast<size_t>(TacticalPath::CellIndex(40, 40))] = 5;
    g_Heights[static_cast<size_t>(TacticalPath::CellIndex(39, 40))] = 0;
    auto heightSkill = MakeSkill(1, 77, 2, false, 0);
    GoalJob blockedHeight{};
    if (!Expect(blockedHeight.Begin(1, state, {heightSkill}), "height-restricted skill job begins")) return false;
    if (!Expect(RunToCompletion(blockedHeight), "height-restricted skill job completes")) return false;
    return Expect(blockedHeight.Goals()[static_cast<size_t>(TacticalPath::CellIndex(39, 40))] == 0,
                  "descriptor vertical tolerance rejects an otherwise in-range source tile");
}

bool TestHealingThresholdAndSafeArea() {
    ResetImage();
    SetShapeTable({{88, 0, 1, 0}}, {{0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)}});
    Combat::BattleSnapshot state{};
    SetUnit(state, 1, Combat::Faction::Player, 5, 5);
    SetUnit(state, 2, Combat::Faction::Player, 20, 20, 49, 100);
    auto heal = MakeSkill(2, 88, 0, true);
    GoalJob useful{};
    if (!Expect(useful.Begin(1, state, {heal}), "healing-position job begins")) return false;
    if (!Expect(RunToCompletion(useful), "healing-position job completes for an injured ally")) return false;
    if (!Expect(useful.Goals()[static_cast<size_t>(TacticalPath::CellIndex(20, 20))] != 0,
                "healing skill generates a source position for an ally below 50 percent HP")) return false;

    state.units[2].hp = 50;
    GoalJob healthy{};
    if (!Expect(healthy.Begin(1, state, {heal}), "threshold healing job begins")) return false;
    if (!Expect(RunToCompletion(healthy), "threshold healing job completes")) return false;
    if (!Expect(healthy.Goals()[static_cast<size_t>(TacticalPath::CellIndex(20, 20))] == 0,
                "healing positions are not generated at exactly 50 percent projected HP")) return false;

    state.reservations.push_back({1, Combat::ActionKind::BasicAttack, -1, 0.0, {{2, 50.0, 0.0}}});
    GoalJob projectedDead{};
    if (!Expect(projectedDead.Begin(1, state, {heal}), "projected-casualty healing job begins")) return false;
    if (!Expect(RunToCompletion(projectedDead), "projected-casualty healing job completes")) return false;
    if (!Expect(std::all_of(projectedDead.Goals().begin(), projectedDead.Goals().end(),
                            [](uint8_t value) { return value == 0; }),
                "healing goals are omitted for an ally projected to zero HP")) return false;
    state.reservations.clear();

    state.units[2].hp = 49;
    state.units[3].index = 3;
    state.units[3].faction = Combat::Faction::Enemy;
    state.units[3].alive = true;
    state.units[3].deployed = true;
    state.units[3].gridX = 20;
    state.units[3].gridZ = 20;
    GoalJob mixedTile{};
    if (!Expect(mixedTile.Begin(1, state, {heal}), "safe-healing job begins")) return false;
    if (!Expect(RunToCompletion(mixedTile), "safe-healing job completes")) return false;
    return Expect(mixedTile.Goals()[static_cast<size_t>(TacticalPath::CellIndex(20, 20))] == 0,
                  "healing area is rejected when an enemy occupies a tile it would affect");
}

bool TestOffensiveFriendlyFireAndCasterExclusion() {
    ResetImage();
    SetShapeTable({{99, 0, 5, 0}}, {
        {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {1, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {-1, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {0, 1, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {0, -1, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)}});
    Combat::BattleSnapshot state{};
    SetUnit(state, 1, Combat::Faction::Player, 19, 20);
    SetUnit(state, 20, Combat::Faction::Enemy, 20, 20);
    for (int i = 0; i < 4; ++i) {
        const int xs[] = {21, 19, 20, 20};
        const int zs[] = {20, 20, 21, 19};
        SetUnit(state, 30 + i, Combat::Faction::Player, xs[i], zs[i]);
    }
    auto attack = MakeSkill(3, 99, 0, false);
    GoalJob unsafe{};
    if (!Expect(unsafe.Begin(1, state, {attack}), "friendly-fire candidate job begins")) return false;
    if (!Expect(RunToCompletion(unsafe), "friendly-fire candidate job completes")) return false;
    return Expect(unsafe.Goals().empty() ||
                  std::all_of(unsafe.Goals().begin(), unsafe.Goals().end(), [](uint8_t value) { return value == 0; }),
                  "offensive placements touching any ally are rejected");
}

bool TestCurrentActorTileIsHypotheticalAndFutureSelfHitIsRejected() {
    ResetImage();
    SetShapeTable({
        {101, 0, 2, 0},
        {102, 2, 1, 0}
    }, {
        {1, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {2, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)}});
    Combat::BattleSnapshot state{};
    SetUnit(state, 1, Combat::Faction::Player, 21, 20);
    SetUnit(state, 20, Combat::Faction::Enemy, 20, 20);
    auto hypothetical = MakeSkill(6, 101, 0, false);
    GoalJob job{};
    if (!Expect(job.Begin(1, state, {hypothetical}), "hypothetical actor-position job begins")) return false;
    if (!Expect(RunToCompletion(job), "hypothetical actor-position job completes")) return false;
    if (!Expect(job.Goals()[static_cast<size_t>(TacticalPath::CellIndex(19, 20))] != 0,
                "current actor occupancy is excluded while evaluating a post-move skill area")) return false;

    auto selfHit = MakeSkill(7, 102, 1, false);
    GoalJob rejected{};
    if (!Expect(rejected.Begin(1, state, {selfHit}), "future self-hit job begins")) return false;
    if (!Expect(RunToCompletion(rejected), "future self-hit job completes")) return false;
    return Expect(rejected.Goals()[static_cast<size_t>(TacticalPath::CellIndex(20, 20))] == 0,
                  "an offensive source position inside its own affected area is rejected");
}

bool TestIndexedShapesPreserveGoalMask() {
    ResetImage();
    NativeGeometry::ResetShapeCache();
    SetShapeTable({{77, 0, 2, 0}}, {
        {1, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)}});
    Combat::BattleSnapshot state{};
    SetUnit(state, 1, Combat::Faction::Player, 5, 5);
    SetUnit(state, 20, Combat::Faction::Enemy, 40, 40);
    const auto skill = MakeSkill(1, 77, 4, false, 0);
    GoalJob linear;
    if (!linear.Begin(1, state, {skill}) || !RunToCompletion(linear)) return false;
    if (!NativeGeometry::StepShapeCache(256)) return false;
    GoalJob indexed;
    if (!indexed.Begin(1, state, {skill})) return false;
    Planning::Telemetry timing;
    for (int frame = 0; frame < 1000 && !indexed.IsComplete(); ++frame) {
        Planning::PlanningBudget b; Planning::Scope s(b);
        Planning::Run("goals", timing, [&] { return indexed.IsComplete(); }, [&] { indexed.Step(); });
        if (b.used.work > 8192) return false;
    }
    return Expect(indexed.Succeeded() && linear.Goals() == indexed.Goals() &&
                  indexed.ProcessedShapeRecords() == 0 && linear.ProcessedShapeRecords() > 0,
                  "indexed same-frame shape lookup preserves the complete tactical goal mask");
}

bool TestLargeShapeSearchIsIncremental() {
    ResetImage();
    std::vector<NativeGeometry::ShapeRecord> records;
    std::vector<NativeGeometry::ShapeCell> cells;
    records.reserve(600);
    cells.reserve(600);
    for (uint32_t i = 1; i <= 600; ++i) {
        records.push_back({i, static_cast<uint16_t>(cells.size()), 1, 0});
        cells.push_back({0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)});
    }
    SetShapeTable(records, cells);
    Combat::BattleSnapshot state{};
    SetUnit(state, 1, Combat::Faction::Player, 2, 2);
    SetUnit(state, 20, Combat::Faction::Enemy, 50, 50);
    auto skill = MakeSkill(4, 600, 1, false);
    GoalJob job{};
    if (!Expect(job.Begin(1, state, {skill}), "large-table skill job begins")) return false;
    size_t maxRecordsPerStep = 0;
    size_t maxWork = 0;
    for (size_t frame = 0; frame < 10000 && !job.IsComplete(); ++frame) {
        const size_t priorRecords = job.ProcessedShapeRecords();
        job.Step(37);
        maxRecordsPerStep = std::max(maxRecordsPerStep, job.ProcessedShapeRecords() - priorRecords);
        maxWork = std::max(maxWork, job.LastWorkUnits());
    }
    if (!Expect(job.Succeeded(), "large-table shape eventually resolves and generates goals")) return false;
    if (!Expect(maxRecordsPerStep <= 37 && maxWork <= 37,
                "shape-table scanning and all other generation work stay within the per-step budget")) return false;
    return Expect(job.Goals()[static_cast<size_t>(TacticalPath::CellIndex(49, 50))] != 0,
                  "incremental shape lookup finds the final table record correctly");
}

} // namespace

int main() {
    InstallReaders();
    bool ok = true;
    ok = TestBasicAdjacentPositionsAndQueuedDeadEnemy() && ok;
    ok = TestBasicReachUsesBothOccupancyFootprints() && ok;
    ok = TestNativeOccupancyFootprintMemoryBounds() && ok;
    ok = TestBasicLineAndVerticalLegality() && ok;
    ok = TestSkillRotationOrthogonalRangeAndHeight() && ok;
    ok = TestHealingThresholdAndSafeArea() && ok;
    ok = TestOffensiveFriendlyFireAndCasterExclusion() && ok;
    ok = TestCurrentActorTileIsHypotheticalAndFutureSelfHitIsRejected() && ok;
    ok = TestLargeShapeSearchIsIncremental() && ok;
    ok = TestIndexedShapesPreserveGoalMask() && ok;
    if (g_Image) VirtualFree(reinterpret_cast<void*>(g_Image), 0, MEM_RELEASE);
    return ok ? 0 : 1;
}
