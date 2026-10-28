#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../native_geometry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using namespace AutoPlay;
using namespace AutoPlay::NativeGeometry;

constexpr size_t kFakeImageSize = 128ull * 1024ull * 1024ull;
constexpr uintptr_t kHeaderOffset = 0x01000000;
constexpr uintptr_t kRecordsOffset = 0x01001000;
constexpr uintptr_t kCellsOffset = 0x01002000;
constexpr uintptr_t kAltHeaderOffset = 0x01003000;
constexpr uintptr_t kAltRecordsOffset = 0x01003100;
constexpr uintptr_t kAltCellsOffset = 0x01003200;

uintptr_t g_FakeImage = 0;

bool Expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        return false;
    }
    std::printf("PASS: %s\n", label);
    return true;
}

bool EnsureImage() {
    if (g_FakeImage) return true;
    void* image = VirtualAlloc(nullptr, kFakeImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) {
        std::fprintf(stderr, "VirtualAlloc fake image failed (%lu)\n", GetLastError());
        return false;
    }
    g_FakeImage = reinterpret_cast<uintptr_t>(image);
    AutoPlay::g_Base = g_FakeImage;
    return true;
}

void PointGlobal(uintptr_t rva, uintptr_t value) {
    *(uintptr_t*)(g_FakeImage + rva) = value;
}

void PopulateValidTable() {
    std::memset(reinterpret_cast<void*>(g_FakeImage + 0x01000000), 0, 0x3000);
    auto* header = reinterpret_cast<ShapeTableHeader*>(g_FakeImage + kHeaderOffset);
    auto* records = reinterpret_cast<ShapeRecord*>(g_FakeImage + kRecordsOffset);
    auto* cells = reinterpret_cast<ShapeCell*>(g_FakeImage + kCellsOffset);

    *header = {3, 5};
    records[0] = {0, 0, 3, 1};
    records[1] = {77, 2, 3, 0};
    records[2] = {123, 0, 1, 0};
    cells[0] = {0, 0, static_cast<int8_t>(ShapeCellKind::Hit)};
    cells[1] = {2, -1, static_cast<int8_t>(ShapeCellKind::HitWithAlternateRule)};
    cells[2] = {-3, 4, static_cast<int8_t>(ShapeCellKind::Move)};
    cells[3] = {1, 2, static_cast<int8_t>(ShapeCellKind::Hit)};
    cells[4] = {0, -2, static_cast<int8_t>(ShapeCellKind::Move)};

    PointGlobal(kShapeHeaderPointerRva, reinterpret_cast<uintptr_t>(header));
    PointGlobal(kShapeRecordsPointerRva, reinterpret_cast<uintptr_t>(records));
    PointGlobal(kShapeCellsPointerRva, reinterpret_cast<uintptr_t>(cells));
}

bool TestPackedHeaderObservedInRunningGame() {
    if (!EnsureImage()) return false;
    // Literal bytes and pointer spacing from the running game, independent of
    // our C++ header/record definitions. The former DWORD header misread these
    // first four bytes as 104792304 records and blocked the battle hook.
    const uintptr_t header = g_FakeImage + kHeaderOffset;
    const uintptr_t records = header + 4;
    const uintptr_t cells = records + 240 * 8;
    std::memset(reinterpret_cast<void*>(header), 0, 0x3000);
    constexpr uint8_t headerBytes[] = {0xF0, 0x00, 0x3F, 0x06};
    constexpr uint8_t firstRecord[] = {1, 0, 0, 0, 0, 0, 1, 0};
    constexpr uint8_t lastRecord[] = {0xF0, 0, 0, 0, 0x3E, 0x06, 1, 0};
    constexpr uint8_t firstCell[] = {0, 0, 3};
    constexpr uint8_t lastCell[] = {1, 0xFF, 1};
    std::memcpy(reinterpret_cast<void*>(header), headerBytes, sizeof(headerBytes));
    std::memcpy(reinterpret_cast<void*>(records), firstRecord, sizeof(firstRecord));
    std::memcpy(reinterpret_cast<void*>(records + 239 * 8), lastRecord, sizeof(lastRecord));
    std::memcpy(reinterpret_cast<void*>(cells), firstCell, sizeof(firstCell));
    std::memcpy(reinterpret_cast<void*>(cells + 1598 * 3), lastCell, sizeof(lastCell));
    PointGlobal(kShapeHeaderPointerRva, header);
    PointGlobal(kShapeRecordsPointerRva, records);
    PointGlobal(kShapeCellsPointerRva, cells);
    ResetShapeCache();
    ShapeTableView view{};
    if (!Expect(ReadShapeTableView(view) && view.recordCount == 240 && view.cellCount == 1599,
                "actual packed WORD header decodes 240 records and 1599 cells")) return false;
    if (!Expect(StepShapeCache(256) && ShapeCacheRecordsIndexed() == 240,
                "actual table size completes indexing in one bounded frame")) return false;
    ShapeDescriptor shape{};
    if (!Expect(ReadShape(1, shape) && shape.count == 1 && shape.cells[0].kind == 3,
                "record immediately after the four-byte header reads correctly")) return false;
    return Expect(ReadShape(240, shape) && shape.count == 1 &&
                  shape.cells[0].x == 1 && shape.cells[0].z == -1,
                  "packed final record and cell remain correctly aligned");
}

bool TestReadShapeMatchesLoaderLayoutAndIdZeroRule() {
    if (!EnsureImage()) return false;
    PopulateValidTable();
    ShapeDescriptor shape{};
    if (!Expect(ReadShape(77, shape), "ReadShape finds a nonzero shape by table ID")) return false;
    if (!Expect(shape.id == 77 && shape.count == 3 && shape.farthest == 7 && !shape.flag,
                "ReadShape decodes id, count, farthest-cell distance and record flag")) return false;
    if (!Expect(shape.cells[0].x == -3 && shape.cells[0].z == 4 &&
                shape.cells[0].kind == static_cast<int8_t>(ShapeCellKind::Move),
                "record cell offset selects the correct packed signed x/z/kind data")) return false;

    if (!Expect(ReadShape(0, shape), "shape ID zero selects record zero like the native loader")) return false;
    return Expect(shape.id == 0 && shape.count == 3 && shape.flag,
                  "shape ID zero returns record zero metadata");
}

bool TestFrameCacheRevalidatesNextFrame() {
    if (!EnsureImage()) return false;
    PopulateValidTable();
    ResetShapeCache();
    ShapeDescriptor shape;
    {
        Planning::PlanningBudget budget; Planning::Scope scope(budget);
        if (!ReadShape(77, shape) || !ReadShape(77, shape)) return false;
        if (!Expect(g_FrameShapes.count == 1, "same-frame repeated shape reads reuse one decoded descriptor")) return false;
    }
    auto* cells = reinterpret_cast<ShapeCell*>(g_FakeImage + kCellsOffset);
    cells[2].x = -4;
    {
        Planning::PlanningBudget budget; Planning::Scope scope(budget);
        if (!Expect(ReadShape(77, shape) && shape.cells[0].x == -4,
                    "new frame revalidates and reads changed native shape cells")) return false;
    }
    PointGlobal(kShapeCellsPointerRva, 1);
    Planning::PlanningBudget budget; Planning::Scope scope(budget);
    return Expect(!ReadShape(77, shape), "new frame rejects unreadable pointers instead of using cached shapes");
}

bool TestInvalidIdAndNullOrUnreadablePointersFailSafely() {
    if (!EnsureImage()) return false;
    PopulateValidTable();
    ShapeDescriptor shape{};
    shape.id = 12345;
    if (!Expect(!ReadShape(999, shape) && shape.count == 0,
                "missing shape ID returns false with a cleared output")) return false;

    PointGlobal(kShapeRecordsPointerRva, 0);
    if (!Expect(!ReadShape(77, shape), "null record-table pointer is rejected safely")) return false;

    PopulateValidTable();
    PointGlobal(kShapeCellsPointerRva, 1);
    if (!Expect(!ReadShape(77, shape), "unreadable cell-table pointer is rejected safely")) return false;

    PopulateValidTable();
    PointGlobal(kShapeHeaderPointerRva, g_FakeImage + kFakeImageSize - sizeof(ShapeTableHeader) + 1);
    return Expect(!ReadShape(77, shape), "header range crossing the image mapping is rejected safely");
}

bool TestRecordAndCellRangesAreValidated() {
    if (!EnsureImage()) return false;
    PopulateValidTable();
    auto* header = reinterpret_cast<ShapeTableHeader*>(g_FakeImage + kHeaderOffset);
    auto* records = reinterpret_cast<ShapeRecord*>(g_FakeImage + kRecordsOffset);
    ShapeDescriptor shape{};

    records[1] = {77, 4, 2, 0}; // Offset + count exceeds the declared five cells.
    if (!Expect(!ReadShape(77, shape), "shape record cell range must fit the table header")) return false;

    PopulateValidTable();
    header->recordCount = 0;
    if (!Expect(!ReadShape(77, shape), "empty record count is rejected before scanning")) return false;

    PopulateValidTable();
    PointGlobal(kShapeRecordsPointerRva, g_FakeImage + kFakeImageSize - sizeof(ShapeRecord) + 1);
    return Expect(!ReadShape(77, shape), "record table span crossing the image mapping is rejected safely");
}

bool TestIncrementalShapeIndexAndNegativeCache() {
    if (!EnsureImage()) return false;
    PopulateValidTable();
    auto* records = reinterpret_cast<ShapeRecord*>(g_FakeImage + kRecordsOffset);
    records[2] = {77, 3, 1, 0}; // Duplicate ID: the native linear search selects record 1.

    ResetShapeCache();
    if (!Expect(!ShapeCacheReady(), "shape cache starts incomplete after reset")) return false;
    if (!Expect(!StepShapeCache(1) && ShapeCacheRecordsIndexed() == 1,
                "one-record budget indexes only one table record")) return false;
    ShapeDescriptor shape{};
    if (!Expect(ReadShape(77, shape) && shape.cells[0].x == -3,
                "ReadShape retains standalone linear lookup while cache construction is incomplete")) return false;

    while (!ShapeCacheReady()) StepShapeCache(1);
    if (!Expect(ShapeCacheReady(), "incremental cache eventually reaches ready state")) return false;
    if (!Expect(ReadShape(77, shape) && shape.cells[0].x == -3 && shape.count == 3,
                "complete index preserves first-match behavior for duplicate shape IDs")) return false;
    shape.id = 12345;
    if (!Expect(!ReadShape(999, shape) && shape.count == 0,
                "complete cache negatively caches absent IDs and clears failed output")) return false;

    // The first matching ID is malformed, while a later duplicate is valid.
    // Cache lookup must preserve the native first-record contract and fail.
    records[1] = {88, 4, 2, 0};
    records[2] = {88, 0, 1, 0};
    ResetShapeCache();
    while (!ShapeCacheReady()) StepShapeCache(2);
    if (!Expect(!ReadShape(88, shape),
                "malformed first matching record remains a cached failure despite a later duplicate")) return false;
    return Expect(!ReadShape(999, shape), "missing ID remains a negative-cache hit");
}

bool TestShapeCacheInvalidatesWhenTableIdentityChanges() {
    if (!EnsureImage()) return false;
    PopulateValidTable();
    ResetShapeCache();
    while (!ShapeCacheReady()) StepShapeCache(256);
    if (!Expect(ShapeCacheReady(), "initial table shape cache is ready")) return false;

    auto* altHeader = reinterpret_cast<ShapeTableHeader*>(g_FakeImage + kAltHeaderOffset);
    auto* altRecords = reinterpret_cast<ShapeRecord*>(g_FakeImage + kAltRecordsOffset);
    auto* altCells = reinterpret_cast<ShapeCell*>(g_FakeImage + kAltCellsOffset);
    *altHeader = {2, 1};
    altRecords[0] = {0, 0, 1, 0};
    altRecords[1] = {55, 0, 1, 0};
    altCells[0] = {3, -2, static_cast<int8_t>(ShapeCellKind::Hit)};

    PointGlobal(kShapeHeaderPointerRva, reinterpret_cast<uintptr_t>(altHeader));
    PointGlobal(kShapeRecordsPointerRva, reinterpret_cast<uintptr_t>(altRecords));
    PointGlobal(kShapeCellsPointerRva, reinterpret_cast<uintptr_t>(altCells));
    if (!Expect(!ShapeCacheReady() && ShapeCacheRecordsIndexed() == 0,
                "pointer changes invalidate the old index without clearing its 65,536-entry arrays")) return false;
    if (!Expect(!StepShapeCache(1) && ShapeCacheRecordsIndexed() == 1,
                "new table rebuild still obeys the 256-record cap")) return false;
    if (!Expect(StepShapeCache(1) && ShapeCacheReady(), "new table index completes incrementally")) return false;
    ShapeDescriptor shape{};
    if (!Expect(ReadShape(55, shape) && shape.cells[0].x == 3 && shape.cells[0].z == -2,
                "indexed read uses the changed table pointers")) return false;

    // Change the header count without moving any pointer. The identity check
    // must restart the cursor so no stale negative lookup survives.
    altHeader->recordCount = 1;
    if (!Expect(!ShapeCacheReady() && ShapeCacheRecordsIndexed() == 0,
                "record-count changes invalidate the existing index")) return false;
    if (!Expect(StepShapeCache(1) && ShapeCacheReady(), "changed-count table rebuild completes")) return false;
    return Expect(!ReadShape(55, shape), "removed table entry is no longer visible through the cache");
}

bool TestShapeCacheCapsPerCallWork() {
    if (!EnsureImage()) return false;
    PopulateValidTable();
    auto* header = reinterpret_cast<ShapeTableHeader*>(g_FakeImage + kHeaderOffset);
    auto* records = reinterpret_cast<ShapeRecord*>(g_FakeImage + kRecordsOffset);
    auto* cells = reinterpret_cast<ShapeCell*>(g_FakeImage + kCellsOffset);
    constexpr uint32_t count = 600;
    header->recordCount = count;
    header->cellCount = 1;
    cells[0] = {0, 0, static_cast<int8_t>(ShapeCellKind::Hit)};
    for (uint32_t i = 0; i < count; ++i) records[i] = {i + 1, 0, 1, 0};
    ResetShapeCache();
    if (!Expect(!ShapeCacheReady(), "large shape table begins uncached")) return false;
    if (!Expect(!StepShapeCache(1000) && ShapeCacheRecordsIndexed() == 256,
                "caller cannot raise cache work above 256 records per invocation")) return false;
    if (!Expect(!StepShapeCache(256) && ShapeCacheRecordsIndexed() == 512,
                "second bounded pass advances exactly another 256 records")) return false;
    return Expect(StepShapeCache(256) && ShapeCacheRecordsIndexed() == count,
                  "final bounded pass completes the cache at the actual table size");
}

bool TestRotationMatchesNativeQuarterTurns() {
    const ShapeCell cell{1, 2, static_cast<int8_t>(ShapeCellKind::Hit)};
    const GridTile expected[] = {{21, 22}, {22, 19}, {19, 18}, {18, 21}};
    for (int rotation = 0; rotation < 4; ++rotation) {
        GridTile actual{};
        if (!RotateShapeCell(20, 20, cell.x, cell.z, rotation, actual)) return false;
        if (actual.x != expected[rotation].x || actual.z != expected[rotation].z) {
            std::fprintf(stderr, "FAIL: rotation %d returned (%d,%d), expected (%d,%d)\n",
                rotation, actual.x, actual.z, expected[rotation].x, expected[rotation].z);
            return false;
        }
    }
    return Expect(true, "four quarter-turns match the transforms in FUN_1402da750");
}

bool TestHitCollectionSkipsMoveCellsAndRejectsUnknownKinds() {
    ShapeDescriptor shape{};
    shape.count = 3;
    shape.cells[0] = {1, 2, static_cast<int8_t>(ShapeCellKind::Hit)};
    shape.cells[1] = {-2, 1, static_cast<int8_t>(ShapeCellKind::HitWithAlternateRule)};
    shape.cells[2] = {4, 4, static_cast<int8_t>(ShapeCellKind::Move)};
    RotatedHitTiles result{};
    if (!Expect(CollectRotatedHitTiles(shape, 20, 20, 0, result),
                "known hit geometry generates affected tiles")) return false;
    if (!Expect(result.count == 2 && result.tiles[0].x == 21 && result.tiles[0].z == 22 &&
                result.tiles[1].x == 18 && result.tiles[1].z == 21,
                "kinds 1/3 emit hit tiles while kind 2 movement cells are excluded")) return false;

    shape.cells[2].kind = 99;
    if (!Expect(!CollectRotatedHitTiles(shape, 20, 20, 0, result) && result.count == 0,
                "unknown cell kind rejects the whole affected-area result")) return false;

    shape.count = 1;
    shape.cells[0] = {0, 0, static_cast<int8_t>(ShapeCellKind::Move)};
    return Expect(!CollectRotatedHitTiles(shape, 20, 20, 0, result),
                  "movement-only shape is not exposed as an offensive hit area");
}

bool TestInvalidRotationsAndOutOfMapHitTilesAreRejected() {
    GridTile tile{};
    if (!Expect(!RotateShapeCell(10, 10, 0, 0, 4, tile), "unsupported rotation is rejected")) return false;

    ShapeDescriptor shape{};
    shape.count = 1;
    shape.cells[0] = {-20, 0, static_cast<int8_t>(ShapeCellKind::Hit)};
    RotatedHitTiles result{};
    if (!Expect(!CollectRotatedHitTiles(shape, 10, 10, 0, result) && result.count == 0,
                "hit tile outside the 128x128 battle map rejects the placement")) return false;

    shape.count = 255; // Native shape count is a byte; the fixed buffer has 256 slots.
    for (size_t i = 0; i < shape.count; ++i)
        shape.cells[i] = {0, 0, static_cast<int8_t>(ShapeCellKind::Move)};
    shape.cells[0] = {1, 0, static_cast<int8_t>(ShapeCellKind::Hit)};
    return Expect(CollectRotatedHitTiles(shape, 10, 10, 0, result) && result.count == 1,
                  "maximum native cell count stays inside the fixed tile buffer");
}

} // namespace

int main() {
    if (!EnsureImage()) return 1;
    const bool passed =
        TestPackedHeaderObservedInRunningGame() &&
        TestReadShapeMatchesLoaderLayoutAndIdZeroRule() &&
        TestFrameCacheRevalidatesNextFrame() &&
        TestInvalidIdAndNullOrUnreadablePointersFailSafely() &&
        TestRecordAndCellRangesAreValidated() &&
        TestIncrementalShapeIndexAndNegativeCache() &&
        TestShapeCacheInvalidatesWhenTableIdentityChanges() &&
        TestShapeCacheCapsPerCallWork() &&
        TestRotationMatchesNativeQuarterTurns() &&
        TestHitCollectionSkipsMoveCellsAndRejectsUnknownKinds() &&
        TestInvalidRotationsAndOutOfMapHitTilesAreRejected();
    VirtualFree(reinterpret_cast<void*>(g_FakeImage), 0, MEM_RELEASE);
    AutoPlay::g_Base = 0;
    g_FakeImage = 0;
    return passed ? 0 : 1;
}
