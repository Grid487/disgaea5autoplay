#pragma once
#include "game_types.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

namespace AutoPlay {

static uintptr_t g_Base = 0;
static bool      g_Enabled = false;
static uint32_t  g_TickCount = 0;
static uint32_t  g_CooldownTicks = 0;
static int32_t   g_LastBattleState = -1;
static int32_t   g_StateStuckTicks = 0;

// Per-turn tracking so our AI doesn't loop on a unit that already finished our sequence
static bool      g_UnitDoneThisTurn[256]  = { false };
static bool      g_UnitMovedThisTurn[256] = { false };
static bool      g_BasePanelExhaustedThisTurn = false;
static int32_t   g_DeployCountThisStage = 0;
static int32_t   g_CurrentTargetEnemyIdx = -1;
static int32_t   g_CurrentTargetListIndex = 0;

// Virtual input injection state for current frame
static int32_t   g_PulseAction = -1;
static int32_t   g_HoldAction  = -1;

// Engine function pointers
static FnBattleExecute       orig_BattleExecute    = nullptr;
static FnCommandEndTurn      fnCommandEndTurn      = nullptr;
static FnUpdateCursorHover   fnUpdateCursorHover   = nullptr;
static FnGetTileHeight       fnGetTileHeight       = nullptr;
static FnIsControllableUnit  fnIsControllableUnit  = nullptr;
static FnIsMapObject         fnIsMapObject         = nullptr;
static FnGetUnitFaction      fnGetUnitFaction      = nullptr;
static FnCanUnitMove         fnCanUnitMove         = nullptr;
static FnGetUnitMoveStat     fnGetUnitMoveStat     = nullptr;
static FnHasActionRemaining  fnHasActionRemaining  = nullptr;
static FnCanUnitAttack       fnCanUnitAttack       = nullptr;
static FnGetUnitPackedXZ     fnGetUnitPackedXZ     = nullptr;
static FnComputeMoveRange    fnComputeMoveRange    = nullptr;
static FnComputePathToTile   fnComputePathToTile   = nullptr;
static FnPopulateDeployUnits fnPopulateDeployUnits = nullptr;
static FnPlaySoundEffect     fnPlaySoundEffect     = nullptr;

void LogMsg(const char* fmt, ...);

inline void ResetTurnTracking() {
    for (int i = 0; i < 256; ++i) {
        g_UnitDoneThisTurn[i]  = false;
        g_UnitMovedThisTurn[i] = false;
    }
    g_BasePanelExhaustedThisTurn = false;
    g_CurrentTargetEnemyIdx = -1;
    g_CurrentTargetListIndex = 0;
}

inline void EmitSubTaskCommand(uintptr_t subTask, int32_t cmdId, int32_t param) {
    if (!subTask) return;
    int32_t count = *(int32_t*)(subTask + 0x100);
    if (count >= 0 && count < 15) {
        *(int32_t*)(subTask + 0x80 + count * 8)     = cmdId;
        *(int32_t*)(subTask + 0x80 + count * 8 + 4) = param;
        *(int32_t*)(subTask + 0x100) = count + 1;
        *(int32_t*)(subTask + 0x18)  = 2;
    }
}

inline uintptr_t GetUnitPtr(int idx) {
    if (idx < 0 || idx >= 256) return 0;
    return g_Base + RVA::ADDR_BattleUnitArray + (uintptr_t)idx * 0x1260;
}

inline bool IsUnitAliveAndDeployed(uintptr_t unit) {
    if (!unit) return false;
    uint8_t isDeployed = *(uint8_t*)(unit + 0x2E8);
    if (!isDeployed) return false;
    if (*(int16_t*)(unit + 0x2EE) >= 0) return false; // Being carried
    uintptr_t charaData = *(uintptr_t*)(unit + 0x240);
    if (!charaData) return false;
    int64_t hp = *(int64_t*)(charaData + 0x2CC8);
    return hp > 0;
}

inline bool IsEnemyFaction(int faction) {
    // In Disgaea 5 Complete: 0 = Player, 3 = Enemy, 1 = Neutral/Hostile, 5 = Hostile
    return (faction == 3 || faction == 1 || faction == 5);
}

inline void GetUnitGridPos(uintptr_t unit, int& outX, int& outZ) {
    float wx = *(float*)(unit + 0x08);
    float wz = *(float*)(unit + 0x10);
    outX = (int)floorf(wx / 12.0f);
    outZ = (int)floorf(wz / 12.0f);
}

inline bool GetBasePanelPosition(float& outWX, float& outWY, float& outWZ, int& outGX, int& outGZ) {
    uintptr_t mapGrid = g_Base + RVA::ADDR_MapGrid;
    if (*(uint8_t*)(mapGrid + 0x1740) == 0) {
        return false;
    }
    outWY = *(float*)(mapGrid + 0x1750);
    outWX = *(float*)(mapGrid + 0x1754);
    outWZ = *(float*)(mapGrid + 0x1758);
    outGX = (int)floorf(outWX / 12.0f);
    outGZ = (int)floorf(outWZ / 12.0f);
    return true;
}

inline void SnapCursorToWorldPos(uintptr_t battleCtx, float wx, float wy, float wz) {
    uintptr_t cursor = g_Base + RVA::ADDR_MapCursor;
    *(float*)(cursor + 0x08) = wx;
    *(float*)(cursor + 0x0C) = wy;
    *(float*)(cursor + 0x10) = wz;
    if (battleCtx && fnUpdateCursorHover) {
        fnUpdateCursorHover((void*)battleCtx);
    }
}

inline void SnapCursorToUnit(uintptr_t battleCtx, uintptr_t unit) {
    if (!unit) return;
    float wx = *(float*)(unit + 0x08);
    float wy = *(float*)(unit + 0x0C);
    float wz = *(float*)(unit + 0x10);
    SnapCursorToWorldPos(battleCtx, wx, wy, wz);
}

inline void SnapCursorToGridTile(uintptr_t battleCtx, int gx, int gz) {
    float wx = (float)gx * 12.0f + 6.0f;
    float wz = (float)gz * 12.0f + 6.0f;
    float wy = fnGetTileHeight ? fnGetTileHeight(wx, wz) : 0.0f;
    SnapCursorToWorldPos(battleCtx, wx, wy, wz);
}

inline bool IsTileOccupiedByAnyUnit(int gx, int gz, int ignoreUnitIdx) {
    float bpWX = 0, bpWY = 0, bpWZ = 0;
    int bpGX = -1, bpGZ = -1;
    if (ignoreUnitIdx >= 0 && GetBasePanelPosition(bpWX, bpWY, bpWZ, bpGX, bpGZ)) {
        // Never treat the Base Panel tile as a valid destination tile for moving a unit
        if (gx == bpGX && gz == bpGZ) return true;
    }
    for (int i = 0; i < 256; ++i) {
        if (i == ignoreUnitIdx) continue;
        uintptr_t u = GetUnitPtr(i);
        if (!u) continue;
        if (*(uint8_t*)(u + 0x2E8) == 0) continue;
        uintptr_t cd = *(uintptr_t*)(u + 0x240);
        if (!cd) continue;
        if (*(int64_t*)(cd + 0x2CC8) <= 0) continue;
        int ux = 0, uz = 0;
        GetUnitGridPos(u, ux, uz);
        if (ux == gx && uz == gz) return true;
    }
    return false;
}

inline int CountControllablePlayerUnits() {
    int count = 0;
    for (int i = 0; i < 256; ++i) {
        uintptr_t u = GetUnitPtr(i);
        if (!IsUnitAliveAndDeployed(u)) continue;
        if (fnIsControllableUnit((void*)u, 0)) {
            count++;
        }
    }
    return count;
}

inline int FindNearestEnemy(int fromX, int fromZ, int& outEnemyX, int& outEnemyZ) {
    int bestIdx = -1;
    int bestDist = 999999;
    for (int i = 0; i < 256; ++i) {
        uintptr_t u = GetUnitPtr(i);
        if (!IsUnitAliveAndDeployed(u)) continue;
        uintptr_t cd = *(uintptr_t*)(u + 0x240);
        if (!cd || fnIsMapObject((void*)cd) != 0) continue;
        int faction = fnGetUnitFaction((void*)u, 1);
        if (IsEnemyFaction(faction)) {
            int ex = 0, ez = 0;
            GetUnitGridPos(u, ex, ez);
            int dist = abs(ex - fromX) + abs(ez - fromZ);
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = i;
                outEnemyX = ex;
                outEnemyZ = ez;
            }
        }
    }
    return bestIdx;
}

inline int FindNextControllablePlayerUnit() {
    for (int i = 0; i < 256; ++i) {
        if (g_UnitDoneThisTurn[i]) continue;
        uintptr_t u = GetUnitPtr(i);
        if (!IsUnitAliveAndDeployed(u)) continue;
        if (!fnIsControllableUnit((void*)u, 0)) continue;

        // +2F0 links a carried child. Actions live in the battle queue.
        const uintptr_t context = *(uintptr_t*)(g_Base + RVA::PTR_BattleContext);
        const int queueCount = context ? *(int32_t*)(context + 0x950) : 0;
        bool queued = false;
        if (queueCount >= 0 && queueCount <= 256) {
            for (int q = 0; q < queueCount && !queued; ++q) {
                const auto action = g_Base + 0x1E4FF80 + q * 0x27730;
                const int count = *(int16_t*)(action + 0x1310);
                if (count < 1 || count > 10) continue;
                for (int a = 0; a < count; ++a)
                    if (*(int16_t*)(action + a * 0x1E8) == i) { queued = true; break; }
            }
        }
        if (queued) {
            g_UnitDoneThisTurn[i] = true;
            continue;
        }
        bool canMove = !g_UnitMovedThisTurn[i] && fnCanUnitMove((void*)u);
        bool canAct  = fnHasActionRemaining((void*)u);
        if (!canMove && !canAct) {
            g_UnitDoneThisTurn[i] = true;
            continue;
        }
        return i;
    }
    return -1;
}

// ComputePathToTile uses the unit's pathLength as an in/out field and writes
// its path arrays. Movement selection only needs the result for scoring, so
// keep the engine's live unit path state unchanged after a probe.
constexpr uintptr_t UNIT_PATH_STATE_OFFSET = 0x416;
constexpr size_t UNIT_PATH_STATE_SIZE = 0x204; // length + 128 X and Z coordinates

inline bool ComputedPathEndsAtTile(uintptr_t unit, int targetX, int targetZ) {
    int16_t pathLength = *(int16_t*)(unit + 0x416);
    // ConfirmMoveToTile indexes the destination at path[pathLength], so that
    // endpoint must remain inside the 128-element arrays.
    if (pathLength <= 0 || pathLength >= 128) return false;

    const int16_t* pathX = (const int16_t*)(unit + 0x41A);
    const int16_t* pathZ = (const int16_t*)(unit + 0x51A);
    return pathX[pathLength] == targetX && pathZ[pathLength] == targetZ;
}

inline bool ChooseBestMoveTileForUnit(int unitIdx, int& outTargetX, int& outTargetZ) {
    uintptr_t u = GetUnitPtr(unitIdx);
    if (!u) return false;

    int ux = 0, uz = 0;
    GetUnitGridPos(u, ux, uz);

    float bpWX = 0, bpWY = 0, bpWZ = 0;
    int bpGX = -1, bpGZ = -1;
    bool onBasePanel = false;
    if (GetBasePanelPosition(bpWX, bpWY, bpWZ, bpGX, bpGZ)) {
        if (ux == bpGX && uz == bpGZ) {
            onBasePanel = true;
        }
    }

    int ex = ux, ez = uz;
    int enemyIdx = FindNearestEnemy(ux, uz, ex, ez);
    if (enemyIdx < 0 && !onBasePanel) return false;

    uint8_t originalPathState[UNIT_PATH_STATE_SIZE];
    memcpy(originalPathState, (const void*)(u + UNIT_PATH_STATE_OFFSET), sizeof(originalPathState));

    // Populate pathGrid and MoveCostGrid (0xB76A30) for this unit's movement capability
    uint32_t packedXZ = 0;
    fnGetUnitPackedXZ((void*)u, &packedXZ);
    fnComputeMoveRange(nullptr, (void*)(g_Base + RVA::ADDR_PathGrid), (void*)u, packedXZ, 0);
    memcpy((void*)(u + UNIT_PATH_STATE_OFFSET), originalPathState, sizeof(originalPathState));

    int maxMove = fnGetUnitMoveStat ? fnGetUnitMoveStat((void*)u) : 6;
    if (maxMove <= 0) maxMove = 6;

    int bestX = ux;
    int bestZ = uz;
    int bestEnemyDist = onBasePanel ? 999999 : (abs(ux - ex) + abs(uz - ez));

    // Search candidate tiles within radius 14 around unit
    for (int dz = -14; dz <= 14; ++dz) {
        for (int dx = -14; dx <= 14; ++dx) {
            int tx = ux + dx;
            int tz = uz + dz;
            if (tx < 0 || tz < 0 || tx >= 128 || tz >= 128) continue;
            if (tx == ux && tz == uz) continue;
            if (IsTileOccupiedByAnyUnit(tx, tz, unitIdx)) continue;

            // Check MoveCostGrid (0xB76A30) and TileFlagGrid (0xB7AA30) populated by ComputeMoveRange
            uint8_t moveCost = *(uint8_t*)(g_Base + RVA::ADDR_MoveCostGrid + (uintptr_t)tx * 128 + tz);
            uint8_t tileFlag = *(uint8_t*)(g_Base + RVA::ADDR_TileFlagGrid + (uintptr_t)tx * 128 + tz);
            if (moveCost == 0 || moveCost > (uint8_t)maxMove || moveCost >= 0x80) continue;
            if (tileFlag >= 0xFC) continue;

            int distToEnemy = (enemyIdx >= 0) ? (abs(tx - ex) + abs(tz - ez)) : (abs(dx) + abs(dz));
            if (enemyIdx >= 0 && distToEnemy == 0) continue; // Cannot stand on enemy tile

            if (distToEnemy < bestEnemyDist) {
                // Each candidate is an isolated query. The engine writes path
                // length and path coordinates into the live BattleUnit.
                memcpy((void*)(u + UNIT_PATH_STATE_OFFSET), originalPathState, sizeof(originalPathState));
                // Match the native move-confirm setup (RVA 0x5FD843):
                // capacity 128 and a cleared path cursor in the adjacent word.
                *(int32_t*)(u + 0x416) = 128;
                bool computed = fnComputePathToTile((void*)u, tx, tz, 0);
                bool pathEndsAtTarget = computed && ComputedPathEndsAtTile(u, tx, tz);
                memcpy((void*)(u + UNIT_PATH_STATE_OFFSET), originalPathState, sizeof(originalPathState));
                if (pathEndsAtTarget) {
                    bestEnemyDist = distToEnemy;
                    bestX = tx;
                    bestZ = tz;
                }
            }
        }
    }

    // The path routine is used only to validate candidate tiles. The caller
    // receives coordinates and must never inherit a probe's unit-path writes.
    memcpy((void*)(u + UNIT_PATH_STATE_OFFSET), originalPathState, sizeof(originalPathState));
    if (bestX == ux && bestZ == uz) {
        return false;
    }
    outTargetX = bestX;
    outTargetZ = bestZ;
    return true;
}

} // namespace AutoPlay
