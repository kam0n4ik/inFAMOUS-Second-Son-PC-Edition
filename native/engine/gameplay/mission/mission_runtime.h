// SecondSonPC native runtime - mission runtime.
//
// MissionManager runs one mission definition at a time: objectives in order (start actions, completion
// and failure conditions, completion actions), mission-wide or objective-scoped triggers (once or
// repeat), flags and an experience counter. Conditions are fed by GameEventBus events, flags and
// timers; actions run in SequencePlayers (action list with waits, cancellable, restorable from a save).
// Everything that touches the world (hints, subtitles, interactions, hero placement) goes through
// IMissionHost, so the runtime knows nothing about rendering, input or the character.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/types.h"
#include "gameplay/events.h"
#include "gameplay/mission/mission_definition.h"

namespace sspc::mission {

enum class MissionState : u8 { NotStarted, Active, ObjectiveCompleted, Completed, Failed, Suspended };
const char* ToString(MissionState s);

class IMissionHost {
public:
    virtual ~IMissionHost() = default;
    virtual void ObjectiveChanged(const MissionDef& mission, const ObjectiveDef* objective) = 0;  // null = none
    virtual void ShowHint(const Action& a) = 0;
    virtual void ShowSubtitle(const Action& a) = 0;
    virtual void SetInteractionEnabled(u64 id, bool enabled) = 0;
    virtual void PlaceHero(u64 point_id) = 0;
    virtual void MissionLog(std::string_view text) = 0;
};

// Which action list a sequence plays (for saving/restoring it).
enum class SequenceKind : u8 { None, ObjectiveStart, ObjectiveComplete, ObjectiveFail, Trigger };
struct SequenceRef {
    SequenceKind kind = SequenceKind::None;
    u16 index = 0;  // objective or trigger index
    bool operator==(const SequenceRef&) const = default;
};

class SequencePlayer {
public:
    void Start(SequenceRef ref, const std::vector<Action>* actions);
    // Restores a sequence saved at `index` with `wait_left` seconds remaining in its current wait.
    bool Restore(SequenceRef ref, const std::vector<Action>* actions, u32 index, float wait_left);
    void Cancel();
    bool Running() const { return actions_ != nullptr; }
    // Executes actions until a wait or the end. `exec` performs one non-wait action and returns false
    // if the sequence must stop (e.g. the action ended the objective that owns it). Returns true when
    // the sequence ran to its end during this call.
    bool Update(float dt, const std::function<bool(const Action&)>& exec);
    SequenceRef Ref() const { return ref_; }
    u32 Index() const { return index_; }
    float WaitLeft() const { return wait_; }

private:
    SequenceRef ref_;
    const std::vector<Action>* actions_ = nullptr;
    u32 index_ = 0;
    float wait_ = -1;    // < 0: the current wait action has not started
    u32 generation_ = 0;  // changes on Start/Restore/Cancel (an action may stop its own sequence)
};

class MissionManager {
public:
    // Validates and stores a definition (replaces one with the same id).
    bool Add(MissionDef def, std::vector<std::string>* errors = nullptr);
    const MissionDef* Find(u64 mission_id) const;

    void Attach(GameEventBus* bus, IMissionHost* host);
    void Detach();

    bool Start(u64 mission_id);
    void Update(float dt);
    void Suspend();
    void Resume();
    // After a failure: restart the failed objective (its start actions run again).
    bool Retry();

    MissionState State() const { return state_; }
    const MissionDef* Current() const { return mission_; }
    int ObjectiveIndex() const { return objective_; }
    const ObjectiveDef* Objective() const;
    float ObjectiveTime() const { return objective_time_; }
    bool Flag(std::string_view name) const { return flags_.contains(std::string(name)); }
    s64 Xp() const { return xp_; }
    bool TriggerFired(std::string_view name) const;
    u32 TriggerFireCount(std::string_view name) const;
    size_t RunningSequences() const;

    // State of the running mission; Deserialize changes nothing unless the whole block is valid and
    // refers to a mission definition that is loaded.
    std::vector<u8> Serialize() const;
    bool Deserialize(std::span<const u8> data, std::string* error);

    std::vector<std::string> DebugLines() const;

private:
    struct TriggerState {
        bool armed = false;
        bool fired = false;          // once-triggers: never again
        bool last_satisfied = false;  // repeat-triggers: fire on the rising edge of flag/timer conditions
        u32 fire_count = 0;
        float armed_time = 0;
        std::vector<u8> latched;     // per condition (event conditions latch when their event arrives)
    };
    struct Sequence {
        SequencePlayer player;
    };

    void OnEvent(const GameEvent& e);
    void HandleEvent(const GameEvent& e);
    void Evaluate();
    bool Satisfied(const std::vector<Condition>& conds, const std::vector<u8>& latched, float time) const;
    static void Latch(const std::vector<Condition>& conds, std::vector<u8>& latched, const GameEvent& e);
    void BeginObjective(int index);
    void CompleteObjective();
    void FailMission();
    void FinishMission();
    void ArmTriggers();
    void RunSequence(SequenceRef ref);
    void SequenceFinished(SequenceRef ref);
    bool Execute(const Action& a, SequenceRef owner);
    const std::vector<Action>* ActionsOf(SequenceRef ref) const;
    void Post(EventType t, u64 subject, s64 param = 0);

    // Owned definitions; sequences point into their action lists, so addresses must stay stable.
    std::vector<std::unique_ptr<MissionDef>> defs_;
    GameEventBus* bus_ = nullptr;
    IMissionHost* host_ = nullptr;
    u32 subscription_ = 0;

    const MissionDef* mission_ = nullptr;
    MissionState state_ = MissionState::NotStarted;
    MissionState resume_state_ = MissionState::NotStarted;
    int objective_ = -1;
    float objective_time_ = 0;
    std::vector<u8> complete_latched_, fail_latched_;
    std::vector<TriggerState> triggers_;
    std::set<std::string> flags_;
    s64 xp_ = 0;
    // deque: a sequence started from an action of another one must not move the running player.
    std::deque<Sequence> sequences_;
    std::vector<GameEvent> suspended_events_;
};

}  // namespace sspc::mission
