#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "autoplay_ai.h"
#include "planning_budget.h"

// Other translation-unit headers may have included windows.h before
// NOMINMAX was set. Keep these macros out of the planner/geometry code.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace AutoPlay::NativeGeometry {

// Recovered from FUN_14008b410 and its callers. BEC400 is a global pointer to
// {uint16 recordCount, uint16 cellCount}; BEC408 and BEC410 point to the
// 8-byte record table and packed 3-byte cell table.
constexpr uintptr_t kShapeHeaderPointerRva = 0x00BEC400;
constexpr uintptr_t kShapeRecordsPointerRva = 0x00BEC408;
constexpr uintptr_t kShapeCellsPointerRva = 0x00BEC410;
constexpr size_t kShapeOutputSize = 0x818;
constexpr size_t kMaxShapeCells = 256;
constexpr uint32_t kMaxShapeRecords = 65535;
constexpr uint32_t kMaxShapeTableCells = 65535;

enum class ShapeCellKind : int8_t {
    Hit = 1,
    Move = 2,
    HitWithAlternateRule = 3,
    Unknown = 0
};

struct ShapeTableHeader {
    uint16_t recordCount = 0;
    uint16_t cellCount = 0;
};

struct ShapeRecord {
    uint32_t id = 0;
    uint16_t firstCell = 0;
    uint8_t cellCount = 0;
    uint8_t flags = 0;
};

struct ShapeCell {
    int8_t x = 0;
    int8_t z = 0;
    int8_t kind = 0;
};

// 08B451 loads WORD [header], and 08B55E loads WORD [header+2]. The running
// game's records pointer is header+4 (240 records and 1599 cells observed).
static_assert(sizeof(ShapeTableHeader) == 4, "native shape header contains two 16-bit counts");
static_assert(sizeof(ShapeRecord) == 8, "shape records are 8 bytes in the recovered table");
static_assert(sizeof(ShapeCell) == 3, "shape cells are 3 packed bytes in the recovered table");

struct ShapeDescriptor {
    uint32_t id = 0;
    uint8_t count = 0;
    uint32_t farthest = 0; // Maximum |x| + |z|, matching the native loader.
    bool flag = false;
    std::array<ShapeCell, kMaxShapeCells> cells{};
};

// Attack-map assets cannot be reloaded by the original task while our
// battle-thread hook is running. Reuse decoded shapes within that hook only,
// avoiding repeated VirtualQuery calls for every roster skill/placement.
// A fresh frame always revalidates native pointers and decodes fresh cells.
struct FrameShapeCache {
    uint64_t serial = 0;
    uintptr_t imageBase = 0;
    size_t count = 0;
    std::array<ShapeDescriptor, 256> shapes{};
};
inline thread_local FrameShapeCache g_FrameShapes;

struct GridTile {
    int x = 0;
    int z = 0;
};

struct RotatedHitTiles {
    std::array<GridTile, kMaxShapeCells> tiles{};
    uint16_t count = 0;
};

struct FrameImageRanges {
    struct Range { uintptr_t begin = 0, end = 0; bool assetOnly = false; };
    uint64_t serial = 0;
    uintptr_t imageBase = 0;
    size_t count = 0;
    std::array<Range, 16> ranges{};
};
inline thread_local FrameImageRanges g_FrameImageRanges;

inline bool IsReadableRange(uintptr_t address, size_t byteCount, bool stableShapeAsset = false) {
    if (address == 0 || byteCount == 0 ||
        address > (std::numeric_limits<uintptr_t>::max)() - byteCount) return false;

    // Native image arrays cannot be unloaded while our battle-thread hook
    // owns execution. Cache only their page bounds/protection for this hook,
    // never their values. Explicit shape-asset reads may also reuse immutable
    // attack-map allocations; ordinary heap objects never use those entries.
    // A new budget rechecks Windows before reusing either type of range.
    auto& cache = g_FrameImageRanges;
    if (Planning::active) {
        if (cache.serial != Planning::active->serial || cache.imageBase != AutoPlay::g_Base) {
            cache.serial = Planning::active->serial; cache.imageBase = AutoPlay::g_Base; cache.count = 0;
        }
        for (size_t i=0; i<cache.count; ++i)
            if ((!cache.ranges[i].assetOnly || stableShapeAsset) &&
                address >= cache.ranges[i].begin && address + byteCount <= cache.ranges[i].end) return true;
    }
    Planning::QueryScope profile(Planning::QueryKind::Memory);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region))
        return false;
    if (region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD) != 0 ||
        (region.Protect & 0xFF) == PAGE_NOACCESS) return false;

    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(region.BaseAddress);
    if (region.RegionSize > (std::numeric_limits<uintptr_t>::max)() - regionStart) return false;
    const uintptr_t regionEnd = regionStart + region.RegionSize;
    const bool valid = address >= regionStart && address + byteCount <= regionEnd;
    if (valid && Planning::active && (region.Type == MEM_IMAGE || stableShapeAsset) && cache.count < cache.ranges.size())
        cache.ranges[cache.count++] = {regionStart, regionEnd, region.Type != MEM_IMAGE};
    return valid;
}

inline bool ReadGlobalPointer(uintptr_t imageBase, uintptr_t rva, uintptr_t& outPointer) {
    outPointer = 0;
    if (imageBase == 0 || imageBase > (std::numeric_limits<uintptr_t>::max)() - rva) return false;
    const uintptr_t slot = imageBase + rva;
    if (!IsReadableRange(slot, sizeof(uintptr_t))) return false;
    std::memcpy(&outPointer, reinterpret_cast<const void*>(slot), sizeof(outPointer));
    return outPointer != 0;
}

struct ShapeTableView {
    uintptr_t imageBase = 0;
    uintptr_t headerPointer = 0;
    uintptr_t recordsPointer = 0;
    uintptr_t cellsPointer = 0;
    uint32_t recordCount = 0;
    uint32_t cellCount = 0;
};

struct ShapeIndexCache {
    uintptr_t imageBase = 0;
    uintptr_t headerPointer = 0;
    uintptr_t recordsPointer = 0;
    uintptr_t cellsPointer = 0;
    uint32_t recordCount = 0;
    uint32_t cellCount = 0;
    uint32_t nextRecord = 0;
    uint64_t generation = 1;
    bool ready = false;
    std::array<uint32_t, 1u << 16> recordById{};
    std::array<uint64_t, 1u << 16> generationById{};
};

// The game uses 16-bit shape IDs. Generation stamps let cache invalidation be
// O(1) instead of clearing both 65,536-entry arrays in a battle hook.
inline ShapeIndexCache g_ShapeIndexCache{};

inline void ResetShapeCache() {
    g_FrameShapes.count = 0;
    ShapeIndexCache& cache = g_ShapeIndexCache;
    if (++cache.generation == 0) cache.generation = 1;
    cache.imageBase = 0;
    cache.headerPointer = 0;
    cache.recordsPointer = 0;
    cache.cellsPointer = 0;
    cache.recordCount = 0;
    cache.cellCount = 0;
    cache.nextRecord = 0;
    cache.ready = false;
}

inline bool ReadShapeTableView(ShapeTableView& out) {
    out = ShapeTableView{};
    const uintptr_t imageBase = AutoPlay::g_Base;
    uintptr_t headerPointer = 0;
    uintptr_t recordsPointer = 0;
    uintptr_t cellsPointer = 0;
    if (!ReadGlobalPointer(imageBase, kShapeHeaderPointerRva, headerPointer) ||
        !ReadGlobalPointer(imageBase, kShapeRecordsPointerRva, recordsPointer) ||
        !ReadGlobalPointer(imageBase, kShapeCellsPointerRva, cellsPointer) ||
        !IsReadableRange(headerPointer, sizeof(ShapeTableHeader), true)) return false;

    ShapeTableHeader header{};
    std::memcpy(&header, reinterpret_cast<const void*>(headerPointer), sizeof(header));
    if (header.recordCount == 0 || header.recordCount > kMaxShapeRecords ||
        header.cellCount > kMaxShapeTableCells) return false;
    const uint64_t recordBytes = static_cast<uint64_t>(header.recordCount) * sizeof(ShapeRecord);
    if (recordBytes > (std::numeric_limits<size_t>::max)() ||
        !IsReadableRange(recordsPointer, static_cast<size_t>(recordBytes), true)) return false;

    out.imageBase = imageBase;
    out.headerPointer = headerPointer;
    out.recordsPointer = recordsPointer;
    out.cellsPointer = cellsPointer;
    out.recordCount = header.recordCount;
    out.cellCount = header.cellCount;
    return true;
}

inline bool ShapeCacheIdentityMatches(const ShapeIndexCache& cache, const ShapeTableView& view) {
    return cache.imageBase == view.imageBase && cache.headerPointer == view.headerPointer &&
        cache.recordsPointer == view.recordsPointer && cache.cellsPointer == view.cellsPointer &&
        cache.recordCount == view.recordCount && cache.cellCount == view.cellCount;
}

inline void SetShapeCacheIdentity(ShapeIndexCache& cache, const ShapeTableView& view) {
    if (++cache.generation == 0) cache.generation = 1;
    cache.imageBase = view.imageBase;
    cache.headerPointer = view.headerPointer;
    cache.recordsPointer = view.recordsPointer;
    cache.cellsPointer = view.cellsPointer;
    cache.recordCount = view.recordCount;
    cache.cellCount = view.cellCount;
    cache.nextRecord = 0;
    cache.ready = false;
}

inline bool UpdateShapeCacheIdentity(const ShapeTableView& view) {
    ShapeIndexCache& cache = g_ShapeIndexCache;
    if (!ShapeCacheIdentityMatches(cache, view)) SetShapeCacheIdentity(cache, view);
    return true;
}

inline bool InvalidateShapeCacheIfUnreadable() {
    ShapeTableView view{};
    if (ReadShapeTableView(view)) {
        UpdateShapeCacheIdentity(view);
        return true;
    }
    ResetShapeCache();
    return false;
}

inline bool ShapeCacheReady() {
    ShapeTableView view{};
    if (!ReadShapeTableView(view)) {
        ResetShapeCache();
        return false;
    }
    UpdateShapeCacheIdentity(view);
    return g_ShapeIndexCache.ready;
}

// Indexes at most 256 records per call. Duplicate IDs retain the first record,
// matching the original linear lookup. ID zero is handled specially by
// ReadShape and always selects record zero.
inline bool StepShapeCache(size_t maxRecords = 256) {
    ShapeTableView view{};
    if (!ReadShapeTableView(view)) {
        ResetShapeCache();
        return false;
    }
    UpdateShapeCacheIdentity(view);
    ShapeIndexCache& cache = g_ShapeIndexCache;
    if (cache.ready) return true;
    maxRecords = std::min<size_t>(maxRecords, 256);
    size_t processed = 0;
    while (processed < maxRecords && cache.nextRecord < cache.recordCount) {
        ShapeRecord record{};
        const uintptr_t address = cache.recordsPointer +
            static_cast<uintptr_t>(cache.nextRecord) * sizeof(ShapeRecord);
        std::memcpy(&record, reinterpret_cast<const void*>(address), sizeof(record));
        ++cache.nextRecord;
        ++processed;
        if (record.id == 0 || record.id > 0xFFFFu) continue;
        const uint16_t id = static_cast<uint16_t>(record.id);
        if (cache.generationById[id] == cache.generation) continue;
        cache.generationById[id] = cache.generation;
        cache.recordById[id] = cache.nextRecord - 1;
    }
    if (cache.nextRecord >= cache.recordCount) cache.ready = true;
    return cache.ready;
}

inline uint32_t ShapeCacheRecordsIndexed() {
    return g_ShapeIndexCache.nextRecord;
}

inline bool ReadShapeRecord(const ShapeTableView& table, uint32_t requestedId,
                            uint32_t recordIndex, ShapeDescriptor& out) {
    if (recordIndex >= table.recordCount) return false;
    ShapeRecord record{};
    const uintptr_t recordAddress = table.recordsPointer +
        static_cast<uintptr_t>(recordIndex) * sizeof(ShapeRecord);
    std::memcpy(&record, reinterpret_cast<const void*>(recordAddress), sizeof(record));
    const uint16_t lookupId = static_cast<uint16_t>(requestedId);
    if (lookupId != 0 && record.id != lookupId) return false;
    const uint64_t endCell = static_cast<uint64_t>(record.firstCell) + record.cellCount;
    if (endCell > table.cellCount || record.cellCount > kMaxShapeCells) return false;
    if (record.cellCount != 0) {
        const uint64_t cellBytes = static_cast<uint64_t>(record.cellCount) * sizeof(ShapeCell);
        const uint64_t firstCellOffset = static_cast<uint64_t>(record.firstCell) * sizeof(ShapeCell);
        if (firstCellOffset > (std::numeric_limits<uintptr_t>::max)() - table.cellsPointer ||
            cellBytes > (std::numeric_limits<size_t>::max)() ||
            !IsReadableRange(table.cellsPointer + static_cast<uintptr_t>(firstCellOffset),
                             static_cast<size_t>(cellBytes), true)) return false;
    }

    out = ShapeDescriptor{};
    out.id = requestedId;
    out.count = record.cellCount;
    out.flag = record.flags != 0;
    for (uint32_t i = 0; i < record.cellCount; ++i) {
        const uint64_t cellIndex = static_cast<uint64_t>(record.firstCell) + i;
        const uintptr_t address = table.cellsPointer + static_cast<uintptr_t>(cellIndex * sizeof(ShapeCell));
        std::memcpy(&out.cells[i], reinterpret_cast<const void*>(address), sizeof(ShapeCell));
        const int x = out.cells[i].x;
        const int z = out.cells[i].z;
        const uint32_t distance = static_cast<uint32_t>((x < 0 ? -x : x) + (z < 0 ? -z : z));
        if (distance > out.farthest) out.farthest = distance;
    }
    return true;
}

// Reads the same indexed record the native loader would select, but validates
// every pointer, count, record span and cell span before dereferencing it.
// Unlike the loader, a missing ID cleanly returns false instead of using a null
// record pointer while filling the output structure.
inline bool ReadShape(uint32_t requestedId, ShapeDescriptor& out) {
    out = ShapeDescriptor{};
    if (Planning::active) {
        auto& frame = g_FrameShapes;
        if (frame.serial != Planning::active->serial || frame.imageBase != AutoPlay::g_Base) {
            frame.serial = Planning::active->serial;
            frame.imageBase = AutoPlay::g_Base;
            frame.count = 0;
        }
        for (size_t i = 0; i < frame.count; ++i)
            if (frame.shapes[i].id == requestedId) { out = frame.shapes[i]; return true; }
    }
    ShapeTableView table{};
    if (!ReadShapeTableView(table)) return false;
    UpdateShapeCacheIdentity(table);
    const uint16_t lookupId = static_cast<uint16_t>(requestedId);
    uint32_t recordIndex = 0;
    bool found = false;
    const bool cacheMatches = ShapeCacheIdentityMatches(g_ShapeIndexCache, table);
    if (cacheMatches && g_ShapeIndexCache.ready) {
        if (lookupId == 0) {
            recordIndex = 0; // The engine selects record zero for shape ID zero.
            found = true;
        } else if (g_ShapeIndexCache.generationById[lookupId] == g_ShapeIndexCache.generation) {
            recordIndex = g_ShapeIndexCache.recordById[lookupId];
            found = true;
        } else {
            return false; // A complete cache is also a negative cache.
        }
    } else {
        found = lookupId == 0;
        if (!found) {
            for (uint32_t i = 0; i < table.recordCount; ++i) {
            ShapeRecord candidate{};
            const uintptr_t address = table.recordsPointer + static_cast<uintptr_t>(i) * sizeof(ShapeRecord);
            std::memcpy(&candidate, reinterpret_cast<const void*>(address), sizeof(candidate));
            if (candidate.id == lookupId) {
                recordIndex = i;
                found = true;
                break;
            }
        }
        }
    }
    if (!found) return false;
    if (!ReadShapeRecord(table, requestedId, recordIndex, out)) return false;
    if (Planning::active && g_FrameShapes.count < g_FrameShapes.shapes.size())
        g_FrameShapes.shapes[g_FrameShapes.count++] = out;
    return true;
}

inline bool IsHitKind(int8_t kind) {
    return kind == static_cast<int8_t>(ShapeCellKind::Hit) ||
           kind == static_cast<int8_t>(ShapeCellKind::HitWithAlternateRule);
}

inline bool IsKnownKind(int8_t kind) {
    return IsHitKind(kind) || kind == static_cast<int8_t>(ShapeCellKind::Move);
}

inline bool RotateShapeCell(int originX, int originZ, int8_t offsetX, int8_t offsetZ,
                            int rotation, GridTile& out) {
    if (rotation < 0 || rotation > 3) return false;
    switch (rotation) {
    case 0: out = {originX + offsetX, originZ + offsetZ}; break;
    case 1: out = {originX + offsetZ, originZ - offsetX}; break;
    case 2: out = {originX - offsetX, originZ - offsetZ}; break;
    case 3: out = {originX - offsetZ, originZ + offsetX}; break;
    default: return false;
    }
    return out.x >= 0 && out.x < 128 && out.z >= 0 && out.z < 128;
}

// Rotations are taken from FUN_1402da750: 0=(x,z), 1=(z,-x),
// 2=(-x,-z), 3=(-z,x), then translated by the target origin. Kind 2
// participates in native movement/range validation but is never a hit tile;
// unknown kinds invalidate the geometry so callers can skip that skill.
inline bool CollectRotatedHitTiles(const ShapeDescriptor& shape, int originX, int originZ,
                                   int rotation, RotatedHitTiles& out) {
    out = RotatedHitTiles{};
    if (shape.count > kMaxShapeCells || rotation < 0 || rotation > 3) return false;
    for (uint32_t i = 0; i < shape.count; ++i) {
        const ShapeCell& cell = shape.cells[i];
        if (!IsKnownKind(cell.kind)) {
            out = RotatedHitTiles{};
            return false;
        }
        if (!IsHitKind(cell.kind)) continue;

        GridTile tile{};
        if (!RotateShapeCell(originX, originZ, cell.x, cell.z, rotation, tile)) {
            out = RotatedHitTiles{};
            return false;
        }
        bool duplicate = false;
        for (uint16_t j = 0; j < out.count; ++j) {
            if (out.tiles[j].x == tile.x && out.tiles[j].z == tile.z) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) out.tiles[out.count++] = tile;
    }
    return out.count != 0;
}

} // namespace AutoPlay::NativeGeometry
