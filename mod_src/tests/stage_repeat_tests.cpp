#include "../stage_repeat.h"
#include <cstdio>
#include <cstring>
using namespace AutoPlay::StageRepeat;
bool Check(bool condition, const char* label) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", label); return condition;
}
Observation Battle(int stage = 17) {
    Observation o; o.screen = Screen::Battle; o.stage = {stage, 0}; o.normalStage = true;
    o.ready = o.settled = true; o.instance = 10; return o;
}
Observation Results(uint64_t now = 100) {
    auto o = Battle(); o.screen = Screen::Results; o.outcome = Outcome::Victory;
    o.canReplay = true; o.nowMs = now; return o;
}
int main() {
    bool ok = true;
    ToggleLatch latch;
    ok &= Check(latch.Pressed(true) && !latch.Pressed(true) && !latch.Pressed(true) &&
        !latch.Pressed(false) && latch.Pressed(true), "held F2 toggles only once per press across repeated polls");
    Controller c;
    ok &= Check(c.Step(Results()).command == Command::None && !c.enabled(), "F2 defaults off even with verified victory");
    c.Toggle(); auto b = Battle();
    ok &= Check(c.Step(b).command == Command::None && c.stage() == b.stage, "F2 records manual battle without issuing combat commands");
    // No F1 flag is consulted anywhere in this pure controller.
    auto r = Results();
    ok &= Check(c.Step(r).command == Command::Replay && c.pending().attempts == 1, "verified win proposes replay independently of F1");
    ok &= Check(c.Step(r).command == Command::None && c.cycles() == 0, "proposal waits for actual engine acceptance before counting cycle");
    r.screen = Screen::Loading; r.outcome = Outcome::Unknown; r.nowMs += 1;
    c.Step(r);
    ok &= Check(c.state() == State::WaitingForBattle && c.cycles() == 0, "native loading acknowledges replay without premature cycle count");
    for (int i = 0; i < 3; ++i) {
        b.nowMs = 1000 + i * 1000; c.Step(b);
        ok &= Check(c.cycles() == static_cast<unsigned>(i+1) && c.state() == State::InBattle, "same-stage battle entry confirms repeat cycle");
        r = Results(b.nowMs + 100); c.Step(r); r.screen = Screen::Loading; r.outcome = Outcome::Unknown; c.Step(r);
    }
    c.Toggle();
    ok &= Check(!c.enabled() && c.Step(Results()).command == Command::None, "F2 off stops all new navigation commands");
    c.Toggle(); b = Battle(); c.Step(b); b.stage.stage = 99; c.Step(b);
    ok &= Check(!c.enabled() && !strcmp(c.reason(), "stage identity changed"), "manual stage change stops stale replay");
    c.Toggle(); b = Battle(); c.Step(b); b.outcome = Outcome::Defeat; c.Step(b);
    ok &= Check(!c.enabled() && !strcmp(c.reason(), "defeat"), "defeat stops repeat without replay");
    c.Toggle(); c.Step(Battle()); r = Results(); c.Step(r); r.nowMs = 3100;
    ok &= Check(c.Step(r).command == Command::Replay && c.pending().attempts == 2, "unchanged rejected replay permits one retry");
    r.nowMs = 6100; c.Step(r);
    ok &= Check(!c.enabled(), "second unacknowledged replay stops repeat");
    c.Toggle(); c.Step(Battle()); r = Results(); c.Step(r); c.DispatchRejected();
    ok &= Check(c.Step(r).command == Command::Replay && c.pending().attempts == 2, "pre-confirmation rejection consumes bounded retry");
    c.Toggle(); c.Toggle(); c.Step(Battle()); r = Results(); r.canReplay = false;
    ok &= Check(c.Step(r).command == Command::AdvanceResults, "fallback begins with verified result progression");
    r.screen = Screen::Hub; r.outcome = Outcome::Unknown; r.canReturn = true;
    ok &= Check(c.Step(r).command == Command::ReturnToSelection, "verified hub route requests native selector");
    r.screen = Screen::Selection; r.stageAvailable = true; r.selected = {88,0};
    ok &= Check(c.Step(r).command == Command::SelectStage, "select by stable stage key rather than remembered row");
    r.selected = {17,0};
    ok &= Check(c.Step(r).command == Command::StartStage, "observed matching selection permits stage start");
    r.screen = Screen::StartMenu;
    ok &= Check(c.Step(r).command == Command::StartStage, "stage start may require a distinct verified pre-battle menu");
    c.Toggle(); c.Toggle(); c.Step(Battle()); r = Results(); r.canReplay = false; c.Step(r);
    r.screen = Screen::Selection; r.outcome = Outcome::Unknown; r.stageAvailable = false; c.Step(r);
    ok &= Check(!c.enabled(), "unavailable stage stops repeat");
    c.Toggle(); c.Step(Battle()); r = Results(); c.Step(r); r.screen = Screen::Loading; r.nowMs = 101; c.Step(r);
    r.nowMs = 30102; c.Step(r);
    ok &= Check(!c.enabled() && !strcmp(c.reason(), "navigation timeout"), "loading/navigation timeout remains bounded after command acceptance");
    c.Toggle(); c.Step(Battle()); r = Results(); r.screen = Screen::ClearNotice;
    ok &= Check(c.OwnsClear(r) && c.Step(r).command == Command::ConfirmNotice, "F2 can own verified clear notices with combat autoplay off");
    r.stage.stage = 18;
    ok &= Check(!c.OwnsClear(r), "F2 cannot own a notice from another stage");
    c.Toggle(); c.Toggle(); b = Battle(); b.normalStage = false; c.Step(b);
    ok &= Check(!c.enabled(), "unsupported stage modes fail closed");
    c.Toggle(); Observation unknown; unknown.screen = Screen::Hub; unknown.settled = true;
    ok &= Check(c.Step(unknown).command == Command::None && c.state() == State::Armed, "F2 armed in hub waits for manually entered stage");
    c.Step(Battle()); c.Step(Results()); unknown.nowMs = 200; c.Step(unknown);
    ok &= Check(!c.enabled(), "unknown/unavailable return route stops without blind input");
    return ok ? 0 : 1;
}
