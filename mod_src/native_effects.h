#pragma once

#include "native_skills.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace AutoPlay::NativeEffects {

// These helpers are only called against the private preview built by
// NativeActions::BuildPreview. Never pass a live action-queue entry here.
constexpr uintptr_t kAttackSelectorRva = 0x1CE1E0;
constexpr uintptr_t kGetStatRva = 0x15D760;
constexpr uintptr_t kGetDefenseStatRva = 0x15D6F0;
constexpr uintptr_t kDefenseOverrideRva = 0x200310;
constexpr uintptr_t kEffectPowerRva = 0x1D7A20;
constexpr uintptr_t kSkillBonusRva = 0x1D7CA0;
constexpr uintptr_t kCriticalAttackRva = 0x1C1940;
constexpr uintptr_t kCriticalDefenseRva = 0x1C17E0;
constexpr uintptr_t kExpectedScaleRva = 0x1C1650;
constexpr uintptr_t kTargetAdjustmentRva = 0x1C0C60;
constexpr uintptr_t kDamageSignRva = 0x1C0840;
constexpr uintptr_t kAccuracyRva = 0x1C1EE0;
constexpr uintptr_t kPreviewNeedsRefreshRva = 0x2D8BA0;

using FnAttackSelector = uint32_t(__fastcall*)(void* actorChara, void* descriptor, uint8_t mode);
using FnGetStat = int64_t(__fastcall*)(void* charaData, int selector);
using FnGetDefenseStat = int64_t(__fastcall*)(void* charaData, int selector, int overrideIndex);
using FnDefenseOverride = int(__fastcall*)(void* actorChara, void* descriptor, void* targetChara);
using FnEffectPower = int(__fastcall*)(void* descriptor, int skillRank, int effectSlot);
using FnSkillBonus = int32_t(__fastcall*)(void* actorUnit, void* descriptor);
using FnCriticalModifier = int32_t(__fastcall*)(void* actorUnit, void* queue,
                                                 int targetOrdinal, int mode);
using FnExpectedScale = int64_t(__fastcall*)(void* actorUnit, void* queue, int targetOrdinal);
using FnTargetAdjustment = uint64_t(__fastcall*)(int64_t* magnitude, void* actorUnit,
                                                  void* queue, int targetOrdinal, int mode);
using FnDamageSign = uint8_t(__fastcall*)(void* actorUnit, void* targetUnit, void* queue);
using FnAccuracy = uint32_t(__fastcall*)(void* actorUnit, void* queue, int targetOrdinal);
using FnPreviewNeedsRefresh = uint16_t(__fastcall*)(void* queue);

inline FnAttackSelector fnAttackSelector = nullptr;
inline FnGetStat fnGetStat = nullptr;
inline FnGetDefenseStat fnGetDefenseStat = nullptr;
inline FnDefenseOverride fnDefenseOverride = nullptr;
inline FnEffectPower fnEffectPower = nullptr;
inline FnSkillBonus fnSkillBonus = nullptr;
inline FnCriticalModifier fnCriticalAttack = nullptr;
inline FnCriticalModifier fnCriticalDefense = nullptr;
inline FnExpectedScale fnExpectedScale = nullptr;
inline FnTargetAdjustment fnTargetAdjustment = nullptr;
inline FnDamageSign fnDamageSign = nullptr;
inline FnAccuracy fnAccuracy = nullptr;
inline FnPreviewNeedsRefresh fnPreviewNeedsRefresh = nullptr;

inline void Initialize(uintptr_t base) {
    AutoPlay::g_Base = base;
    fnAttackSelector = reinterpret_cast<FnAttackSelector>(base + kAttackSelectorRva);
    fnGetStat = reinterpret_cast<FnGetStat>(base + kGetStatRva);
    fnGetDefenseStat = reinterpret_cast<FnGetDefenseStat>(base + kGetDefenseStatRva);
    fnDefenseOverride = reinterpret_cast<FnDefenseOverride>(base + kDefenseOverrideRva);
    fnEffectPower = reinterpret_cast<FnEffectPower>(base + kEffectPowerRva);
    fnSkillBonus = reinterpret_cast<FnSkillBonus>(base + kSkillBonusRva);
    fnCriticalAttack = reinterpret_cast<FnCriticalModifier>(base + kCriticalAttackRva);
    fnCriticalDefense = reinterpret_cast<FnCriticalModifier>(base + kCriticalDefenseRva);
    fnExpectedScale = reinterpret_cast<FnExpectedScale>(base + kExpectedScaleRva);
    fnTargetAdjustment = reinterpret_cast<FnTargetAdjustment>(base + kTargetAdjustmentRva);
    fnDamageSign = reinterpret_cast<FnDamageSign>(base + kDamageSignRva);
    fnAccuracy = reinterpret_cast<FnAccuracy>(base + kAccuracyRva);
    fnPreviewNeedsRefresh = reinterpret_cast<FnPreviewNeedsRefresh>(base + kPreviewNeedsRefreshRva);
}

inline bool HasAllHelpers() {
    return fnAttackSelector && fnGetStat && fnGetDefenseStat && fnDefenseOverride &&
        fnEffectPower && fnSkillBonus && fnCriticalAttack && fnCriticalDefense &&
        fnExpectedScale && fnTargetAdjustment && fnDamageSign && fnAccuracy &&
        fnPreviewNeedsRefresh;
}

inline bool IsFiniteNativeValue(int64_t value) {
    // Native character stats and percentages are well below this bound. It
    // also rejects malformed queue/descriptor data before it reaches the AI.
    return value >= -1000000000000LL && value <= 1000000000000LL;
}

inline bool ScalePercent(int64_t value, int64_t percent, int64_t& out) {
    if (!IsFiniteNativeValue(value) || percent < -100000 || percent > 100000) return false;
    const long double scaled = static_cast<long double>(value) *
        static_cast<long double>(percent) / 100.0L;
    if (!std::isfinite(static_cast<double>(scaled)) ||
        scaled > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
        scaled < static_cast<long double>(std::numeric_limits<int64_t>::min())) return false;
    out = static_cast<int64_t>(scaled); // The native integer path truncates toward zero.
    return true;
}

inline bool ReadQueueActors(uintptr_t queue, int actorOrdinal, int targetOrdinal,
                            int& actorIndex, int& targetIndex,
                            uintptr_t& actorUnit, uintptr_t& targetUnit,
                            uintptr_t& actorChara, uintptr_t& targetChara,
                            uintptr_t& descriptor) {
    if (!queue || actorOrdinal < 0 || targetOrdinal < 0) return false;
    const int actorCount = *reinterpret_cast<const int16_t*>(queue + NativeCombat::kQueueActorCount);
    const int targetCount = *reinterpret_cast<const int16_t*>(queue + NativeCombat::kQueueTargetCount);
    if (actorCount < 1 || actorCount > 10 || actorOrdinal >= actorCount ||
        targetCount < 1 || targetCount > 256 || targetOrdinal >= targetCount) return false;

    actorIndex = *reinterpret_cast<const int16_t*>(queue + actorOrdinal * 0x1E8);
    targetIndex = *reinterpret_cast<const int16_t*>(queue + NativeCombat::kQueueTargets +
        targetOrdinal * NativeCombat::kQueueTargetStride);
    if (actorIndex < 0 || actorIndex >= Combat::kMaxUnits ||
        targetIndex < 0 || targetIndex >= Combat::kMaxUnits) return false;
    actorUnit = AutoPlay::GetUnitPtr(actorIndex);
    targetUnit = AutoPlay::GetUnitPtr(targetIndex);
    if (!actorUnit || !targetUnit) return false;
    actorChara = *reinterpret_cast<const uintptr_t*>(actorUnit + 0x240);
    targetChara = *reinterpret_cast<const uintptr_t*>(targetUnit + 0x240);
    if (!actorChara || !targetChara) return false;
    descriptor = *reinterpret_cast<const uintptr_t*>(queue + 0x27678);
    return true;
}

inline bool SafeDescriptor(uintptr_t descriptor, bool healing, NativeSkills::DescriptorInfo& info) {
    if (!descriptor || !NativeSkills::ReadSupportedDescriptor(descriptor, info)) return false;
    return info.healing == healing;
}

// Mirrors the ordinary one-effect scalar pipeline used by the native target
// preview. It deliberately does not call 1BE860, 1BF3C0, 1BFAE0, or 1C13E0:
// those routines consume RNG, update preview/global state, or resolve status
// procs. In particular, the rare critical/proc bonus from 1C13E0 is omitted.
inline bool Evaluate(uintptr_t queue, int actorOrdinal, int targetOrdinal, bool healing,
                     Combat::EffectEstimate& out) {
    out = {};
    if (!HasAllHelpers()) return false;

    int actorIndex = -1, targetIndex = -1;
    uintptr_t actorUnit = 0, targetUnit = 0, actorChara = 0, targetChara = 0, descriptor = 0;
    if (!ReadQueueActors(queue, actorOrdinal, targetOrdinal, actorIndex, targetIndex,
                         actorUnit, targetUnit, actorChara, targetChara, descriptor)) return false;

    NativeSkills::DescriptorInfo info{};
    const bool isSkill = descriptor != 0;
    if (isSkill) {
        if (!SafeDescriptor(descriptor, healing, info)) return false;
        // These descriptor IDs have non-scalar or random preview branches.
        // The surrounding filter excludes most; keep these explicit here too.
        if (info.skillId == 0x390 || info.skillId == 0x391 || info.skillId == 0x392 ||
            info.skillId == 0xBE0) return false;
    } else if (healing || *reinterpret_cast<const int32_t*>(queue + 0x27670) != 0) {
        return false;
    }

    // 1C1EE0 refreshes character caches when this predicate is nonzero. That
    // side path is outside the read-only estimator contract, so skip it.
    if (fnPreviewNeedsRefresh(reinterpret_cast<void*>(queue)) != 0) return false;

    const uint32_t selector = fnAttackSelector(reinterpret_cast<void*>(actorChara),
        isSkill ? reinterpret_cast<void*>(descriptor) : nullptr, 0);
    if (selector > 10) return false;
    int64_t attackTerm = fnGetStat(reinterpret_cast<void*>(actorChara), static_cast<int>(selector));
    if (!IsFiniteNativeValue(attackTerm)) return false;

    if (isSkill) {
        const int rank = *reinterpret_cast<const int16_t*>(queue + 0x27682);
        // D7A20 indexes the descriptor's ten signed words at +0x420..+0x432.
        if (rank < 0 || rank > 9) return false;
        const int effectPower = fnEffectPower(reinterpret_cast<void*>(descriptor), rank, 0);
        const int32_t skillBonus = fnSkillBonus(reinterpret_cast<void*>(actorUnit),
            reinterpret_cast<void*>(descriptor));
        int64_t powered = 0;
        if (effectPower < -100 || effectPower > 100000 || skillBonus < -100000 || skillBonus > 100000 ||
            !ScalePercent(attackTerm, 100LL + effectPower + skillBonus, powered)) return false;
        attackTerm = powered;
    }

    int64_t magnitude = 0;
    const bool reverseSign = fnDamageSign(reinterpret_cast<void*>(actorUnit),
        reinterpret_cast<void*>(targetUnit), reinterpret_cast<void*>(queue)) != 0;
    if (healing) {
        // Healing skills use the descriptor-selected stat (normally RES).
        // Native healing-only ally/evility adjustments in 1FFA50/1FFF40 are
        // intentionally omitted; these are bounded base estimates. The native
        // heal path carries a negative scalar until 1C0840 flips it for an ally.
        magnitude = -attackTerm;
        if (reverseSign) magnitude = -magnitude;
        if (magnitude <= 0) return false;
    } else {
        const int overrideIndex = fnDefenseOverride(reinterpret_cast<void*>(actorChara),
            isSkill ? reinterpret_cast<void*>(descriptor) : nullptr,
            reinterpret_cast<void*>(targetChara));
        if (overrideIndex < -1 || overrideIndex > 16) return false;
        const int64_t defenseTerm = fnGetDefenseStat(reinterpret_cast<void*>(targetChara),
            static_cast<int>(selector), overrideIndex);
        if (!IsFiniteNativeValue(defenseTerm)) return false;
        const int32_t criticalAttack = fnCriticalAttack(reinterpret_cast<void*>(actorUnit),
            reinterpret_cast<void*>(queue), targetOrdinal, 1);
        const int32_t criticalDefense = fnCriticalDefense(reinterpret_cast<void*>(actorUnit),
            reinterpret_cast<void*>(queue), targetOrdinal, 1);
        const long double raw =
            (static_cast<long double>(100LL + criticalAttack) * attackTerm / 100.0L) -
            (static_cast<long double>(100LL + criticalDefense) * defenseTerm / 200.0L);
        if (!std::isfinite(static_cast<double>(raw)) ||
            raw > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
            raw < static_cast<long double>(std::numeric_limits<int64_t>::min())) return false;
        magnitude = std::max<int64_t>(0, static_cast<int64_t>(raw));
    }

    const int64_t scale = fnExpectedScale(reinterpret_cast<void*>(actorUnit),
        reinterpret_cast<void*>(queue), targetOrdinal);
    int64_t scaled = 0;
    if (scale < 0 || scale > 100000 || !ScalePercent(magnitude, scale, scaled)) return false;
    magnitude = scaled;

    // This routine applies area and target resistance modifiers. Mode 1 skips
    // its RNG branch; the queue supplied by BuildPreview is private scratch.
    fnTargetAdjustment(&magnitude, reinterpret_cast<void*>(actorUnit),
        reinterpret_cast<void*>(queue), targetOrdinal, 1);
    if (!IsFiniteNativeValue(magnitude) || magnitude < 0) return false;

    // Damage is signed after target adjustment by the native path; healing is
    // signed before its scale/target adjustment, as in 1BF3C0.
    if (!healing && reverseSign) magnitude = -magnitude;
    if (magnitude < 0) return false;

    double expected = static_cast<double>(magnitude);
    if (!healing) {
        const uint32_t hitPercent = fnAccuracy(reinterpret_cast<void*>(actorUnit),
            reinterpret_cast<void*>(queue), targetOrdinal);
        expected *= static_cast<double>(std::min<uint32_t>(hitPercent, 100)) / 100.0;
    }
    if (!std::isfinite(expected) || expected < 0.0 || expected > 1.0e12) return false;

    out.targetIndex = targetIndex;
    if (healing) out.expectedHealing = expected;
    else out.expectedDamage = expected;
    return true;
}

} // namespace AutoPlay::NativeEffects
