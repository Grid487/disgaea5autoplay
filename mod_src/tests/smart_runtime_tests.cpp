// Offline integration checks for the smart combat driver. The implementation
// is included so tests can call its internal state-machine helpers directly;
// all native game entry points are replaced with local stubs.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../xinput_proxy.cpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using namespace AutoPlay;

constexpr size_t kImageSize = 128ull * 1024ull * 1024ull;
constexpr uintptr_t kTaskOffset = 0x03000000;
constexpr uintptr_t kContextOffset = 0x07800000;
constexpr uintptr_t kSkillTableOffset = 0x00800000;
constexpr uintptr_t kShapeHeaderOffset = 0x01000000;
constexpr uintptr_t kShapeRecordOffset = 0x01001000;
constexpr uintptr_t kShapeCellOffset = 0x01002000;
constexpr uintptr_t kCharaOffset = 0x05000000;
constexpr uintptr_t kSkillId = 50;
constexpr int kActor = 16;
constexpr int kAlly = 17;
constexpr int kEnemy = 0;
constexpr int kAreaShape = 2;
constexpr int kHealCost = 10;

uintptr_t g_Image = 0;
int g_TargetX = -1;
int g_TargetZ = -1;
int g_NativePreviewCalls = 0;
int g_EffectCallbackCalls = 0;
int g_TargetEvaluationRejected = 0;
int g_BasicTargetValidationFailures = 0;
int g_BasicLegalCalls = 0;
int g_BasicLegalArgumentFailures = 0;
int g_BasicRangeValue = 1;
int g_EffectFailureTarget = -1;
double g_DamageValue = 100.0;
double g_HealingValue = 500.0;
bool g_UnitCanAct[256]{};
bool g_UnitCanMove[256]{};
bool g_ForceSkillTargets = false;
std::array<int, 8> g_ForcedTargets{};
int g_ForcedTargetCount = 0;

uintptr_t MainTask() { return g_Image + kTaskOffset; }
uintptr_t BattleContext() { return g_Image + kContextOffset; }
uintptr_t Unit(int index) { return GetUnitPtr(index); }
uintptr_t Chara(int index) { return g_Image + kCharaOffset + static_cast<uintptr_t>(index) * 0x4000; }
uintptr_t SkillTable() { return g_Image + kSkillTableOffset; }
uintptr_t Descriptor() { return SkillTable() + kSkillId * NativeSkills::kDescriptorStride; }
uintptr_t ActionMenu() { return MainTask() + 0x196C78; }
uintptr_t TargetTask() { return MainTask() + 0x24EDD0; }

bool Expect(bool condition, const char* label) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", label);
    else std::printf("PASS: %s\n", label);
    return condition;
}

bool OpenIsolatedTestLog() {
    if (AutoPlay::g_LogFile) return true;
    FILE* temporaryLog = nullptr;
    if (tmpfile_s(&temporaryLog) != 0 || !temporaryLog) {
        std::fprintf(stderr, "Cannot create an isolated offline-test log\n");
        return false;
    }
    AutoPlay::g_LogFile = temporaryLog;
    return true;
}

bool IsPlayerUnit(void* unit) {
    const uintptr_t cd = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(unit) + 0x240);
    return cd && *reinterpret_cast<uint8_t*>(cd + 0x2E32) == 0;
}

bool __fastcall StubControllable(void* unit, int) { return IsPlayerUnit(unit); }
int __fastcall StubFaction(void* unit, uint8_t) {
    const uintptr_t cd = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(unit) + 0x240);
    return cd ? *reinterpret_cast<uint8_t*>(cd + 0x2E32) : -1;
}
int __fastcall StubMapObject(void*) { return 0; }
bool __fastcall StubCanMove(void* unit) {
    const int index = *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(unit) + 0x2F4);
    return index >= 0 && index < 256 && g_UnitCanMove[index];
}
bool __fastcall StubHasAction(void* unit) {
    const int index = *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(unit) + 0x2F4);
    return index >= 0 && index < 256 && g_UnitCanAct[index];
}
bool __fastcall StubCanAttack(void*, int, uint8_t) { return true; }
float __fastcall StubTileHeight(float, float) { return 0.0f; }
int __fastcall StubGetBasicRange(void* unit) { return unit ? g_BasicRangeValue : 0; }
uint8_t __fastcall StubBasicTargetLegal(void* actor, void* target, int range,
                                        uint8_t occupancy, uint8_t mode) {
    ++g_BasicLegalCalls;
    if (!actor || !target || range < 1 || occupancy != 0 || mode != 0) {
        ++g_BasicLegalArgumentFailures;
        return 0;
    }
    int ax = 0, az = 0, tx = 0, tz = 0;
    GetUnitGridPos(reinterpret_cast<uintptr_t>(actor), ax, az);
    GetUnitGridPos(reinterpret_cast<uintptr_t>(target), tx, tz);
    return std::abs(ax - tx) + std::abs(az - tz) <= range ? 1 : 0;
}
uint8_t __fastcall StubBasicReach(void* actor, int fromX, int fromZ, void* target,
                                  int range, int, int) {
    if (!actor || !target || range < 1) return 0;
    int tx = 0, tz = 0;
    GetUnitGridPos(reinterpret_cast<uintptr_t>(target), tx, tz);
    return std::abs(fromX - tx) + std::abs(fromZ - tz) <= range ? 1 : 0;
}
uint8_t __fastcall StubLineWeapon(void*) { return 0; }
void __fastcall StubUpdateCursor(void*) {}

void InstallUnit(int index, int x, int z, uint8_t faction, int64_t hp, int64_t maxHp,
                 int64_t sp, int64_t maxSp, bool canAct, bool canMove) {
    const uintptr_t unit = Unit(index);
    const uintptr_t cd = Chara(index);
    std::memset(reinterpret_cast<void*>(unit), 0, 0x1260);
    std::memset(reinterpret_cast<void*>(cd), 0, 0x4000);
    *reinterpret_cast<float*>(unit + 0x08) = static_cast<float>(x * 12 + 6);
    *reinterpret_cast<float*>(unit + 0x10) = static_cast<float>(z * 12 + 6);
    *reinterpret_cast<uintptr_t*>(unit + 0x240) = cd;
    *reinterpret_cast<uint8_t*>(unit + 0x2E8) = 1;
    *reinterpret_cast<int16_t*>(unit + 0x2EE) = -1;
    *reinterpret_cast<int16_t*>(unit + 0x2F0) = -1;
    *reinterpret_cast<int16_t*>(unit + 0x2F4) = static_cast<int16_t>(index);
    *reinterpret_cast<uint8_t*>(cd + 0x2E32) = faction;
    *reinterpret_cast<int64_t*>(cd + 0x2CC8) = hp;
    *reinterpret_cast<int64_t*>(cd + 0x2CD8) = maxHp;
    *reinterpret_cast<int64_t*>(cd + 0x2CD0) = sp;
    *reinterpret_cast<int64_t*>(cd + 0x2CE0) = maxSp;
    g_UnitCanAct[index] = canAct;
    g_UnitCanMove[index] = canMove;
}

void InstallShapeTable() {
    auto* header = reinterpret_cast<NativeGeometry::ShapeTableHeader*>(g_Image + kShapeHeaderOffset);
    auto* records = reinterpret_cast<NativeGeometry::ShapeRecord*>(g_Image + kShapeRecordOffset);
    auto* cells = reinterpret_cast<NativeGeometry::ShapeCell*>(g_Image + kShapeCellOffset);
    std::memset(header, 0, sizeof(*header));
    std::memset(records, 0, sizeof(NativeGeometry::ShapeRecord) * 3);
    std::memset(cells, 0, sizeof(NativeGeometry::ShapeCell) * 3);
    *header = {3, 3};
    records[0] = {1, 0, 1, 0};
    records[1] = {kAreaShape, 1, 1, 0};
    records[2] = {3, 2, 1, 0};
    cells[0] = {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)};
    cells[1] = {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)};
    cells[2] = {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)};
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeHeaderPointerRva) =
        reinterpret_cast<uintptr_t>(header);
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeRecordsPointerRva) =
        reinterpret_cast<uintptr_t>(records);
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeCellsPointerRva) =
        reinterpret_cast<uintptr_t>(cells);
}

void InstallAoeShapeTable() {
    auto* header = reinterpret_cast<NativeGeometry::ShapeTableHeader*>(g_Image + kShapeHeaderOffset);
    auto* records = reinterpret_cast<NativeGeometry::ShapeRecord*>(g_Image + kShapeRecordOffset);
    auto* cells = reinterpret_cast<NativeGeometry::ShapeCell*>(g_Image + kShapeCellOffset);
    constexpr NativeGeometry::ShapeCell footprint[] = {
        {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {-1, -1, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {0, -1, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {1, -1, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {-1, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {1, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {-1, 1, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)},
        {0, 1, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)}
    };
    std::memset(header, 0, sizeof(*header));
    std::memset(records, 0, sizeof(NativeGeometry::ShapeRecord) * 3);
    std::memset(cells, 0, sizeof(NativeGeometry::ShapeCell) * 10);
    *header = {3, 10};
    records[0] = {1, 0, 1, 0};
    records[1] = {kAreaShape, 1, 8, 0};
    records[2] = {3, 9, 1, 0};
    cells[0] = {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)};
    std::memcpy(cells + 1, footprint, sizeof(footprint));
    cells[9] = {0, 0, static_cast<int8_t>(NativeGeometry::ShapeCellKind::Hit)};
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeHeaderPointerRva) =
        reinterpret_cast<uintptr_t>(header);
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeRecordsPointerRva) =
        reinterpret_cast<uintptr_t>(records);
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeCellsPointerRva) =
        reinterpret_cast<uintptr_t>(cells);
    NativeGeometry::ResetShapeCache();
}

void InstallSkill() {
    *reinterpret_cast<uintptr_t*>(g_Image + NativeSkills::kSkillTablePointerRva) = SkillTable();
    *reinterpret_cast<int32_t*>(g_Image + NativeSkills::kSkillTableCountRva) = 128;
    const uintptr_t d = Descriptor();
    std::memset(reinterpret_cast<void*>(d), 0, NativeSkills::kDescriptorStride);
    *reinterpret_cast<uint32_t*>(d) = static_cast<uint32_t>(kSkillId);
    *reinterpret_cast<uint16_t*>(d + 6) = kHealCost;
    *reinterpret_cast<uint8_t*>(d + 0x3E0) = 1;
    *reinterpret_cast<uint16_t*>(d + 0x3E6) = 100;
    *reinterpret_cast<uint8_t*>(d + 0x435) = 2;
    *reinterpret_cast<uint8_t*>(d + 0x436) = 0;
    *reinterpret_cast<uint16_t*>(d + 0x438) = 0; // Generic spell: use its saved area.
    *reinterpret_cast<uint8_t*>(d + 0x44E) = 0;
    *reinterpret_cast<uint8_t*>(d + 0x3F6) = 4; // Healing formula.
    *reinterpret_cast<uint8_t*>(d + 0x3F3) = 0;
    const uintptr_t cd = Chara(kActor);
    *reinterpret_cast<int16_t*>(cd + 0x2CC0) = 1;
    *reinterpret_cast<uint32_t*>(cd + 0xCC4) = static_cast<uint32_t>(kSkillId);
    *reinterpret_cast<uint8_t*>(cd + 0xCC9) = 0;
    *reinterpret_cast<uint8_t*>(cd + 0xCCB) = 0;
    *reinterpret_cast<uint8_t*>(cd + 0xCCE) = kAreaShape;
}

int __fastcall StubSkillIndex(const void*, uint32_t skillId) {
    return skillId < 128 ? static_cast<int>(skillId) : -1;
}
int __fastcall StubEnumerateSkills(void* charaData, uint32_t* outRows) {
    const int count = *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(charaData) + 0x2CC0);
    for (int i = 0; i < count && i < 8; ++i) outRows[i] = static_cast<uint32_t>(i);
    return count;
}
int64_t __fastcall StubSkillCost(void* descriptor, void*, void*, uint32_t, uint32_t) {
    return *reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(descriptor) + 6);
}
int __fastcall StubCanUseSkill(void* unit, void*, int64_t cost) {
    const uintptr_t cd = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(unit) + 0x240);
    return cd && *reinterpret_cast<int64_t*>(cd + 0x2CD0) >= cost;
}
uint8_t __fastcall StubSkillRange(void*, void* descriptor, void*) {
    return *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(descriptor) + 0x435);
}
int __fastcall StubMaxAreas(void*, void*) { return 9; }

void __fastcall StubInitPreview(void* queue) {
    std::memset(queue, 0, NativeCombat::kQueueStride);
}
void __fastcall StubSetActor(void* queue, void* unit, void* descriptor) {
    const int index = *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(unit) + 0x2F4);
    *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(queue) + NativeCombat::kQueueActorCount) = 1;
    *reinterpret_cast<int16_t*>(queue) = static_cast<int16_t>(index);
    *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(queue) + NativeCombat::kQueueSkillId) =
        descriptor ? static_cast<int32_t>(*reinterpret_cast<uint32_t*>(descriptor)) : 0;
    *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(queue) + 0x27678) =
        reinterpret_cast<uintptr_t>(descriptor);
}
void __fastcall StubSetLearned(void* queue, void* charaData, int slot, uint32_t) {
    const uintptr_t row = reinterpret_cast<uintptr_t>(charaData) + 0xCC4 + static_cast<uintptr_t>(slot) * 16;
    *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(queue) + 0x27680) =
        *reinterpret_cast<int8_t*>(row + 5);
    *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(queue) + 0x27682) =
        *reinterpret_cast<int8_t*>(row + 6);
}
void __fastcall StubSetDescriptor(void* queue, void*, uint32_t) {
    *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(queue) + NativeCombat::kQueueSkillId) = 0;
    *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(queue) + 0x27678) = 0;
}
void __fastcall StubSetTarget(void*, float worldX, float worldZ) {
    g_TargetX = static_cast<int>(std::floor((worldX - 6.0f) / 12.0f));
    g_TargetZ = static_cast<int>(std::floor((worldZ - 6.0f) / 12.0f));
}
int UnitIndexFromPointer(uintptr_t unitPointer) {
    for (int index = 0; index < 256; ++index)
        if (Unit(index) == unitPointer) return index;
    return -1;
}

uint32_t __fastcall StubEvaluateTargets(void* queue, uint8_t, uintptr_t selectedBasicTarget) {
    ++g_NativePreviewCalls;
    if (g_TargetEvaluationRejected) return 1;
    const uintptr_t q = reinterpret_cast<uintptr_t>(queue);
    const uintptr_t descriptor = *reinterpret_cast<uintptr_t*>(q + 0x27678);
    if (!descriptor) {
        const int actor = *reinterpret_cast<int16_t*>(q);
        const uintptr_t actorUnit = Unit(actor);
        const int selectedIndex = UnitIndexFromPointer(selectedBasicTarget);
        const uintptr_t selectedUnit = selectedIndex >= 0 ? Unit(selectedIndex) : 0;
        int selectedX = -1, selectedZ = -1;
        if (selectedUnit) GetUnitGridPos(selectedUnit, selectedX, selectedZ);
        const int expectedRange = StubGetBasicRange(reinterpret_cast<void*>(actorUnit));
        const int expectedLineWeapon = StubLineWeapon(reinterpret_cast<void*>(actorUnit));
        if (!selectedBasicTarget || selectedUnit == actorUnit || selectedX != g_TargetX || selectedZ != g_TargetZ ||
            *reinterpret_cast<int32_t*>(q + 0x27698) != expectedRange ||
            *reinterpret_cast<int32_t*>(q + 0x2769C) != expectedLineWeapon) {
            ++g_BasicTargetValidationFailures;
            return 1;
        }
    } else if (selectedBasicTarget) {
        ++g_BasicTargetValidationFailures;
        return 1;
    }
    if (g_ForceSkillTargets && descriptor &&
        g_ForcedTargetCount > 0) {
        for (int i = 0; i < g_ForcedTargetCount; ++i)
            *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargets +
                i * NativeCombat::kQueueTargetStride) = static_cast<int16_t>(g_ForcedTargets[i]);
        *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargetCount) =
            static_cast<int16_t>(g_ForcedTargetCount);
        return 0;
    }
    int count = 0;
    for (int index = 0; index < 256; ++index) {
        const uintptr_t unit = Unit(index);
        if (!*reinterpret_cast<uint8_t*>(unit + 0x2E8)) continue;
        const uintptr_t cd = *reinterpret_cast<uintptr_t*>(unit + 0x240);
        if (!cd || *reinterpret_cast<int64_t*>(cd + 0x2CC8) <= 0) continue;
        int x = 0, z = 0;
        GetUnitGridPos(unit, x, z);
        if (x != g_TargetX || z != g_TargetZ) continue;
        *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargets +
            count * NativeCombat::kQueueTargetStride) = static_cast<int16_t>(index);
        ++count;
    }
    *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargetCount) =
        static_cast<int16_t>(count);
    return 0;
}
bool StubEvaluateEffect(uintptr_t queue, int, int targetOrdinal, bool healing,
                        Combat::EffectEstimate& out) {
    ++g_EffectCallbackCalls;
    const int target = *reinterpret_cast<int16_t*>(queue + NativeCombat::kQueueTargets +
        targetOrdinal * NativeCombat::kQueueTargetStride);
    out = {};
    out.targetIndex = target;
    if (healing) out.expectedHealing = g_HealingValue;
    else out.expectedDamage = g_DamageValue;
    return target != g_EffectFailureTarget;
}

void InstallNativeStubs() {
    fnIsControllableUnit = &StubControllable;
    fnIsMapObject = &StubMapObject;
    fnGetUnitFaction = &StubFaction;
    fnCanUnitMove = &StubCanMove;
    fnHasActionRemaining = &StubHasAction;
    fnCanUnitAttack = &StubCanAttack;
    fnGetTileHeight = &StubTileHeight;
    fnUpdateCursorHover = &StubUpdateCursor;

    NativeSkills::fnSkillIndex = &StubSkillIndex;
    NativeSkills::fnEnumerateSkills = &StubEnumerateSkills;
    NativeSkills::fnEffectiveCost = &StubSkillCost;
    NativeSkills::fnCanUseSkill = &StubCanUseSkill;
    NativeSkills::fnEffectiveRange = &StubSkillRange;
    NativeSkills::fnMaxShapeChoices = &StubMaxAreas;

    NativeActions::initializeQueue = &StubInitPreview;
    NativeActions::setActor = &StubSetActor;
    NativeActions::setLearned = &StubSetLearned;
    NativeActions::setDescriptor = &StubSetDescriptor;
    NativeActions::setTarget = &StubSetTarget;
    NativeActions::evaluateTargets = &StubEvaluateTargets;
    NativeActions::getBasicRange = &StubGetBasicRange;
    NativeActions::basicTargetLegal = &StubBasicTargetLegal;
    NativeActions::basicReachFrom = &StubBasicReach;
    NativeActions::isLineWeapon = &StubLineWeapon;
    NativeActions::evaluateEffect = &StubEvaluateEffect;
}

bool ResetWorld(int64_t allyHp = 200, int64_t actorSp = 20) {
    if (!g_Image) {
        void* image = VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!image) {
            std::fprintf(stderr, "VirtualAlloc smart runtime image failed (%lu)\n", GetLastError());
            return false;
        }
        g_Image = reinterpret_cast<uintptr_t>(image);
    }
    AutoPlay::g_Base = g_Image;
    std::memset(reinterpret_cast<void*>(g_Image + RVA::ADDR_BattleUnitArray), 0, 256 * 0x1260);
    std::memset(reinterpret_cast<void*>(MainTask()), 0, 0x00280000);
    std::memset(reinterpret_cast<void*>(BattleContext()), 0, 0x10000);
    std::memset(reinterpret_cast<void*>(NativeCombat::QueueEntry(0)), 0, NativeCombat::kQueueStride);
    std::memset(reinterpret_cast<void*>(SkillTable()), 0, 128 * NativeSkills::kDescriptorStride);
    std::memset(reinterpret_cast<void*>(g_Image + kShapeHeaderOffset), 0, 0x3000);
    std::memset(g_UnitCanAct, 0, sizeof(g_UnitCanAct));
    std::memset(g_UnitCanMove, 0, sizeof(g_UnitCanMove));
    g_TargetX = g_TargetZ = -1;
    g_NativePreviewCalls = 0;
    g_EffectCallbackCalls = 0;
    g_TargetEvaluationRejected = 0;
    g_BasicTargetValidationFailures = 0;
    g_BasicLegalCalls = 0;
    g_BasicLegalArgumentFailures = 0;
    g_BasicRangeValue = 1;
    g_EffectFailureTarget = -1;
    g_DamageValue = 100.0;
    g_HealingValue = 500.0;
    g_ForceSkillTargets = false;
    g_ForcedTargets.fill(-1);
    g_ForcedTargetCount = 0;

    InstallUnit(kActor, 10, 10, 0, 1000, 1000, actorSp, 100, true, false);
    InstallUnit(kAlly, 11, 10, 0, allyHp, 1000, 50, 100, false, false);
    InstallUnit(kEnemy, 14, 10, 3, 500, 1000, 50, 100, false, false);
    InstallSkill();
    InstallShapeTable();
    InstallNativeStubs();
    *reinterpret_cast<int32_t*>(BattleContext() + 0xB8) = kActor;
    *reinterpret_cast<int32_t*>(BattleContext() + 0x950) = 0;

    ResetAutoPlayTurnTracking();
    NativeGeometry::ResetShapeCache();
    g_SkillTableReady = false;
    g_LastBattleState = -1;
    g_LastSubState = -1;
    g_StateStuckTicks = 0;
    g_CooldownTicks = 0;
    g_ActionAttemptTicks = 0;
    g_PulseAction = -1;
    g_HoldAction = -1;
    g_CombatUiPhase = -1;
    g_CombatUiFrames = 0;
    g_Enabled = true;
    return true;
}

bool TestMissingSkillTableKeepsBasicBattleAutomationWorking() {
    if (!ResetWorld()) return false;
    *reinterpret_cast<uintptr_t*>(g_Image + NativeGeometry::kShapeHeaderPointerRva) = 0;
    *reinterpret_cast<uint8_t*>(g_Image + RVA::ADDR_MapGrid + 0x1740) = 0;
    *reinterpret_cast<int32_t*>(MainTask() + 0x14) = 0x02;
    for (int frame = 0; frame < 2; ++frame) StepBattleDecision(MainTask(), BattleContext());
    if (!Expect(!SmartCombatAvailable() && g_LastSubState == 0 && g_PulseAction == ACT_CONFIRM,
                "missing skill metadata still selects a player unit from the map")) return false;

    *reinterpret_cast<int32_t*>(MainTask() + 0x14) = 0x03;
    *reinterpret_cast<int32_t*>(BattleContext() + 0x94C) = 1;
    *reinterpret_cast<uint16_t*>(BattleContext() + 0x74C) = kEnemy;
    g_CooldownTicks = 0;
    g_PulseAction = -1;
    for (int frame = 0; frame < 3; ++frame) StepBattleDecision(MainTask(), BattleContext());
    if (!Expect(g_PulseAction == ACT_CONFIRM &&
                *reinterpret_cast<int32_t*>(ActionMenu() + 0x128) == 1,
                "missing skill metadata still chooses the basic Attack command")) return false;

    *reinterpret_cast<int32_t*>(MainTask() + 0x14) = 0x08;
    g_CooldownTicks = 0;
    g_PulseAction = -1;
    for (int frame = 0; frame < 3; ++frame) StepBattleDecision(MainTask(), BattleContext());
    if (!Expect(g_PulseAction == ACT_CONFIRM && g_UnitDoneThisTurn[kActor],
                "missing skill metadata still confirms the basic attack target")) return false;

    *reinterpret_cast<int32_t*>(MainTask() + 0x14) = 0x10;
    g_CooldownTicks = 0;
    g_PulseAction = -1;
    StepBattleDecision(MainTask(), BattleContext());
    if (!Expect(g_PulseAction == ACT_CANCEL,
                "missing skill metadata cancels skill tile targeting without a basic-list confirmation")) return false;

    InstallShapeTable();
    g_CooldownTicks = 0;
    StepBattleDecision(MainTask(), BattleContext());
    return Expect(SmartCombatAvailable(), "smart combat recovers when the skill table becomes available");
}

NativeSkills::Entry TestSkill() {
    NativeSkills::Entry skill{};
    skill.slot = 0;
    skill.skillId = static_cast<int>(kSkillId);
    skill.descriptor = Descriptor();
    skill.spCost = kHealCost;
    skill.range = 2;
    skill.rangeFlag = 0;
    skill.shapeId = 0;
    skill.formula = 4;
    skill.skillGroup = 0;
    skill.maxShapeChoices = 9;
    skill.learned = true;
    skill.currentlyUsable = true;
    skill.affordableFromSp = true;
    skill.healing = true;
    return skill;
}

Combat::ActionCandidate TestHealCandidate() {
    Combat::ActionCandidate candidate{};
    candidate.kind = Combat::ActionKind::HealSkill;
    candidate.actorIndex = kActor;
    candidate.skillId = static_cast<int>(kSkillId);
    candidate.menuIndex = 0;
    candidate.targetIndex = kAlly;
    candidate.shapeId = kAreaShape;
    candidate.targetX = 11;
    candidate.targetZ = 10;
    candidate.spCost = kHealCost;
    candidate.legal = candidate.verified = true;
    candidate.effects.push_back({kAlly, 0.0, g_HealingValue});
    return candidate;
}

void InstallSkillCatalog() {
    auto& job = TeamPlanning::current;
    job.active = true;
    job.actors = {kActor};
    NativeSkills::CatalogJob catalog{};
    catalog.actorIndex = kActor;
    catalog.battleUnit = Unit(kActor);
    catalog.charaData = Chara(kActor);
    catalog.complete = true;
    catalog.entries.push_back(TestSkill());
    job.catalogs = {catalog};
    job.fingerprint = TeamPlanning::Fingerprint(BattleContext());
}

Combat::PendingAction MakePendingHeal(Combat::DecisionReason reason = Combat::DecisionReason::EmergencyHeal) {
    Combat::PendingAction pending{};
    pending.valid = true;
    pending.candidate = TestHealCandidate();
    pending.reason = reason;
    pending.effectiveHealing = g_HealingValue;
    return pending;
}

bool StartPendingHeal(Combat::DecisionReason reason = Combat::DecisionReason::EmergencyHeal) {
    InstallSkillCatalog();
    return SmartCombat::Begin(MakePendingHeal(reason), BattleContext());
}

void SetupSkillMenu(int inner) {
    const uintptr_t menu = ActionMenu();
    const uintptr_t row = menu + NativeSkills::kMenuRowsOffset;
    *reinterpret_cast<int32_t*>(menu + 0x14) = inner;
    *reinterpret_cast<int32_t*>(menu + 0x2C) = 0;
    *reinterpret_cast<int32_t*>(menu + NativeSkills::kMenuCountOffset) = 1;
    *reinterpret_cast<int32_t*>(menu + NativeSkills::kMenuCursorOffset) = 0;
    *reinterpret_cast<uintptr_t*>(menu + 0x4228) = Chara(kActor);
    *reinterpret_cast<uintptr_t*>(menu + 0x4220) = Unit(kActor);
    *reinterpret_cast<uintptr_t*>(row) = Descriptor();
    *reinterpret_cast<uintptr_t*>(row + 8) = Chara(kActor) + 0xCC0;
    *reinterpret_cast<uint32_t*>(row + 0x10) = 0x80808000u;
    *reinterpret_cast<uint32_t*>(row + 0x18) = 0;
}

void ResetDriverFrames() {
    g_CombatUiPhase = -1;
    g_CombatUiFrames = 0;
    g_PulseAction = -1;
    g_CooldownTicks = 0;
}

void InstallNativeTargetTask(const Combat::ActionCandidate& action, int target = kAlly,
                             uint32_t firstFlags = 0, uint8_t areaFlag = 0) {
    const uintptr_t task = TargetTask();
    *reinterpret_cast<uint32_t*>(task + 8) = firstFlags;
    *reinterpret_cast<uint8_t*>(task + 0x27829) = areaFlag;
    uintptr_t q = task + 0xC0;
    std::memset(reinterpret_cast<void*>(q), 0, NativeCombat::kQueueStride);
    *reinterpret_cast<int16_t*>(q) = static_cast<int16_t>(action.actorIndex);
    *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueActorCount) = 1;
    *reinterpret_cast<int32_t*>(q + NativeCombat::kQueueSkillId) = action.skillId;
    *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargetCount) = 1;
    *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargets) = static_cast<int16_t>(target);
}

void SetGenericIdentity(int selectedArea) {
    *reinterpret_cast<int32_t*>(BattleContext() + 0x9000) = 0;
    *reinterpret_cast<uintptr_t*>(BattleContext() + 0x9008) = Descriptor();
    *reinterpret_cast<int32_t*>(BattleContext() + 0x9014) = selectedArea;
}

bool TestState03ChoosesSpecialOption() {
    if (!ResetWorld() || !StartPendingHeal()) return false;
    ResetDriverFrames();
    bool held = DrivePlannedAction(MainTask(), BattleContext(), 0x03);
    if (!Expect(held && g_PulseAction == -1, "state 0x03 waits one frame before opening the special menu choice")) return false;
    held = DrivePlannedAction(MainTask(), BattleContext(), 0x03);
    return Expect(held && *reinterpret_cast<int32_t*>(ActionMenu() + 0x128) == 2 &&
                  g_PulseAction == ACT_CONFIRM,
                  "state 0x03 selects menu option 2 for a planned skill");
}

bool TestState0ESelectsNativeIdentityAndPreservesConfiguredArea() {
    if (!ResetWorld() || !StartPendingHeal()) return false;
    const int configuredArea = *reinterpret_cast<uint8_t*>(Chara(kActor) + 0xCCE);
    SetupSkillMenu(1);
    ResetDriverFrames();
    DrivePlannedAction(MainTask(), BattleContext(), 0x0E);
    DrivePlannedAction(MainTask(), BattleContext(), 0x0E);
    DrivePlannedAction(MainTask(), BattleContext(), 0x0E);
    if (!Expect(*reinterpret_cast<int32_t*>(ActionMenu() + NativeSkills::kMenuCursorOffset) == 0 &&
                g_PulseAction == ACT_CONFIRM,
                "state 0x0E inner 1 selects the verified learned-skill row by native identity")) return false;

    SetGenericIdentity(0);
    SetupSkillMenu(2);
    ResetDriverFrames();
    DrivePlannedAction(MainTask(), BattleContext(), 0x0E);
    DrivePlannedAction(MainTask(), BattleContext(), 0x0E);
    DrivePlannedAction(MainTask(), BattleContext(), 0x0E);
    if (!Expect(*reinterpret_cast<int32_t*>(BattleContext() + 0x9014) == kAreaShape - 1 &&
                g_PulseAction == ACT_CONFIRM,
                "state 0x0E inner 2 selects the saved area in native zero-based selector form")) return false;
    return Expect(*reinterpret_cast<uint8_t*>(Chara(kActor) + 0xCCE) == configuredArea,
                  "area selection leaves the learned skill configuration byte unchanged");
}

bool EnterState10AndDrive(Combat::ActionCandidate action, uint32_t firstFlags = 0,
                          uint8_t areaFlag = 0, int target = kAlly) {
    SetGenericIdentity(action.shapeId); // The native inner-2 confirm increments 0-based area to ID.
    InstallNativeTargetTask(action, target, firstFlags, areaFlag);
    ResetDriverFrames();
    bool held = true;
    for (int i = 0; i < 3 && held; ++i)
        held = DrivePlannedAction(MainTask(), BattleContext(), 0x10);
    return held;
}

bool TestState10ValidatesAffectedSetAndDefersAcceptance() {
    if (!ResetWorld() || !StartPendingHeal()) return false;
    const auto action = TestHealCandidate();
    if (!EnterState10AndDrive(action)) return false;
    if (!Expect(SmartCombat::pending.valid && SmartCombat::confirmationSent &&
                g_PulseAction == ACT_CONFIRM,
                "state 0x10 confirms after native identity, area, and affected target set match")) return false;
    if (!Expect(SmartCombat::accepted.empty() && NativeCombat::QueueCount(BattleContext()) == 0,
                "sending confirmation reserves nothing until the action appears in the battle queue")) return false;

    if (!ResetWorld() || !StartPendingHeal()) return false;
    if (!EnterState10AndDrive(TestHealCandidate(), 1, 0)) return false;
    if (!Expect(!SmartCombat::pending.valid && SmartCombat::accepted.empty() &&
                NativeCombat::QueueCount(BattleContext()) == 0 && g_PulseAction == ACT_CANCEL,
                "state 0x10 cancels when the native target task reports a rejection flag")) return false;

    if (!ResetWorld() || !StartPendingHeal()) return false;
    if (!EnterState10AndDrive(TestHealCandidate(), 0, 1)) return false;
    if (!Expect(!SmartCombat::pending.valid && SmartCombat::accepted.empty() &&
                NativeCombat::QueueCount(BattleContext()) == 0 && g_PulseAction == ACT_CANCEL,
                "state 0x10 cancels when the native area task sets its invalid-area flag")) return false;

    if (!ResetWorld() || !StartPendingHeal()) return false;
    if (!EnterState10AndDrive(TestHealCandidate(), 0, 0, kEnemy)) return false;
    return Expect(!SmartCombat::pending.valid && SmartCombat::accepted.empty() &&
                  NativeCombat::QueueCount(BattleContext()) == 0 && g_PulseAction == ACT_CANCEL,
                  "state 0x10 rejects an affected-unit set different from the planned target set");
}

bool TestRejectedAreaAndStaleSpCancelWithoutQueueOrReservation() {
    if (!ResetWorld() || !StartPendingHeal()) return false;
    g_TargetEvaluationRejected = 1;
    if (!EnterState10AndDrive(TestHealCandidate())) return false;
    if (!Expect(!SmartCombat::pending.valid && SmartCombat::accepted.empty() &&
                NativeCombat::QueueCount(BattleContext()) == 0 && g_PulseAction == ACT_CANCEL,
                "rejected refreshed area cancels without adding a queue action or reservation")) return false;

    if (!ResetWorld(200, 5) || !StartPendingHeal()) return false;
    ResetDriverFrames();
    DrivePlannedAction(MainTask(), BattleContext(), 0x03);
    return Expect(!SmartCombat::pending.valid && SmartCombat::accepted.empty() &&
                  NativeCombat::QueueCount(BattleContext()) == 0 && g_PulseAction == ACT_CANCEL,
                  "stale SP cancels before confirmation with no queue or pending reservation");
}

void InstallAcceptedQueue(const Combat::ActionCandidate& action) {
    const uintptr_t q = NativeCombat::QueueEntry(0);
    std::memset(reinterpret_cast<void*>(q), 0, NativeCombat::kQueueStride);
    *reinterpret_cast<int16_t*>(q) = static_cast<int16_t>(action.actorIndex);
    *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueActorCount) = 1;
    *reinterpret_cast<int32_t*>(q + NativeCombat::kQueueSkillId) = action.skillId;
    *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargetCount) =
        static_cast<int16_t>(action.effects.size());
    for (size_t i = 0; i < action.effects.size(); ++i)
        *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargets +
            i * NativeCombat::kQueueTargetStride) = static_cast<int16_t>(action.effects[i].targetIndex);
    *reinterpret_cast<int32_t*>(BattleContext() + 0x950) = 1;
}

void InstallExternalQueue(int actor, int skillId, uintptr_t descriptor, int64_t spCost,
                          const std::vector<int>& targets) {
    const uintptr_t q = NativeCombat::QueueEntry(0);
    std::memset(reinterpret_cast<void*>(q), 0, NativeCombat::kQueueStride);
    *reinterpret_cast<int16_t*>(q) = static_cast<int16_t>(actor);
    *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueActorCount) = 1;
    *reinterpret_cast<int32_t*>(q + NativeCombat::kQueueSkillId) = skillId;
    *reinterpret_cast<uintptr_t*>(q + 0x27678) = descriptor;
    *reinterpret_cast<int64_t*>(q + 0x276D0) = spCost;
    *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargetCount) =
        static_cast<int16_t>(targets.size());
    for (size_t i = 0; i < targets.size(); ++i)
        *reinterpret_cast<int16_t*>(q + NativeCombat::kQueueTargets +
            i * NativeCombat::kQueueTargetStride) = static_cast<int16_t>(targets[i]);
    *reinterpret_cast<int32_t*>(BattleContext() + 0x950) = 1;
}

void AddPlayerSkillRow(int actorIndex) {
    const uintptr_t cd = Chara(actorIndex);
    *reinterpret_cast<int16_t*>(cd + 0x2CC0) = 1;
    *reinterpret_cast<uint32_t*>(cd + 0xCC4) = static_cast<uint32_t>(kSkillId);
    *reinterpret_cast<uint8_t*>(cd + 0xCC9) = 0;
    *reinterpret_cast<uint8_t*>(cd + 0xCCB) = 0;
    *reinterpret_cast<uint8_t*>(cd + 0xCCE) = kAreaShape;
}

bool TestAcceptedEmergencyExecutesBeforePlanning() {
    if (!ResetWorld() || !StartPendingHeal()) return false;
    const auto action = TestHealCandidate();
    SmartCombat::ConfirmSent(BattleContext());
    if (!Expect(!SmartCombat::ObserveQueue(BattleContext()) && SmartCombat::accepted.empty() &&
                SmartCombat::pending.valid,
                "an unaccepted confirmation does not create a reservation")) return false;
    InstallAcceptedQueue(action);
    *reinterpret_cast<int32_t*>(MainTask() + 0x14) = 0x02;
    *reinterpret_cast<int32_t*>(MainTask() + 0x2C) = 0;
    *reinterpret_cast<int32_t*>(MainTask() + 0x11E0 + 0x14) = 0;
    g_LastBattleState = 0x02;
    g_LastSubState = 0;
    g_CooldownTicks = 0;
    StepBattleDecision(MainTask(), BattleContext());
    const uintptr_t mapCursorTask = MainTask() + 0x11E0;
    return Expect(SmartCombat::accepted.size() == 1 && !SmartCombat::pending.valid &&
                  *reinterpret_cast<int32_t*>(mapCursorTask + 0x100) == 1 &&
                  *reinterpret_cast<int32_t*>(mapCursorTask + 0x80) == 0x4E20 &&
                  g_PulseAction == ACT_EXEC_SHORT && !SmartCombat::executeEmergency &&
                  !TeamPlanning::current.active,
                  "accepted emergency healing executes with command 0x4E20 before another plan");
}

bool TestTwoRejectedConfirmationsFinishActor() {
    if (!ResetWorld(200, 5)) return false;
    if (!StartPendingHeal()) return false;
    ResetDriverFrames();
    DrivePlannedAction(MainTask(), BattleContext(), 0x03);
    if (!Expect(SmartCombat::failedAttempts[kActor] == 1 && !g_UnitDoneThisTurn[kActor],
                "first stale confirmation increments the failure count without ending the actor")) return false;
    if (!StartPendingHeal()) return false;
    ResetDriverFrames();
    DrivePlannedAction(MainTask(), BattleContext(), 0x03);
    return Expect(SmartCombat::failedAttempts[kActor] == 2 && g_UnitDoneThisTurn[kActor] &&
                  NativeCombat::QueueCount(BattleContext()) == 0,
                  "second rejected confirmation ends the actor and stops further retries");
}

bool RunTeamPlanning(bool injuredAlly, Combat::ActionKind expectedKind) {
    if (!ResetWorld(injuredAlly ? 200 : 800, 20)) return false;
    InstallUnit(kAlly, 11, 11, 0, injuredAlly ? 200 : 800, 1000, 50, 100, false, false);
    InstallUnit(kEnemy, 11, 10, 3, 500, 1000, 50, 100, false, false);
    TeamPlanning::Reset();
    int steps = 0;
    int previews = 0;
    while (!TeamPlanning::current.complete && steps++ < 2000) {
        const int before = g_NativePreviewCalls;
        TeamPlanning::Step(BattleContext());
        const int delta = g_NativePreviewCalls - before;
        if (delta > 1) return Expect(false, "team planning evaluates at most one native preview per frame");
        previews = std::max(previews, delta);
    }
    if (!Expect(TeamPlanning::current.complete, "team-planning job completes within a bounded number of steps")) return false;
    if (!Expect(previews <= 1 && g_NativePreviewCalls > 0,
                "team-planning native preview work is spread across frames")) return false;
    if (!Expect(TeamPlanning::current.best.valid &&
                TeamPlanning::current.best.candidate.kind == expectedKind,
                injuredAlly ? "emergency healing wins while the basic attack remains available" :
                              "healthy allies leave the basic attack selected and conserve SP")) return false;
    if (injuredAlly)
        return Expect(TeamPlanning::current.best.reason == Combat::DecisionReason::EmergencyHeal &&
                      TeamPlanning::current.best.candidate.spCost == kHealCost,
                      "the best team action identifies emergency healing and its SP cost");
    return Expect(TeamPlanning::current.best.candidate.spCost == 0.0,
                  "basic attack selection spends no SP when healing is not useful");
}

bool TestTeamPlanningIsBoundedAndBalancesHealAgainstBasic() {
    if (!RunTeamPlanning(true, Combat::ActionKind::HealSkill)) return false;
    return RunTeamPlanning(false, Combat::ActionKind::BasicAttack);
}

bool TestAoEEffectPreviewIsStagedAcrossFourTargetFrames() {
    if (!ResetWorld()) return false;
    InstallUnit(kActor, 12, 10, 0, 1000, 1000, 50, 100, true, false);
    InstallSkill();
    InstallAoeShapeTable();
    *reinterpret_cast<uint8_t*>(Descriptor() + 0x3F6) = 2;
    constexpr int positions[8][2] = {
        {14, 10}, {13, 9}, {14, 9}, {15, 9},
        {13, 10}, {15, 10}, {13, 11}, {14, 11}
    };
    for (int i = 0; i < 8; ++i) {
        InstallUnit(i, positions[i][0], positions[i][1], 3, 500, 500, 20, 100, false, false);
        g_ForcedTargets[static_cast<size_t>(i)] = i;
    }
    g_ForcedTargetCount = 8;
    g_ForceSkillTargets = true;

    const auto state = NativeCombat::CaptureSnapshot(BattleContext(), SmartCombat::accepted);
    auto skill = TestSkill();
    skill.healing = false;
    skill.formula = 2;
    NativeActions::Preview preview{};
    Combat::ActionCandidate candidate{};
    if (!Expect(NativeActions::BuildPreview(preview, state, kActor, &skill, kAreaShape,
            14, 10, 0, candidate, false),
            "AoE preview records its native target set before running effect estimates")) return false;
    if (!Expect(candidate.effects.size() == 8 && !candidate.verified && g_EffectCallbackCalls == 0,
                "calculateEffects=false leaves eight targets pending without invoking the estimator")) return false;

    TeamPlanning::Reset();
    auto& job = TeamPlanning::current;
    job.active = true;
    job.fingerprint = TeamPlanning::Fingerprint(BattleContext());
    job.state = state;
    job.preview = preview;
    job.evaluation = candidate;
    job.evaluating = true;
    job.evaluationTarget = 0;

    const int beforeFirst = g_EffectCallbackCalls;
    TeamPlanning::Step(BattleContext());
    const int firstFrame = g_EffectCallbackCalls - beforeFirst;
    if (!Expect(firstFrame == 4 && job.evaluating && job.evaluationTarget == 4 &&
                !job.evaluation.verified,
                "team planning evaluates at most four pending AoE targets in its first frame")) return false;
    const int beforeSecond = g_EffectCallbackCalls;
    TeamPlanning::Step(BattleContext());
    const int secondFrame = g_EffectCallbackCalls - beforeSecond;
    if (!Expect(secondFrame == 4 && !job.evaluating && job.evaluation.verified &&
                job.evaluation.effects.size() == 8,
                "the remaining AoE estimates finish next frame with all effects verified")) return false;
    return Expect(job.best.valid && job.best.candidate.verified &&
                  job.best.candidate.effects.size() == 8,
                  "the staged AoE estimates produce a verified team action");
}

bool TestQueuedReservationEffectsAreBatchedAcrossFrames() {
    if (!ResetWorld()) return false;
    constexpr int positions[8][2] = {
        {14, 10}, {13, 9}, {14, 9}, {15, 9},
        {13, 10}, {15, 10}, {13, 11}, {14, 11}
    };
    std::vector<int> targets;
    for (int i = 0; i < 8; ++i) {
        InstallUnit(i, positions[i][0], positions[i][1], 3, 500, 500, 20, 100, false, false);
        targets.push_back(i);
    }
    InstallExternalQueue(kActor, 0, 0, 0, targets);
    TeamPlanning::Reset();
    int maxCallbacksInFrame = 0;
    int steps = 0;
    while (TeamPlanning::current.externalQueue == 0 && steps++ < 10) {
        const int before = g_EffectCallbackCalls;
        TeamPlanning::Step(BattleContext());
        const int delta = g_EffectCallbackCalls - before;
        maxCallbacksInFrame = std::max(maxCallbacksInFrame, delta);
        if (delta > 4) return Expect(false, "queued reservations estimate no more than four targets per frame");
    }
    if (!Expect(steps >= 2 && maxCallbacksInFrame == 4 &&
                TeamPlanning::current.state.reservations.size() == 1 &&
                TeamPlanning::current.state.reservations.front().effects.size() == 8,
                "an eight-target queued action is projected in four-effect batches across frames")) return false;
    return Expect(Combat::ProjectedHp(TeamPlanning::current.state, 0) == 400.0 &&
                  g_EffectCallbackCalls == 8,
                  "the completed queued reservation retains all eight evaluated target effects");
}

bool TestQueuedLethalDamageAndHealingSuppressDuplicateTargets() {
    if (!ResetWorld()) return false;
    InstallUnit(18, 13, 10, 0, 1000, 1000, 20, 100, true, false);
    g_DamageValue = 500.0;
    InstallExternalQueue(kActor, 0, 0, 0, {kEnemy});
    TeamPlanning::Reset();
    int steps = 0;
    while (!TeamPlanning::current.complete && steps++ < 2000) {
        const int before = g_EffectCallbackCalls;
        TeamPlanning::Step(BattleContext());
        if (g_EffectCallbackCalls - before > 4)
            return Expect(false, "external queued damage and later candidates stay within the four-effect frame budget");
    }
    if (!Expect(TeamPlanning::current.complete &&
                Combat::ProjectedHp(TeamPlanning::current.state, kEnemy) == 0.0 &&
                !TeamPlanning::current.best.valid,
                "a queued lethal attack prevents another unit from selecting the same enemy")) return false;

    if (!ResetWorld(200, 20)) return false;
    InstallUnit(18, 13, 10, 0, 1000, 1000, 20, 100, true, false);
    AddPlayerSkillRow(18);
    g_HealingValue = 800.0;
    InstallExternalQueue(kActor, static_cast<int>(kSkillId), Descriptor(), kHealCost, {kAlly});
    TeamPlanning::Reset();
    steps = 0;
    while (!TeamPlanning::current.complete && steps++ < 2000) {
        const int before = g_EffectCallbackCalls;
        TeamPlanning::Step(BattleContext());
        if (g_EffectCallbackCalls - before > 4)
            return Expect(false, "external queued healing and later candidates stay within the four-effect frame budget");
    }
    return Expect(TeamPlanning::current.complete &&
                  Combat::ProjectedHp(TeamPlanning::current.state, kAlly) == 1000.0 &&
                  TeamPlanning::current.best.valid &&
                  TeamPlanning::current.best.candidate.kind == Combat::ActionKind::BasicAttack,
                  "a queued heal fills the ally and prevents another unit from choosing duplicate healing");
}

bool TestBasicAttackUsesNativeRangeAndTargetContracts() {
    if (!ResetWorld()) return false;
    InstallUnit(kAlly, 9, 10, 0, 800, 1000, 50, 100, false, false);
    InstallUnit(kEnemy, 11, 10, 3, 500, 1000, 50, 100, false, false);
    auto state = NativeCombat::CaptureSnapshot(BattleContext(), SmartCombat::accepted);
    NativeActions::Preview preview{};
    Combat::ActionCandidate candidate{};
    if (!Expect(NativeActions::BuildPreview(preview, state, kActor, nullptr, 1,
            11, 10, 0, candidate, false),
            "a basic attack at native range one accepts an adjacent enemy with occupancy flags zero")) return false;
    if (!Expect(g_BasicLegalCalls == 1 && g_BasicLegalArgumentFailures == 0 &&
                g_BasicTargetValidationFailures == 0 && candidate.effects.size() == 1,
                "basic targeting passes occupancy arguments 0,0 and the selected unit pointer with exact range metadata")) return false;

    InstallUnit(kEnemy, 12, 10, 3, 500, 1000, 50, 100, false, false);
    state = NativeCombat::CaptureSnapshot(BattleContext(), SmartCombat::accepted);
    if (!Expect(!NativeActions::BuildPreview(preview, state, kActor, nullptr, 1,
            12, 10, 0, candidate, false) && g_BasicLegalCalls == 2,
            "basic targeting rejects a target beyond native range one")) return false;

    const uintptr_t q = reinterpret_cast<uintptr_t>(preview.bytes.data());
    StubInitPreview(reinterpret_cast<void*>(q));
    StubSetActor(reinterpret_cast<void*>(q), reinterpret_cast<void*>(Unit(kActor)), nullptr);
    StubSetDescriptor(reinterpret_cast<void*>(q), nullptr, 1);
    StubSetTarget(reinterpret_cast<void*>(q), 11 * 12.f + 6.f, 10 * 12.f + 6.f);
    *reinterpret_cast<int32_t*>(q + 0x27698) = 2; // Deliberately disagree with range one.
    *reinterpret_cast<int32_t*>(q + 0x2769C) = 0;
    if (!Expect(StubEvaluateTargets(reinterpret_cast<void*>(q), 0, Unit(kEnemy)) != 0,
                "the target stub rejects incorrect queue range metadata")) return false;

    *reinterpret_cast<int32_t*>(q + 0x27698) = 1;
    return Expect(StubEvaluateTargets(reinterpret_cast<void*>(q), 0, Unit(kActor)) != 0 &&
                  g_BasicTargetValidationFailures == 2,
                  "the target stub rejects a mismatched basic target pointer instead of accepting a fake candidate");
}

bool TestUnmodeledQueuedEffectRequiresExecutionResolution() {
    if (!ResetWorld()) return false;
    InstallExternalQueue(kActor, 0, 0, 0, {kEnemy});
    g_EffectFailureTarget = kEnemy;
    TeamPlanning::Reset();
    const bool finished = TeamPlanning::Step(BattleContext());
    return Expect(finished && TeamPlanning::current.complete &&
                  TeamPlanning::current.requiresQueueExecution &&
                  TeamPlanning::current.state.reservations.empty() &&
                  !TeamPlanning::current.best.valid,
                  "an unevaluable queued effect stops planning and requests native queue execution resolution");
}

} // namespace

int main() {
    if (!g_Image) {
        void* image = VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!image) {
            std::fprintf(stderr, "VirtualAlloc smart runtime image failed (%lu)\n", GetLastError());
            return 1;
        }
        g_Image = reinterpret_cast<uintptr_t>(image);
        AutoPlay::g_Base = g_Image;
    }
    if (!OpenIsolatedTestLog()) return 1;
    const bool passed =
        TestMissingSkillTableKeepsBasicBattleAutomationWorking() &&
        TestState03ChoosesSpecialOption() &&
        TestState0ESelectsNativeIdentityAndPreservesConfiguredArea() &&
        TestState10ValidatesAffectedSetAndDefersAcceptance() &&
        TestRejectedAreaAndStaleSpCancelWithoutQueueOrReservation() &&
        TestAcceptedEmergencyExecutesBeforePlanning() &&
        TestTwoRejectedConfirmationsFinishActor() &&
        TestTeamPlanningIsBoundedAndBalancesHealAgainstBasic() &&
        TestAoEEffectPreviewIsStagedAcrossFourTargetFrames() &&
        TestQueuedReservationEffectsAreBatchedAcrossFrames() &&
        TestQueuedLethalDamageAndHealingSuppressDuplicateTargets() &&
        TestBasicAttackUsesNativeRangeAndTargetContracts() &&
        TestUnmodeledQueuedEffectRequiresExecutionResolution();
    if (AutoPlay::g_LogFile) {
        std::fclose(AutoPlay::g_LogFile);
        AutoPlay::g_LogFile = nullptr;
    }
    if (g_Image) {
        VirtualFree(reinterpret_cast<void*>(g_Image), 0, MEM_RELEASE);
        g_Image = 0;
        AutoPlay::g_Base = 0;
    }
    return passed ? 0 : 1;
}
