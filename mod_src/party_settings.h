#pragma once
#include "party_roster.h"
#include <map>
#include <sstream>
#include <limits>

namespace AutoPlay::Party {
using Profiles = std::map<std::string, PartySettings>;
inline bool ValidNamespace(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
inline bool ParseKey(const std::string& value, CharacterKey& result) {
    if (value.empty() || value.size() > 20) return false;
    result = 0;
    for (const char c : value) {
        if (c < '0' || c > '9' || result > (std::numeric_limits<uint64_t>::max() - (c-'0')) / 10)
            return false;
        result = result * 10 + static_cast<uint64_t>(c-'0');
    }
    return result != 0;
}
inline std::string Encode(const Profiles& profiles) {
    std::ostringstream out;
    out << "; Disgaea 5 Autoplay party preferences. Game saves are never modified.\n"
           "[AutoplayParty]\nFormat=1\n";
    for (const auto& item : profiles) {
        const PartySettings& p = item.second;
        if (!ValidNamespace(item.first) || item.first != p.saveNamespace || Validate(p)) continue;
        out << "\n[Profile." << item.first << "]\nEnabled=" << (p.enabled ? 1 : 0) << "\nMembers=";
        for (size_t i = 0; i < p.members.size(); ++i) {
            if (i) out << ',';
            out << p.members[i];
        }
        out << '\n';
    }
    return out.str();
}
inline bool Decode(std::string input, Profiles& profiles, std::string& error) {
    profiles.clear(); error.clear();
    if (input.size() > 128 * 1024) { error = "Party settings file is too large"; return false; }
    if (input.compare(0,3,"\xEF\xBB\xBF") == 0) input.erase(0,3);
    std::istringstream stream(input);
    std::string line, section;
    bool formatSeen = false, enabledSeen = false, membersSeen = false;
    PartySettings current;
    auto finish = [&]() {
        if (current.saveNamespace.empty()) return true;
        if (!enabledSeen || !membersSeen || Validate(current) || profiles.size() >= 128 ||
            profiles.count(current.saveNamespace)) return false;
        profiles.emplace(current.saveNamespace, current); return true;
    };
    auto fail = [&]() { profiles.clear(); error = "Invalid party settings; open F3 and select the party again"; return false; };
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            if (!finish()) return fail();
            current = {}; enabledSeen = membersSeen = false;
            section = line.substr(1,line.size()-2);
            if (section == "AutoplayParty") { if (formatSeen) return fail(); }
            else if (section.compare(0,8,"Profile.") == 0 && ValidNamespace(section.substr(8)))
                current.saveNamespace = section.substr(8);
            else return fail();
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) return fail();
        const std::string key = line.substr(0,equals), value = line.substr(equals+1);
        if (section == "AutoplayParty" && key == "Format" && value == "1" && !formatSeen) {
            formatSeen = true; continue;
        }
        if (!formatSeen || current.saveNamespace.empty()) return fail();
        if (key == "Enabled" && !enabledSeen && (value == "0" || value == "1")) {
            enabledSeen = true; current.enabled = value == "1";
        } else if (key == "Members" && !membersSeen) {
            membersSeen = true;
            if (value.empty()) continue;
            size_t begin = 0;
            while (begin <= value.size()) {
                const size_t comma = value.find(',', begin);
                CharacterKey serial = 0;
                if (!ParseKey(value.substr(begin,comma == std::string::npos ? comma : comma-begin),serial)) return fail();
                current.members.push_back(serial);
                if (current.members.size() > kMaximumMembers) return fail();
                if (comma == std::string::npos) break;
                begin = comma+1;
            }
        } else return fail();
    }
    if (!formatSeen || !finish()) return fail();
    return true;
}
} // namespace AutoPlay::Party
