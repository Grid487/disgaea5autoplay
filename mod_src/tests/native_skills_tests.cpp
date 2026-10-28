#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../native_skills.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace AutoPlay;
using namespace AutoPlay::NativeSkills;

constexpr size_t kFakeImageSize = 128ull * 1024ull * 1024ull;
constexpr uintptr_t kFakeSkillTable = 0x04000000;
constexpr uintptr_t kFakeChara = 0x06000000;
constexpr uintptr_t kFakeMenu = 0x07000000;
constexpr uintptr_t kFakeSkillCount = 20000;
constexpr int kActor = 7;

uintptr_t g_Image = 0;
std::vector<uint32_t> g_EnumeratedRows;
std::vector<int> g_UsableById(kFakeSkillCount, 1);
int64_t g_CostDelta = 0;
int g_EnumerateCalls = 0;
int g_LastCostSlot = -1;
uint32_t g_LastAreaPanels = 0;
int g_MaxShapeChoices = 4;

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

uintptr_t ActorUnit() { return GetUnitPtr(kActor); }
uintptr_t CharaData() { return g_Image + kFakeChara; }
uintptr_t MenuTask() { return g_Image + kFakeMenu; }
uintptr_t SkillTableAddress() { return g_Image + kFakeSkillTable; }

int StubEnumerate(void*, uint32_t* outRows) {
    ++g_EnumerateCalls;
    std::copy(g_EnumeratedRows.begin(), g_EnumeratedRows.end(), outRows);
    return static_cast<int>(g_EnumeratedRows.size());
}

int64_t StubCost(void* descriptor, void*, void*, uint32_t slot, uint32_t areaPanels) {
    g_LastCostSlot = static_cast<int>(slot);
    g_LastAreaPanels = areaPanels;
    return static_cast<int64_t>(*reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(descriptor) + 6)) +
        g_CostDelta;
}

int StubCanUse(void*, void* descriptor, int64_t) {
    const uint32_t id = *reinterpret_cast<uint32_t*>(descriptor);
    return id < g_UsableById.size() ? g_UsableById[id] : 0;
}

uint8_t StubRange(void*, void* descriptor, void*) {
    return *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(descriptor) + 0x435);
}

int StubLookup(const void*, uint32_t skillId) {
    return skillId < kFakeSkillCount ? static_cast<int>(skillId) : -1;
}

int StubMaxShapeChoices(void*, void*) { return g_MaxShapeChoices; }

uintptr_t Descriptor(uint32_t skillId) {
    return SkillTableAddress() + static_cast<uintptr_t>(skillId) * kDescriptorStride;
}

void SetSkill(uint32_t slot, uint32_t id, uint8_t formula, uint8_t group,
              uint8_t effectType = 1, uint16_t cost = 5) {
    const uintptr_t rowId = CharaData() + 0xCC4 + static_cast<uintptr_t>(slot) * kSkillRowStride;
    *reinterpret_cast<uint32_t*>(rowId) = id;
    *reinterpret_cast<uint8_t*>(rowId + 5) = static_cast<uint8_t>(slot + 3);
    *reinterpret_cast<uint8_t*>(rowId + 6) = 0;
    *reinterpret_cast<uint8_t*>(rowId + 7) = 0;
    *reinterpret_cast<uint8_t*>(CharaData() + 0xCCE + static_cast<uintptr_t>(slot) * kSkillRowStride) = 1;

    const uintptr_t desc = Descriptor(id);
    std::memset(reinterpret_cast<void*>(desc), 0, kDescriptorStride);
    *reinterpret_cast<uint32_t*>(desc) = id;
    *reinterpret_cast<uint16_t*>(desc + 6) = cost;
    *reinterpret_cast<uint8_t*>(desc + 0x3E0) = effectType;
    *reinterpret_cast<uint8_t*>(desc + 0x3F2) = 0;
    *reinterpret_cast<uint8_t*>(desc + 0x3F3) = 0;
    *reinterpret_cast<uint8_t*>(desc + 0x3F6) = formula;
    *reinterpret_cast<uint8_t*>(desc + 0x435) = 3;
    *reinterpret_cast<uint8_t*>(desc + 0x436) = 0;
    *reinterpret_cast<uint16_t*>(desc + 0x438) = 8;
    *reinterpret_cast<uint8_t*>(desc + 0x441) = 0;
    *reinterpret_cast<uint8_t*>(desc + 0x43D) = 0;
    *reinterpret_cast<uint8_t*>(desc + 0x43E) = 0;
    *reinterpret_cast<uint8_t*>(desc + 0x43F) = 0;
    *reinterpret_cast<uint8_t*>(desc + 0x44C) = 0;
    *reinterpret_cast<uint8_t*>(desc + 0x44E) = group;
    *reinterpret_cast<uint16_t*>(desc + 0x464) = 0;
    *reinterpret_cast<int16_t*>(desc + 0x3E6) = 110;
    *reinterpret_cast<int16_t*>(desc + 0x420) = 100;
}

bool ResetWorld() {
    if (!EnsureImage()) return false;
    AutoPlay::g_Base = g_Image;
    std::memset(reinterpret_cast<void*>(g_Image + RVA::ADDR_BattleUnitArray), 0, 256 * 0x1260);
    std::memset(reinterpret_cast<void*>(CharaData()), 0, 0x4000);
    std::memset(reinterpret_cast<void*>(MenuTask()), 0, 0x5000);
    *reinterpret_cast<uintptr_t*>(g_Image + kSkillTablePointerRva) = SkillTableAddress();
    *reinterpret_cast<int32_t*>(g_Image + kSkillTableCountRva) = static_cast<int32_t>(kFakeSkillCount);
    *reinterpret_cast<uintptr_t*>(ActorUnit() + 0x240) = CharaData();
    *reinterpret_cast<int64_t*>(CharaData() + 0x2CD0) = 30;
    *reinterpret_cast<int64_t*>(CharaData() + 0x2CC8) = 100;
    *reinterpret_cast<int64_t*>(CharaData() + 0x2CD8) = 100;
    *reinterpret_cast<int64_t*>(CharaData() + 0x2CE0) = 50;
    *reinterpret_cast<uint8_t*>(ActorUnit() + 0x2E8) = 1;
    *reinterpret_cast<int16_t*>(ActorUnit() + 0x2EE) = -1;

    AutoPlay::NativeSkills::Initialize(g_Image);
    fnEnumerateSkills = &StubEnumerate;
    fnEffectiveCost = &StubCost;
    fnCanUseSkill = &StubCanUse;
    fnEffectiveRange = &StubRange;
    fnSkillIndex = &StubLookup;
    fnMaxShapeChoices = &StubMaxShapeChoices;
    g_EnumeratedRows.clear();
    std::fill(g_UsableById.begin(), g_UsableById.end(), 1);
    g_CostDelta = 0;
    g_EnumerateCalls = 0;
    g_LastCostSlot = -1;
    g_LastAreaPanels = 0;
    g_MaxShapeChoices = 4;
    return true;
}

CatalogJob FinishCatalog(int actor = kActor) {
    CatalogJob job = Begin(actor);
    while (!Step(job, 64)) {}
    return job;
}

bool TestCatalogIsIncrementalAndUsesNativeMetadata() {
    if (!ResetWorld()) return false;
    SetSkill(2, 321, 2, 1, 1, 7);
    SetSkill(4, 1401, 4, 0, 1, 9);
    g_EnumeratedRows = {2, 4};
    CatalogJob job = Begin(kActor);
    if (!Expect(!Step(job, 1) && job.nextRow == 1 && job.entries.size() == 1,
                "catalog candidate evaluation is bounded by maxSlots")) return false;
    if (!Expect(g_EnumerateCalls == 1, "engine learned-skill enumeration runs once per job")) return false;
    if (!Expect(Step(job, 1) && job.complete && job.entries.size() == 2,
                "second bounded step completes the learned catalog")) return false;
    const Entry& physical = job.entries[0];
    const Entry& heal = job.entries[1];
    return Expect(physical.learned && physical.currentlyUsable && physical.affordableFromSp &&
                  physical.slot == 2 && physical.skillId == 321 && physical.spCost == 7 &&
                  physical.range == 3 && physical.shapeId == 8 && physical.maxShapeChoices == 4 &&
                  physical.learnedRowLevelByte == 5 && g_LastAreaPanels == 1 &&
                  heal.healing && heal.formula == 4 && heal.skillId == 1401,
                  "catalog preserves native learned IDs, cost, range, shape, and heal classification");
}

bool TestCatalogRequiresSpAndNativeUsability() {
    if (!ResetWorld()) return false;
    SetSkill(0, 321, 2, 1, 1, 12);
    SetSkill(1, 322, 3, 0, 1, 8);
    g_EnumeratedRows = {0, 1};
    g_UsableById[322] = 0;
    *reinterpret_cast<int64_t*>(CharaData() + 0x2CD0) = 10;
    CatalogJob job = FinishCatalog();
    if (!Expect(job.entries.size() == 2, "structurally supported rows remain inspectable")) return false;
    if (!Expect(!job.entries[0].affordableFromSp && job.entries[0].currentlyUsable,
                "native HP-substitution usability cannot bypass the SP-only budget flag")) return false;
    return Expect(job.entries[1].affordableFromSp && !job.entries[1].currentlyUsable,
                  "engine-disabled skills remain nonselectable in the catalog");
}

bool TestCatalogRejectsUnknownOrSideEffectSkills() {
    if (!ResetWorld()) return false;
    SetSkill(0, 321, 2, 1);
    SetSkill(1, 322, 3, 1);
    SetSkill(2, 323, 4, 0);
    SetSkill(3, 324, 3, 10);
    SetSkill(4, 325, 2, 1);
    SetSkill(5, 326, 2, 1);
    SetSkill(6, 327, 2, 1);
    SetSkill(7, 328, 2, 1);
    SetSkill(8, 0x2BA4, 2, 1);
    SetSkill(9, 329, 2, 1);
    SetSkill(10, 330, 3, 1);
    *reinterpret_cast<uint8_t*>(Descriptor(322) + 0x3E1) = 23; // Extra effect.
    *reinterpret_cast<uint8_t*>(Descriptor(323) + 0x3F3) = 1; // Moves a unit.
    *reinterpret_cast<uint8_t*>(Descriptor(325) + 0x43D) = 2; // Blow distance.
    *reinterpret_cast<uint8_t*>(Descriptor(326) + 0x441) = 2; // Tower requirement.
    *reinterpret_cast<uint8_t*>(Descriptor(327) + 0x44C) = 1; // Partners.
    *reinterpret_cast<uint16_t*>(Descriptor(329) + 0x438) = 30000; // Custom target algorithm.
    *reinterpret_cast<uint8_t*>(Descriptor(330) + 0x436) = 2; // Unverified range mode.
    g_EnumeratedRows = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CatalogJob job = FinishCatalog();
    return Expect(job.entries.size() == 2 && job.entries[0].skillId == 321 &&
                  job.entries[1].skillId == 328,
                  "unsupported formula, group, side effect, movement, blow, tower, partner, and special ID are rejected");
}

void SetMenuRow(int menuIndex, int slot, uintptr_t descriptor, bool disabled) {
    const uintptr_t task = MenuTask();
    const uintptr_t row = task + kMenuRowsOffset + static_cast<uintptr_t>(menuIndex) * kMenuRowStride;
    const uintptr_t charaSkillRow = CharaData() + 0xCC0 + static_cast<uintptr_t>(slot) * kSkillRowStride;
    *reinterpret_cast<uintptr_t*>(row) = descriptor;
    *reinterpret_cast<uintptr_t*>(row + 8) = charaSkillRow;
    // Literal live row layout: disabled byte followed by nonzero metadata.
    *reinterpret_cast<uint32_t*>(row + 0x10) = 0x80808000u | (disabled ? 1u : 0u);
    *reinterpret_cast<int32_t*>(row + 0x18) = slot;
}

bool TestSkillMenuReadFindAndSelectRevalidateRow() {
    if (!ResetWorld()) return false;
    SetSkill(3, 401, 3, 0, 1, 4);
    SetSkill(5, 501, 4, 0, 1, 6);
    const uintptr_t task = MenuTask();
    *reinterpret_cast<uintptr_t*>(task + 0x4228) = CharaData();
    *reinterpret_cast<uintptr_t*>(task + 0x4220) = ActorUnit();
    *reinterpret_cast<int32_t*>(task + kMenuCountOffset) = 2;
    SetMenuRow(0, 3, Descriptor(401), false);
    SetMenuRow(1, 5, Descriptor(501), true);

    const std::vector<MenuRow> rows = ReadSkillList(task);
    if (!Expect(rows.size() == 2 && rows[0].safe && rows[0].selectable &&
                rows[1].safe && !rows[1].selectable,
                "live packed menu metadata does not disable usable skills; low disabled byte still rejects")) return false;
    if (!Expect(FindSelectableRow(task, 3, Descriptor(401)) == 0 &&
                FindSelectableRow(task, 5, Descriptor(501)) == -1,
                "menu row search ignores disabled skills")) return false;
    if (!Expect(SelectSkillCursor(task, 0, 3, Descriptor(401)) &&
                *reinterpret_cast<int32_t*>(task + kMenuCursorOffset) == 0,
                "cursor selection writes only the verified native menu cursor")) return false;
    if (!Expect(!SelectSkillCursor(task, 1, 5, Descriptor(501)),
                "disabled skill selection is rejected")) return false;
    *reinterpret_cast<uintptr_t*>(task + kMenuRowsOffset) = Descriptor(402);
    return Expect(!SelectSkillCursor(task, 0, 3, Descriptor(401)) &&
                  *reinterpret_cast<int32_t*>(task + kMenuCursorOffset) == 0,
                  "stale descriptor selection is rejected without changing the cursor");
}

bool TestMenuRejectsStaleCharacterRowAndBadCount() {
    if (!ResetWorld()) return false;
    SetSkill(2, 701, 2, 1);
    const uintptr_t task = MenuTask();
    *reinterpret_cast<uintptr_t*>(task + 0x4228) = CharaData();
    *reinterpret_cast<uintptr_t*>(task + 0x4220) = ActorUnit();
    *reinterpret_cast<int32_t*>(task + kMenuCountOffset) = 1;
    SetMenuRow(0, 2, Descriptor(701), false);
    auto rows = ReadSkillList(task);
    if (!Expect(rows.size() == 1 && rows[0].selectable,
                "current ordinary learned row is readable")) return false;
    *reinterpret_cast<uintptr_t*>(task + kMenuRowsOffset + 8) = CharaData() + 0xCC0 + 3 * kSkillRowStride;
    if (!Expect(ReadSkillList(task).empty(), "stale CharaData row pointer is rejected")) return false;
    *reinterpret_cast<int32_t*>(task + kMenuCountOffset) = 513;
    return Expect(ReadSkillList(task).empty(), "out-of-range native skill list count is rejected");
}

} // namespace

int main() {
    const bool ok = TestCatalogIsIncrementalAndUsesNativeMetadata() &&
        TestCatalogRequiresSpAndNativeUsability() &&
        TestCatalogRejectsUnknownOrSideEffectSkills() &&
        TestSkillMenuReadFindAndSelectRevalidateRow() &&
        TestMenuRejectsStaleCharacterRowAndBadCount();
    return ok ? 0 : 1;
}
