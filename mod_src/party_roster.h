#pragma once

// Pure party policy and editor state. No native memory, file I/O, or input calls.
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace AutoPlay::Party {
constexpr int kAutomaticLimit = 5;
constexpr int kMaximumMembers = 10;
using CharacterKey = uint64_t;

struct PartySettings {
    std::string saveNamespace;
    bool enabled = false;
    std::vector<CharacterKey> members;
};

inline const char* Validate(const PartySettings& settings) {
    if (settings.members.size() > kMaximumMembers) return "Choose at most 10 heroes";
    if (settings.enabled && settings.members.empty()) return "Choose at least one hero";
    for (size_t i = 0; i < settings.members.size(); ++i) {
        if (!settings.members[i]) return "A hero has no verified identity";
        if (std::find(settings.members.begin(), settings.members.begin() + i,
                      settings.members[i]) != settings.members.begin() + i)
            return "The same hero cannot occupy two slots";
    }
    return nullptr;
}

struct RosterEntry {
    CharacterKey key = 0;
    std::string name;
    int ordinal = -1;
    int level = 0;
    bool alive = false;
    bool deployed = false;
};

inline bool UniqueRoster(const std::vector<RosterEntry>& roster) {
    for (size_t i = 0; i < roster.size(); ++i) {
        if (!roster[i].key) return false;
        for (size_t j = 0; j < i; ++j)
            if (roster[i].key == roster[j].key) return false;
    }
    return true;
}

struct DeploymentPolicy {
    bool custom = false;
    int limit = kAutomaticLimit;
    std::vector<CharacterKey> preferred;

    static DeploymentPolicy From(const PartySettings& settings) {
        DeploymentPolicy policy;
        if (settings.enabled && !Validate(settings)) {
            policy.custom = true;
            policy.preferred = settings.members;
            policy.limit = static_cast<int>(settings.members.size());
        }
        return policy;
    }
    bool CanAdd(int livingControllable, bool nativeAvailable = true) const {
        return nativeAvailable && livingControllable >= 0 &&
            livingControllable < std::clamp(limit, 1, kMaximumMembers);
    }
    int PreferredOrdinal(const std::vector<RosterEntry>& eligible) const {
        if (!custom || !UniqueRoster(eligible)) return -1;
        for (CharacterKey key : preferred) {
            const auto row = std::find_if(eligible.begin(), eligible.end(),
                [key](const RosterEntry& r) { return r.key == key && r.alive && !r.deployed; });
            if (row != eligible.end()) return row->ordinal;
        }
        return -1; // Caller uses the verified automatic ranking for a replacement.
    }
};

struct BattleParty {
    bool inCombat = false;
    PartySettings active;
    void Started(const PartySettings& configured) { active = configured; inCombat = true; }
    void Finished() { inCombat = false; }
    DeploymentPolicy Policy() const { return DeploymentPolicy::From(active); }
};

enum class Page { Options, Roster, Selected, SelectedActions };
enum class EditorResult { None, Applied, Canceled };
class Editor {
public:
    PartySettings draft;
    std::vector<RosterEntry> roster;
    Page page = Page::Options;
    size_t cursor = 0;
    size_t selectedCursor = 0;
    bool appliesNextBattle = false;
    std::string message;
    EditorResult result = EditorResult::None;

    void Open(const PartySettings& saved, std::vector<RosterEntry> entries, bool inCombat) {
        *this = Editor{};
        draft = saved; roster = std::move(entries); appliesNextBattle = inCombat;
    }
    size_t RowCount() const {
        return page == Page::Options ? 5u : page == Page::SelectedActions ? 4u :
            page == Page::Roster ? roster.size() : draft.members.size();
    }
    void Move(int direction) {
        const size_t count = RowCount();
        if (!count) { cursor = 0; return; }
        cursor = direction < 0 ? (cursor ? cursor - 1 : count - 1) : (cursor + 1) % count;
    }
    void SwitchPage(int direction) {
        if (page == Page::SelectedActions) { page = Page::Selected; cursor = selectedCursor; return; }
        const int next = (static_cast<int>(page) + (direction < 0 ? 2 : 1)) % 3;
        page = static_cast<Page>(next); cursor = 0; message.clear();
    }
    bool Toggle(CharacterKey key) {
        const auto existing = std::find(draft.members.begin(), draft.members.end(), key);
        if (existing != draft.members.end()) { draft.members.erase(existing); message.clear(); return true; }
        if (!key) { message = "This hero has no verified identity"; return false; }
        if (draft.members.size() >= kMaximumMembers) { message = "Maximum 10 heroes selected"; return false; }
        draft.members.push_back(key); message.clear(); return true;
    }
    bool Reorder(int direction) {
        if (page != Page::Selected || cursor >= draft.members.size()) return false;
        const int other = static_cast<int>(cursor) + direction;
        if (other < 0 || other >= static_cast<int>(draft.members.size())) return false;
        std::swap(draft.members[cursor], draft.members[static_cast<size_t>(other)]);
        cursor = static_cast<size_t>(other); return true;
    }
    void Confirm() {
        if (page == Page::Roster) {
            if (cursor < roster.size()) Toggle(roster[cursor].key);
        } else if (page == Page::Selected) {
            if (cursor < draft.members.size()) { selectedCursor = cursor; cursor = 0; page = Page::SelectedActions; }
        } else if (page == Page::SelectedActions) {
            const size_t action = cursor;
            page = Page::Selected; cursor = selectedCursor;
            if (action == 0) Reorder(-1);
            else if (action == 1) Reorder(1);
            else if (action == 2 && cursor < draft.members.size()) {
                draft.members.erase(draft.members.begin()+cursor);
                if (cursor && cursor >= draft.members.size()) --cursor;
            }
        } else {
            switch (cursor) {
            case 0: draft.enabled = !draft.enabled; message.clear(); break;
            case 1: page = Page::Roster; cursor = 0; break;
            case 2: page = Page::Selected; cursor = 0; break;
            case 3: Apply(); break;
            case 4: result = EditorResult::Canceled; break;
            }
        }
    }
    bool Apply() {
        if (!UniqueRoster(roster)) { message = "Roster identity is ambiguous; reopen after loading"; return false; }
        if (const char* error = Validate(draft)) { message = error; return false; }
        if (draft.saveNamespace.empty()) { message = "Load a saved game before saving a custom party"; return false; }
        result = EditorResult::Applied; return true;
    }
    void Cancel() { result = EditorResult::Canceled; }
    void Back() {
        if (page == Page::SelectedActions) { page = Page::Selected; cursor = selectedCursor; }
        else if (page != Page::Options) { page = Page::Options; cursor = 0; }
        else Cancel();
    }
};

// Only automation time pauses. Native UI animation keeps its ordinary clock.
struct PausableClock {
    uint64_t pausedAt = 0, excluded = 0;
    bool paused = false;
    uint64_t Now(uint64_t wall) const {
        const uint64_t current = paused ? pausedAt : wall;
        return current >= excluded ? current - excluded : 0;
    }
    void Pause(uint64_t wall) { if (!paused) { pausedAt = wall; paused = true; } }
    void Resume(uint64_t wall) {
        if (paused) { if (wall >= pausedAt) excluded += wall - pausedAt; paused = false; }
    }
};
} // namespace AutoPlay::Party
