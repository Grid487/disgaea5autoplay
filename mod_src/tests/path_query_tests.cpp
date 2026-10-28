#include "../autoplay_ai.h"

#include <cstdio>
#include <cstring>

namespace {

enum class PathMode { Valid, Reject, BadLength, WrongEndpoint };
PathMode g_PathMode = PathMode::Valid;
int g_PathCalls = 0;
bool g_PathSawNativeSentinel = false;
constexpr size_t kImageSize = 0x05000000;
constexpr int kPlayerIndex = 16;
constexpr int kEnemyIndex = 0;
constexpr int kTargetX = 11;
constexpr int kTargetZ = 10;

uintptr_t Unit(int index) {
    return AutoPlay::GetUnitPtr(index);
}

void StubComputeMoveRange(void*, void*, void* unit, uint32_t, int) {
    const auto u = reinterpret_cast<uintptr_t>(unit);
    std::memset(reinterpret_cast<void*>(u + AutoPlay::UNIT_PATH_STATE_OFFSET), 0x5A,
                AutoPlay::UNIT_PATH_STATE_SIZE);

    std::memset(reinterpret_cast<void*>(AutoPlay::g_Base + RVA::ADDR_MoveCostGrid), 0xFF, 128 * 128);
    std::memset(reinterpret_cast<void*>(AutoPlay::g_Base + RVA::ADDR_TileFlagGrid), 0xFC, 128 * 128);
    const size_t tile = static_cast<size_t>(kTargetX) * 128 + kTargetZ;
    *(uint8_t*)(AutoPlay::g_Base + RVA::ADDR_MoveCostGrid + tile) = 1;
    *(uint8_t*)(AutoPlay::g_Base + RVA::ADDR_TileFlagGrid + tile) = 0;
}

bool StubComputePath(void* unit, int targetX, int targetZ, uint8_t) {
    ++g_PathCalls;
    const auto u = reinterpret_cast<uintptr_t>(unit);
    g_PathSawNativeSentinel = *(uint32_t*)(u + 0x416) == 0x80;
    *(int16_t*)(u + 0x416) = g_PathMode == PathMode::BadLength ? 128 : 1;
    int16_t endpointX = static_cast<int16_t>(targetX);
    int16_t endpointZ = static_cast<int16_t>(targetZ);
    if (g_PathMode == PathMode::WrongEndpoint) {
        ++endpointX;
    }
    *(int16_t*)(u + 0x41A) = 10;
    *(int16_t*)(u + 0x51A) = 10;
    *(int16_t*)(u + 0x41C) = endpointX;
    *(int16_t*)(u + 0x51C) = endpointZ;
    return g_PathMode != PathMode::Reject;
}

void StubGetPackedXZ(void*, uint32_t* out) {
    *out = 0;
}

int StubGetMoveStat(void*) {
    return 6;
}

int StubGetFaction(void* unit, uint8_t) {
    const auto u = reinterpret_cast<uintptr_t>(unit);
    const auto chara = *(uintptr_t*)(u + 0x240);
    return *(uint8_t*)(chara + 0x2E32);
}

int StubIsMapObject(void*) {
    return 0;
}

void InitializeUnit(int index, int gridX, int gridZ, uint8_t faction, int64_t hp) {
    auto unit = Unit(index);
    auto chara = AutoPlay::g_Base + 0x03F00000 + static_cast<uintptr_t>(index) * 0x4000;
    std::memset(reinterpret_cast<void*>(unit), 0, 0x1260);
    std::memset(reinterpret_cast<void*>(chara), 0, 0x4000);
    *(float*)(unit + 0x08) = static_cast<float>(gridX * 12 + 6);
    *(float*)(unit + 0x10) = static_cast<float>(gridZ * 12 + 6);
    *(uintptr_t*)(unit + 0x240) = chara;
    *(uint8_t*)(unit + 0x2E8) = 1;
    *(int16_t*)(unit + 0x2EE) = -1;
    *(uint8_t*)(chara + 0x2E32) = faction;
    *(int64_t*)(chara + 0x2CC8) = hp;
}

bool Expect(bool condition, const char* name) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        return false;
    }
    std::printf("PASS: %s\n", name);
    return true;
}

bool ResetWorld() {
    if (AutoPlay::g_Base) {
        VirtualFree(reinterpret_cast<void*>(AutoPlay::g_Base), 0, MEM_RELEASE);
        AutoPlay::g_Base = 0;
    }
    auto memory = VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) {
        std::fprintf(stderr, "VirtualAlloc failed (%lu)\n", GetLastError());
        return false;
    }
    AutoPlay::g_Base = reinterpret_cast<uintptr_t>(memory);
    std::memset(memory, 0, kImageSize);
    InitializeUnit(kPlayerIndex, 10, 10, 0, 100);
    InitializeUnit(kEnemyIndex, 14, 10, 3, 100);
    std::memset(reinterpret_cast<void*>(Unit(kPlayerIndex) + AutoPlay::UNIT_PATH_STATE_OFFSET),
                0xA5, AutoPlay::UNIT_PATH_STATE_SIZE);
    AutoPlay::fnGetUnitPackedXZ = &StubGetPackedXZ;
    AutoPlay::fnComputeMoveRange = &StubComputeMoveRange;
    AutoPlay::fnComputePathToTile = &StubComputePath;
    AutoPlay::fnGetUnitMoveStat = &StubGetMoveStat;
    AutoPlay::fnGetUnitFaction = &StubGetFaction;
    AutoPlay::fnIsMapObject = &StubIsMapObject;
    g_PathMode = PathMode::Valid;
    g_PathCalls = 0;
    g_PathSawNativeSentinel = false;
    return true;
}

bool PathStateIsOriginal() {
    const auto path = Unit(kPlayerIndex) + AutoPlay::UNIT_PATH_STATE_OFFSET;
    for (size_t i = 0; i < AutoPlay::UNIT_PATH_STATE_SIZE; ++i) {
        if (*(const uint8_t*)(path + i) != 0xA5) return false;
    }
    return true;
}

bool RunPathCase(PathMode mode, const char* label, bool expectedResult) {
    if (!ResetWorld()) return false;
    g_PathMode = mode;
    int targetX = -1;
    int targetZ = -1;
    const bool found = AutoPlay::ChooseBestMoveTileForUnit(kPlayerIndex, targetX, targetZ);
    const bool correctTarget = expectedResult ? (targetX == kTargetX && targetZ == kTargetZ)
                                              : (targetX == -1 && targetZ == -1);
    return Expect(found == expectedResult && correctTarget && PathStateIsOriginal() && g_PathCalls == 1,
                  label);
}

bool TestPathStateRestoredOnValidSelection() {
    if (!ResetWorld()) return false;
    int targetX = -1;
    int targetZ = -1;
    const bool found = AutoPlay::ChooseBestMoveTileForUnit(kPlayerIndex, targetX, targetZ);
    return Expect(found && targetX == kTargetX && targetZ == kTargetZ &&
                  PathStateIsOriginal() && g_PathCalls == 1 && g_PathSawNativeSentinel,
                  "valid closer tile is selected, native sentinel is set, and path bytes are restored");
}

bool TestOccupiedTileIsRejected() {
    if (!ResetWorld()) return false;
    InitializeUnit(1, kTargetX, kTargetZ, 0, 100);
    int targetX = -1;
    int targetZ = -1;
    const bool found = AutoPlay::ChooseBestMoveTileForUnit(kPlayerIndex, targetX, targetZ);
    return Expect(!found && targetX == -1 && targetZ == -1 &&
                  PathStateIsOriginal() && g_PathCalls == 0,
                  "occupied candidate is rejected without calling pathfinder");
}

} // namespace

int main() {
    bool ok = true;
    ok = TestPathStateRestoredOnValidSelection() && ok;
    ok = RunPathCase(PathMode::Reject, "pathfinder rejection is respected and bytes are restored", false) && ok;
    ok = RunPathCase(PathMode::BadLength, "out-of-bounds path length is rejected and bytes are restored", false) && ok;
    ok = RunPathCase(PathMode::WrongEndpoint, "endpoint mismatch is rejected and bytes are restored", false) && ok;
    ok = TestOccupiedTileIsRejected() && ok;
    if (AutoPlay::g_Base) {
        VirtualFree(reinterpret_cast<void*>(AutoPlay::g_Base), 0, MEM_RELEASE);
    }
    if (!ok) return 1;
    std::puts("All offline movement path-query checks passed.");
    return 0;
}
