#include "gameplay/events.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace sspc {
namespace {

constexpr std::array<const char*, static_cast<size_t>(EventType::Count)> kNames = {
    "None",           "MissionStarted",     "MissionCompleted",  "MissionFailed",       "MissionSuspended",
    "MissionResumed", "ObjectiveStarted",   "ObjectiveCompleted", "PlayerEnteredArea",  "PlayerLeftArea",
    "InteractionAvailable", "InteractionPressed", "AnimationFinished", "ActionFinished", "DialogueFinished",
    "StoryTrigger",   "ObjectStateChanged", "GraffitiStarted",   "GraffitiStageCompleted", "GraffitiCompleted",
    "GraffitiAborted", "SequenceFinished",
};

bool EqualNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

}  // namespace

const char* ToString(EventType t) {
    const auto i = static_cast<size_t>(t);
    return i < kNames.size() ? kNames[i] : "?";
}

std::optional<EventType> EventTypeFromString(std::string_view s) {
    for (size_t i = 1; i < kNames.size(); ++i) {
        if (EqualNoCase(s, kNames[i])) return static_cast<EventType>(i);
    }
    return std::nullopt;
}

u32 GameEventBus::Subscribe(EventType type, Handler handler) {
    const u32 id = next_id_++;
    subs_.push_back({id, type, std::move(handler)});
    return id;
}

void GameEventBus::Unsubscribe(u32 id) {
    std::erase_if(subs_, [id](const Sub& s) { return s.id == id; });
}

void GameEventBus::Post(EventType type, u64 subject, s64 param) { Post(GameEvent{type, subject, param, time_}); }

void GameEventBus::Post(const GameEvent& e) {
    GameEvent ev = e;
    if (ev.time == 0) ev.time = time_;
    queue_.push_back(ev);
}

size_t GameEventBus::Dispatch(u32 max_rounds) {
    size_t delivered = 0;
    for (u32 round = 0; round < max_rounds && !queue_.empty(); ++round) {
        // Deliver the events queued at the start of this round; handlers may queue more.
        std::deque<GameEvent> batch;
        batch.swap(queue_);
        for (const GameEvent& e : batch) {
            history_.push_back(e);
            while (history_.size() > history_limit_) history_.pop_front();
            // Copy: a handler may subscribe or unsubscribe.
            const std::vector<Sub> subs = subs_;
            for (const Sub& s : subs) {
                if (s.type == EventType::None || s.type == e.type) s.fn(e);
            }
            ++delivered;
        }
    }
    return delivered;
}

}  // namespace sspc
