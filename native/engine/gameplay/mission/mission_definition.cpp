#include "gameplay/mission/mission_definition.h"

#include <cmath>
#include <cstdlib>
#include <format>
#include <set>

#include "core/file.h"
#include "core/name_hash.h"

namespace sspc::mission {
namespace {

constexpr float kMaxSeconds = 3600.0f;

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(sep, pos);
        if (end == std::string_view::npos) end = s.size();
        out.push_back(Trim(s.substr(pos, end - pos)));
        pos = end + 1;
    }
    return out;
}

std::vector<std::string_view> Words(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool ParseSeconds(std::string_view w, float& out) {
    const std::string s(w);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != 0 || !std::isfinite(v) || v < 0 || v > kMaxSeconds) return false;
    out = static_cast<float>(v);
    return true;
}

bool ParseInt(std::string_view w, s64& out) {
    const std::string s(w);
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != 0) return false;
    out = v;
    return true;
}

bool IsName(std::string_view w) {
    if (w.empty() || w.size() > 128) return false;
    for (char c : w) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                        c == '(' || c == ')' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// 0x<hex> = literal value (an OID), otherwise the NameHash of the name.
bool ParseId(std::string_view w, u64& out) {
    if (w.size() > 2 && w[0] == '0' && (w[1] == 'x' || w[1] == 'X')) {
        if (w.size() > 18) return false;
        u64 v = 0;
        for (char c : w.substr(2)) {
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<u64>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<u64>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<u64>(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }
    if (!IsName(w)) return false;
    out = NameHash(w);
    return true;
}

class Parser {
public:
    Parser(MissionDef& def, std::vector<std::string>& errors) : def_(def), errors_(errors) {}

    void Line(int no, std::string_view raw) {
        line_ = no;
        const size_t hash = raw.find('#');
        std::string_view s = Trim(hash == std::string_view::npos ? raw : raw.substr(0, hash));
        if (s.empty()) return;
        const size_t sp = s.find_first_of(" \t");
        const std::string_view kw = s.substr(0, sp);
        const std::string_view rest = sp == std::string_view::npos ? std::string_view{} : Trim(s.substr(sp));
        if (!header_) {
            if (kw == "sspc-mission" && rest == "1") header_ = true;
            else Error("the first statement must be 'sspc-mission 1'");
            return;
        }
        if (kw == "mission") return Mission(rest);
        if (kw == "objective") return Objective(rest);
        if (kw == "trigger") return Trigger(rest);
        if (block_ == Block::None) return Error(std::format("'{}' outside a mission/objective/trigger block", kw));
        if (block_ == Block::Mission) return MissionProp(kw, rest);
        if (block_ == Block::Objective) return ObjectiveProp(kw, rest);
        TriggerProp(kw, rest);
    }

    bool Finish() {
        if (!header_) Error("missing 'sspc-mission 1' header");
        if (!have_mission_) Error("no 'mission' statement");
        // Resolve trigger scopes now that every objective is known.
        for (size_t i = 0; i < def_.triggers.size(); ++i) {
            if (scope_names_[i].empty()) continue;
            int found = -1;
            for (size_t o = 0; o < def_.objectives.size(); ++o) {
                if (def_.objectives[o].name == scope_names_[i]) found = static_cast<int>(o);
            }
            if (found < 0) {
                errors_.push_back(std::format("line {}: trigger '{}' scope '{}' is not an objective", def_.triggers[i].line,
                                              def_.triggers[i].name, scope_names_[i]));
            }
            def_.triggers[i].scope = found;
        }
        return errors_.empty();
    }

private:
    enum class Block { None, Mission, Objective, Trigger };

    void Error(std::string msg) { errors_.push_back(std::format("line {}: {}", line_, msg)); }

    void Mission(std::string_view rest) {
        if (have_mission_) return Error("only one mission per file");
        if (!IsName(rest)) return Error("mission needs a name");
        have_mission_ = true;
        def_.name = std::string(rest);
        def_.id = NameHash(rest);
        block_ = Block::Mission;
    }
    void MissionProp(std::string_view kw, std::string_view rest) {
        if (kw == "source") def_.source = std::string(rest);
        else if (kw == "provenance") def_.provenance = std::string(rest);
        else Error(std::format("unknown mission property '{}'", kw));
    }

    void Objective(std::string_view rest) {
        if (!have_mission_) return Error("objective before the mission statement");
        if (!IsName(rest)) return Error("objective needs a name");
        ObjectiveDef o;
        o.name = std::string(rest);
        o.id = NameHash(rest);
        o.line = line_;
        def_.objectives.push_back(std::move(o));
        block_ = Block::Objective;
        seen_.clear();
    }
    void ObjectiveProp(std::string_view kw, std::string_view rest) {
        ObjectiveDef& o = def_.objectives.back();
        if (!seen_.insert(std::string(kw)).second) return Error(std::format("duplicate '{}' in objective '{}'", kw, o.name));
        if (kw == "brief" || kw == "note") {
            if (!IsName(rest)) return Error(std::format("'{}' needs a text key", kw));
            (kw == "brief" ? o.brief : o.note) = std::string(rest);
            (kw == "brief" ? o.brief_key : o.note_key) = NameHash(rest);
        } else if (kw == "on_start") Actions(rest, o.on_start);
        else if (kw == "on_complete") Actions(rest, o.on_complete);
        else if (kw == "on_fail") Actions(rest, o.on_fail);
        else if (kw == "complete_when") Conditions(rest, o.complete_when);
        else if (kw == "fail_when") Conditions(rest, o.fail_when);
        else Error(std::format("unknown objective property '{}'", kw));
    }

    void Trigger(std::string_view rest) {
        if (!have_mission_) return Error("trigger before the mission statement");
        const auto w = Words(rest);
        if (w.size() != 2 || !IsName(w[0]) || (w[1] != "once" && w[1] != "repeat")) {
            return Error("expected 'trigger <name> once|repeat'");
        }
        TriggerDef t;
        t.name = std::string(w[0]);
        t.id = NameHash(w[0]);
        t.once = w[1] == "once";
        t.line = line_;
        def_.triggers.push_back(std::move(t));
        scope_names_.emplace_back();
        block_ = Block::Trigger;
        seen_.clear();
    }
    void TriggerProp(std::string_view kw, std::string_view rest) {
        TriggerDef& t = def_.triggers.back();
        if (!seen_.insert(std::string(kw)).second) return Error(std::format("duplicate '{}' in trigger '{}'", kw, t.name));
        if (kw == "scope") {
            if (!IsName(rest)) return Error("scope needs an objective name");
            scope_names_.back() = std::string(rest);
        } else if (kw == "when") Conditions(rest, t.when);
        else if (kw == "do") Actions(rest, t.actions);
        else Error(std::format("unknown trigger property '{}'", kw));
    }

    void Conditions(std::string_view rest, std::vector<Condition>& out) {
        if (rest.empty()) return Error("empty condition list");
        for (std::string_view part : Split(rest, '&')) {
            const auto w = Words(part);
            if (w.empty()) return Error("empty condition");
            Condition c;
            if (w[0] == "event" && (w.size() == 2 || w.size() == 3)) {
                const auto t = EventTypeFromString(w[1]);
                if (!t) return Error(std::format("unknown event type '{}'", w[1]));
                c.kind = ConditionKind::Event;
                c.event = *t;
                if (w.size() == 3) {
                    if (!ParseId(w[2], c.subject)) return Error(std::format("bad subject '{}'", w[2]));
                    c.any_subject = false;
                }
            } else if ((w[0] == "flag" || w[0] == "!flag") && w.size() == 2 && IsName(w[1])) {
                c.kind = w[0] == "flag" ? ConditionKind::Flag : ConditionKind::NotFlag;
                c.flag = std::string(w[1]);
            } else if (w[0] == "timer" && w.size() == 2) {
                c.kind = ConditionKind::Timer;
                if (!ParseSeconds(w[1], c.seconds)) return Error(std::format("bad timer '{}'", w[1]));
            } else {
                return Error(std::format("bad condition '{}'", part));
            }
            out.push_back(std::move(c));
        }
    }

    void Actions(std::string_view rest, std::vector<Action>& out) {
        if (rest.empty()) return Error("empty action list");
        for (std::string_view part : Split(rest, ';')) {
            const auto w = Words(part);
            if (w.empty()) return Error("empty action");
            Action a;
            const std::string_view k = w[0];
            auto need = [&](size_t n) {
                if (w.size() != n) Error(std::format("'{}' takes {} argument(s)", k, n - 1));
                return w.size() == n;
            };
            if (k == "hint" || k == "subtitle") {
                if (!need(3)) return;
                a.kind = k == "hint" ? ActionKind::Hint : ActionKind::Subtitle;
                if (!IsName(w[1]) || !ParseSeconds(w[2], a.seconds)) return Error(std::format("bad '{}' arguments", k));
                a.text = std::string(w[1]);
                a.id = NameHash(w[1]);
            } else if (k == "wait") {
                if (!need(2)) return;
                a.kind = ActionKind::Wait;
                if (!ParseSeconds(w[1], a.seconds)) return Error(std::format("bad wait '{}'", w[1]));
            } else if (k == "set" || k == "clear") {
                if (!need(2) || !IsName(w[1])) return Error(std::format("'{}' needs a flag name", k));
                a.kind = k == "set" ? ActionKind::SetFlag : ActionKind::ClearFlag;
                a.text = std::string(w[1]);
            } else if (k == "emit") {
                if (w.size() < 2 || w.size() > 4) return Error("'emit' takes an event type, optional subject and param");
                const auto t = EventTypeFromString(w[1]);
                if (!t) return Error(std::format("unknown event type '{}'", w[1]));
                a.kind = ActionKind::Emit;
                a.event = *t;
                if (w.size() >= 3 && !ParseId(w[2], a.id)) return Error(std::format("bad subject '{}'", w[2]));
                if (w.size() == 4 && !ParseInt(w[3], a.value)) return Error(std::format("bad param '{}'", w[3]));
            } else if (k == "enable" || k == "disable" || k == "place_hero") {
                if (!need(2)) return;
                a.kind = k == "enable" ? ActionKind::Enable : k == "disable" ? ActionKind::Disable : ActionKind::PlaceHero;
                if (!ParseId(w[1], a.id)) return Error(std::format("bad id '{}'", w[1]));
                a.text = std::string(w[1]);
            } else if (k == "award_xp") {
                if (!need(2)) return;
                a.kind = ActionKind::AwardXp;
                if (!ParseInt(w[1], a.value) || a.value < 0 || a.value > 1000000) return Error(std::format("bad xp '{}'", w[1]));
            } else if (k == "complete_objective" || k == "complete_mission" || k == "fail_mission") {
                if (!need(1)) return;
                a.kind = k == "complete_objective" ? ActionKind::CompleteObjective
                         : k == "complete_mission" ? ActionKind::CompleteMission
                                                   : ActionKind::FailMission;
            } else if (k == "log") {
                a.kind = ActionKind::Log;
                a.text = std::string(Trim(part.substr(3)));
            } else {
                return Error(std::format("unknown action '{}'", k));
            }
            out.push_back(std::move(a));
        }
    }

    MissionDef& def_;
    std::vector<std::string>& errors_;
    int line_ = 0;
    bool header_ = false;
    bool have_mission_ = false;
    Block block_ = Block::None;
    std::set<std::string> seen_;
    std::vector<std::string> scope_names_;
};

}  // namespace

const char* ToString(ActionKind k) {
    static const char* const kNames[] = {"hint", "subtitle", "wait", "set", "clear", "emit", "enable", "disable",
                                         "place_hero", "award_xp", "complete_objective", "complete_mission",
                                         "fail_mission", "log"};
    const auto i = static_cast<size_t>(k);
    return i < std::size(kNames) ? kNames[i] : "?";
}

bool ValidateMission(const MissionDef& def, std::vector<std::string>& errors) {
    const size_t before = errors.size();
    if (def.name.empty()) errors.push_back("mission has no name");
    if (def.objectives.empty()) errors.push_back(std::format("mission '{}' has no objectives", def.name));
    if (def.objectives.size() > 4096 || def.triggers.size() > 4096) errors.push_back("too many objectives/triggers");
    std::set<std::string> names;
    for (const ObjectiveDef& o : def.objectives) {
        if (!names.insert(o.name).second) errors.push_back(std::format("line {}: duplicate objective '{}'", o.line, o.name));
    }
    std::set<std::string> tnames;
    for (const TriggerDef& t : def.triggers) {
        if (!tnames.insert(t.name).second) errors.push_back(std::format("line {}: duplicate trigger '{}'", t.line, t.name));
        if (t.when.empty()) errors.push_back(std::format("line {}: trigger '{}' has no 'when'", t.line, t.name));
        if (t.actions.empty()) errors.push_back(std::format("line {}: trigger '{}' has no 'do'", t.line, t.name));
        if (t.scope >= static_cast<int>(def.objectives.size())) errors.push_back(std::format("trigger '{}' has a bad scope", t.name));
    }
    return errors.size() == before;
}

bool ParseMission(std::string_view text, MissionDef& out, std::vector<std::string>& errors) {
    out = MissionDef{};
    const size_t before = errors.size();
    Parser p(out, errors);
    int no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        p.Line(++no, text.substr(pos, end - pos));
        pos = end + 1;
    }
    p.Finish();
    if (errors.size() == before) ValidateMission(out, errors);
    return errors.size() == before;
}

bool LoadMissionFile(const std::wstring& path, MissionDef& out, std::vector<std::string>& errors) {
    std::vector<u8> bytes;
    if (!file::ReadAll(path, bytes)) {
        errors.push_back("cannot read the mission file");
        return false;
    }
    if (bytes.size() > (1u << 20)) {
        errors.push_back("mission file larger than 1 MiB");
        return false;
    }
    return ParseMission(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), out, errors);
}

}  // namespace sspc::mission
