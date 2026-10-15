// Mission runtime tests (no game data). The definitions here are TEST definitions, not game content.
#include <string>
#include <vector>

#include "core/name_hash.h"
#include "core/save_file.h"
#include "gameplay/events.h"
#include "gameplay/mission/mission_definition.h"
#include "gameplay/mission/mission_runtime.h"
#include "test.h"

using namespace sspc;
using namespace sspc::mission;

namespace {

struct Host final : IMissionHost {
    std::vector<std::string> calls;
    const ObjectiveDef* objective = nullptr;
    void ObjectiveChanged(const MissionDef&, const ObjectiveDef* o) override {
        objective = o;
        calls.push_back("objective " + (o ? o->name : std::string("-")));
    }
    void ShowHint(const Action& a) override { calls.push_back("hint " + a.text); }
    void ShowSubtitle(const Action& a) override { calls.push_back("subtitle " + a.text); }
    void SetInteractionEnabled(u64 id, bool e) override { calls.push_back(std::string(e ? "enable " : "disable ") + std::to_string(id)); }
    void PlaceHero(u64 id) override { calls.push_back("place " + std::to_string(id)); }
    void MissionLog(std::string_view t) override { calls.push_back("log " + std::string(t)); }
    int Count(const std::string& prefix) const {
        int n = 0;
        for (const auto& c : calls) n += c.rfind(prefix, 0) == 0 ? 1 : 0;
        return n;
    }
};

const char* kTestMission = R"(sspc-mission 1
# TEST definition
mission test_mission
  provenance unit test
objective first
  brief key_first
  on_start hint h1 2 ; wait 1.0 ; subtitle s1 1 ; set started_first
  complete_when event GraffitiCompleted 0x1234
  on_complete award_xp 100 ; wait 0.5 ; log first done
objective second
  on_start enable door
  complete_when event PlayerEnteredArea gate & flag door_open
  fail_when event MissionFailed
objective third
  complete_when timer 2
trigger open once
  scope second
  when event InteractionPressed door
  do set door_open ; log opened
trigger tick repeat
  when timer 1
  do log tick
)";

struct Fixture {
    GameEventBus bus;
    MissionManager mm;
    Host host;
    u64 id = 0;
    bool Init(const char* text = kTestMission) {
        MissionDef def;
        std::vector<std::string> errors;
        if (!ParseMission(text, def, errors)) return false;
        id = def.id;
        if (!mm.Add(std::move(def))) return false;
        mm.Attach(&bus, &host);
        return true;
    }
    void Step(float dt, int n = 1) {
        for (int i = 0; i < n; ++i) {
            mm.Update(dt);
            bus.Dispatch();
        }
    }
    void Send(EventType t, u64 subject) {
        bus.Post(t, subject);
        bus.Dispatch();
    }
};

}  // namespace

TEST(mission_definition_parses_and_validates) {
    MissionDef def;
    std::vector<std::string> errors;
    CHECK(ParseMission(kTestMission, def, errors));
    CHECK(errors.empty());
    CHECK_EQ(def.objectives.size(), 3u);
    CHECK_EQ(def.triggers.size(), 2u);
    CHECK_EQ(def.triggers[0].scope, 1);
    CHECK(!def.triggers[1].once);
    CHECK_EQ(def.objectives[0].complete_when[0].subject, 0x1234u);
    CHECK_EQ(def.objectives[1].complete_when.size(), 2u);
    CHECK_EQ(def.objectives[1].complete_when[0].subject, NameHash("gate"));
    CHECK_EQ(def.objectives[0].on_start.size(), 4u);
    CHECK(def.objectives[0].on_start[1].kind == ActionKind::Wait);
}

TEST(mission_definition_rejects_malformed_input) {
    const char* bad[] = {
        "",                                                                   // empty
        "mission x\nobjective a\n",                                          // no header
        "sspc-mission 2\nmission x\nobjective a\n",                          // wrong version
        "sspc-mission 1\nmission x\n",                                       // no objectives
        "sspc-mission 1\nmission x\nobjective a\nobjective a\n",             // duplicate objective
        "sspc-mission 1\nmission x\nobjective a\n  complete_when event Nope\n",  // unknown event
        "sspc-mission 1\nmission x\nobjective a\n  on_start wait -1\n",      // negative wait
        "sspc-mission 1\nmission x\nobjective a\n  on_start wait 1e9\n",     // absurd wait
        "sspc-mission 1\nmission x\nobjective a\n  on_start fly away\n",     // unknown action
        "sspc-mission 1\nmission x\nobjective a\n  bogus 1\n",               // unknown property
        "sspc-mission 1\nmission x\nobjective a\ntrigger t sometimes\n",     // bad trigger mode
        "sspc-mission 1\nmission x\nobjective a\ntrigger t once\n  scope zzz\n  when flag f\n  do log x\n",  // bad scope
        "sspc-mission 1\nmission x\nobjective a\ntrigger t once\n  when flag f\n",  // trigger without do
        "sspc-mission 1\nmission x\nobjective a\n  complete_when event GraffitiCompleted 0xZZ\n",  // bad id
        "sspc-mission 1\nmission x\nobjective a\n  brief a\n  brief b\n",    // duplicate property
        "sspc-mission 1\n  brief a\nmission x\nobjective a\n",               // property outside a block
        "sspc-mission 1\nmission x\nmission y\nobjective a\n",               // two missions
    };
    for (const char* text : bad) {
        MissionDef def;
        std::vector<std::string> errors;
        CHECK(!ParseMission(text, def, errors));
        CHECK(!errors.empty());
    }
    // Binary garbage must not crash the parser.
    std::string garbage(4096, '\0');
    for (size_t i = 0; i < garbage.size(); ++i) garbage[i] = static_cast<char>((i * 7919u) & 0xff);
    MissionDef def;
    std::vector<std::string> errors;
    CHECK(!ParseMission(garbage, def, errors));
}

TEST(mission_runs_objectives_in_order_with_sequences) {
    Fixture f;
    CHECK(f.Init());
    CHECK(f.mm.Start(f.id));
    f.bus.Dispatch();
    CHECK(f.mm.State() == MissionState::Active);
    CHECK_EQ(f.mm.ObjectiveIndex(), 0);
    CHECK_EQ(f.host.Count("hint h1"), 1);
    CHECK_EQ(f.host.Count("subtitle s1"), 0);  // after the 1 s wait
    f.Step(0.5f);
    CHECK_EQ(f.host.Count("subtitle s1"), 0);
    f.Step(0.6f);
    CHECK_EQ(f.host.Count("subtitle s1"), 1);
    CHECK(f.mm.Flag("started_first"));
    // Wrong subject: nothing.
    f.Send(EventType::GraffitiCompleted, 0x9999);
    CHECK_EQ(f.mm.ObjectiveIndex(), 0);
    // Completion: on_complete runs (award now, log after 0.5 s), then the next objective starts.
    f.Send(EventType::GraffitiCompleted, 0x1234);
    CHECK(f.mm.State() == MissionState::ObjectiveCompleted);
    CHECK_EQ(f.mm.Xp(), 100);
    f.Step(0.6f);
    CHECK(f.mm.State() == MissionState::Active);
    CHECK_EQ(f.mm.ObjectiveIndex(), 1);
    CHECK_EQ(f.host.Count("enable"), 1);
    // A repeated completion event of the finished objective changes nothing (no second award).
    f.Send(EventType::GraffitiCompleted, 0x1234);
    CHECK_EQ(f.mm.Xp(), 100);
    CHECK_EQ(f.mm.ObjectiveIndex(), 1);
    // Conjunction: the area alone is not enough; the once-trigger sets the flag.
    f.Send(EventType::PlayerEnteredArea, NameHash("gate"));
    CHECK_EQ(f.mm.ObjectiveIndex(), 1);
    f.Send(EventType::InteractionPressed, NameHash("door"));
    CHECK(f.mm.TriggerFired("open"));
    // The area event was latched earlier: flag + latch complete the objective.
    f.Step(0.01f);
    CHECK_EQ(f.mm.ObjectiveIndex(), 2);
    // A once-trigger never fires again.
    f.Send(EventType::InteractionPressed, NameHash("door"));
    CHECK_EQ(f.mm.TriggerFireCount("open"), 1u);
    // Timer objective -> mission complete.
    f.Step(0.5f, 5);
    CHECK(f.mm.State() == MissionState::Completed);
    CHECK(f.host.objective == nullptr);
    // The repeat timer trigger fired about once per second while the mission ran.
    CHECK(f.mm.TriggerFireCount("tick") >= 3u);
}

TEST(mission_cancels_running_sequence_when_objective_completes) {
    Fixture f;
    CHECK(f.Init());
    f.mm.Start(f.id);
    f.bus.Dispatch();
    // Complete during the on_start wait: the rest of on_start (subtitle, flag) must not run.
    f.Send(EventType::GraffitiCompleted, 0x1234);
    f.Step(2.0f);
    CHECK_EQ(f.host.Count("subtitle s1"), 0);
    CHECK(!f.mm.Flag("started_first"));
    CHECK_EQ(f.mm.ObjectiveIndex(), 1);
}

TEST(mission_suspend_buffers_events) {
    Fixture f;
    CHECK(f.Init());
    f.mm.Start(f.id);
    f.bus.Dispatch();
    f.mm.Suspend();
    f.bus.Dispatch();
    CHECK(f.mm.State() == MissionState::Suspended);
    f.Send(EventType::GraffitiCompleted, 0x1234);
    f.Step(5.0f);
    CHECK_EQ(f.mm.ObjectiveIndex(), 0);  // frozen
    CHECK_EQ(f.mm.Xp(), 0);
    f.mm.Resume();
    f.bus.Dispatch();
    CHECK_EQ(f.mm.Xp(), 100);  // the buffered event was delivered on resume
}

TEST(mission_fail_and_retry) {
    Fixture f;
    CHECK(f.Init());
    f.mm.Start(f.id);
    f.bus.Dispatch();
    f.Send(EventType::GraffitiCompleted, 0x1234);
    f.Step(1.0f);
    CHECK_EQ(f.mm.ObjectiveIndex(), 1);
    f.Send(EventType::MissionFailed, 0);  // objective 2 fails on this event
    CHECK(f.mm.State() == MissionState::Failed);
    CHECK(f.mm.Retry());
    f.bus.Dispatch();
    CHECK(f.mm.State() == MissionState::Active);
    CHECK_EQ(f.mm.ObjectiveIndex(), 1);
    CHECK_EQ(f.host.Count("enable"), 2);  // start actions ran again
}

TEST(mission_save_load_restores_without_repeating_actions) {
    Fixture a;
    CHECK(a.Init());
    a.mm.Start(a.id);
    a.bus.Dispatch();
    a.Send(EventType::GraffitiCompleted, 0x1234);  // award 100, then wait 0.5 in on_complete
    a.Step(0.2f);
    const std::vector<u8> saved = a.mm.Serialize();

    Fixture b;  // a fresh runtime with the same definition
    CHECK(b.Init());
    std::string err;
    CHECK(b.mm.Deserialize(saved, &err));
    CHECK(b.mm.State() == MissionState::ObjectiveCompleted);
    CHECK_EQ(b.mm.Xp(), 100);
    CHECK_EQ(b.host.Count("hint"), 0);  // nothing re-ran on load
    b.Step(0.35f);                      // the remaining 0.3 s of the wait, then "log first done"
    CHECK_EQ(b.host.Count("log first done"), 1);
    CHECK_EQ(b.mm.ObjectiveIndex(), 1);
    CHECK_EQ(b.mm.Xp(), 100);  // no duplicate reward
    // A once-trigger fired before saving stays fired after loading.
    b.Send(EventType::InteractionPressed, NameHash("door"));
    const std::vector<u8> saved2 = b.mm.Serialize();
    Fixture c;
    CHECK(c.Init());
    CHECK(c.mm.Deserialize(saved2, &err));
    c.Send(EventType::InteractionPressed, NameHash("door"));
    CHECK_EQ(c.mm.TriggerFireCount("open"), 1u);
    CHECK_EQ(c.host.Count("log opened"), 0);
}

TEST(mission_rejects_corrupted_state_and_keeps_running_state) {
    Fixture a;
    CHECK(a.Init());
    a.mm.Start(a.id);
    a.bus.Dispatch();
    const std::vector<u8> good = a.mm.Serialize();
    std::string err;
    // Truncations.
    for (size_t n = 0; n < good.size(); n += 3) {
        std::vector<u8> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        Fixture b;
        CHECK(b.Init());
        CHECK(!b.mm.Deserialize(cut, &err));
        CHECK(b.mm.State() == MissionState::NotStarted);
    }
    // Unknown mission id.
    std::vector<u8> other = good;
    other[4] ^= 0xff;
    Fixture b;
    CHECK(b.Init());
    CHECK(!b.mm.Deserialize(other, &err));
    // Save container: checksum, version, size and section structure are verified.
    SaveSections s;
    s[SaveTag("MISN")] = good;
    std::vector<u8> file = EncodeSave(s);
    SaveSections out;
    CHECK(DecodeSave(file, out, &err));
    CHECK(out[SaveTag("MISN")] == good);
    for (size_t i = 0; i < file.size(); i += 5) {
        std::vector<u8> bad = file;
        bad[i] ^= 0x40;
        CHECK(!DecodeSave(bad, out, &err));
        CHECK(out.empty());
    }
    file.pop_back();
    CHECK(!DecodeSave(file, out, &err));
}

TEST(event_bus_delivers_in_order_and_bounds_feedback) {
    GameEventBus bus;
    std::vector<u64> seen;
    bus.Subscribe(EventType::StoryTrigger, [&](const GameEvent& e) {
        seen.push_back(e.subject);
        bus.Post(EventType::StoryTrigger, e.subject + 1);  // feedback loop
    });
    bus.Post(EventType::StoryTrigger, 1);
    const size_t delivered = bus.Dispatch(4);
    CHECK_EQ(delivered, 4u);
    CHECK(seen == std::vector<u64>({1, 2, 3, 4}));
    CHECK_EQ(bus.Pending(), 1u);  // the 5th waits for the next frame
    CHECK(EventTypeFromString("graffiticompleted") == EventType::GraffitiCompleted);
    CHECK(!EventTypeFromString("NoSuchEvent"));
}
