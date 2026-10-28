#pragma once
#include "stage_repeat.h"
#include "native_scenes.h"

namespace AutoPlay::NativeRepeat {
namespace P = StageRepeat;
namespace N = NativeNotifications;
constexpr uintptr_t kSelectorPointer = 0x7B58E80;
constexpr uintptr_t kSelectorVtable = 0x98D018;
constexpr uintptr_t kSelectorExecute = 0x646040;
constexpr uintptr_t kSelectionManager = 0xA726E8;
constexpr uintptr_t kRecords = 0x54432D8;
constexpr uintptr_t kVariantFlag = 0x4FCCFF0;
constexpr uintptr_t kBattleMode = 0x5574AAF;
constexpr uintptr_t kEntryChoiceVtable = 0x98A038;
constexpr uintptr_t kEntryChoiceUpdate = 0x09BD90;
struct EntryChoice {
    int phase = -1, selection = -1, result = -1;
    bool ready = false;
};
inline bool ReadEntryChoice(uintptr_t base, uintptr_t object, EntryChoice& out) {
    out = {};
    uintptr_t type = 0, manager = 0, resultPointer = 0, gate = 0;
    int kind = -1;
    if (!N::Read(object, type) || type != base + kEntryChoiceVtable ||
        !N::Read(base + N::kQueuePointerRva, manager) || !manager ||
        !N::Read(object + 0x630, resultPointer) || resultPointer != manager + 0x408 ||
        !N::Read(object + 0x18, kind) || kind != 0 ||
        !N::Read(object + 0x600, out.phase) || out.phase < 0 || out.phase > 2 ||
        !N::Read(object + 0x62C, out.selection) || out.selection < 0 || out.selection > 1 ||
        !N::Read(resultPointer, out.result) || !N::Read(object + 0x10, gate) || !gate ||
        !NativeGeometry::IsReadableRange(gate, 0x18)) return false;
    // 09B420 stores the layout kind at +18; +0C is generic lifecycle flags.
    // 09BD90 accepts input in phase1. The UI gate's state3 is normal here,
    // not a close signal (the update itself changes it only after acceptance).
    out.ready = out.phase == 1 && out.selection == 0 && out.result == -1;
    return true;
}
inline P::Controller controller;
inline bool verifiedClear = false;
inline uintptr_t observedBattle = 0;
inline int lastBattleState = -1;
inline P::Observation lastObservation;
inline uint32_t reportedCycles = 0;
inline bool stopReported = false;

inline bool SpecialStage(int id) {
    // 642070 routes these IDs through custom capacity/choice handling.
    return id >= 2203 && id <= 2903 && (id - 2203) % 100 == 0;
}
inline bool Variant(uintptr_t base, int& variant) {
    uint8_t flag = 0;
    if (!N::Read(base + kVariantFlag, flag)) return false;
    variant = flag & 1; return true;
}
inline bool CurrentStage(uintptr_t base, P::StageKey& key, uint16_t* clears = nullptr) {
    uintptr_t manager = 0, record = 0, full = 0;
    int16_t index = -1;
    int id = -1, fullId = -1, variant = -1;
    if (!N::Read(base + kSelectionManager, manager) || !manager ||
        !N::Read(manager, record) || !N::Read(manager + 8, full) || !full ||
        !N::Read(manager + 0x10, index) || index < 0 || index >= 1024 ||
        record != base + kRecords + static_cast<uintptr_t>(index) * 0x80 ||
        !N::Read(record, id) || id <= 0 || !N::Read(full + 4, fullId) || fullId != id ||
        !Variant(base, variant)) return false;
    key = {id, variant};
    if (clears && !N::Read(record + (variant ? 0x12 : 4), *clears)) return false;
    return true;
}

inline bool WinningParty() {
    if (!fnGetUnitFaction || !fnIsMapObject || !fnIsControllableUnit) return false;
    int players = 0;
    // Bounded once per clear decision, not a map/skill search.
    for (int i = 0; i < 256; ++i) {
        const uintptr_t unit = GetUnitPtr(i);
        if (!IsUnitAliveAndDeployed(unit)) continue;
        const uintptr_t data = *reinterpret_cast<uintptr_t*>(unit + 0x240);
        if (fnIsMapObject(reinterpret_cast<void*>(data))) continue;
        if (IsEnemyFaction(fnGetUnitFaction(reinterpret_cast<void*>(unit), 1))) return false;
        if (fnIsControllableUnit(reinterpret_cast<void*>(unit), 0)) ++players;
    }
    return players > 0;
}

inline bool ReadBattle(uintptr_t base, uintptr_t task, P::Observation& out) {
    out = {}; out.nowMs = GetTickCount64(); out.instance = task;
    uintptr_t vtable = 0, context = 0, globalContext = 0;
    int state = -1, transition = -1;
    uint8_t mode = 0;
    uint16_t clears = 0;
    if (!task || !N::Read(task, vtable) || vtable != base + RVA::VTABLE_BattleMainTask ||
        !N::Read(task + 0x80, context) || !context ||
        !N::Read(base + RVA::PTR_BattleContext, globalContext) || context != globalContext ||
        !N::Read(task + 0x14, state) || !N::Read(task + 0x2C, transition) ||
        !N::Read(base + kBattleMode, mode) || !CurrentStage(base, out.stage, &clears)) return false;
    if (task != observedBattle || (lastBattleState >= 0x20 && (state <= 0x02 || state == 0x32))) {
        verifiedClear = false; observedBattle = task;
    }
    lastBattleState = state;
    out.normalStage = mode == 1 && clears > 0 && !SpecialStage(out.stage.stage);
    out.settled = transition == 1;
    out.screen = P::Screen::Battle;
    out.ready = out.settled;
    out.revision = static_cast<uint64_t>(state) << 32;
    if (state == 0x20) {
        if (N::BattlePhase(base, task) != N::Phase::Clear || !out.normalStage) return false;
        if (!verifiedClear && WinningParty()) {
            verifiedClear = true;
            LogMsg("[Repeat] Verified clear stage=%d variant=%d state=0x20 mode=1 livingParty=1 liveEnemies=0",
                   out.stage.stage, out.stage.variant);
        }
        out.outcome = verifiedClear ? P::Outcome::Victory : P::Outcome::Unknown;
        uintptr_t notice = 0;
        if (!N::Foreground(base, notice)) return false;
        out.screen = notice ? P::Screen::ClearNotice : P::Screen::Results;
        if (notice) {
            uintptr_t type = 0; int inputMode = 0;
            out.instance = notice; out.ready = N::Ready(base, notice, task);
            if (!N::Read(notice, type) || type != base + N::kVtableRva ||
                !N::Read(notice + 0x1C, inputMode) || inputMode != 1) {
                out.screen = P::Screen::Unknown; out.settled = true;
            }
        }
        else {
            int inner = -1, sub = -1, childTransition = -1;
            const uintptr_t clearTask = task + N::kClearTaskOffset;
            if (!N::Read(clearTask + 0x14, inner) || !N::Read(clearTask + 0x65E0, sub) ||
                !N::Read(clearTask + 0x2C, childTransition)) return false;
            out.revision |= static_cast<uint64_t>(inner & 0xFF) << 16 | static_cast<uint16_t>(sub);
            // 61DD30 waits on fresh Confirm at state2; 61D2A0 at state3/sub8.
            out.ready = out.settled && childTransition == 1 && (inner == 2 || (inner == 3 && sub == 8));
        }
    } else if (state == 0x21 || state == 0x23 || state == 0x2A) {
        out.screen = P::Screen::Loading; out.settled = false; out.ready = false;
        out.outcome = verifiedClear ? P::Outcome::Victory : P::Outcome::Unknown;
    } else if (state <= 1 || state == 0x30 || state == 0x31) {
        out.screen = P::Screen::Loading; out.ready = out.settled = false;
    } else if (state == 0x32) {
        out.screen = P::Screen::StartMenu; out.selected = out.stage;
        out.ready = transition == 1;
    } else if (state >= 0x20 && state != 0x2F) {
        out.screen = P::Screen::Unknown; // Abort on unverified result/failure screens.
    }
    return true;
}

struct Selector {
    uintptr_t task = 0;
    int targetRow = -1, selectedRow = -1, count = 0;
    P::Observation observation;
};
inline bool ReadSelector(uintptr_t base, uintptr_t task, P::StageKey target, Selector& out) {
    out = {}; out.task = task;
    out.observation.nowMs = GetTickCount64(); out.observation.instance = task;
    uintptr_t vtable = 0;
    int state = -1, transition = -1, cursor = -1, scroll = -1;
    int16_t count = 0;
    uint8_t variant = 0, pending = 0, special = 0;
    NativeScenes::Registry scene;
    if (!NativeScenes::ReadRegistry(base, scene) || scene.current != 9 ||
        !N::Read(task, vtable) || vtable != base + kSelectorVtable ||
        !N::Read(task + 0x14, state) || !N::Read(task + 0x2C, transition) ||
        !N::Read(task + 0x1646, variant) || variant > 1) return false;
    auto& o = out.observation;
    CurrentStage(base, o.stage);
    o.screen = state == 5 ? P::Screen::Selection : P::Screen::Loading;
    o.normalStage = true; o.settled = state == 5 && transition == 1;
    o.revision = static_cast<uint64_t>(state);
    if (state != 5 || transition != 1) return true;
    if (!N::Read(task + 0x978, count) || count < 1 || count > 10 ||
        !N::Read(task + 0x1630, cursor) || !N::Read(task + 0x1634, scroll) ||
        cursor < 0 || cursor >= 8 || scroll < 0 || cursor + scroll >= count ||
        !N::Read(task + 0x163E, pending) || !N::Read(task + 0x163C, special)) return false;
    out.count = count; out.selectedRow = cursor + scroll;
    int matches = 0;
    for (int i = 0; i < count; ++i) {
        int16_t recordIndex = -1;
        int id = -1;
        if (!N::Read(task + 0x950 + static_cast<uintptr_t>(i) * 4, recordIndex) ||
            recordIndex < 0 || recordIndex >= 1024 ||
            !N::Read(base + kRecords + static_cast<uintptr_t>(recordIndex) * 0x80, id) || id <= 0) return false;
        const P::StageKey key{id, variant};
        if (i == out.selectedRow) o.selected = key;
        if (key == target) { ++matches; out.targetRow = i; }
    }
    o.stageAvailable = matches == 1 && !SpecialStage(target.stage);
    o.ready = !pending && !special && !N::BlocksBattlePlanning(base);
    uintptr_t foreground = 0;
    if (!N::Foreground(base, foreground)) return false;
    if (foreground) {
        EntryChoice choice;
        if (pending && !special && o.selected == target && o.stage == target &&
            ReadEntryChoice(base, foreground, choice) && choice.selection == 0 && choice.result <= 0) {
            o.instance = foreground;
            o.screen = choice.phase == 2 && choice.result == 0 ? P::Screen::Loading : P::Screen::StartMenu;
            o.settled = o.screen == P::Screen::StartMenu;
            o.ready = choice.ready;
        } else { o.screen = P::Screen::Unknown; o.settled = true; }
    }
    return true;
}

inline P::Decision Tick(const P::Observation& observation) {
    const auto pending = controller.pending();
    const bool wasEnabled = controller.enabled();
    const auto decision = controller.Step(observation);
    lastObservation = observation;
    if (pending.command != P::Command::None && controller.pending().command != pending.command)
        LogMsg("[Repeat] Observed transition command=%d stage=%d scene=%d revision=%llu",
            static_cast<int>(pending.command), observation.stage.stage, static_cast<int>(observation.screen),
            static_cast<unsigned long long>(observation.revision));
    if (controller.cycles() != reportedCycles) {
        reportedCycles = controller.cycles();
        LogMsg("[Repeat] Entered same-stage battle stage=%d variant=%d repeat=%u",
               controller.stage().stage, controller.stage().variant, reportedCycles);
    }
    if (wasEnabled && !controller.enabled()) {
        stopReported = true;
        LogMsg("[Repeat] Stopped: %s", controller.reason());
    }
    if (decision.command != P::Command::None)
        LogMsg("[Repeat] Propose command=%d stage=%d variant=%d attempt=%d reason=%s",
            static_cast<int>(decision.command), decision.stage.stage, decision.stage.variant,
            controller.pending().attempts, decision.reason);
    return decision;
}
inline bool SelectStage(uintptr_t base, uintptr_t task, P::StageKey target) {
    Selector current;
    if (!ReadSelector(base, task, target, current) || !current.observation.ready ||
        !current.observation.stageAvailable) return false;
    const int scroll = current.targetRow > 7 ? current.targetRow - 7 : 0;
    *reinterpret_cast<int*>(task + 0x1630) = current.targetRow - scroll;
    *reinterpret_cast<int*>(task + 0x1634) = scroll;
    return true;
}
} // namespace AutoPlay::NativeRepeat
