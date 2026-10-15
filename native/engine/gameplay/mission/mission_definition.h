// SecondSonPC native runtime - mission definitions (native format).
//
// The game's own missions are compiled splice scripts (docs/native/missions/MISSION_FORMAT_RESEARCH.md);
// their virtual machine is not reconstructed. A native definition lists objectives, triggers,
// conditions and actions and refers to the original data by OID (objects of the mission/chunk
// packages) and by text key (localized texts of the game). Text format, one statement per line,
// '#' starts a comment:
//
//   sspc-mission 1
//   mission <name>
//     source <package>                 provenance <free text>
//   objective <name>
//     brief <text key>                 note <text key>
//     on_start <actions>               on_complete <actions>          on_fail <actions>
//     complete_when <conditions>       fail_when <conditions>
//   trigger <name> once|repeat
//     scope <objective name>           (armed only while that objective is active)
//     when <conditions>                do <actions>
//
// conditions: c1 & c2 & ...  with  event <EventType> [subject] | flag <name> | !flag <name> | timer <s>
// actions:    a1 ; a2 ; ...  with  hint <key> <s> | subtitle <key> <s> | wait <s> | set <flag> |
//             clear <flag> | emit <EventType> [subject] [param] | enable <id> | disable <id> |
//             place_hero <id> | award_xp <n> | complete_objective | complete_mission | fail_mission |
//             log <text>
// subject/id: 0x<hex> (an OID) or a name (its NameHash).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"
#include "gameplay/events.h"

namespace sspc::mission {

enum class ConditionKind : u8 { Event, Flag, NotFlag, Timer };

struct Condition {
    ConditionKind kind = ConditionKind::Event;
    EventType event = EventType::None;
    bool any_subject = true;
    u64 subject = 0;
    std::string flag;
    float seconds = 0;
};

enum class ActionKind : u8 {
    Hint,
    Subtitle,
    Wait,
    SetFlag,
    ClearFlag,
    Emit,
    Enable,
    Disable,
    PlaceHero,
    AwardXp,
    CompleteObjective,
    CompleteMission,
    FailMission,
    Log,
};
const char* ToString(ActionKind k);

struct Action {
    ActionKind kind = ActionKind::Log;
    std::string text;  // text key, flag name, id name or log text
    u64 id = 0;        // NameHash of a text key / id, or an OID
    float seconds = 0;
    EventType event = EventType::None;
    s64 value = 0;
};

struct ObjectiveDef {
    std::string name;
    u64 id = 0;
    std::string brief, note;  // text keys
    u64 brief_key = 0, note_key = 0;
    std::vector<Action> on_start, on_complete, on_fail;
    std::vector<Condition> complete_when, fail_when;  // empty = never
    int line = 0;
};

struct TriggerDef {
    std::string name;
    u64 id = 0;
    bool once = true;
    int scope = -1;  // objective index, -1 = whole mission
    std::vector<Condition> when;
    std::vector<Action> actions;
    int line = 0;
};

struct MissionDef {
    std::string name;
    u64 id = 0;
    std::string source;      // original package the definition refers to
    std::string provenance;  // how the definition was derived
    std::vector<ObjectiveDef> objectives;
    std::vector<TriggerDef> triggers;
};

// Parses and validates. On failure `errors` lists every problem with its line number and `out` must
// not be used.
bool ParseMission(std::string_view text, MissionDef& out, std::vector<std::string>& errors);
bool LoadMissionFile(const std::wstring& path, MissionDef& out, std::vector<std::string>& errors);
// Structural checks shared by the parser and the runtime (unique names, sane numbers, scopes).
bool ValidateMission(const MissionDef& def, std::vector<std::string>& errors);

}  // namespace sspc::mission
