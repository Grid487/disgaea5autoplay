#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../native_effects.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using namespace AutoPlay;
using namespace AutoPlay::NativeEffects;

constexpr size_t kImageSize = 128ull * 1024ull * 1024ull;
constexpr uintptr_t kActorCharaOffset = 0x06000000;
constexpr uintptr_t kTargetCharaOffset = 0x06100000;
constexpr uintptr_t kDescriptorOffset = 0x06200000;
constexpr uintptr_t kQueueOffset = 0x07000000;
constexpr int kActorIndex = 7;
constexpr int kTargetIndex = 9;

uintptr_t g_Image = 0;
int g_Accuracy = 80;
int g_ResistancePercent = 100;
bool g_RefreshNeeded = false;
bool g_NegativeSign = false;
int g_AdjustmentCalls = 0;

uintptr_t ActorChara() { return g_Image + kActorCharaOffset; }
uintptr_t TargetChara() { return g_Image + kTargetCharaOffset; }
uintptr_t Descriptor() { return g_Image + kDescriptorOffset; }
uintptr_t Queue() { return g_Image + kQueueOffset; }
uintptr_t Unit(int index) { return GetUnitPtr(index); }

bool Expect(bool condition, const char* label) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", label);
    else std::printf("PASS: %s\n", label);
    return condition;
}

uint32_t StubSelector(void*, void* descriptor, uint8_t) {
    if (!descriptor) return 1;
    switch (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(descriptor) + 0x3F6)) {
    case 2: return 1;
    case 3: return 3;
    case 4: return 4;
    default: return 11;
    }
}

int64_t StatAt(uintptr_t chara, int selector) {
    switch (selector) {
    case 0: case 1: return *reinterpret_cast<int64_t*>(chara + 0x2CE8);
    case 2: return *reinterpret_cast<int64_t*>(chara + 0x2CF0);
    case 3: return *reinterpret_cast<int64_t*>(chara + 0x2CF8);
    case 4: return *reinterpret_cast<int64_t*>(chara + 0x2D00);
    case 5: return *reinterpret_cast<int64_t*>(chara + 0x2D08);
    default: return 0;
    }
}

int64_t __fastcall StubGetStat(void* chara, int selector) {
    return StatAt(reinterpret_cast<uintptr_t>(chara), selector);
}

int64_t __fastcall StubGetDefenseStat(void* chara, int selector, int overrideIndex) {
    (void)overrideIndex;
    if (selector == 3 || selector == 4) return *reinterpret_cast<int64_t*>(reinterpret_cast<uintptr_t>(chara) + 0x2D00);
    return *reinterpret_cast<int64_t*>(reinterpret_cast<uintptr_t>(chara) + 0x2CF0);
}

int __fastcall StubDefenseOverride(void*, void*, void*) { return -1; }

int __fastcall StubEffectPower(void* descriptor, int rank, int effectSlot) {
    const auto d = reinterpret_cast<uintptr_t>(descriptor);
    return *reinterpret_cast<int16_t*>(d + 0x3E6 + effectSlot * 2) - 100 +
        *reinterpret_cast<int16_t*>(d + 0x420 + rank * 2);
}

int32_t __fastcall StubSkillBonus(void*, void*) { return 0; }
int32_t __fastcall StubCritical(void*, void*, int, int) { return 0; }
int64_t __fastcall StubScale(void*, void*, int) { return 100; }

uint64_t __fastcall StubAdjust(int64_t* magnitude, void*, void*, int, int mode) {
    ++g_AdjustmentCalls;
    if (mode != 1) return 0;
    *magnitude = *magnitude * g_ResistancePercent / 100;
    return 0;
}

uint8_t __fastcall StubSign(void*, void*, void*) { return g_NegativeSign ? 1 : 0; }
uint32_t __fastcall StubAccuracy(void*, void*, int) { return static_cast<uint32_t>(g_Accuracy); }
uint16_t __fastcall StubRefresh(void*) { return g_RefreshNeeded ? 1 : 0; }

void SetUnit(int index, uintptr_t chara) {
    const uintptr_t unit = Unit(index);
    *reinterpret_cast<uintptr_t*>(unit + 0x240) = chara;
}

void ResetQueue(bool skill, uint8_t formula) {
    const uintptr_t queue = Queue();
    std::memset(reinterpret_cast<void*>(queue), 0, NativeCombat::kQueueStride);
    *reinterpret_cast<int16_t*>(queue + NativeCombat::kQueueActorCount) = 1;
    *reinterpret_cast<int16_t*>(queue) = static_cast<int16_t>(kActorIndex);
    *reinterpret_cast<int16_t*>(queue + NativeCombat::kQueueTargetCount) = 1;
    *reinterpret_cast<int16_t*>(queue + NativeCombat::kQueueTargets) = static_cast<int16_t>(kTargetIndex);
    *reinterpret_cast<int32_t*>(queue + NativeCombat::kQueueSkillId) = skill ? 0x500 : 0;
    *reinterpret_cast<int16_t*>(queue + 0x27682) = 2;
    if (!skill) {
        *reinterpret_cast<uintptr_t*>(queue + 0x27678) = 0;
        return;
    }

    const uintptr_t descriptor = Descriptor();
    std::memset(reinterpret_cast<void*>(descriptor), 0, NativeSkills::kDescriptorStride);
    *reinterpret_cast<uint32_t*>(descriptor) = 0x500;
    *reinterpret_cast<uint8_t*>(descriptor + 0x3E0) = 1;
    *reinterpret_cast<uint8_t*>(descriptor + 0x3F6) = formula;
    *reinterpret_cast<uint16_t*>(descriptor + 0x3E6) = formula == 4 ? 120 : 50;
    *reinterpret_cast<uint16_t*>(descriptor + 0x424) = 10; // rank 2 additional power
    *reinterpret_cast<uint8_t*>(descriptor + 0x435) = 1;
    *reinterpret_cast<uint16_t*>(descriptor + 0x438) = 1;
    *reinterpret_cast<uint8_t*>(descriptor + 0x44E) = 0;
    *reinterpret_cast<uintptr_t*>(queue + 0x27678) = descriptor;
}

bool InstallFakeImage() {
    void* image = VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) {
        std::fprintf(stderr, "VirtualAlloc fake image failed (%lu)\n", GetLastError());
        return false;
    }
    g_Image = reinterpret_cast<uintptr_t>(image);
    AutoPlay::g_Base = g_Image;
    SetUnit(kActorIndex, ActorChara());
    SetUnit(kTargetIndex, TargetChara());
    *reinterpret_cast<int64_t*>(ActorChara() + 0x2CE8) = 100; // ATK
    *reinterpret_cast<int64_t*>(ActorChara() + 0x2CF0) = 50;  // DEF
    *reinterpret_cast<int64_t*>(ActorChara() + 0x2CF8) = 120; // INT
    *reinterpret_cast<int64_t*>(ActorChara() + 0x2D00) = 100; // RES
    *reinterpret_cast<int64_t*>(ActorChara() + 0x2D08) = 80;  // HIT
    *reinterpret_cast<int64_t*>(ActorChara() + 0x2D10) = 70;  // SPD
    *reinterpret_cast<int64_t*>(TargetChara() + 0x2CF0) = 50;
    *reinterpret_cast<int64_t*>(TargetChara() + 0x2D00) = 40;

    fnAttackSelector = &StubSelector;
    fnGetStat = &StubGetStat;
    fnGetDefenseStat = &StubGetDefenseStat;
    fnDefenseOverride = &StubDefenseOverride;
    fnEffectPower = &StubEffectPower;
    fnSkillBonus = &StubSkillBonus;
    fnCriticalAttack = &StubCritical;
    fnCriticalDefense = &StubCritical;
    fnExpectedScale = &StubScale;
    fnTargetAdjustment = &StubAdjust;
    fnDamageSign = &StubSign;
    fnAccuracy = &StubAccuracy;
    fnPreviewNeedsRefresh = &StubRefresh;
    return true;
}

} // namespace

int main() {
    if (!InstallFakeImage()) return 1;
    bool ok = true;
    Combat::EffectEstimate estimate{};

    ResetQueue(false, 0);
    g_Accuracy = 80;
    g_ResistancePercent = 100;
    g_AdjustmentCalls = 0;
    ok &= Expect(Evaluate(Queue(), 0, 0, false, estimate), "basic attack preview is supported");
    ok &= Expect(estimate.targetIndex == kTargetIndex && estimate.expectedDamage == 60.0,
        "basic damage includes physical defense and expected accuracy");
    ok &= Expect(g_AdjustmentCalls == 1, "native resistance adjustment is applied once");

    ResetQueue(true, 2);
    g_ResistancePercent = 80;
    ok &= Expect(Evaluate(Queue(), 0, 0, false, estimate), "single-effect learned damage skill is supported");
    ok &= Expect(std::abs(estimate.expectedDamage - 22.4) < 0.0001,
        "skill level power, elemental adjustment, and accuracy combine as expected");

    ResetQueue(true, 4);
    g_ResistancePercent = 80;
    g_NegativeSign = false;
    ok &= Expect(!Evaluate(Queue(), 0, 0, true, estimate), "healing to an invalid opposing target is rejected");
    g_NegativeSign = true;
    ok &= Expect(Evaluate(Queue(), 0, 0, true, estimate), "single-effect learned healing skill is supported");
    ok &= Expect(estimate.expectedHealing == 104.0 && estimate.expectedDamage == 0.0,
        "native negative heal scalar is flipped for allies before target adjustment");

    g_RefreshNeeded = true;
    ok &= Expect(!Evaluate(Queue(), 0, 0, true, estimate), "state-refreshing preview is rejected");
    g_RefreshNeeded = false;
    ResetQueue(true, 2);
    g_NegativeSign = false;
    ok &= Expect(Evaluate(Queue(), 0, 0, false, estimate), "normal hostile-target damage sign is accepted");
    g_NegativeSign = true;
    ok &= Expect(!Evaluate(Queue(), 0, 0, false, estimate), "reversed friendly-fire damage sign is rejected");
    g_NegativeSign = false;

    ResetQueue(true, 2);
    *reinterpret_cast<uint8_t*>(Descriptor() + 0x3E1) = 1;
    ok &= Expect(!Evaluate(Queue(), 0, 0, false, estimate), "secondary effect slots are rejected");
    ResetQueue(true, 2);
    ok &= Expect(!Evaluate(Queue(), 0, 1, false, estimate), "stale target ordinals are rejected");

    VirtualFree(reinterpret_cast<void*>(g_Image), 0, MEM_RELEASE);
    return ok ? 0 : 1;
}
