#include "autoplay_ai.h"
#include "team_planning.h"
#include "native_tactics.h"
#include "native_effects.h"
#include "native_action_positions.h"
#include "native_deployment.h"
#include "native_notifications.h"
#include "native_repeat.h"
#ifdef D5_NATIVE_NOTIFICATION_QA
#include "native_notification_qa.h"
#endif
#include <atomic>
#include <stdarg.h>
#include <share.h>

namespace AutoPlay {

static FILE* g_LogFile = nullptr;
static HWND  g_GameHwnd = nullptr;
static bool  g_PrevToggleKey = false;
static bool  g_PrevPadCombo  = false;
static uintptr_t g_NotificationBattleTask = 0;
static NativeNotifications::Update orig_NotificationUpdate = nullptr;
static FnBattleExecute orig_SelectorExecute = nullptr;
using FnEntryChoiceUpdate = uint32_t (__fastcall*)(void*);
static FnEntryChoiceUpdate orig_EntryChoiceUpdate = nullptr;
static uintptr_t g_EntryChoiceSentObject = 0;
static int g_EntryChoiceSentAttempt = 0;
static NativeScenes::Update orig_BaseSceneUpdate = nullptr;
static std::atomic<bool> g_RepeatRequested{false};
static StageRepeat::ToggleLatch g_RepeatKey;
static int32_t g_LastSubState = -1;
static int32_t g_ActionAttemptTicks = 0;
static uint8_t g_MoveAttemptsThisTurn[256] = {};
static bool g_PendingMove = false;
static bool g_PendingMoveConfirmSent = false;
static int32_t g_PendingMoveUnit = -1;
static int32_t g_PendingMoveFromX = -1;
static int32_t g_PendingMoveFromZ = -1;
static int32_t g_PendingMoveToX = -1;
static int32_t g_PendingMoveToZ = -1;
static float g_PendingMovePriorTargetWX = 0.0f;
static float g_PendingMovePriorTargetWZ = 0.0f;
static bool g_PendingDeployment = false;
static int32_t g_PendingDeploymentCount = 0;
static int32_t g_PendingDeploymentTicks = 0;
static int g_CombatUiPhase = -1;
static int g_CombatUiFrames = 0;
static NativeActionPositions::GoalJob g_GoalJob;
static NativeTactics::MovementJob g_MovementJob;
static NativeDeployment::Job g_DeploymentJob;
static int g_TacticalActor = -1;
static bool g_TacticalRouteStarted = false;
static bool g_DeploymentPlanning = false;
static int g_DeploymentFailures = 0;
static bool g_SkillTableReady = false;
static bool g_SkillTableFallbackLogged = false;
static Planning::Telemetry g_GoalTiming, g_MoveTiming, g_DeploymentTiming;
static void ReportPlanning(const char* phase, const Planning::Telemetry& timing, const char* reason) {
    LogMsg("[Planning] phase=%s actor=%d elapsed=%.2fms cpu=%.2fms active=%.2fms frames=%u candidates=%u effects=%u paths=%u work=%u reuse=%u cursor=%lld/%lld/%lld/%lld yield=%s reason=%s",
        phase, timing.actor, timing.elapsed / 1000000.0, timing.cpu / 1000000.0,
        timing.activeTime / 1000000.0,
        timing.frames, timing.calls.previews, timing.calls.effects, timing.calls.paths,
        timing.calls.work, timing.reuses,
        static_cast<long long>(timing.cursor[0]), static_cast<long long>(timing.cursor[1]),
        static_cast<long long>(timing.cursor[2]), static_cast<long long>(timing.cursor[3]),
        Planning::YieldName(timing.yield), reason);
#ifdef D5_QUERY_PROFILE
    const auto& q=timing.queries;
    LogMsg("[PlanningCost] phase=%s reason=%s memory=%llu/%.3fms enumerate=%llu/%.3fms rosterSkill=%llu/%.3fms rosterScore=%llu/%.3fms fingerprint=%llu/%.3fms source=%llu/%.3fms",
        phase,reason,q[0].count,q[0].ns/1000000.0,q[1].count,q[1].ns/1000000.0,
        q[2].count,q[2].ns/1000000.0,q[3].count,q[3].ns/1000000.0,
        q[4].count,q[4].ns/1000000.0,q[6].count,q[6].ns/1000000.0);
#endif
}
static void DriveTacticalPlanning(uintptr_t mainTask, uintptr_t context, int actor, bool destinationOpen = false);

static bool SmartCombatAvailable() { return NativeActions::evaluateEffect != nullptr && g_SkillTableReady; }
static void ResetCombatDriver() { g_CombatUiPhase = -1; g_CombatUiFrames = 0; }

static void RejectCombatAction(const char* reason) {
    SmartCombat::Fail(reason);
    TeamPlanning::Reset(reason);
    ResetCombatDriver();
    g_TacticalActor = -1;
    g_TacticalRouteStarted = false;
    g_DeploymentPlanning = false;
    g_PulseAction = ACT_CANCEL;
    g_CooldownTicks = 6;
}

static bool BeginCombatAction(uintptr_t context) {
    if (!SmartCombat::Begin(TeamPlanning::current.best, context)) return false;
    ResetCombatDriver();
    return true;
}

static bool IsValidUnitIndex(int32_t idx) {
    return idx >= 0 && idx < 256;
}

static void ResetAutoPlayTurnTracking() {
    Planning::Report("goals", g_GoalTiming, "turn reset");
    Planning::Report("movement", g_MoveTiming, "turn reset");
    Planning::Report("deployment", g_DeploymentTiming, "turn reset");
    g_GoalTiming = {}; g_MoveTiming = {}; g_DeploymentTiming = {};
    ResetTurnTracking();
    SmartCombat::Reset();
    TeamPlanning::Reset();
    ResetCombatDriver();
    g_TacticalActor = -1;
    g_TacticalRouteStarted = false;
    g_DeploymentPlanning = false;
    g_DeploymentFailures = 0;
    g_SkillTableFallbackLogged = false;
    for (int i = 0; i < 256; ++i) g_MoveAttemptsThisTurn[i] = 0;
    g_LastSubState = -1;
    g_ActionAttemptTicks = 0;
    g_PendingMove = false;
    g_PendingMoveConfirmSent = false;
    g_PendingMoveUnit = -1;
    g_PendingMoveFromX = g_PendingMoveFromZ = -1;
    g_PendingMoveToX = g_PendingMoveToZ = -1;
    g_PendingDeployment = false;
    g_PendingDeploymentTicks = 0;
}

static void BeginPendingMove(int32_t unitIdx, int targetX, int targetZ) {
    if (!IsValidUnitIndex(unitIdx)) return;
    uintptr_t unit = GetUnitPtr(unitIdx);
    if (!unit) return;
    GetUnitGridPos(unit, g_PendingMoveFromX, g_PendingMoveFromZ);
    g_PendingMovePriorTargetWX = *(float*)(unit + 0x34);
    g_PendingMovePriorTargetWZ = *(float*)(unit + 0x3C);
    g_PendingMoveUnit = unitIdx;
    g_PendingMoveToX = targetX;
    g_PendingMoveToZ = targetZ;
    g_PendingMove = true;
    g_PendingMoveConfirmSent = false;
    g_ActionAttemptTicks = 0;
}

static void ClearPendingMove() {
    g_PendingMove = false;
    g_PendingMoveConfirmSent = false;
    g_PendingMoveUnit = -1;
    g_PendingMoveFromX = g_PendingMoveFromZ = -1;
    g_PendingMoveToX = g_PendingMoveToZ = -1;
    g_PendingMovePriorTargetWX = 0.0f;
    g_PendingMovePriorTargetWZ = 0.0f;
    g_ActionAttemptTicks = 0;
}

static void FinishPendingMoveIfObserved(uintptr_t mainTask, uintptr_t battleCtx, int32_t state, bool forceExit = false) {
    if (!g_PendingMove || !g_PendingMoveConfirmSent || !IsValidUnitIndex(g_PendingMoveUnit)) return;
    int32_t subState = -1;
    if (state == 0x02) subState = *(int32_t*)(mainTask + 0x11E0 + 0x14);
    // State 06 is the native movement animation, not a rejected confirm.
    // Observe acceptance only once the engine returns to its stable cursor.
    if (!forceExit && (state != 0x02 || subState != 0)) return;
    if (*(int32_t*)(mainTask + 0x2C) == 2) return;

    uintptr_t unit = GetUnitPtr(g_PendingMoveUnit);
    int nowX = -1, nowZ = -1;
    if (unit) GetUnitGridPos(unit, nowX, nowZ);
    const float targetWorldX = unit ? *(float*)(unit + 0x34) : 0.0f;
    const float targetWorldZ = unit ? *(float*)(unit + 0x3C) : 0.0f;
    const int acceptedTargetX = unit ? (int)floorf(targetWorldX / 12.0f) : -1;
    const int acceptedTargetZ = unit ? (int)floorf(targetWorldZ / 12.0f) : -1;
    const bool targetUpdated = fabsf(targetWorldX - g_PendingMovePriorTargetWX) > 0.01f ||
                               fabsf(targetWorldZ - g_PendingMovePriorTargetWZ) > 0.01f;
    const bool moved = unit &&
        ((nowX != g_PendingMoveFromX || nowZ != g_PendingMoveFromZ) ||
         (targetUpdated && acceptedTargetX == g_PendingMoveToX && acceptedTargetZ == g_PendingMoveToZ));
    if (moved) {
        g_UnitMovedThisTurn[g_PendingMoveUnit] = true;
        TeamPlanning::Reset("accepted movement");
        LogMsg("[Move] Accepted unit idx=%d from (%d,%d) current=(%d,%d), target=(%d,%d)",
               g_PendingMoveUnit, g_PendingMoveFromX, g_PendingMoveFromZ, nowX, nowZ,
               g_PendingMoveToX, g_PendingMoveToZ);
    } else {
        uint8_t attempts = g_MoveAttemptsThisTurn[g_PendingMoveUnit];
        LogMsg("[Move] Unit idx=%d did not move after confirm (attempt=%u, state=0x%02X)",
               g_PendingMoveUnit, attempts, state);
        if (attempts >= 2) g_UnitDoneThisTurn[g_PendingMoveUnit] = true;
    }
    ClearPendingMove();
}

static void ObservePendingDeployment(uintptr_t battleCtx, int32_t state) {
    if (!g_PendingDeployment) return;
    const int32_t deployed = CountControllablePlayerUnits();
    if (deployed > g_PendingDeploymentCount) {
        const int32_t added = deployed - g_PendingDeploymentCount;
        g_DeployCountThisStage += added;
        g_BasePanelExhaustedThisTurn = false;
        TeamPlanning::Reset("deployment changed");
        LogMsg("[Deploy] Observed %d new deployed player unit(s); stage count=%d",
               added, g_DeployCountThisStage);
        g_PendingDeployment = false;
        g_PendingDeploymentTicks = 0;
        return;
    }
    if (state == 0x0C || state == 0x11 || state == 0x12 || state == 0x05) {
        if (++g_PendingDeploymentTicks <= 30) return;
    }
    g_PendingDeployment = false;
    g_PendingDeploymentTicks = 0;
    if (++g_DeploymentFailures >= 2) g_BasePanelExhaustedThisTurn = true;
}

static bool GetPendingOrBestMoveTarget(int32_t unitIdx, int& targetX, int& targetZ) {
    if (g_PendingMove && g_PendingMoveUnit == unitIdx) {
        targetX = g_PendingMoveToX;
        targetZ = g_PendingMoveToZ;
        return true;
    }
    if (!ChooseBestMoveTileForUnit(unitIdx, targetX, targetZ)) return false;
    BeginPendingMove(unitIdx, targetX, targetZ);
    return g_PendingMove;
}

static void DriveMoveDestination(uintptr_t mainTask, uintptr_t battleCtx, int32_t unitIdx) {
    if (!IsValidUnitIndex(unitIdx)) {
        ClearPendingMove();
        g_PulseAction = ACT_CANCEL;
        g_CooldownTicks = 8;
        LogMsg("[Move] Invalid selected unit idx=%d; cancelling", unitIdx);
        return;
    }
    if (SmartCombatAvailable() && !g_PendingMove) {
        if (!TeamPlanning::Step(battleCtx)) { g_StateStuckTicks = 0; return; }
        if (TeamPlanning::current.requiresQueueExecution) {
            g_PulseAction = ACT_CANCEL; g_CooldownTicks = 5; return;
        }
        DriveTacticalPlanning(mainTask, battleCtx, unitIdx, true);
        return;
    }
    if (g_StateStuckTicks > 25) {
        if (g_PendingMoveConfirmSent) {
            FinishPendingMoveIfObserved(mainTask, battleCtx, *(int32_t*)(mainTask + 0x14), true);
        }
        if (IsValidUnitIndex(unitIdx) && !g_UnitMovedThisTurn[unitIdx] &&
            g_MoveAttemptsThisTurn[unitIdx] >= 2)
            g_UnitDoneThisTurn[unitIdx] = true;
        if (g_PendingMove) ClearPendingMove();
        g_PulseAction = ACT_CANCEL;
        g_CooldownTicks = 8;
        LogMsg("[Move] Destination selection timed out for unit idx=%d; cancelling", unitIdx);
        return;
    }

    int targetX = 0, targetZ = 0;
    if (!GetPendingOrBestMoveTarget(unitIdx, targetX, targetZ)) {
        if (IsValidUnitIndex(unitIdx)) g_UnitDoneThisTurn[unitIdx] = true;
        g_PulseAction = ACT_CANCEL;
        g_CooldownTicks = 8;
        LogMsg("[Move] No valid destination for unit idx=%d; cancelling", unitIdx);
        return;
    }
    if (g_PendingMoveConfirmSent) return; // Wait for the engine to leave destination selection.

    SnapCursorToGridTile(battleCtx, targetX, targetZ);
    if (++g_ActionAttemptTicks >= 2) {
        g_PendingMoveConfirmSent = true;
        ++g_MoveAttemptsThisTurn[unitIdx];
        g_PulseAction = ACT_CONFIRM;
        g_CooldownTicks = 10;
        g_ActionAttemptTicks = 0;
        LogMsg("[Move] Confirming unit idx=%d destination (%d,%d)", unitIdx, targetX, targetZ);
    }
}

void LogMsg(const char* fmt, ...) {
    if (!g_LogFile) {
        g_LogFile = _fsopen("autoplay_mod.log", "a", _SH_DENYNO);
    }
    if (g_LogFile) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_LogFile, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        vfprintf(g_LogFile, fmt, args);
        va_end(args);
        fprintf(g_LogFile, "\n");
        fflush(g_LogFile);
    }
}

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
        char title[256] = { 0 };
        GetWindowTextA(hwnd, title, sizeof(title));
        if (title[0] != '\0') {
            g_GameHwnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static void UpdateWindowTitleIndicator() {
    if (!g_GameHwnd || !IsWindow(g_GameHwnd)) {
        EnumWindows(EnumWindowsCallback, 0);
    }
    if (g_GameHwnd && IsWindow(g_GameHwnd)) {
        char newTitle[160];
        snprintf(newTitle, sizeof(newTitle), "Disgaea 5 Complete [AUTO-PLAY: %s (F1)] [REPEAT: %s (F2)]",
                 g_Enabled ? "ON" : "OFF", g_RepeatRequested.load() ? "ON" : "OFF");
        SetWindowTextA(g_GameHwnd, newTitle);
    }
}

static void ToggleAutoPlay(const char* source) {
    g_Enabled = !g_Enabled;
    g_CooldownTicks = 0;
    g_StateStuckTicks = 0;
    g_ActionAttemptTicks = 0;
    ResetAutoPlayTurnTracking();
    UpdateWindowTitleIndicator();
    if (fnPlaySoundEffect) {
        fnPlaySoundEffect(g_Enabled ? 1 : 2, 0, 1.0f, 1.0f, 0.0f);
    }
    LogMsg("Auto-Play toggled %s via %s", g_Enabled ? "ON" : "OFF", source);
}

static void PollHotkeys(WORD padButtons) {
    bool f1Down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (f1Down && !g_PrevToggleKey) {
        ToggleAutoPlay("Keyboard F1");
    }
    g_PrevToggleKey = f1Down;
    if (g_RepeatKey.Pressed((GetAsyncKeyState(VK_F2) & 0x8000) != 0)) {
        const bool enabled = !g_RepeatRequested.load();
        g_RepeatRequested.store(enabled);
        UpdateWindowTitleIndicator();
        if (fnPlaySoundEffect) fnPlaySoundEffect(enabled ? 1 : 2, 0, 1.0f, 1.0f, 0.0f);
        LogMsg("[Repeat] F2 toggled %s independently of F1=%d", enabled ? "ON" : "OFF", g_Enabled ? 1 : 0);
    }

    bool padCombo = ((padButtons & (XBTN_BACK | XBTN_RIGHT_THUMB)) == (XBTN_BACK | XBTN_RIGHT_THUMB));
    if (padCombo && !g_PrevPadCombo) {
        ToggleAutoPlay("Controller Back+R3");
    }
    g_PrevPadCombo = padCombo;
}

static uint64_t GetActionVirtualKey(int32_t actionId) {
    if (actionId < 0 || actionId > 0x79) return 0;
    uintptr_t keyCfg = *(uintptr_t*)(g_Base + RVA::PTR_KeyConfig);
    if (!keyCfg) return 0;
    uint64_t vk = *(uint64_t*)(keyCfg + (uintptr_t)actionId * 8 + 0x50);
    if (vk < 1 || vk > 0xFE) {
        vk = 0xFD;
        *(uint64_t*)(keyCfg + (uintptr_t)actionId * 8 + 0x50) = vk;
    }
    return vk;
}

static bool DrivePlannedAction(uintptr_t mainTask, uintptr_t context, int state) {
    if (!SmartCombat::pending.valid) return false;
    if (SmartCombat::confirmationSent) return true;
    if (!Planning::Available()) return true;
    const auto& action = SmartCombat::pending.candidate;
    const int actor = *(int32_t*)(context + 0xB8);
    if (actor != action.actorIndex || !SmartCombat::RevalidateCommon(context, action) ||
        TeamPlanning::current.fingerprint != TeamPlanning::Fingerprint(context)) {
        RejectCombatAction("actor, targets or battle state changed");
        return true;
    }
    const auto* skill = action.kind == Combat::ActionKind::BasicAttack ? nullptr :
        TeamPlanning::FindSkill(actor, action.menuIndex);
    if (action.kind != Combat::ActionKind::BasicAttack && !skill) {
        RejectCombatAction("learned skill disappeared"); return true;
    }
    const uintptr_t menu = mainTask + 0x196C78;
    const int inner = state == 0x0E ? *(int32_t*)(menu + 0x14) : 0;
    const int phase = state * 16 + inner;
    if (phase != g_CombatUiPhase) { g_CombatUiPhase = phase; g_CombatUiFrames = 0; }
    if (++g_CombatUiFrames > 45) { RejectCombatAction("menu or target confirmation rejected"); return true; }
    if (state == 0x03) {
        if (g_CombatUiFrames != 2) return true;
        *(int32_t*)(menu + 0x128) = skill ? 2 : 1;
        g_PulseAction = ACT_CONFIRM;
        g_CooldownTicks = 4;
        return true;
    }
    if (state == 0x0E) {
        if (!skill) { RejectCombatAction("unexpected skill menu for basic attack"); return true; }
        if (*(int32_t*)(menu + 0x2C) == 2 || g_CombatUiFrames < 3) return true;
        if (inner == 1 && g_CombatUiFrames == 3) {
            if (!NativeSkills::SelectSkillCursor(menu, *skill)) {
                RejectCombatAction("skill menu row stale or disabled"); return true;
            }
            g_PulseAction = ACT_CONFIRM;
            g_CooldownTicks = 4;
        } else if (inner == 2 && g_CombatUiFrames == 3) {
            if (skill->shapeId || NativeActions::ConfiguredShape(actor, *skill) != action.shapeId ||
                action.shapeId < 1 || action.shapeId > skill->maxShapeChoices ||
                *(int32_t*)(context + 0x9000) != skill->slot ||
                *(uintptr_t*)(context + 0x9008) != skill->descriptor) {
                RejectCombatAction("spell area or learned row changed"); return true;
            }
            // Reuse the player's existing selected spell area. This field is
            // transient targeting UI state; the learned row is never written.
            *(int32_t*)(context + 0x9014) = action.shapeId - 1;
            g_PulseAction = ACT_CONFIRM;
            g_CooldownTicks = 4;
        }
        return true;
    }
    if (state == 0x08 && !skill) {
        const int count = *(int32_t*)(context + 0x94C);
        int row = -1;
        for (int i = 0; i < count && i < 256; ++i)
            if (*(uint16_t*)(context + 0x74C + i * 2) == action.targetIndex) { row = i; break; }
        if (row < 0) { RejectCombatAction("basic attack target left native list"); return true; }
        const auto list = mainTask + 0x4AD80;
        *(int32_t*)(list + 0xBE38) = row < 7 ? row : 6;
        *(int32_t*)(list + 0xBE3C) = row < 7 ? 0 : row - 6;
        SnapCursorToUnit(context, GetUnitPtr(action.targetIndex));
        if (g_CombatUiFrames < 3) return true;
    } else if (state == 0x10 && skill) {
        if (*(int32_t*)(context + 0x9000) != skill->slot ||
            *(uintptr_t*)(context + 0x9008) != skill->descriptor ||
            *(int32_t*)(context + 0x9014) != action.shapeId) {
            RejectCombatAction("targeting skill identity changed"); return true;
        }
        *(int32_t*)(context + 0x901C) = action.rotation;
        SnapCursorToGridTile(context, action.targetX, action.targetZ);
        if (g_CombatUiFrames < 3) return true;
        const auto task = mainTask + 0x24EDD0;
        if (*(uint32_t*)(task + 8) != 0 || *(uint8_t*)(task + 0x27829) != 0 ||
            !NativeCombat::QueueMatches(task + 0xC0, action)) {
            RejectCombatAction("native target or affected area rejected"); return true;
        }
    } else return true;
    // Query the private preview once more immediately before confirmation.
    Planning::Unit confirmation(Planning::Resource::Preview);
    if (!confirmation) return true;
    auto stateNow = NativeCombat::CaptureSnapshot(context, SmartCombat::accepted);
    Combat::ActionCandidate refreshed;
    if (!NativeActions::BuildPreview(TeamPlanning::current.preview, stateNow, actor, skill,
        action.shapeId, action.targetX, action.targetZ, action.rotation, refreshed, false) ||
        refreshed.spCost != action.spCost ||
        !NativeCombat::QueueMatches((uintptr_t)TeamPlanning::current.preview.bytes.data(), action)) {
        RejectCombatAction("cost, targets or area changed before confirmation"); return true;
    }
    SmartCombat::ConfirmSent(context);
    g_PulseAction = ACT_CONFIRM;
    g_CooldownTicks = 5;
    return true;
}

static void FinishSmartUnit(int actor) {
    if (IsValidUnitIndex(actor)) g_UnitDoneThisTurn[actor] = true;
    g_TacticalActor = -1;
    g_TacticalRouteStarted = false;
    // A completed search proving there is no immediate team action is still
    // valid when this unit cannot move. Keep its catalogs for the next unit.
    if (!TeamPlanning::current.complete || TeamPlanning::current.best.valid)
        TeamPlanning::Reset("unit planning finished");
    else ++TeamPlanning::current.timing.reuses;
    g_PulseAction = ACT_CANCEL;
    g_CooldownTicks = 5;
}

static void DriveTacticalPlanning(uintptr_t mainTask, uintptr_t context, int actor, bool destinationOpen) {
    if (g_UnitMovedThisTurn[actor] || g_MoveAttemptsThisTurn[actor] >= 2 ||
        !fnCanUnitMove((void*)GetUnitPtr(actor))) { FinishSmartUnit(actor); return; }
    if (g_TacticalActor != actor) {
        if (!Planning::Available()) return;
        Planning::Report("goals", g_GoalTiming, "actor changed");
        Planning::Report("movement", g_MoveTiming, "actor changed");
        g_GoalTiming = {}; g_MoveTiming = {};
        g_GoalTiming.actor = g_MoveTiming.actor = actor;
        const std::vector<NativeSkills::Entry>* skills = nullptr;
        for (size_t i = 0; i < TeamPlanning::current.actors.size(); ++i)
            if (TeamPlanning::current.actors[i] == actor) skills = &TeamPlanning::current.catalogs[i].entries;
        static const std::vector<NativeSkills::Entry> none;
        if (!g_GoalJob.Begin(actor, TeamPlanning::current.state, skills ? *skills : none)) {
            FinishSmartUnit(actor); return;
        }
        g_TacticalActor = actor;
        g_TacticalRouteStarted = false;
    }
    if (TeamPlanning::current.fingerprint != TeamPlanning::Fingerprint(context)) {
        g_TacticalActor = -1;
        TeamPlanning::Reset("tactical inputs changed");
        g_PulseAction = ACT_CANCEL;
        return;
    }
    g_StateStuckTicks = 0; // Work is explicitly budgeted across update frames.
    if (!Planning::Run("goals", g_GoalTiming, [] { return g_GoalJob.IsComplete(); },
        [] {
            g_GoalJob.Step();
            g_GoalTiming.cursor = {static_cast<int64_t>(g_GoalJob.Stage()),
                static_cast<int64_t>(g_GoalJob.ProcessedBasicPairs()),
                static_cast<int64_t>(g_GoalJob.ProcessedShapeRecords()),
                static_cast<int64_t>(g_GoalJob.ProcessedSeeds())};
        })) return;
    if (g_GoalJob.Failed()) { FinishSmartUnit(actor); return; }
    if (!g_TacticalRouteStarted) {
        if (!Planning::Available()) return;
        TacticalPath::CellMask reserved{};
        if (g_PendingMove && g_PendingMoveUnit != actor &&
            TacticalPath::InBounds(g_PendingMoveToX, g_PendingMoveToZ))
            reserved[TacticalPath::CellIndex(g_PendingMoveToX, g_PendingMoveToZ)] = 1;
        if (!g_MovementJob.Begin(actor, TeamPlanning::current.state, g_GoalJob.Goals(), &reserved)) {
            FinishSmartUnit(actor); return;
        }
        g_TacticalRouteStarted = true;
    }
    if (!Planning::Run("movement", g_MoveTiming, [] { return g_MovementJob.IsComplete(); },
        [] {
            g_MovementJob.Step();
            g_MoveTiming.cursor = {static_cast<int64_t>(g_MovementJob.Phase()),
                static_cast<int64_t>(g_MovementJob.LastGraphCellsProcessed()),
                static_cast<int64_t>(g_MovementJob.LastRouteNodesProcessed()),
                static_cast<int64_t>(g_MovementJob.LastEndpointCellsScanned())};
        })) return;
    int x = -1, z = -1;
    if (!g_MovementJob.GetResult(x, z)) {
        LogMsg("[Move] No useful native-legal action position for actor=%d", actor);
        FinishSmartUnit(actor); return;
    }
    BeginPendingMove(actor, x, z);
    g_TacticalActor = -1;
    if (destinationOpen) return;
    *(int32_t*)(mainTask + 0x196C78 + 0x128) = 0;
    g_PulseAction = ACT_CONFIRM;
    g_CooldownTicks = 5;
    LogMsg("[Move] Tactical actor=%d destination=(%d,%d) enables configured attack/healing range", actor, x, z);
}

static void StepBattleDecision(uintptr_t mainTask, uintptr_t battleCtx,
                               Planning::Clock clock = Planning::SteadyNow) {
    Planning::reporter = &ReportPlanning;
    NativeNotifications::ObserveQueue(g_Base);
    // Dialogs have their own update task. Freeze planner retries and inputs
    // until the native manager removes the modal, including unknown choices.
    if (NativeNotifications::BlocksBattlePlanning(g_Base)) {
        g_PulseAction = g_HoldAction = -1;
        return;
    }
    int32_t state = *(int32_t*)(mainTask + 0x14);
    int32_t transFlag = *(int32_t*)(mainTask + 0x2C);

    if (state != g_LastBattleState) {
        if (state == 0x15 || state == 0x1C) TeamPlanning::Reset("action execution");
        LogMsg("BattleState transition: 0x%02X -> 0x%02X", g_LastBattleState, state);
        g_LastBattleState = state;
        g_StateStuckTicks = 0;
        g_ActionAttemptTicks = 0;
    } else {
        g_StateStuckTicks++;
    }

    FinishPendingMoveIfObserved(mainTask, battleCtx, state);
    ObservePendingDeployment(battleCtx, state);
    if (SmartCombat::ObserveQueue(battleCtx)) TeamPlanning::Reset("queue acceptance");

    // Wait while state transition is pending
    if (transFlag == 2) return;

    // Hold Confirm during Enemy Turn (0x09), Action Execution (0x15), or Auto-Execute Before Turn End (0x1C)
    if (state == 0x09 || state == 0x15 || state == 0x1C) {
        if (state == 0x09) {
            ResetAutoPlayTurnTracking();
        }
        g_HoldAction = ACT_CONFIRM;
        return;
    }

    // Turn start / end banners
    if (state == 0x1A || state == 0x1B) {
        ResetAutoPlayTurnTracking();
        return;
    }

    if (NativeActions::evaluateEffect && (state == 0x02 || state == 0x03 || state == 0x05 ||
        state == 0x08 || state == 0x0C || state == 0x0E || state == 0x10)) {
        // Index at most 256 shape records per frame. All subsequent shape
        // reads in smart planning use the completed constant-time index.
        // Optional skill metadata must never block basic battle automation.
        const bool previouslyReady = g_SkillTableReady;
        g_SkillTableReady = NativeGeometry::StepShapeCache(256);
        if (!g_SkillTableReady) {
            if (previouslyReady) {
                SmartCombat::ClearPending();
                TeamPlanning::Reset();
                ResetCombatDriver();
                g_TacticalActor = -1;
                g_TacticalRouteStarted = false;
                g_DeploymentPlanning = false;
            }
            if (!g_SkillTableFallbackLogged) {
                LogMsg("[Combat] Skill area table unavailable or indexing; continuing basic autoplay");
                g_SkillTableFallbackLogged = true;
            }
        } else if (!previouslyReady || g_SkillTableFallbackLogged) {
            LogMsg("[Combat] Skill area table ready: records=%u; smart combat enabled",
                NativeGeometry::ShapeCacheRecordsIndexed());
            g_SkillTableFallbackLogged = false;
        }
    }

    if (g_CooldownTicks > 0) {
        g_CooldownTicks--;
        return;
    }

    // Preliminary state/queue/shape checks are not planning work. Starting the
    // timer before them can exhaust every frame before a job even starts.
    Planning::PlanningBudget frameBudget(clock);
    Planning::Scope planningScope(frameBudget);

    switch (state) {
    case 0x32: // Pre-Battle Fight Menu ("Battle Start" is default index 0)
        ResetAutoPlayTurnTracking();
        g_DeployCountThisStage = 0;
        g_PulseAction = ACT_CONFIRM;
        g_CooldownTicks = 15;
        LogMsg("[State 0x32] Starting battle stage");
        break;

    case 0x20: // Stage Clear banner (observed transition from Turn End)
    case 0x22: // Post-battle transition
    case 0x26: // Post-Battle Bonus / Result Screen
        ResetAutoPlayTurnTracking();
        g_DeployCountThisStage = 0;
        g_PulseAction = ACT_CONFIRM;
        g_CooldownTicks = 8;
        break;

    case 0x04: // Status / Sub-Menu; never count this as a deployment attempt
        if (g_StateStuckTicks >= 3) {
            g_PulseAction = ACT_CANCEL;
            g_CooldownTicks = 8;
            LogMsg("[State 0x04] Closing status menu");
        }
        break;

    case 0x0C: // Base Panel Character Dispatch Menu (+0x141510, cmd=0x32)
        if (SmartCombatAvailable()) {
            const uintptr_t task = mainTask + 0x141510;
            if (g_StateStuckTicks < 3 || *(int32_t*)(task + 0x2C) == 2) break;
            if (g_PendingDeployment) break;
            if (g_BasePanelExhaustedThisTurn) { g_PulseAction = ACT_CANCEL; g_CooldownTicks = 5; break; }
            if (*(int32_t*)(task + 0x14) != 0) {
                g_PulseAction = ACT_CANCEL; g_CooldownTicks = 5; break;
            }
            if (!g_DeploymentPlanning) {
                if (!Planning::Available()) break;
                g_DeploymentTiming = {};
                auto snapshot = NativeCombat::CaptureSnapshot(battleCtx, SmartCombat::accepted);
                g_DeploymentJob.Begin(battleCtx, snapshot);
                g_DeploymentPlanning = true;
            }
            if (!Planning::Run("deployment", g_DeploymentTiming, [] { return g_DeploymentJob.IsComplete(); },
                [] {
                    g_DeploymentJob.Step(4);
                    g_DeploymentTiming.cursor = {static_cast<int64_t>(g_DeploymentJob.nextRosterRow),
                        g_DeploymentJob.activeCandidate.nextSkill,
                        static_cast<int64_t>(g_DeploymentJob.activeCandidate.scoreEnemy),
                        static_cast<int64_t>(g_DeploymentJob.activeCandidate.scoreAttack)};
                })) { g_StateStuckTicks = 3; break; }
            if (!g_DeploymentJob.SelectCursor(task, battleCtx)) {
                g_BasePanelExhaustedThisTurn = true;
                g_DeploymentPlanning = false;
                g_PulseAction = ACT_CANCEL; g_CooldownTicks = 5;
                LogMsg("[Deploy] No verified affordable roster selection; closing dispatch");
                break;
            }
            g_PendingDeployment = true;
            g_PendingDeploymentCount = CountControllablePlayerUnits();
            g_PendingDeploymentTicks = 0;
            g_DeploymentPlanning = false;
            g_PulseAction = ACT_CONFIRM; g_CooldownTicks = 5;
            LogMsg("[Deploy] Selecting roster=%d nativeRow=%d healer=%d",
                g_DeploymentJob.SelectedRoster(), g_DeploymentJob.SelectedRow(), g_DeploymentJob.SelectedIsHealer());
            break;
        }
        [[fallthrough]];
    case 0x11: // Target / Character List Sub-Task
    case 0x12: // Base Panel Character List (+0x4AD80 State 3)
        if (g_StateStuckTicks < 3) {
            break; // Let list initialize
        }
        if (g_StateStuckTicks > 20) {
            g_BasePanelExhaustedThisTurn = true;
            g_PendingDeployment = false;
            g_PendingDeploymentTicks = 0;
            g_PulseAction = ACT_CANCEL;
            g_CooldownTicks = 8;
            LogMsg("[State 0x%02X] Closing menu / Base Panel exhausted", state);
        } else {
            if (!g_PendingDeployment) {
                g_PendingDeployment = true;
                g_PendingDeploymentCount = CountControllablePlayerUnits();
                g_PendingDeploymentTicks = 0;
            }
            g_PulseAction = ACT_CONFIRM;
            g_CooldownTicks = 10;
            LogMsg("[State 0x%02X] Confirming Base Panel dispatch; deployed=%d, stage count=%d",
                   state, g_PendingDeploymentCount, g_DeployCountThisStage);
        }
        break;

    case 0x02: // Player Turn Map Cursor
    {
        uintptr_t mapCursorTask = mainTask + 0x11E0;
        int32_t subState = *(int32_t*)(mapCursorTask + 0x14);
        if (subState != g_LastSubState) {
            LogMsg("[State 0x02] MapCursor subState: %d -> %d", g_LastSubState, subState);
            g_LastSubState = subState;
            g_StateStuckTicks = 0;
            g_ActionAttemptTicks = 0;
        }

        if (subState == 0) { // Free Map Cursor
            if (SmartCombatAvailable() && (SmartCombat::executeEmergency || TeamPlanning::current.requiresQueueExecution)) {
                const bool queued = NativeCombat::QueueCount(battleCtx) > 0;
                SmartCombat::executeEmergency = false;
                TeamPlanning::Reset();
                if (queued) {
                    EmitSubTaskCommand(mapCursorTask, 0x4E20, 0);
                    g_PulseAction = ACT_EXEC_SHORT;
                    g_CooldownTicks = 8;
                    LogMsg("[Combat] Executing urgent healing or an unmodeled queued action before replanning");
                    break;
                }
            }
            if (SmartCombatAvailable() && SmartCombat::pending.valid && SmartCombat::confirmationSent) {
                RejectCombatAction("engine returned to map without matching queue entry");
                break;
            }
            int ex = 0, ez = 0;
            int nearestEnemy = FindNearestEnemy(0, 0, ex, ez);
            if (nearestEnemy < 0) {
                LogMsg("[State 0x02] No living enemies found; ending turn");
                EmitSubTaskCommand(mapCursorTask, 0x4E21, 0);
                g_PulseAction = ACT_END_SHORT;
                g_CooldownTicks = 20;
                break;
            }

            // 1. Check Base Panel tile position and whether a unit is currently standing on it
            float bpWX = 0, bpWY = 0, bpWZ = 0;
            int bpGX = -1, bpGZ = -1;
            bool hasBasePanel = GetBasePanelPosition(bpWX, bpWY, bpWZ, bpGX, bpGZ);
            int unitOnBasePanelIdx = -1;
            if (hasBasePanel) {
                for (int i = 0; i < 256; ++i) {
                    uintptr_t u = GetUnitPtr(i);
                    if (!IsUnitAliveAndDeployed(u)) continue;
                    int ux = 0, uz = 0;
                    GetUnitGridPos(u, ux, uz);
                    if (ux == bpGX && uz == bpGZ) {
                        unitOnBasePanelIdx = i;
                        break;
                    }
                }
            }

            // If a controllable player unit is standing on the Base Panel and hasn't finished its turn, select it first!
            if (!SmartCombatAvailable() && IsValidUnitIndex(unitOnBasePanelIdx) &&
                fnIsControllableUnit((void*)GetUnitPtr(unitOnBasePanelIdx), 0) &&
                !g_UnitDoneThisTurn[unitOnBasePanelIdx]) {
                uintptr_t u = GetUnitPtr(unitOnBasePanelIdx);
                SnapCursorToUnit(battleCtx, u);
                g_ActionAttemptTicks++;
                if (g_ActionAttemptTicks >= 2) {
                    g_PulseAction = ACT_CONFIRM;
                    g_CooldownTicks = 8;
                    g_ActionAttemptTicks = 0;
                    LogMsg("[State 0x02] Selecting unit on Base Panel idx=%d", unitOnBasePanelIdx);
                }
                break;
            }

            // 2. Deploy up to 5 controllable player units from Base Panel if Base Panel is unoccupied
            int deployedPlayers = CountControllablePlayerUnits();
            if (hasBasePanel && unitOnBasePanelIdx < 0 && !g_BasePanelExhaustedThisTurn &&
                deployedPlayers < 5 && g_DeployCountThisStage < 10) {
                if (battleCtx && fnPopulateDeployUnits) {
                    fnPopulateDeployUnits((void*)battleCtx);
                }
                int16_t deployableCount = battleCtx ? *(int16_t*)(battleCtx + 0x968) : 0;
                if (deployableCount > 0) {
                    SnapCursorToWorldPos(battleCtx, bpWX, bpWY, bpWZ);
                    g_ActionAttemptTicks++;
                    if (g_ActionAttemptTicks >= 2) {
                        g_PulseAction = ACT_CONFIRM;
                        g_CooldownTicks = 10;
                        g_ActionAttemptTicks = 0;
                        LogMsg("[State 0x02] Selecting Base Panel at (%d, %d) (deployed=%d, deployable=%d)",
                               bpGX, bpGZ, deployedPlayers, (int)deployableCount);
                    }
                    break;
                } else {
                    g_BasePanelExhaustedThisTurn = true;
                }
            }

            // 3. Otherwise select the next controllable player unit that hasn't finished its turn
            int nextUnitIdx = -1;
            if (SmartCombatAvailable()) {
                if (!SmartCombat::pending.valid) {
                    if (!TeamPlanning::Run(battleCtx)) { g_StateStuckTicks = 0; break; }
                    if (TeamPlanning::current.requiresQueueExecution) break;
                    if (TeamPlanning::current.best.valid && !BeginCombatAction(battleCtx)) {
                        TeamPlanning::Reset(); break;
                    }
                }
                if (SmartCombat::pending.valid) nextUnitIdx = SmartCombat::pending.candidate.actorIndex;
                else if (IsValidUnitIndex(unitOnBasePanelIdx) &&
                    fnIsControllableUnit((void*)GetUnitPtr(unitOnBasePanelIdx), 0) &&
                    !g_UnitDoneThisTurn[unitOnBasePanelIdx] && !g_UnitMovedThisTurn[unitOnBasePanelIdx])
                    nextUnitIdx = unitOnBasePanelIdx;
                else nextUnitIdx = FindNextControllablePlayerUnit();
            } else nextUnitIdx = FindNextControllablePlayerUnit();
            if (IsValidUnitIndex(nextUnitIdx)) {
                uintptr_t u = GetUnitPtr(nextUnitIdx);
                SnapCursorToUnit(battleCtx, u);
                g_ActionAttemptTicks++;
                if (g_ActionAttemptTicks >= 2) {
                    g_PulseAction = ACT_CONFIRM;
                    g_CooldownTicks = 8;
                    g_ActionAttemptTicks = 0;
                    LogMsg("[State 0x02] Selecting player unit idx=%d (nearestEnemy=%d at %d,%d)",
                           nextUnitIdx, nearestEnemy, ex, ez);
                }
            } else {
                // All controllable player units have acted/queued attacks -> execute & end turn!
                LogMsg("[State 0x02] All player units finished; emitting End Turn + Execute (0x4E21)");
                EmitSubTaskCommand(mapCursorTask, 0x4E21, 0);
                g_PulseAction = ACT_END_SHORT;
                g_CooldownTicks = 25;
            }
        } else if (subState == 2) { // Move Destination Selection Cursor
            int32_t selIdx = battleCtx ? *(int32_t*)(battleCtx + 0xB8) : -1;
            DriveMoveDestination(mainTask, battleCtx, selIdx);
        }
        break;
    }

    case 0x05: // Move Destination Selection (from Base Panel deploy or Unit Action Menu Move)
    {
        int32_t selIdx = battleCtx ? *(int32_t*)(battleCtx + 0xB8) : -1;
        DriveMoveDestination(mainTask, battleCtx, selIdx);
        break;
    }

    case 0x03: // Unit Action Menu (+0x196C78)
    {
        if (SmartCombatAvailable() && DrivePlannedAction(mainTask, battleCtx, state)) break;
        if (SmartCombatAvailable()) {
            if (g_StateStuckTicks < 2) break;
            const int actor = battleCtx ? *(int32_t*)(battleCtx + 0xB8) : -1;
            if (!IsValidUnitIndex(actor)) { g_PulseAction = ACT_CANCEL; break; }
            if (NativeCombat::HasQueuedAction(battleCtx, actor) || g_UnitDoneThisTurn[actor]) {
                FinishSmartUnit(actor); break;
            }
            if (!TeamPlanning::Run(battleCtx)) { g_StateStuckTicks = 2; break; }
            if (TeamPlanning::current.requiresQueueExecution) {
                g_PulseAction = ACT_CANCEL; g_CooldownTicks = 5; break;
            }
            if (TeamPlanning::current.best.valid) {
                if (TeamPlanning::current.best.candidate.actorIndex == actor && BeginCombatAction(battleCtx))
                    DrivePlannedAction(mainTask, battleCtx, state);
                else { g_PulseAction = ACT_CANCEL; g_CooldownTicks = 5; }
                break;
            }
            DriveTacticalPlanning(mainTask, battleCtx, actor);
            break;
        }
        if (g_StateStuckTicks < 2) {
            break; // Wait 2 frames for OnEnter (0x631870 -> 0x632240) to populate validAttackTargetIndices
        }
        int32_t selIdx = battleCtx ? *(int32_t*)(battleCtx + 0xB8) : -1;
        uintptr_t u = GetUnitPtr(selIdx);
        if (!u || g_StateStuckTicks > 30) {
            if (IsValidUnitIndex(selIdx)) g_UnitDoneThisTurn[selIdx] = true;
            g_PulseAction = ACT_CANCEL;
            g_CooldownTicks = 8;
            LogMsg("[State 0x03] Unit idx=%d timeout; closing menu", selIdx);
            break;
        }

        int32_t validTargetCount = battleCtx ? *(int32_t*)(battleCtx + 0x94C) : 0;
        uint8_t alreadyQueued    = NativeCombat::HasQueuedAction(battleCtx, selIdx) ? 1 : 0;
        int enemyTargetIdx = -1;
        int enemyListIdx   = 0;

        if (validTargetCount > 0 && !alreadyQueued && fnCanUnitAttack((void*)u, validTargetCount, alreadyQueued)) {
            for (int i = 0; i < validTargetCount && i < 256; ++i) {
                uint16_t tid = *(uint16_t*)(battleCtx + 0x74C + i * 2);
                uintptr_t tu = GetUnitPtr(tid);
                if (!IsUnitAliveAndDeployed(tu)) continue;
                uintptr_t cd = *(uintptr_t*)(tu + 0x240);
                if (!cd || fnIsMapObject((void*)cd) != 0) continue;
                if (IsEnemyFaction(fnGetUnitFaction((void*)tu, 1))) {
                    enemyTargetIdx = (int)tid;
                    enemyListIdx   = i;
                    break;
                }
            }
        }

        uintptr_t actionMenuTask = mainTask + 0x196C78;
        if (enemyTargetIdx >= 0) {
            // Select "Attack" (Menu Index 1 -> emits cmd 0x1E -> State 0x08)
            g_CurrentTargetEnemyIdx  = enemyTargetIdx;
            g_CurrentTargetListIndex = enemyListIdx;
            *(int32_t*)(actionMenuTask + 0x128) = 1;
            g_PulseAction = ACT_CONFIRM;
            g_CooldownTicks = 8;
            LogMsg("[State 0x03] Unit idx=%d selecting Attack (menuIdx=1) on enemy idx=%d (listIdx=%d)",
                   selIdx, enemyTargetIdx, enemyListIdx);
        } else if (!g_UnitMovedThisTurn[selIdx] && fnCanUnitMove((void*)u)) {
            int tx = 0, tz = 0;
            if (ChooseBestMoveTileForUnit(selIdx, tx, tz)) {
                if (g_MoveAttemptsThisTurn[selIdx] >= 2) {
                    g_UnitDoneThisTurn[selIdx] = true;
                    g_PulseAction = ACT_CANCEL;
                    g_CooldownTicks = 8;
                    LogMsg("[State 0x03] Unit idx=%d move retry limit reached; done", selIdx);
                    break;
                }
                // Select "Move" (Menu Index 0 -> emits cmd 0x14 -> State 0x05)
                BeginPendingMove(selIdx, tx, tz);
                *(int32_t*)(actionMenuTask + 0x128) = 0;
                g_PulseAction = ACT_CONFIRM;
                g_CooldownTicks = 8;
                LogMsg("[State 0x03] Unit idx=%d selecting Move (menuIdx=0) toward (%d, %d)", selIdx, tx, tz);
            } else {
                g_UnitDoneThisTurn[selIdx] = true;
                g_PulseAction = ACT_CANCEL;
                g_CooldownTicks = 8;
                LogMsg("[State 0x03] Unit idx=%d has no closer reachable tile and no enemy in range; done", selIdx);
            }
        } else {
            g_UnitDoneThisTurn[selIdx] = true;
            g_PulseAction = ACT_CANCEL;
            g_CooldownTicks = 8;
            LogMsg("[State 0x03] Unit idx=%d finished actions (moved=%d, targets=%d); closing menu",
                   selIdx, (int)g_UnitMovedThisTurn[selIdx], validTargetCount);
        }
        break;
    }

    case 0x08: // Attack Target Selection List (+0x4AD80 State 4, cmd=0x1E)
    case 0x10: // Map / Direction Target Selection (+0x24EDD0)
    {
        if (SmartCombatAvailable()) {
            if (!DrivePlannedAction(mainTask, battleCtx, state)) {
                g_PulseAction = ACT_CANCEL; g_CooldownTicks = 5; TeamPlanning::Reset();
            }
            break;
        }
        if (state == 0x10) {
            // Skill tile/direction targeting is not a basic target list.
            // If its metadata disappears, leave it without confirming.
            g_PulseAction = ACT_CANCEL;
            g_CooldownTicks = 5;
            break;
        }
        if (g_StateStuckTicks < 2) {
            break;
        }
        int32_t selIdx = battleCtx ? *(int32_t*)(battleCtx + 0xB8) : -1;
        uintptr_t targetUnit = GetUnitPtr(g_CurrentTargetEnemyIdx);
        if (targetUnit && g_StateStuckTicks <= 25) {
            // Set target list cursor index in +0x4AD80 (+0xBE38 = cursor, +0xBE3C = scroll)
            uintptr_t targetListTask = mainTask + 0x4AD80;
            *(int32_t*)(targetListTask + 0xBE38) = g_CurrentTargetListIndex;
            *(int32_t*)(targetListTask + 0xBE3C) = 0;
            SnapCursorToUnit(battleCtx, targetUnit);
            g_PulseAction = ACT_CONFIRM;
            g_CooldownTicks = 8;
            if (IsValidUnitIndex(selIdx)) g_UnitDoneThisTurn[selIdx] = true;
            LogMsg("[State 0x%02X] Confirming attack target enemy idx=%d (listIdx=%d) for unit idx=%d",
                   state, g_CurrentTargetEnemyIdx, g_CurrentTargetListIndex, selIdx);
        } else {
            if (IsValidUnitIndex(selIdx)) g_UnitDoneThisTurn[selIdx] = true;
            g_PulseAction = ACT_CANCEL;
            g_CooldownTicks = 8;
        }
        break;
    }

    case 0x07: // General Menu (Triangle/Y menu)
        g_PulseAction = ACT_CANCEL;
        g_CooldownTicks = 8;
        break;

    case 0x0E: // Learned skill list and spell area selector
        if (!SmartCombatAvailable() || !DrivePlannedAction(mainTask, battleCtx, state)) {
            g_PulseAction = ACT_CANCEL; g_CooldownTicks = 5;
        }
        break;

    default:
        break;
    }
}

static void SyncRepeatToggle() {
    if (g_RepeatRequested.load() && !NativeRepeat::controller.enabled()) {
        NativeRepeat::controller.Toggle();
        g_EntryChoiceSentObject = 0; g_EntryChoiceSentAttempt = 0;
        NativeRepeat::reportedCycles = 0;
        LogMsg("[Repeat] Armed; waiting for current/next verified battle");
    } else if (!g_RepeatRequested.load() && NativeRepeat::controller.enabled()) {
        NativeRepeat::controller.Stop("F2 disabled");
    }
}
static void PublishRepeatStop() {
    if (!NativeRepeat::controller.enabled() && g_RepeatRequested.exchange(false)) UpdateWindowTitleIndicator();
}
static uint64_t __fastcall Hooked_BaseSceneUpdate() {
    PollHotkeys(0); SyncRepeatToggle();
    if (NativeRepeat::controller.enabled()) {
        StageRepeat::Observation o;
        o.screen = StageRepeat::Screen::Hub; o.ready = o.settled = true; o.nowMs = GetTickCount64();
        NativeRepeat::Tick(o); PublishRepeatStop();
    }
    return orig_BaseSceneUpdate();
}
static void TryInstallBaseSceneHook() {
    if (!orig_BaseSceneUpdate && NativeScenes::InstallUpdate(g_Base, 8, NativeScenes::kBaseUpdateRva,
            &Hooked_BaseSceneUpdate, orig_BaseSceneUpdate))
        LogMsg("[Repeat] Hooked verified base scene update for unexpected-return detection");
}
static void __fastcall Hooked_SelectorExecute(void* task, int callType) {
    bool confirm = false;
    if (callType == 4) {
        PollHotkeys(0); SyncRepeatToggle(); TryInstallBaseSceneHook();
        if (NativeRepeat::controller.enabled()) {
            NativeRepeat::Selector selector;
            if (NativeRepeat::ReadSelector(g_Base, reinterpret_cast<uintptr_t>(task), NativeRepeat::controller.stage(), selector)) {
                const auto decision = NativeRepeat::Tick(selector.observation);
                if (decision.command == StageRepeat::Command::SelectStage) {
                    if (!NativeRepeat::SelectStage(g_Base, reinterpret_cast<uintptr_t>(task), decision.stage))
                        NativeRepeat::controller.DispatchRejected();
                } else if (decision.command == StageRepeat::Command::StartStage) {
                    NativeRepeat::Selector checked;
                    const bool valid = NativeRepeat::ReadSelector(g_Base, reinterpret_cast<uintptr_t>(task), decision.stage, checked);
                    if (valid && checked.observation.screen == StageRepeat::Screen::Selection) {
                        confirm = checked.observation.ready && checked.observation.stageAvailable && checked.observation.selected == decision.stage;
                        if (!confirm) NativeRepeat::controller.DispatchRejected();
                    } // Expected Yes/No stage-entry UI is owned by its own update hook.
                }
            } else {
                NativeRepeat::controller.Stop("selector identity or metadata changed");
                LogMsg("[Repeat] Stopped: selector identity or metadata changed");
            }
            PublishRepeatStop();
        }
    }
    NativeNotifications::ScopedConfirm input(g_Base, confirm && g_RepeatRequested.load());
    if (confirm && !input.active()) NativeRepeat::controller.DispatchRejected();
    orig_SelectorExecute(task, callType);
}
static uint32_t __fastcall Hooked_EntryChoiceUpdate(void* object) {
    PollHotkeys(0); SyncRepeatToggle();
    bool confirm = false;
    uintptr_t selectorTask = 0;
    NativeRepeat::Selector selector;
    const uintptr_t address = reinterpret_cast<uintptr_t>(object);
    NativeRepeat::EntryChoice lifetime;
    if (address == g_EntryChoiceSentObject && NativeRepeat::ReadEntryChoice(g_Base, address, lifetime) && lifetime.phase == 0) {
        g_EntryChoiceSentObject = 0; g_EntryChoiceSentAttempt = 0;
    }
    if (NativeRepeat::controller.enabled() && NativeRepeat::controller.stage().valid() &&
        NativeNotifications::Read(g_Base + NativeRepeat::kSelectorPointer, selectorTask) && selectorTask &&
        NativeRepeat::ReadSelector(g_Base, selectorTask, NativeRepeat::controller.stage(), selector) &&
        selector.observation.screen == StageRepeat::Screen::StartMenu && selector.observation.instance == address) {
        NativeRepeat::Tick(selector.observation);
        const auto& pending = NativeRepeat::controller.pending();
        confirm = selector.observation.ready && pending.command == StageRepeat::Command::StartStage &&
            pending.source == StageRepeat::Screen::StartMenu && pending.instance == address &&
            (g_EntryChoiceSentObject != address || g_EntryChoiceSentAttempt != pending.attempts);
        if (confirm) {
            g_EntryChoiceSentObject = address; g_EntryChoiceSentAttempt = pending.attempts;
            LogMsg("[Repeat] Confirming verified stage-entry Yes stage=%d variant=%d attempt=%d",
                pending.command == StageRepeat::Command::StartStage ? NativeRepeat::controller.stage().stage : -1,
                NativeRepeat::controller.stage().variant, pending.attempts);
        }
        PublishRepeatStop();
    }
    uint32_t result = 0;
    {
        NativeNotifications::ScopedConfirm input(g_Base, confirm && g_RepeatRequested.load());
        if (confirm && !input.active()) NativeRepeat::controller.DispatchRejected();
        result = orig_EntryChoiceUpdate(object);
    }
    if (confirm) {
        NativeRepeat::EntryChoice observed;
        if (NativeRepeat::ReadEntryChoice(g_Base, address, observed) && observed.result == 0)
            LogMsg("[Repeat] Accepted native stage-entry Yes stage=%d", NativeRepeat::controller.stage().stage);
        else NativeRepeat::controller.DispatchRejected();
    }
    return result;
}
static uint64_t __fastcall Hooked_NotificationUpdate(void* notification) {
    PollHotkeys(0);
    SyncRepeatToggle();
    bool repeatOwns = false;
    if (NativeRepeat::controller.enabled()) {
        StageRepeat::Observation observation;
        if (NativeRepeat::ReadBattle(g_Base, g_NotificationBattleTask, observation)) {
            // This also permits F2 to bind an already-open verified clear notice.
            if (NativeRepeat::controller.state() == StageRepeat::State::Armed &&
                observation.outcome == StageRepeat::Outcome::Victory) NativeRepeat::Tick(observation);
            repeatOwns = NativeRepeat::controller.OwnsClear(observation);
        }
        PublishRepeatStop();
    }
    bool notificationEnabled = g_Enabled || repeatOwns;
#ifdef D5_NATIVE_NOTIFICATION_QA
    notificationEnabled = notificationEnabled && NativeNotificationQA::AllowConfirm();
#endif
    return NativeNotifications::Drive(orig_NotificationUpdate, g_Base, notification,
        g_NotificationBattleTask, notificationEnabled,
        repeatOwns ? NativeNotifications::Owner::StageRepeat : NativeNotifications::Owner::CombatAutoplay,
        g_RepeatRequested.load());
}

static void __fastcall Hooked_BattleMainTask_Execute(void* mainTask, int callType) {
    if (callType == 4 && mainTask != nullptr) {
        // Observe while F1 is off too: it may be enabled behind a modal.
        g_NotificationBattleTask = reinterpret_cast<uintptr_t>(mainTask);
        g_TickCount++;
        PollHotkeys(0);

        g_PulseAction = -1;
        g_HoldAction  = -1;
        SyncRepeatToggle(); TryInstallBaseSceneHook();
#ifdef D5_NATIVE_NOTIFICATION_QA
        NativeNotificationQA::MaybeQueue(g_Base, reinterpret_cast<uintptr_t>(mainTask), g_Enabled);
#endif
        bool repeatConfirm = false, repeatOwns = false;
        if (NativeRepeat::controller.enabled()) {
            StageRepeat::Observation observation;
            if (NativeRepeat::ReadBattle(g_Base, reinterpret_cast<uintptr_t>(mainTask), observation)) {
                const uint32_t cycles = NativeRepeat::controller.cycles();
                const auto decision = NativeRepeat::Tick(observation);
                if (NativeRepeat::controller.cycles() != cycles) {
                    ResetAutoPlayTurnTracking(); g_DeployCountThisStage = 0;
                }
                repeatOwns = NativeRepeat::controller.OwnsClear(observation) ||
                    (observation.screen == StageRepeat::Screen::StartMenu &&
                     NativeRepeat::controller.state() != StageRepeat::State::Armed);
                if (decision.command == StageRepeat::Command::AdvanceResults || decision.command == StageRepeat::Command::StartStage) {
                    StageRepeat::Observation checked;
                    repeatConfirm = NativeRepeat::ReadBattle(g_Base, reinterpret_cast<uintptr_t>(mainTask), checked) &&
                        checked.ready && checked.stage == decision.stage && checked.revision == observation.revision;
                    if (!repeatConfirm) NativeRepeat::controller.DispatchRejected();
                }
            } else {
                NativeRepeat::controller.Stop("unverified battle identity");
                LogMsg("[Repeat] Stopped: unverified battle identity");
            }
            PublishRepeatStop();
        }

        if (g_Enabled && !repeatOwns) {
            uintptr_t battleCtx = *(uintptr_t*)(g_Base + RVA::PTR_BattleContext);
            StepBattleDecision((uintptr_t)mainTask, battleCtx);
        }

        uint64_t pulsedVk = 0;
        uint64_t heldVk   = 0;
        int32_t  savedDevMode = *(int32_t*)(g_Base + RVA::ADDR_InputDeviceMode);

        if (g_PulseAction >= 0 || g_HoldAction >= 0) {
            // Ensure game window processes input even if unfocused and reads keyboard state table
            *(uint8_t*)(g_Base + RVA::ADDR_WindowFocused)   = 1;
            *(int32_t*)(g_Base + RVA::ADDR_InputDeviceMode) = 0;

            uintptr_t sysMgr = *(uintptr_t*)(g_Base + RVA::PTR_InputMgrSys);
            if (sysMgr) {
                *(uint8_t*)(sysMgr + 0x12) = 0;
            }

            if (g_PulseAction >= 0) {
                pulsedVk = GetActionVirtualKey(g_PulseAction);
                if (pulsedVk > 0 && pulsedVk < 0xFF) {
                    *(uint8_t*)(g_Base + RVA::ADDR_KeyJustPressed + pulsedVk) = 1;
                    *(uint8_t*)(g_Base + RVA::ADDR_KeyHeld        + pulsedVk) = 1;
                }
            }
            if (g_HoldAction >= 0) {
                heldVk = GetActionVirtualKey(g_HoldAction);
                if (heldVk > 0 && heldVk < 0xFF) {
                    *(uint8_t*)(g_Base + RVA::ADDR_KeyHeld   + heldVk) = 1;
                    *(uint8_t*)(g_Base + RVA::ADDR_KeyRepeat + heldVk) = 1;
                }
            }
        }

        {
            NativeNotifications::ScopedConfirm input(g_Base, repeatConfirm && g_RepeatRequested.load());
            if (repeatConfirm && !input.active()) NativeRepeat::controller.DispatchRejected();
            orig_BattleExecute(mainTask, callType);
        }

        if (pulsedVk > 0 && pulsedVk < 0xFF) {
            *(uint8_t*)(g_Base + RVA::ADDR_KeyJustPressed + pulsedVk) = 0;
            *(uint8_t*)(g_Base + RVA::ADDR_KeyHeld        + pulsedVk) = 0;
        }
        if (heldVk > 0 && heldVk < 0xFF) {
            *(uint8_t*)(g_Base + RVA::ADDR_KeyHeld   + heldVk) = 0;
            *(uint8_t*)(g_Base + RVA::ADDR_KeyRepeat + heldVk) = 0;
        }
        if (g_PulseAction >= 0 || g_HoldAction >= 0) {
            *(int32_t*)(g_Base + RVA::ADDR_InputDeviceMode) = savedDevMode;
        }
        return;
    }

    orig_BattleExecute(mainTask, callType);
}

void InitializeMod() {
    g_Base = (uintptr_t)GetModuleHandleW(nullptr);
    LogMsg("=== Disgaea 5 Complete Auto-Play Mod v5.4.2 low-latency planning Initializing ===");
#ifdef D5_NATIVE_NOTIFICATION_QA
    LogMsg("[NativeQA] DEVELOPMENT build: two synthetic-text native notification tests enabled");
#endif
    LogMsg("ImageBase = 0x%llX", (unsigned long long)g_Base);

    fnCommandEndTurn      = (FnCommandEndTurn)(g_Base + RVA::FN_CommandEndTurn);
    fnUpdateCursorHover   = (FnUpdateCursorHover)(g_Base + RVA::FN_UpdateCursorHover);
    fnGetTileHeight       = (FnGetTileHeight)(g_Base + RVA::FN_GetTileHeight);
    fnIsControllableUnit  = (FnIsControllableUnit)(g_Base + RVA::FN_IsControllableUnit);
    fnIsMapObject         = (FnIsMapObject)(g_Base + RVA::FN_IsMapObject);
    fnGetUnitFaction      = (FnGetUnitFaction)(g_Base + RVA::FN_GetUnitFaction);
    fnCanUnitMove         = (FnCanUnitMove)(g_Base + RVA::FN_CanUnitMove);
    fnGetUnitMoveStat     = (FnGetUnitMoveStat)(g_Base + RVA::FN_GetUnitMoveStat);
    fnHasActionRemaining  = (FnHasActionRemaining)(g_Base + RVA::FN_HasActionRemaining);
    fnCanUnitAttack       = (FnCanUnitAttack)(g_Base + RVA::FN_CanUnitAttack);
    fnGetUnitPackedXZ     = (FnGetUnitPackedXZ)(g_Base + RVA::FN_GetUnitPackedXZ);
    fnComputeMoveRange    = (FnComputeMoveRange)(g_Base + RVA::FN_ComputeMoveRange);
    fnComputePathToTile   = (FnComputePathToTile)(g_Base + RVA::FN_ComputePathToTile);
    fnPopulateDeployUnits = (FnPopulateDeployUnits)(g_Base + RVA::FN_PopulateDeployUnits);
    fnPlaySoundEffect     = (FnPlaySoundEffect)(g_Base + RVA::FN_PlaySoundEffect);
    NativeSkills::Initialize(g_Base);
    NativeActions::Initialize(g_Base);
    NativeEffects::Initialize(g_Base);
    NativeActions::evaluateEffect = &NativeEffects::Evaluate;
    TryInstallBaseSceneHook();
    uintptr_t* entryChoiceSlot = reinterpret_cast<uintptr_t*>(g_Base + NativeRepeat::kEntryChoiceVtable + 0x10);
    if (*entryChoiceSlot == g_Base + NativeRepeat::kEntryChoiceUpdate) {
        DWORD protection = 0;
        if (VirtualProtect(entryChoiceSlot, sizeof(*entryChoiceSlot), PAGE_READWRITE, &protection)) {
            orig_EntryChoiceUpdate = reinterpret_cast<FnEntryChoiceUpdate>(*entryChoiceSlot);
            *entryChoiceSlot = reinterpret_cast<uintptr_t>(&Hooked_EntryChoiceUpdate);
            VirtualProtect(entryChoiceSlot, sizeof(*entryChoiceSlot), protection, &protection);
            LogMsg("[Repeat] Hooked expected stage-entry choice; unrelated choices remain manual");
        }
    }
    uintptr_t* selectorSlot = reinterpret_cast<uintptr_t*>(g_Base + NativeRepeat::kSelectorVtable + 9 * 8);
    if (*selectorSlot == g_Base + NativeRepeat::kSelectorExecute) {
        DWORD oldProtect = 0;
        if (VirtualProtect(selectorSlot, sizeof(*selectorSlot), PAGE_READWRITE, &oldProtect)) {
            orig_SelectorExecute = reinterpret_cast<FnBattleExecute>(*selectorSlot);
            *selectorSlot = reinterpret_cast<uintptr_t>(&Hooked_SelectorExecute);
            VirtualProtect(selectorSlot, sizeof(*selectorSlot), oldProtect, &oldProtect);
            LogMsg("[Repeat] Hooked verified stage selector Execute vtable98D018 slot9");
        }
    } else LogMsg("ERROR: Selector contract mismatch; F2 cannot navigate stage list");

    // Only the verified single-button class. Choice subclasses are separate.
    uintptr_t* noticeSlot = reinterpret_cast<uintptr_t*>(g_Base + NativeNotifications::kVtableRva + 0x10);
    if (NativeGeometry::IsReadableRange(reinterpret_cast<uintptr_t>(noticeSlot), sizeof(*noticeSlot)) &&
        *noticeSlot == g_Base + NativeNotifications::kUpdateRva) {
        DWORD oldProtect = 0;
        if (VirtualProtect(noticeSlot, sizeof(*noticeSlot), PAGE_READWRITE, &oldProtect)) {
            orig_NotificationUpdate = reinterpret_cast<NativeNotifications::Update>(*noticeSlot);
            *noticeSlot = reinterpret_cast<uintptr_t>(&Hooked_NotificationUpdate);
            VirtualProtect(noticeSlot, sizeof(*noticeSlot), oldProtect, &oldProtect);
            LogMsg("Hooked battle OK notification update (vtable=0x98A0B0 slot=2)");
        } else LogMsg("ERROR: Failed to unprotect notification update slot");
    } else LogMsg("ERROR: Notification contract mismatch; auto-dismiss unavailable");

    // Swap VTable[9] (Execute) on CBattleMainTask
    uintptr_t* vtable = (uintptr_t*)(g_Base + RVA::VTABLE_BattleMainTask);
    if (vtable[9] != (uintptr_t)&Hooked_BattleMainTask_Execute) {
        DWORD oldProtect = 0;
        if (VirtualProtect(&vtable[9], sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
            orig_BattleExecute = (FnBattleExecute)(g_Base + RVA::FN_BattleMain_Execute);
            vtable[9] = (uintptr_t)&Hooked_BattleMainTask_Execute;
            VirtualProtect(&vtable[9], sizeof(uintptr_t), oldProtect, &oldProtect);
            LogMsg("Hooked CBattleMainTask::Execute at VTable[9] (orig=0x%llX)", (unsigned long long)orig_BattleExecute);
        } else {
            LogMsg("ERROR: Failed to unprotect CBattleMainTask VTable[9]");
        }
    }
}

} // namespace AutoPlay

// XINPUT9_1_0.dll Proxy Forwarding
static HMODULE g_SysXInput = nullptr;

using FnXInputGetState                  = DWORD(WINAPI*)(DWORD, XINPUT_STATE_DATA*);
using FnXInputSetState                  = DWORD(WINAPI*)(DWORD, void*);
using FnXInputGetCapabilities           = DWORD(WINAPI*)(DWORD, DWORD, void*);
using FnXInputGetDSoundAudioDeviceGuids = DWORD(WINAPI*)(DWORD, GUID*, GUID*);

static FnXInputGetState                  orig_XInputGetState                  = nullptr;
static FnXInputSetState                  orig_XInputSetState                  = nullptr;
static FnXInputGetCapabilities           orig_XInputGetCapabilities           = nullptr;
static FnXInputGetDSoundAudioDeviceGuids orig_XInputGetDSoundAudioDeviceGuids = nullptr;

static void EnsureSystemXInputLoaded() {
    if (g_SysXInput) return;
    wchar_t sysDir[MAX_PATH] = { 0 };
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wcscat_s(sysDir, L"\\XINPUT9_1_0.dll");
    g_SysXInput = LoadLibraryW(sysDir);
    if (g_SysXInput) {
        orig_XInputGetState                  = (FnXInputGetState)GetProcAddress(g_SysXInput, "XInputGetState");
        orig_XInputSetState                  = (FnXInputSetState)GetProcAddress(g_SysXInput, "XInputSetState");
        orig_XInputGetCapabilities           = (FnXInputGetCapabilities)GetProcAddress(g_SysXInput, "XInputGetCapabilities");
        orig_XInputGetDSoundAudioDeviceGuids = (FnXInputGetDSoundAudioDeviceGuids)GetProcAddress(g_SysXInput, "XInputGetDSoundAudioDeviceGuids");
    }
}

extern "C" DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE_DATA* pState) {
    EnsureSystemXInputLoaded();
    DWORD res = orig_XInputGetState ? orig_XInputGetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    if (dwUserIndex == 0) {
        WORD buttons = (res == ERROR_SUCCESS && pState) ? pState->Gamepad.wButtons : 0;
        AutoPlay::PollHotkeys(buttons);
        if (AutoPlay::g_Base) AutoPlay::TryInstallBaseSceneHook();
    }
    return res;
}

extern "C" DWORD WINAPI XInputSetState(DWORD dwUserIndex, void* pVibration) {
    EnsureSystemXInputLoaded();
    return orig_XInputSetState ? orig_XInputSetState(dwUserIndex, pVibration) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, void* pCapabilities) {
    EnsureSystemXInputLoaded();
    return orig_XInputGetCapabilities ? orig_XInputGetCapabilities(dwUserIndex, dwFlags, pCapabilities) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID* pDSoundRenderGuid, GUID* pDSoundCaptureGuid) {
    EnsureSystemXInputLoaded();
    return orig_XInputGetDSoundAudioDeviceGuids ? orig_XInputGetDSoundAudioDeviceGuids(dwUserIndex, pDSoundRenderGuid, pDSoundCaptureGuid) : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        EnsureSystemXInputLoaded();
        AutoPlay::InitializeMod();
    }
    return TRUE;
}
