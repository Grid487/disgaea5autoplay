#pragma once
#include <cstdint>

// Pure replay policy. Native adapters supply verified identities/outcomes and
// perform commands on the owning game thread; this file never reads game memory.
namespace AutoPlay::StageRepeat {

struct StageKey {
    int32_t stage = -1;
    int32_t variant = -1;
    bool valid() const { return stage >= 0 && variant >= 0; }
    bool operator==(const StageKey& other) const { return stage == other.stage && variant == other.variant; }
    bool operator!=(const StageKey& other) const { return !(*this == other); }
};
enum class Screen { Unknown, Hub, Battle, ClearNotice, Results, Selection, StartMenu, Loading };
enum class Outcome { Unknown, Victory, Defeat };
enum class Command { None, ConfirmNotice, AdvanceResults, Replay, ReturnToSelection, SelectStage, StartStage };
enum class State { Off, Armed, InBattle, Clearing, Navigating, WaitingForBattle };
struct ToggleLatch {
    bool previous = false;
    bool Pressed(bool down) { const bool edge = down && !previous; previous = down; return edge; }
};
struct Observation {
    Screen screen = Screen::Unknown;
    Outcome outcome = Outcome::Unknown;
    StageKey stage{}, selected{};
    uint64_t instance = 0, revision = 0, nowMs = 0;
    bool ready = false, settled = false, normalStage = false, stageAvailable = false;
    bool canReplay = false, canReturn = false;
};
struct Decision {
    Command command = Command::None;
    StageKey stage{};
    const char* reason = "waiting";
};
struct Pending {
    Command command = Command::None;
    Screen source = Screen::Unknown;
    uint64_t instance = 0, revision = 0, deadline = 0;
    int attempts = 0;
};

class Controller {
    State state_ = State::Off;
    StageKey stage_{};
    Pending pending_{};
    bool victory_ = false;
    uint32_t cycles_ = 0;
    uint64_t lastProgress_ = 0;
    const char* reason_ = "off";

    bool Accepted(const Observation& o) const {
        switch (pending_.command) {
        case Command::ConfirmNotice:
            return o.screen != Screen::ClearNotice || o.instance != pending_.instance;
        case Command::SelectStage:
            return o.screen == Screen::Selection && o.selected == stage_;
        case Command::Replay: case Command::StartStage:
            return o.screen == Screen::Loading || o.screen == Screen::Battle ||
                (o.screen == Screen::StartMenu && o.selected == stage_ && pending_.source != Screen::StartMenu);
        case Command::ReturnToSelection:
            return o.screen == Screen::Selection;
        case Command::AdvanceResults:
            return o.screen != pending_.source || o.instance != pending_.instance || o.revision != pending_.revision;
        default: return false;
        }
    }
    Decision Issue(Command command, const Observation& o, const char* why) {
        pending_ = {command, o.screen, o.instance, o.revision, o.nowMs + 3000, 1};
        return {command, stage_, why};
    }
public:
    bool enabled() const { return state_ != State::Off; }
    State state() const { return state_; }
    StageKey stage() const { return stage_; }
    const Pending& pending() const { return pending_; }
    uint32_t cycles() const { return cycles_; }
    const char* reason() const { return reason_; }
    void Stop(const char* reason) { state_ = State::Off; pending_ = {}; victory_ = false; reason_ = reason; }
    void Toggle() {
        if (enabled()) { Stop("F2 disabled"); return; }
        state_ = State::Armed; stage_ = {}; pending_ = {}; victory_ = false; cycles_ = 0; reason_ = "armed for stage";
    }
    // The native dispatcher calls this when pre-confirmation revalidation
    // rejects a proposal. Failed dispatches must not look like accepted actions.
    void DispatchRejected() { if (pending_.command != Command::None) pending_.deadline = 0; }
    bool OwnsClear(const Observation& o) const {
        return enabled() && stage_.valid() && stage_ == o.stage &&
            (victory_ || o.outcome == Outcome::Victory) &&
            (o.screen == Screen::ClearNotice || o.screen == Screen::Results);
    }
    Decision Step(const Observation& o) {
        if (!enabled()) return {Command::None, {}, reason_};
        if (o.outcome == Outcome::Defeat) { Stop("defeat"); return {Command::None, {}, reason_}; }
        if (o.screen == Screen::Battle) {
            if (!o.stage.valid() || !o.normalStage) { Stop("unverified or unsupported stage"); return {Command::None, {}, reason_}; }
            if (stage_.valid() && stage_ != o.stage) { Stop("stage identity changed"); return {Command::None, {}, reason_}; }
            if (state_ == State::WaitingForBattle || (victory_ && state_ == State::Navigating)) ++cycles_;
            stage_ = o.stage; state_ = State::InBattle; victory_ = false; pending_ = {};
            reason_ = "waiting for player or F1 to win";
            return {Command::None, stage_, reason_}; // Never plays combat or changes F1.
        }
        if (state_ == State::Armed) {
            if (o.stage.valid() && o.normalStage && o.outcome == Outcome::Victory &&
                (o.screen == Screen::ClearNotice || o.screen == Screen::Results)) stage_ = o.stage;
            else return {Command::None, {}, "waiting for manual stage entry"};
        }
        if (o.stage.valid() && o.stage != stage_ && o.screen != Screen::Selection) {
            Stop("stage identity changed"); return {Command::None, {}, reason_};
        }
        if (o.outcome == Outcome::Victory && o.stage == stage_ && !victory_) {
            victory_ = true; state_ = State::Clearing; lastProgress_ = o.nowMs;
        }
        if (!victory_) {
            if (o.settled && o.screen != Screen::Loading) Stop("battle ended without verified victory");
            return {Command::None, stage_, reason_};
        }
        if (o.nowMs - lastProgress_ >= 30000) { Stop("navigation timeout"); return {Command::None, {}, reason_}; }
        if (pending_.command != Command::None) {
            if (Accepted(o)) {
                if (pending_.command == Command::Replay || pending_.command == Command::StartStage)
                    state_ = State::WaitingForBattle;
                lastProgress_ = o.nowMs;
                pending_ = {};
            } else {
                const bool changed = o.screen != pending_.source || o.instance != pending_.instance || o.revision != pending_.revision;
                if (changed && o.settled) {
                    Stop("pending command source changed"); return {Command::None, {}, reason_};
                }
                if (o.nowMs < pending_.deadline || !o.ready || changed) return {Command::None, stage_, "awaiting engine acceptance"};
                if (pending_.attempts >= 2) { Stop("two attempts exhausted"); return {Command::None, {}, reason_}; }
                ++pending_.attempts; pending_.deadline = o.nowMs + 3000;
                return {pending_.command, stage_, "retry rejected transition"};
            }
        }
        if (o.screen == Screen::Loading) return {Command::None, stage_, "waiting for native loading"};
        if (o.screen == Screen::Unknown && o.settled) { Stop("unexpected screen"); return {Command::None, {}, reason_}; }
        if (!o.ready) return {Command::None, stage_, "waiting for UI readiness"};
        switch (o.screen) {
        case Screen::ClearNotice: return Issue(Command::ConfirmNotice, o, "verified clear notification");
        case Screen::Results:
            return Issue(o.canReplay ? Command::Replay : Command::AdvanceResults, o, "verified victory results");
        case Screen::Hub:
            state_ = State::Navigating;
            if (o.canReturn) return Issue(Command::ReturnToSelection, o, "return to recorded stage selection");
            Stop("no verified route to stage selection"); break;
        case Screen::Selection:
            state_ = State::Navigating;
            if (!o.stageAvailable) { Stop("recorded stage unavailable"); break; }
            if (o.selected != stage_) return Issue(Command::SelectStage, o, "select stage by native identity");
            return Issue(Command::StartStage, o, "start same verified stage");
        case Screen::StartMenu:
            if (o.selected == stage_) return Issue(Command::StartStage, o, "confirm same-stage battle start");
            Stop("battle-start identity mismatch"); break;
        default: if (o.settled) Stop("unexpected screen"); break;
        }
        return {Command::None, stage_, reason_};
    }
};
} // namespace AutoPlay::StageRepeat
