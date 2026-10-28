#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../native_deployment.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace AutoPlay;
using namespace AutoPlay::NativeDeployment;

constexpr size_t kFakeImageSize = 128ull * 1024ull * 1024ull;
constexpr uintptr_t kFakeSkillTable = 0x04000000;
constexpr uintptr_t kFakeShapeHeader = 0x01000000;
constexpr uintptr_t kFakeShapeRecords = 0x01001000;
constexpr uintptr_t kFakeShapeCells = 0x01002000;
constexpr uintptr_t kFakeContextOffset = 0x03000000;
constexpr uintptr_t kFakeTaskOffset = 0x03100000;
constexpr size_t kFakeSkillTableCount = 20000;

uintptr_t g_Image = 0;
int g_EnumerateCalls = 0;
uint32_t g_LastCostSlot = 0;
uint32_t g_LastCostArea = 0;
bool g_LastCostUnitWasNull = false;

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
    return true;
}

uintptr_t Context() { return g_Image + kFakeContextOffset; }
uintptr_t Task() { return g_Image + kFakeTaskOffset; }
uintptr_t SkillTable() { return g_Image + kFakeSkillTable; }
uintptr_t BattleUnit(int index) {
    return g_Image + RVA::ADDR_BattleUnitArray + static_cast<uintptr_t>(index) * 0x1260;
}
uintptr_t Roster(int ordinal) {
    return g_Image + kRosterBaseRva + static_cast<uintptr_t>(ordinal) * kRosterStride;
}
uintptr_t Descriptor(uint32_t id) {
    return SkillTable() + static_cast<uintptr_t>(id) * NativeSkills::kDescriptorStride;
}

int StubEnumerate(void* charaPointer, uint32_t* outRows) {
    ++g_EnumerateCalls;
    const uintptr_t chara = reinterpret_cast<uintptr_t>(charaPointer);
    const int count = *reinterpret_cast<const int16_t*>(chara + 0x2CC0);
    if (count < 0 || count > static_cast<int>(NativeSkills::kMaxSkillRows)) return -1;
    int outCount = 0;
    for (int slot = 0; slot < count; ++slot) {
        const uint32_t id = *reinterpret_cast<const uint32_t*>(chara + 0xCC4 + slot * 0x10);
        if (id != 0 && id != 0xFFFFFFFFu) outRows[outCount++] = static_cast<uint32_t>(slot);
    }
    return outCount;
}

int64_t StubCost(void* descriptor, void* charaPointer, void* battleUnit, uint32_t slot, uint32_t area) {
    g_LastCostSlot = slot;
    g_LastCostArea = area;
    g_LastCostUnitWasNull = battleUnit == nullptr;
    if (static_cast<int32_t>(slot) < 0)
        return *reinterpret_cast<const uint16_t*>(reinterpret_cast<uintptr_t>(descriptor) + 6);
    const uintptr_t chara = reinterpret_cast<uintptr_t>(charaPointer);
    const int rowBoost = *reinterpret_cast<const uint8_t*>(chara + 0xCC9 + slot * 0x10);
    return *reinterpret_cast<const uint16_t*>(reinterpret_cast<uintptr_t>(descriptor) + 6) +
        rowBoost + static_cast<int64_t>(area - 1);
}

int StubCanUse(void*, void*, int64_t) { return 1; }
uint8_t StubRange(void*, void*, void*) { return 3; }
int StubLookup(const void*, uint32_t id) { return id < kFakeSkillTableCount ? static_cast<int>(id) : -1; }
int StubMaxShape(void*, void*) { return 4; }

int64_t StubGetStat(void* charaPointer, int selector) {
    const uintptr_t chara = reinterpret_cast<uintptr_t>(charaPointer);
    switch (selector) {
    case 0: case 1: return *reinterpret_cast<const int64_t*>(chara + 0x2CE8);
    case 2: return *reinterpret_cast<const int64_t*>(chara + 0x2CF0);
    case 3: return *reinterpret_cast<const int64_t*>(chara + 0x2CF8);
    case 4: return *reinterpret_cast<const int64_t*>(chara + 0x2D00);
    case 5: return *reinterpret_cast<const int64_t*>(chara + 0x2D08);
    default: return 0;
    }
}

uint32_t StubAttackSelector(void*, void* descriptorPointer, uint8_t) {
    const uintptr_t descriptor = reinterpret_cast<uintptr_t>(descriptorPointer);
    const uint8_t formula = *reinterpret_cast<const uint8_t*>(descriptor + 0x3F6);
    return formula == 3 ? 3u : (formula == 4 ? 4u : 0u);
}

int StubEffectPower(void* descriptorPointer, int rank, int effectSlot) {
    const uintptr_t descriptor = reinterpret_cast<uintptr_t>(descriptorPointer);
    return *reinterpret_cast<const int16_t*>(descriptor + 0x3E6 + effectSlot * 2) +
        *reinterpret_cast<const int16_t*>(descriptor + 0x420 + rank * 2) - 100;
}

void SetShapeTable() {
    std::memset(reinterpret_cast<void*>(g_Image + kFakeShapeHeader), 0, 0x3000);
    auto* header = reinterpret_cast<NativeGeometry::ShapeTableHeader*>(g_Image + kFakeShapeHeader);
    auto* records = reinterpret_cast<NativeGeometry::ShapeRecord*>(g_Image + kFakeShapeRecords);
    auto* cells = reinterpret_cast<NativeGeometry::ShapeCell*>(g_Image + kFakeShapeCells);
    *header = {2, 2};
    records[0] = {0, 0, 1, 0};
    records[1] = {8, 1, 1, 0};
    cells[0] = {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)};
    cells[1] = {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)};
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeHeaderPointerRva) =
        reinterpret_cast<uintptr_t>(header);
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeRecordsPointerRva) =
        reinterpret_cast<uintptr_t>(records);
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeCellsPointerRva) =
        reinterpret_cast<uintptr_t>(cells);
}

void SetStats(uintptr_t chara, int64_t hp, int64_t sp, int64_t atk, int64_t def,
              int64_t intelligence, int64_t res, int64_t hit) {
    *reinterpret_cast<int64_t*>(chara + 0x2CC8) = hp;
    *reinterpret_cast<int64_t*>(chara + 0x2CD8) = 100;
    *reinterpret_cast<int64_t*>(chara + 0x2CD0) = sp;
    *reinterpret_cast<int64_t*>(chara + 0x2CE0) = 100;
    *reinterpret_cast<int64_t*>(chara + 0x2CE8) = atk;
    *reinterpret_cast<int64_t*>(chara + 0x2CF0) = def;
    *reinterpret_cast<int64_t*>(chara + 0x2CF8) = intelligence;
    *reinterpret_cast<int64_t*>(chara + 0x2D00) = res;
    *reinterpret_cast<int64_t*>(chara + 0x2D08) = hit;
    *reinterpret_cast<int64_t*>(chara + 0x2D10) = 50;
}

void SetSkill(uintptr_t chara, uint32_t slot, uint32_t id, uint8_t formula,
              uint16_t cost, int16_t effectPower = 0, uint16_t shapeId = 8) {
    const uintptr_t row = chara + 0xCC4 + static_cast<uintptr_t>(slot) * 0x10;
    *reinterpret_cast<uint32_t*>(row) = id;
    *reinterpret_cast<uint8_t*>(row + 5) = 0;
    *reinterpret_cast<uint8_t*>(row + 6) = 0;
    const int16_t count = *reinterpret_cast<const int16_t*>(chara + 0x2CC0);
    if (count <= static_cast<int16_t>(slot))
        *reinterpret_cast<int16_t*>(chara + 0x2CC0) = static_cast<int16_t>(slot + 1);

    const uintptr_t descriptor = Descriptor(id);
    std::memset(reinterpret_cast<void*>(descriptor), 0, NativeSkills::kDescriptorStride);
    *reinterpret_cast<uint32_t*>(descriptor) = id;
    *reinterpret_cast<uint16_t*>(descriptor + 6) = cost;
    *reinterpret_cast<uint8_t*>(descriptor + 0x3E0) = 1;
    *reinterpret_cast<int16_t*>(descriptor + 0x3E6) = effectPower;
    *reinterpret_cast<int16_t*>(descriptor + 0x420) = 100;
    *reinterpret_cast<uint8_t*>(descriptor + 0x3F6) = formula;
    *reinterpret_cast<uint8_t*>(descriptor + 0x435) = 3;
    *reinterpret_cast<uint16_t*>(descriptor + 0x438) = shapeId;
    *reinterpret_cast<uint8_t*>(descriptor + 0x44E) = 0;
}

void SetContextRows(std::initializer_list<int> ordinals) {
    const uintptr_t context = Context();
    std::memset(reinterpret_cast<void*>(context), 0, 0x2000);
    *reinterpret_cast<int16_t*>(context + kRosterCountOffset) = static_cast<int16_t>(ordinals.size());
    int row = 0;
    for (int ordinal : ordinals) {
        *reinterpret_cast<int16_t*>(context + kRosterRowsOffset + row * 2) = static_cast<int16_t>(ordinal);
        *reinterpret_cast<uint32_t*>(context + kRosterFlagsOffset + row * 4) = 0;
        ++row;
    }
}

Combat::BattleSnapshot EmptySnapshot() {
    Combat::BattleSnapshot snapshot{};
    for (auto& unit : snapshot.units) unit.index = -1;
    return snapshot;
}

bool ResetWorld() {
    if (!EnsureImage()) return false;
    g_Base = g_Image;
    std::memset(reinterpret_cast<void*>(g_Image + RVA::ADDR_BattleUnitArray), 0, 256 * 0x1260);
    std::memset(reinterpret_cast<void*>(g_Image + kRosterBaseRva), 0, 256 * kRosterStride);
    std::memset(reinterpret_cast<void*>(g_Image + 0x05000000), 0, 0x4000);
    std::memset(reinterpret_cast<void*>(Context()), 0, 0x2000);
    std::memset(reinterpret_cast<void*>(Task()), 0, 0x400);
    *reinterpret_cast<int16_t*>(g_Image + kRosterTotalCountRva) = 256;
    *reinterpret_cast<uintptr_t*>(g_Image + NativeSkills::kSkillTablePointerRva) = SkillTable();
    *reinterpret_cast<int32_t*>(g_Image + NativeSkills::kSkillTableCountRva) =
        static_cast<int32_t>(kFakeSkillTableCount);
    SetShapeTable();
    NativeSkills::Initialize(g_Image);
    NativeEffects::Initialize(g_Image);
    NativeSkills::fnEnumerateSkills = &StubEnumerate;
    NativeSkills::fnEffectiveCost = &StubCost;
    NativeSkills::fnCanUseSkill = &StubCanUse;
    NativeSkills::fnEffectiveRange = &StubRange;
    NativeSkills::fnSkillIndex = &StubLookup;
    NativeSkills::fnMaxShapeChoices = &StubMaxShape;
    NativeEffects::fnGetStat = &StubGetStat;
    NativeEffects::fnAttackSelector = &StubAttackSelector;
    NativeEffects::fnEffectPower = &StubEffectPower;
    g_EnumerateCalls = 0;
    g_LastCostSlot = 0;
    g_LastCostArea = 0;
    g_LastCostUnitWasNull = false;
    return true;
}

void SetSnapshotDeployed(Combat::BattleSnapshot& snapshot, int index,
                         bool healer = false, double sp = 0.0, double healCost = 0.0) {
    auto& unit = snapshot.units[static_cast<size_t>(index)];
    unit.index = index;
    unit.faction = Combat::Faction::Player;
    unit.alive = true;
    unit.deployed = true;
    unit.sp = sp;
    unit.hasVerifiedHealingSkill = healer;
    unit.cheapestHealingSp = healCost;
}

bool Finish(Job& job, int stepBudget = 4) {
    for (int steps = 0; steps < 1000 && !job.IsComplete(); ++steps) job.Step(stepBudget);
    return job.IsComplete();
}

bool TestHealerFirstAndPreservesNativeSortedRows() {
    if (!ResetWorld()) return false;
    SetContextRows({20, 10, 30});
    SetStats(Roster(20), 100, 50, 300, 10, 60, 30, 30);
    SetStats(Roster(10), 100, 10, 50, 10, 40, 100, 20);
    SetStats(Roster(30), 100, 50, 50, 10, 240, 20, 60);
    SetSkill(Roster(10), 0, 401, 4, 5, 0);
    *reinterpret_cast<uint8_t*>(Roster(10) + 0xCC9) = 2;
    *reinterpret_cast<int64_t*>(Roster(10) + 0x2CD0) = 7;

    Job job;
    job.Begin(Context(), EmptySnapshot());
    if (!Expect(!job.IsComplete(), "deployment job starts with available rows and free slots")) return false;
    if (!Expect(Finish(job), "deployment candidate scan completes in bounded steps")) return false;
    if (!Expect(job.SelectedIsHealer(), "an affordable pure healing skill gets healer priority")) return false;
    if (!Expect(job.SelectedRow() == 1 && job.SelectedRoster() == 10,
                "selection returns the existing native-sorted list row and its roster ordinal")) return false;
    *reinterpret_cast<uintptr_t*>(Task() + kTaskBattleContextOffset) = Context();
    *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) = 99;
    if (!Expect(job.SelectCursor(Task(), Context()) &&
                *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) == 1,
                "a verified healer row can be selected after live task/context validation")) return false;
    *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) = 99;
    *reinterpret_cast<int64_t*>(Roster(10) + 0x2CD0) = 1;
    if (!Expect(!job.SelectCursor(Task(), Context()) &&
                *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) == 99,
                "a healer that became unaffordable is rejected before cursor mutation")) return false;
    return Expect(g_LastCostSlot == 0 && g_LastCostArea == 1 && g_LastCostUnitWasNull,
                  "undeployed skill cost uses its learned row and configured area without a BattleUnit");
}

bool TestDeployedHealerUsesDamageDealerSelection() {
    if (!ResetWorld()) return false;
    SetContextRows({10, 20, 30});
    SetStats(Roster(10), 100, 50, 250, 10, 20, 20, 10);
    SetStats(Roster(20), 100, 50, 50, 10, 250, 20, 80);
    SetStats(Roster(30), 100, 50, 80, 10, 160, 20, 70);
    SetSkill(Roster(10), 0, 402, 4, 5, 0);

    Combat::BattleSnapshot snapshot = EmptySnapshot();
    SetSnapshotDeployed(snapshot, 7, true, 10.0, 5.0);
    Job job;
    job.Begin(Context(), snapshot);
    if (!Expect(Finish(job), "selection completes with a healer already deployed")) return false;
    return Expect(!job.SelectedIsHealer() && job.SelectedRow() == 1 && job.SelectedRoster() == 20,
                  "existing usable healer is preserved and strongest enemy-matched damage dealer is selected");
}

bool TestHealingAffordabilityShapeAndLivingChecks() {
    if (!ResetWorld()) return false;
    SetContextRows({10, 20, 30});
    SetStats(Roster(10), 100, 3, 1, 0, 1, 1000, 0);
    SetStats(Roster(20), 100, 20, 90, 0, 30, 20, 20);
    SetStats(Roster(30), 0, 100, 999, 0, 999, 999, 999);
    SetSkill(Roster(10), 0, 403, 4, 5, 0); // Unaffordable despite very high RES.
    SetSkill(Roster(10), 1, 404, 4, 1, 0, 0); // Unsafe generic shape: no saved area.
    SetSkill(Roster(30), 0, 405, 4, 1, 0);

    Job job;
    job.Begin(Context(), EmptySnapshot());
    if (!Expect(Finish(job), "candidate scan handles unsupported healer candidates")) return false;
    return Expect(!job.SelectedIsHealer() && job.SelectedRoster() == 20,
                  "unaffordable heal, unconfigured area, and fainted roster members are excluded");
}

bool TestHealerTiesUseCostThenRosterOrdinal() {
    if (!ResetWorld()) return false;
    SetContextRows({40, 20, 10});
    SetStats(Roster(40), 100, 20, 10, 0, 10, 120, 10);
    SetStats(Roster(20), 100, 20, 10, 0, 10, 120, 10);
    SetStats(Roster(10), 100, 20, 10, 0, 10, 120, 10);
    SetSkill(Roster(40), 0, 406, 4, 5, 0);
    SetSkill(Roster(20), 0, 407, 4, 3, 0);
    SetSkill(Roster(10), 0, 408, 4, 3, 0);

    Job job;
    job.Begin(Context(), EmptySnapshot());
    if (!Expect(Finish(job), "tied healer candidate scan completes")) return false;
    return Expect(job.SelectedIsHealer() && job.SelectedRoster() == 10 && job.SelectedRow() == 2,
                  "equal healing strength chooses lower SP cost, then lower roster ordinal");
}

bool TestDamageRankingUsesEnemyDefenseAndHitTieBreak() {
    if (!ResetWorld()) return false;
    SetContextRows({11, 22});
    SetStats(Roster(11), 100, 20, 100, 0, 40, 10, 50);
    SetStats(Roster(22), 100, 20, 80, 0, 180, 10, 60);
    const uintptr_t enemyChara = BattleUnit(9) + 0x400;
    SetStats(enemyChara, 100, 20, 0, 100, 0, 20, 0);
    *reinterpret_cast<uintptr_t*>(BattleUnit(9) + 0x240) = enemyChara;
    Combat::BattleSnapshot snapshot = EmptySnapshot();
    snapshot.units[9].index = 9;
    snapshot.units[9].faction = Combat::Faction::Enemy;
    snapshot.units[9].alive = true;
    snapshot.units[9].deployed = true;

    Job job;
    job.Begin(Context(), snapshot);
    if (!Expect(Finish(job), "enemy-relative damage ranking completes")) return false;
    return Expect(job.SelectedRoster() == 22,
                  "magic damage is ranked against enemy RES when it beats physical damage against DEF");
}

bool TestDeployedHealerIsFoundFromLiveSkillCatalog() {
    if (!ResetWorld()) return false;
    SetContextRows({50, 60});
    SetStats(Roster(50), 100, 20, 100, 0, 30, 20, 20);
    SetStats(Roster(60), 100, 20, 80, 0, 180, 30, 30);
    const uintptr_t deployedChara = g_Image + 0x05000000;
    SetStats(deployedChara, 100, 20, 40, 0, 40, 100, 10);
    for (uint32_t slot = 0; slot < 5; ++slot)
        SetSkill(deployedChara, slot, 410 + slot, 2, 2, 0);
    SetSkill(deployedChara, 5, 415, 4, 5, 0);
    *reinterpret_cast<uintptr_t*>(BattleUnit(7) + 0x240) = deployedChara;
    *reinterpret_cast<uint8_t*>(BattleUnit(7) + 0x2E8) = 1;
    *reinterpret_cast<int16_t*>(BattleUnit(7) + 0x2EE) = -1;
    Combat::BattleSnapshot snapshot = EmptySnapshot();
    SetSnapshotDeployed(snapshot, 7, false, 20.0, 0.0);

    Job job;
    job.Begin(Context(), snapshot);
    if (!Expect(!job.Step(4) && !job.deployedCatalogComplete &&
                job.deployedCatalog.entries.size() == 4,
                "deployed learned-skill catalog is built four entries per frame")) return false;
    if (!Expect(!job.Step(4) && job.deployedCatalogComplete &&
                job.deployedCatalog.entries.size() == 6,
                "deployed catalog resumes and completes without an all-entries scan")) return false;
    if (!Expect(!job.Step(4) && !job.deployedHealerFound && job.deployedEntryIndex == 4,
                "healer shape/usability checks inspect at most four entries per frame")) return false;
    if (!Expect(!job.Step(4) && job.deployedHealerFound && !job.deployedCatalogActive,
                "the next bounded entry batch finds the healer and skips remaining deployed units")) return false;
    if (!Expect(Finish(job), "live deployed healer scan completes before roster selection")) return false;
    return Expect(job.SelectedRoster() == 60 && !job.SelectedIsHealer(),
                  "verified deployed healer causes the roster job to select a damage dealer");
}

bool TestCursorRevalidatesContextRowAndFlag() {
    if (!ResetWorld()) return false;
    SetContextRows({70, 80});
    SetStats(Roster(70), 100, 20, 100, 0, 50, 20, 30);
    SetStats(Roster(80), 100, 20, 80, 0, 50, 20, 30);
    *reinterpret_cast<uintptr_t*>(Task() + kTaskBattleContextOffset) = Context();
    *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) = 77;

    Job good;
    good.Begin(Context(), EmptySnapshot());
    if (!Expect(Finish(good) && good.SelectedRoster() == 70, "cursor test selects the first stable candidate")) return false;
    if (!Expect(good.SelectCursor(Task(), Context()) &&
                *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) == 0,
                "cursor write uses only the verified zero-based deployment row")) return false;

    *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) = 44;
    if (!Expect(!good.SelectCursor(Task(), Task()) &&
                *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) == 44,
                "a different active context cannot receive the selected cursor")) return false;
    *reinterpret_cast<uintptr_t*>(Task() + kTaskBattleContextOffset) = Task();
    if (!Expect(!good.SelectCursor(Task(), Context()) &&
                *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) == 44,
                "the task's live BattleContext pointer is rechecked immediately before selection")) return false;
    *reinterpret_cast<uintptr_t*>(Task() + kTaskBattleContextOffset) = Context();
    *reinterpret_cast<int16_t*>(Context() + kRosterRowsOffset) = 80;
    if (!Expect(!good.SelectCursor(Task(), Context()) &&
                *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) == 44,
                "changed sorted-row identity is rejected without changing the cursor")) return false;

    SetContextRows({70, 80});
    Job flagged;
    flagged.Begin(Context(), EmptySnapshot());
    if (!Finish(flagged) || flagged.SelectedRoster() != 70) return false;
    *reinterpret_cast<uint32_t*>(Context() + kRosterFlagsOffset) = 1;
    return Expect(!flagged.SelectCursor(Task(), Context()) &&
                  *reinterpret_cast<int32_t*>(Task() + kTaskRosterCursorOffset) == 44,
                  "a now-disabled deployment row is rejected before cursor mutation");
}

bool TestFiveDeployedSlotsArePreserved() {
    if (!ResetWorld()) return false;
    SetContextRows({90});
    SetStats(Roster(90), 100, 50, 999, 0, 999, 999, 99);
    Combat::BattleSnapshot snapshot = EmptySnapshot();
    for (int i = 0; i < Combat::kMaxDeployments; ++i) SetSnapshotDeployed(snapshot, i);
    Job job;
    job.Begin(Context(), snapshot);
    return Expect(job.IsComplete() && job.SelectedRow() == -1 && job.SelectedRoster() == -1,
                  "full five-unit deployment cap never replaces an already deployed unit");
}

bool TestCandidateWorkIsBounded() {
    if (!ResetWorld()) return false;
    SetContextRows({101});
    SetStats(Roster(101), 100, 10, 101, 0, 50, 10, 10);
    for (uint32_t slot = 0; slot < 5; ++slot)
        SetSkill(Roster(101), slot, 5001 + slot, 2, 2, 0);
    Job job;
    job.Begin(Context(), EmptySnapshot());
    if (!Expect(!job.Step(4) && job.activeCandidate.active && job.activeCandidate.nextSkill == 4 &&
                g_EnumerateCalls == 1,
                "step(maxSlots) evaluates at most four learned skill rows after one enumeration")) return false;
    return Expect(job.Step(4) && job.IsComplete() && g_EnumerateCalls == 1 &&
                  job.candidates.size() == 1,
                  "the next frame resumes the same character without repeating native enumeration");
}

} // namespace

int main() {
    const bool ok = TestHealerFirstAndPreservesNativeSortedRows() &&
        TestDeployedHealerUsesDamageDealerSelection() &&
        TestHealingAffordabilityShapeAndLivingChecks() &&
        TestHealerTiesUseCostThenRosterOrdinal() &&
        TestDamageRankingUsesEnemyDefenseAndHitTieBreak() &&
        TestDeployedHealerIsFoundFromLiveSkillCatalog() &&
        TestCursorRevalidatesContextRowAndFlag() &&
        TestFiveDeployedSlotsArePreserved() &&
        TestCandidateWorkIsBounded();
    return ok ? 0 : 1;
}
