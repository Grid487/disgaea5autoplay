#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>

// XInput structures & constants
struct XINPUT_GAMEPAD_STATE {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XINPUT_STATE_DATA {
    DWORD                dwPacketNumber;
    XINPUT_GAMEPAD_STATE Gamepad;
};

#define XBTN_BACK        0x0020
#define XBTN_LEFT_THUMB  0x0040
#define XBTN_RIGHT_THUMB 0x0080

// Game Action IDs (index into KeyConfig at +0x50 + actionId * 8)
enum GameActionId : int32_t {
    ACT_UP           = 0,
    ACT_LEFT         = 1,
    ACT_DOWN         = 2,
    ACT_RIGHT        = 3,
    ACT_CONFIRM      = 4,
    ACT_CANCEL       = 5,
    ACT_SPECIAL_MENU = 45, // 0x2D (Triangle / Y)
    ACT_EXEC_SHORT   = 48, // 0x30 (Execute Actions shortcut)
    ACT_END_SHORT    = 49, // 0x31 (End Turn shortcut)
};

// RVAs relative to disgaea5.exe ImageBase (0x140000000)
namespace RVA {
    constexpr uintptr_t VTABLE_BattleMainTask   = 0x98BF00;
    constexpr uintptr_t FN_BattleMain_Execute   = 0x4AD390;
    constexpr uintptr_t FN_CommandEndTurn       = 0x4A7150;
    constexpr uintptr_t FN_UpdateCursorHover    = 0x4AFC50;
    constexpr uintptr_t FN_GetTileHeight        = 0x1CF4D0;
    constexpr uintptr_t FN_IsControllableUnit   = 0x5F74B0;
    constexpr uintptr_t FN_IsMapObject          = 0x21F690;
    constexpr uintptr_t FN_GetUnitFaction       = 0x15B910;
    constexpr uintptr_t FN_CanUnitMove          = 0x4CE3A0;
    constexpr uintptr_t FN_GetUnitMoveStat      = 0x157D90;
    constexpr uintptr_t FN_HasActionRemaining   = 0x156AC0;
    constexpr uintptr_t FN_CanUnitAttack        = 0x4CE300;
    constexpr uintptr_t FN_GetUnitPackedXZ      = 0x151DC0;
    constexpr uintptr_t FN_ComputeMoveRange     = 0x2D5940;
    constexpr uintptr_t FN_ComputePathToTile    = 0x2D82A0;
    constexpr uintptr_t FN_PopulateDeployUnits  = 0x53B720;
    constexpr uintptr_t FN_PlaySoundEffect      = 0x227430;

    constexpr uintptr_t PTR_KeyConfig           = 0xBD7B30;
    constexpr uintptr_t PTR_InputMgrSys         = 0xBD7B40;
    constexpr uintptr_t PTR_BattleContext       = 0x7B58E70;

    constexpr uintptr_t ADDR_WindowFocused      = 0x7A69F29;
    constexpr uintptr_t ADDR_InputDeviceMode    = 0x7B2474C;
    constexpr uintptr_t ADDR_KeyJustPressed     = 0x7B24AE0;
    constexpr uintptr_t ADDR_KeyHeld            = 0x7B24BE0;
    constexpr uintptr_t ADDR_KeyRepeat          = 0x7B24DE0;

    constexpr uintptr_t ADDR_MoveCostGrid       = 0xB76A30;
    constexpr uintptr_t ADDR_TileFlagGrid       = 0xB7AA30;
    constexpr uintptr_t ADDR_BattleUnitArray    = 0x1BA33B0;
    constexpr uintptr_t ADDR_MapCursor          = 0x1E2CD60;
    constexpr uintptr_t ADDR_PathGrid           = 0x4BD9750;
    constexpr uintptr_t ADDR_MapGrid            = 0x4FAAF80;
}

// Engine function typedefs
using FnBattleExecute       = void(__fastcall*)(void* mainTask, int callType);
using FnCommandEndTurn      = void(__fastcall*)(void* mainTask);
using FnUpdateCursorHover   = void(__fastcall*)(void* battleContext);
using FnGetTileHeight       = float(__fastcall*)(float worldX, float worldZ);
using FnIsControllableUnit  = bool(__fastcall*)(void* unit, int faction);
using FnIsMapObject         = int(__fastcall*)(void* charaData);
using FnGetUnitFaction      = int(__fastcall*)(void* unit, uint8_t flag);
using FnCanUnitMove         = bool(__fastcall*)(void* unit);
using FnGetUnitMoveStat     = int(__fastcall*)(void* unit);
using FnHasActionRemaining  = bool(__fastcall*)(void* unit);
using FnCanUnitAttack       = bool(__fastcall*)(void* unit, int validTargetCount, uint8_t alreadyQueued);
using FnGetUnitPackedXZ     = void(__fastcall*)(void* unit, uint32_t* outPackedXZ);
using FnComputeMoveRange    = void(__fastcall*)(void* rcxUnused, void* pathGrid, void* unit, uint32_t packedXZ, int flags);
using FnComputePathToTile   = bool(__fastcall*)(void* unit, int targetX, int targetZ, uint8_t flag);
using FnPopulateDeployUnits = void(__fastcall*)(void* battleContext);
using FnPlaySoundEffect     = void(__fastcall*)(int seId, int a2, float vol1, float vol2, float a5);
