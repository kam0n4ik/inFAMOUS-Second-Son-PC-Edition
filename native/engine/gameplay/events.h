// SecondSonPC native runtime - game event bus.
//
// Game systems report what happened (the player entered an area, pressed interact, a graffiti stage
// was finished, ...) as events; the mission runtime and other listeners subscribe to them. Nothing in
// here knows the systems themselves. Events are queued by Post and delivered by Dispatch in FIFO
// order; events posted by handlers during a dispatch are delivered in the same call (bounded rounds,
// so a feedback loop cannot hang the frame).
#pragma once

#include <deque>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace sspc {

enum class EventType : u8 {
    None = 0,  // as a subscription filter: every event
    MissionStarted,
    MissionCompleted,
    MissionFailed,
    MissionSuspended,
    MissionResumed,
    ObjectiveStarted,
    ObjectiveCompleted,
    PlayerEnteredArea,
    PlayerLeftArea,
    InteractionAvailable,
    InteractionPressed,
    AnimationFinished,
    ActionFinished,
    DialogueFinished,
    StoryTrigger,
    ObjectStateChanged,
    GraffitiStarted,
    GraffitiStageCompleted,
    GraffitiCompleted,
    GraffitiAborted,
    SequenceFinished,
    Count,
};
const char* ToString(EventType t);
std::optional<EventType> EventTypeFromString(std::string_view s);

struct GameEvent {
    EventType type = EventType::None;
    u64 subject = 0;  // object id (OID or NameHash of a native name)
    s64 param = 0;    // e.g. stage index
    double time = 0;  // bus time when posted
};

class GameEventBus {
public:
    using Handler = std::function<void(const GameEvent&)>;

    u32 Subscribe(EventType type, Handler handler);  // EventType::None = all events
    void Unsubscribe(u32 id);
    void Post(EventType type, u64 subject = 0, s64 param = 0);
    void Post(const GameEvent& e);
    // Delivers queued events; returns the number delivered. Rounds beyond `max_rounds` stay queued.
    size_t Dispatch(u32 max_rounds = 8);
    size_t Pending() const { return queue_.size(); }
    void Clear() { queue_.clear(); }

    void SetTime(double t) { time_ = t; }
    double Time() const { return time_; }
    // Most recent delivered events (oldest first), for the debug view.
    const std::deque<GameEvent>& History() const { return history_; }
    void SetHistoryLimit(size_t n) { history_limit_ = n; }

private:
    struct Sub {
        u32 id;
        EventType type;
        Handler fn;
    };
    std::vector<Sub> subs_;
    std::deque<GameEvent> queue_;
    std::deque<GameEvent> history_;
    size_t history_limit_ = 32;
    u32 next_id_ = 1;
    double time_ = 0;
};

}  // namespace sspc
