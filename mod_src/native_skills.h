#pragma once
#include "planning_budget.h"
#include "native_combat.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace AutoPlay::NativeSkills {

constexpr uintptr_t kEnumerateSkillsRva = 0x1D68A0;
constexpr uintptr_t kEffectiveCostRva = 0x1D7550;
constexpr uintptr_t kCanUseSkillRva = 0x155280;
constexpr uintptr_t kEffectiveRangeRva = 0x1D7EB0;
constexpr uintptr_t kSkillIndexRva = 0x061FF0;
constexpr uintptr_t kMaxShapeChoicesRva = 0x160140;
constexpr uintptr_t kSkillTablePointerRva = 0xA03868;
constexpr uintptr_t kSkillTableCountRva = 0xA03870;

constexpr size_t kMaxSkillRows = 512;
constexpr size_t kSkillRowStride = 0x10;
constexpr size_t kDescriptorStride = 0x474;
constexpr size_t kMenuListOffset = 0x140;
constexpr size_t kMenuCountOffset = 0x14C;
constexpr size_t kMenuCursorOffset = 0x1D0;
constexpr size_t kMenuRowsOffset = 0x218;
constexpr size_t kMenuRowStride = 0x20;

using FnEnumerateSkills = int(__fastcall*)(void* charaData, uint32_t* outRows);
using FnEffectiveCost = int64_t(__fastcall*)(void* descriptor, void* charaData,
                                               void* battleUnit, uint32_t rowIndex,
                                               uint32_t areaPanels);
using FnCanUseSkill = int(__fastcall*)(void* battleUnit, void* descriptor,
                                       int64_t effectiveCost);
using FnEffectiveRange = uint8_t(__fastcall*)(void* charaData, void* descriptor,
                                               void* battleUnit);
// The first argument is ignored by the native implementation. It is retained
// to match the proven two-argument call sites.
using FnSkillIndex = int(__fastcall*)(const void* unusedRow, uint32_t skillId);
using FnMaxShapeChoices = int(__fastcall*)(void* charaData, void* descriptor);

inline FnEnumerateSkills fnEnumerateSkills = nullptr;
inline FnEffectiveCost fnEffectiveCost = nullptr;
inline FnCanUseSkill fnCanUseSkill = nullptr;
inline FnEffectiveRange fnEffectiveRange = nullptr;
inline FnSkillIndex fnSkillIndex = nullptr;
inline FnMaxShapeChoices fnMaxShapeChoices = nullptr;

inline void Initialize(uintptr_t base) {
    AutoPlay::g_Base = base;
    fnEnumerateSkills = reinterpret_cast<FnEnumerateSkills>(base + kEnumerateSkillsRva);
    fnEffectiveCost = reinterpret_cast<FnEffectiveCost>(base + kEffectiveCostRva);
    fnCanUseSkill = reinterpret_cast<FnCanUseSkill>(base + kCanUseSkillRva);
    fnEffectiveRange = reinterpret_cast<FnEffectiveRange>(base + kEffectiveRangeRva);
    fnSkillIndex = reinterpret_cast<FnSkillIndex>(base + kSkillIndexRva);
    fnMaxShapeChoices = reinterpret_cast<FnMaxShapeChoices>(base + kMaxShapeChoicesRva);
}

inline uintptr_t SkillTable() {
    if (!AutoPlay::g_Base) return 0;
    return *reinterpret_cast<const uintptr_t*>(AutoPlay::g_Base + kSkillTablePointerRva);
}

inline int SkillTableCount() {
    if (!AutoPlay::g_Base) return 0;
    const int count = *reinterpret_cast<const int32_t*>(AutoPlay::g_Base + kSkillTableCountRva);
    return count > 0 && count <= 0x10000 ? count : 0;
}

inline uintptr_t ResolveDescriptor(const void* rowIdPointer, uint32_t skillId) {
    if (!fnSkillIndex || !AutoPlay::g_Base) return 0;
    const uintptr_t table = SkillTable();
    const int count = SkillTableCount();
    if (!table || count <= 0) return 0;
    const int index = fnSkillIndex(rowIdPointer, skillId);
    if (index < 0 || index >= count) return 0;
    return table + static_cast<uintptr_t>(index) * kDescriptorStride;
}

struct DescriptorInfo {
    int skillId = -1;
    uint8_t formula = 0;
    uint8_t skillGroup = 0xFF;
    uint8_t range = 0;
    uint8_t rangeFlag = 0;
    uint16_t shapeId = 0;
    int16_t effectPower = 0;
    int16_t levelOnePower = 0;
    bool healing = false;
};

// Only known damage/heal skills with one primary effect enter the catalog.
// Group, movement, blow, tower, and partner metadata exclude unsupported
// commands and side effects without interpreting localized names.
inline bool ReadSupportedDescriptor(uintptr_t descriptor, DescriptorInfo& out) {
    if (!descriptor) return false;
    const uint32_t skillId = *reinterpret_cast<const uint32_t*>(descriptor);
    if (skillId == 0x2BA4) return false; // Native special branch, not ordinary targeting.

    const uint8_t formula = *reinterpret_cast<const uint8_t*>(descriptor + 0x3F6);
    if (formula != 2 && formula != 3 && formula != 4) return false;
    if (*reinterpret_cast<const uint8_t*>(descriptor + 0x3E0) != 1) return false;
    for (size_t i = 1; i < 6; ++i)
        if (*reinterpret_cast<const uint8_t*>(descriptor + 0x3E0 + i) != 0) return false;

    const uint8_t group = *reinterpret_cast<const uint8_t*>(descriptor + 0x44E);
    if (group > 9) return false; // Excludes item, Overload, tower/system, and unknown groups.
    if (*reinterpret_cast<const uint8_t*>(descriptor + 0x3F7) == 11) return false; // Multi-actor combo route.
    if (*reinterpret_cast<const uint8_t*>(descriptor + 0x3F3) != 0) return false;
    if (*reinterpret_cast<const uint8_t*>(descriptor + 0x43D) != 0 ||
        *reinterpret_cast<const uint8_t*>(descriptor + 0x43E) != 0 ||
        *reinterpret_cast<const uint8_t*>(descriptor + 0x43F) != 0) return false;
    if (*reinterpret_cast<const uint8_t*>(descriptor + 0x441) != 0) return false;
    if (*reinterpret_cast<const uint8_t*>(descriptor + 0x44C) != 0) return false;
    if (*reinterpret_cast<const uint16_t*>(descriptor + 0x464) > 1) return false;
    if (*reinterpret_cast<const uint8_t*>(descriptor + 0x436) > 1 ||
        *reinterpret_cast<const uint16_t*>(descriptor + 0x438) == 30000) return false;

    out.skillId = static_cast<int>(skillId);
    out.formula = formula;
    out.skillGroup = group;
    out.range = *reinterpret_cast<const uint8_t*>(descriptor + 0x435);
    out.rangeFlag = *reinterpret_cast<const uint8_t*>(descriptor + 0x436);
    out.shapeId = *reinterpret_cast<const uint16_t*>(descriptor + 0x438);
    out.effectPower = *reinterpret_cast<const int16_t*>(descriptor + 0x3E6);
    out.levelOnePower = *reinterpret_cast<const int16_t*>(descriptor + 0x420);
    out.healing = formula == 4;
    return true;
}

struct Entry {
    int slot = -1;
    int skillId = -1;
    uintptr_t descriptor = 0;
    int64_t spCost = 0;
    uint8_t learnedRowLevelByte = 0;
    uint8_t range = 0;
    uint8_t rangeFlag = 0;
    uint16_t shapeId = 0;
    uint8_t formula = 0;
    uint8_t skillGroup = 0xFF;
    int16_t effectPower = 0;
    int16_t levelOnePower = 0;
    int maxShapeChoices = 1;
    bool learned = false;
    bool currentlyUsable = false;
    bool affordableFromSp = false;
    bool healing = false;
};

struct CatalogJob {
    int actorIndex = -1;
    uintptr_t battleUnit = 0;
    uintptr_t charaData = 0;
    bool rowsEnumerated = false;
    bool complete = false;
    int rowCount = 0;
    int nextRow = 0;
    std::array<uint32_t, kMaxSkillRows> rows{};
    std::vector<Entry> entries;
};

inline CatalogJob Begin(int actorIndex) {
    CatalogJob job{};
    job.actorIndex = actorIndex;
    if (!AutoPlay::g_Base || actorIndex < 0 || actorIndex >= Combat::kMaxUnits) {
        job.complete = true;
        return job;
    }
    job.battleUnit = AutoPlay::GetUnitPtr(actorIndex);
    if (!job.battleUnit) {
        job.complete = true;
        return job;
    }
    job.charaData = *reinterpret_cast<const uintptr_t*>(job.battleUnit + 0x240);
    if (!job.charaData) job.complete = true;
    return job;
}

inline bool StructurallySafeRow(const CatalogJob& job, uint32_t slot,
                                uint32_t& skillId, uintptr_t& descriptor,
                                DescriptorInfo& info) {
    if (slot >= kMaxSkillRows || !fnSkillIndex) return false;
    const uintptr_t rowId = job.charaData + 0xCC4 + static_cast<uintptr_t>(slot) * kSkillRowStride;
    skillId = *reinterpret_cast<const uint32_t*>(rowId);
    if (!skillId || skillId == 0xFFFFFFFFu) return false;
    descriptor = ResolveDescriptor(reinterpret_cast<const void*>(rowId), skillId);
    if (!descriptor) return false;
    if (!ReadSupportedDescriptor(descriptor, info) || info.skillId != static_cast<int>(skillId)) return false;
    return true;
}

inline bool Step(CatalogJob& job, int maxSlots = 4) {
    if (job.complete) return true;
    if (!job.charaData || !job.battleUnit || !fnEnumerateSkills) {
        job.complete = true;
        return true;
    }
    if (maxSlots <= 0) return false;
    if (!job.rowsEnumerated) {
        Planning::Unit enumeration;
        if (!enumeration) return false;
        const int storedCount = *reinterpret_cast<const int16_t*>(job.charaData + 0x2CC0);
        if (storedCount < 0 || storedCount > static_cast<int>(kMaxSkillRows)) {
            job.complete = true;
            return true;
        }
        { Planning::QueryScope profile(Planning::QueryKind::Enumerate);
          job.rowCount = fnEnumerateSkills(reinterpret_cast<void*>(job.charaData), job.rows.data()); }
        job.rowsEnumerated = true;
        if (job.rowCount < 0 || job.rowCount > static_cast<int>(kMaxSkillRows)) {
            job.rowCount = 0;
            job.complete = true;
            return true;
        }
        job.entries.reserve(static_cast<size_t>(job.rowCount));
    }

    const int stop = std::min(job.rowCount, job.nextRow + maxSlots);
    for (; job.nextRow < stop; ++job.nextRow) {
        Planning::Unit operation;
        if (!operation) return false;
        const uint32_t slot = job.rows[static_cast<size_t>(job.nextRow)];
        uint32_t skillId = 0;
        uintptr_t descriptor = 0;
        DescriptorInfo info{};
        if (!StructurallySafeRow(job, slot, skillId, descriptor, info)) continue;

        const uintptr_t rowId = job.charaData + 0xCC4 + static_cast<uintptr_t>(slot) * kSkillRowStride;
        const int maxShapeChoices = fnMaxShapeChoices
            ? std::min(9, std::max(1, fnMaxShapeChoices(reinterpret_cast<void*>(job.charaData),
                                                      reinterpret_cast<void*>(descriptor)))) : 1;
        const int configuredArea = info.shapeId ? 1 : *reinterpret_cast<const uint8_t*>(rowId + 10);
        if (configuredArea < 1 || configuredArea > maxShapeChoices) continue;
        const int64_t cost = fnEffectiveCost
            ? fnEffectiveCost(reinterpret_cast<void*>(descriptor), reinterpret_cast<void*>(job.charaData),
                              reinterpret_cast<void*>(job.battleUnit), slot, configuredArea)
            : std::numeric_limits<int64_t>::max();
        if (cost < 0 || cost == std::numeric_limits<int64_t>::max()) continue;

        const int64_t currentSp = *reinterpret_cast<const int64_t*>(job.charaData + 0x2CD0);
        Entry entry{};
        entry.slot = static_cast<int>(slot);
        entry.skillId = static_cast<int>(skillId);
        entry.descriptor = descriptor;
        entry.spCost = cost;
        entry.learnedRowLevelByte = *reinterpret_cast<const uint8_t*>(rowId + 5);
        entry.range = fnEffectiveRange
            ? fnEffectiveRange(reinterpret_cast<void*>(job.charaData), reinterpret_cast<void*>(descriptor),
                               reinterpret_cast<void*>(job.battleUnit))
            : info.range;
        entry.rangeFlag = info.rangeFlag;
        entry.shapeId = info.shapeId;
        entry.formula = info.formula;
        entry.skillGroup = info.skillGroup;
        entry.effectPower = info.effectPower;
        entry.levelOnePower = info.levelOnePower;
        entry.maxShapeChoices = maxShapeChoices;
        entry.learned = true; // The engine enumerator returned this row.
        entry.currentlyUsable = fnCanUseSkill &&
            fnCanUseSkill(reinterpret_cast<void*>(job.battleUnit), reinterpret_cast<void*>(descriptor), cost) != 0;
        entry.affordableFromSp = currentSp >= 0 && cost <= currentSp;
        entry.healing = info.healing;
        job.entries.push_back(entry);
    }
    job.complete = job.nextRow >= job.rowCount;
    return job.complete;
}

struct MenuRow {
    int menuIndex = -1;
    int slot = -1;
    int skillId = -1;
    uintptr_t descriptor = 0;
    bool disabled = true;
    bool safe = false;
    bool selectable = false;
};

inline std::vector<MenuRow> ReadSkillList(uintptr_t task) {
    std::vector<MenuRow> rows;
    if (!task) return rows;
    const int count = *reinterpret_cast<const int32_t*>(task + kMenuCountOffset);
    if (count < 0 || count > static_cast<int>(kMaxSkillRows)) return rows;
    const uintptr_t charaData = *reinterpret_cast<const uintptr_t*>(task + 0x4228);
    const uintptr_t battleUnit = *reinterpret_cast<const uintptr_t*>(task + 0x4220);
    if (!charaData || !battleUnit ||
        *reinterpret_cast<const uintptr_t*>(battleUnit + 0x240) != charaData) return rows;

    rows.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const uintptr_t row = task + kMenuRowsOffset + static_cast<uintptr_t>(i) * kMenuRowStride;
        const uintptr_t descriptor = *reinterpret_cast<const uintptr_t*>(row);
        const uintptr_t charaSkillRow = *reinterpret_cast<const uintptr_t*>(row + 8);
        const uint32_t slot = *reinterpret_cast<const uint32_t*>(row + 0x18);
        if (!descriptor || !charaSkillRow || slot >= kMaxSkillRows) continue;
        const uintptr_t expectedRow = charaData + 0xCC0 + static_cast<uintptr_t>(slot) * kSkillRowStride;
        if (charaSkillRow != expectedRow) continue;
        const uint32_t skillId = *reinterpret_cast<const uint32_t*>(charaSkillRow + 4);
        if (skillId != *reinterpret_cast<const uint32_t*>(descriptor)) continue;
        if (ResolveDescriptor(reinterpret_cast<const void*>(charaSkillRow + 4), skillId) != descriptor) continue;

        DescriptorInfo info{};
        const bool safe = ReadSupportedDescriptor(descriptor, info) && info.skillId == static_cast<int>(skillId);
        // 62EAB0 writes only the low byte with (CanUseSkill == 0). The
        // adjacent bytes are packed presentation metadata, observed as 80h
        // in live selectable rows; reading a dword rejects usable skills.
        const bool disabled = *reinterpret_cast<const uint8_t*>(row + 0x10) != 0;
        rows.push_back({i, static_cast<int>(slot), static_cast<int>(skillId), descriptor,
                        disabled, safe, !disabled && safe});
    }
    return rows;
}

inline int FindSelectableRow(uintptr_t task, int slot, uintptr_t descriptor) {
    if (slot < 0 || slot >= static_cast<int>(kMaxSkillRows) || !descriptor) return -1;
    for (const MenuRow& row : ReadSkillList(task))
        if (row.selectable && row.slot == slot && row.descriptor == descriptor) return row.menuIndex;
    return -1;
}

inline bool SelectSkillCursor(uintptr_t task, int menuIndex, int expectedSlot,
                              uintptr_t expectedDescriptor) {
    if (!task || menuIndex < 0 || expectedSlot < 0 || !expectedDescriptor) return false;
    const std::vector<MenuRow> rows = ReadSkillList(task);
    const auto found = std::find_if(rows.begin(), rows.end(),
        [menuIndex, expectedSlot, expectedDescriptor](const MenuRow& row) {
            return row.menuIndex == menuIndex && row.slot == expectedSlot &&
                row.descriptor == expectedDescriptor && row.selectable;
        });
    if (found == rows.end()) return false;
    *reinterpret_cast<int32_t*>(task + kMenuCursorOffset) = menuIndex;
    return true;
}

inline bool SelectSkillCursor(uintptr_t task, const Entry& entry) {
    const int menuIndex = FindSelectableRow(task, entry.slot, entry.descriptor);
    return SelectSkillCursor(task, menuIndex, entry.slot, entry.descriptor);
}

} // namespace AutoPlay::NativeSkills
