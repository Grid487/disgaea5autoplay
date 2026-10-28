#pragma once
#include <chrono>
#include <cstdint>
#include <array>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AutoPlay::Planning {

inline thread_local uint64_t nextSerial = 0;

using Clock = uint64_t(*)();
inline uint64_t SteadyNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline uint64_t ThreadCpuNow() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 100;
#else
    return SteadyNow();
#endif
}

struct Counts {
    uint32_t work = 0, previews = 0, effects = 0, paths = 0;
};

enum class Resource { Work, Preview, Effect, Path };
enum class YieldReason { None, Deadline, WorkLimit, PreviewLimit, EffectLimit, PathLimit, PassLimit };
inline const char* YieldName(YieldReason reason) {
    switch (reason) {
    case YieldReason::Deadline: return "deadline";
    case YieldReason::WorkLimit: return "work limit";
    case YieldReason::PreviewLimit: return "preview limit";
    case YieldReason::EffectLimit: return "effect limit";
    case YieldReason::PathLimit: return "path limit";
    case YieldReason::PassLimit: return "scheduler pass limit";
    default: return "none";
    }
}

// A soft deadline: an engine query already in progress cannot be interrupted.
// Every resumable cursor is checked BEFORE it advances, so yielding loses no
// candidate and never turns an unfinished search into a completed decision.
struct PlanningBudget {
    Clock clock;
    uint64_t start, allowance;
    Counts used;
    Counts limits{8192, 8, 32, 4};
    bool blocked = false;
    uint32_t unitDepth = 0, passes = 0;
    YieldReason yield = YieldReason::None;
    const uint64_t serial = ++nextSerial;

    explicit PlanningBudget(Clock time = SteadyNow, uint64_t nanoseconds = 1000000)
        : clock(time), start(time()), allowance(nanoseconds) {}
    bool Available() {
        if (blocked) return false;
        // An admitted atomic operation is allowed to finish. Setup alone must
        // never consume every frame: the first bounded work unit gets a turn.
        if (unitDepth) return true;
        if (used.work >= limits.work) { yield = YieldReason::WorkLimit; return false; }
        if (used.work == 0 && used.previews == 0 && used.effects == 0 && used.paths == 0) return true;
        if (clock() - start >= allowance) { yield = YieldReason::Deadline; return false; }
        return true;
    }
    bool Admit(Resource resource) {
        if (!Available()) return false;
        if (resource == Resource::Preview && used.previews >= limits.previews) {
            blocked = true; yield = YieldReason::PreviewLimit; return false;
        }
        if (resource == Resource::Effect && used.effects >= limits.effects) {
            blocked = true; yield = YieldReason::EffectLimit; return false;
        }
        if (resource == Resource::Path && used.paths >= limits.paths) {
            blocked = true; yield = YieldReason::PathLimit; return false;
        }
        if (unitDepth++ == 0) ++used.work;
        if (resource == Resource::Effect) ++used.effects;
        if (resource == Resource::Path) ++used.paths;
        return true;
    }
    void FinishUnit() { if (unitDepth) --unitDepth; }
    // One-shot helpers retained for isolated counter tests. Real work uses
    // Unit below to keep its admission alive across validation/native calls.
    bool Work() { if (!Admit(Resource::Work)) return false; FinishUnit(); return true; }
    bool PreviewAvailable() {
        if (!Available()) return false;
        if (used.previews >= limits.previews) { blocked = true; yield = YieldReason::PreviewLimit; return false; }
        return true;
    }
    // Used after PreviewAvailable, within one atomic candidate work item.
    bool Preview() {
        if (used.previews >= limits.previews) { blocked = true; yield = YieldReason::PreviewLimit; return false; }
        ++used.previews;
        return true;
    }
    bool Effect() { if (!Admit(Resource::Effect)) return false; FinishUnit(); return true; }
    bool Path() { if (!Admit(Resource::Path)) return false; FinishUnit(); return true; }
    bool Dispatch() {
        if (!Available()) return false;
        // Phase-only transitions do not spend the first real work admission,
        // but even a broken/no-op job cannot spin indefinitely inside the hook.
        if (passes >= limits.work) { blocked = true; yield = YieldReason::PassLimit; return false; }
        ++passes;
        return true;
    }
};

inline thread_local PlanningBudget* active = nullptr;
enum class QueryKind : size_t { Memory, Enumerate, RosterSkill, RosterScore, Fingerprint, Preview, SourceTile, Count };
struct QuerySample { uint64_t count = 0, ns = 0; };
using QuerySamples = std::array<QuerySample, static_cast<size_t>(QueryKind::Count)>;
inline thread_local QuerySamples querySamples;
#ifdef D5_QUERY_PROFILE
inline constexpr bool queryProfilingEnabled = true;
#else
inline constexpr bool queryProfilingEnabled = false;
#endif
struct QueryScope {
    QueryKind kind;
    bool enabled;
    uint64_t begin;
    explicit QueryScope(QueryKind k) : kind(k), enabled(queryProfilingEnabled && active != nullptr), begin(enabled ? SteadyNow() : 0) {}
    ~QueryScope() { if (enabled) { auto& s=querySamples[static_cast<size_t>(kind)]; ++s.count; s.ns += SteadyNow()-begin; } }
};
struct Scope {
    PlanningBudget* previous;
    explicit Scope(PlanningBudget& budget) : previous(active) { active = &budget; }
    ~Scope() { active = previous; }
};
struct Unit {
    PlanningBudget* owner;
    bool entered;
    explicit Unit(Resource resource = Resource::Work)
        : owner(active), entered(!owner || owner->Admit(resource)) {}
    explicit operator bool() const { return entered; }
    ~Unit() { if (owner && entered) owner->FinishUnit(); }
    Unit(const Unit&) = delete;
    Unit& operator=(const Unit&) = delete;
};
inline bool Work() { return !active || active->Work(); }
inline bool PreviewAvailable() { return !active || active->PreviewAvailable(); }
inline bool Preview() { return !active || active->Preview(); }
inline bool Effect() { return !active || active->Effect(); }
inline bool Path() { return !active || active->Path(); }
inline bool Available() { return !active || active->Available(); }

struct Telemetry {
    uint64_t start = 0, elapsed = 0, cpu = 0, activeTime = 0;
    uint32_t frames = 0, reuses = 0;
    int actor = -1;
    std::array<int64_t, 4> cursor{};
    YieldReason yield = YieldReason::None;
    uint64_t lastProgressReport = 0;
    Counts calls;
    QuerySamples queries{};
    bool started = false, reported = false, startReported = false;
};
using Reporter = void(*)(const char*, const Telemetry&, const char*);
inline Reporter reporter = nullptr;
inline void Report(const char* phase, Telemetry& timing, const char* reason) {
    if (timing.started && reporter) reporter(phase, timing, reason);
    timing.reported = true;
}

// Execute successive small resumable batches in the SAME frame until the
// shared budget is spent. The unscoped path retains one-batch offline APIs.
template<class Complete, class Advance>
bool Run(const char* phase, Telemetry& timing, Complete complete, Advance advance) {
    if (complete()) { ++timing.reuses; return true; }
    const Clock time = active ? active->clock : SteadyNow;
    const uint64_t begin = time();
    const uint64_t cpuBegin = ThreadCpuNow();
    const Counts before = active ? active->used : Counts{};
    const QuerySamples queriesBefore = querySamples;
    if (!timing.started) { timing.started = true; timing.start = begin; }
    if (!timing.startReported) {
        timing.startReported = true;
        timing.lastProgressReport = begin;
        if (reporter) reporter(phase, timing, "start");
    }
    ++timing.frames;
    do {
        if (active && !active->Dispatch()) break;
        advance();
    } while (active && !complete());
    const uint64_t end = time();
    if constexpr (queryProfilingEnabled) for (size_t i=0; i<timing.queries.size(); ++i) {
        timing.queries[i].count += querySamples[i].count - queriesBefore[i].count;
        timing.queries[i].ns += querySamples[i].ns - queriesBefore[i].ns;
    }
    timing.cpu += ThreadCpuNow() - cpuBegin;
    timing.activeTime += end - begin;
    timing.elapsed = end - timing.start;
    if (active) {
        timing.yield = active->yield;
        timing.calls.work += active->used.work - before.work;
        timing.calls.previews += active->used.previews - before.previews;
        timing.calls.effects += active->used.effects - before.effects;
        timing.calls.paths += active->used.paths - before.paths;
    }
    if (complete() && !timing.reported) Report(phase, timing, "complete");
    else if (!complete() && end - timing.lastProgressReport >= 1000000000ull) {
        timing.lastProgressReport = end;
        if (reporter) reporter(phase, timing, "progress");
    }
    return complete();
}

} // namespace AutoPlay::Planning
