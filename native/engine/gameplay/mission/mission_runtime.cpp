#include "gameplay/mission/mission_runtime.h"

#include <algorithm>
#include <format>

#include "core/log.h"
#include "core/save_file.h"

namespace sspc::mission {
namespace {

constexpr u32 kMissionSaveVersion = 1;
constexpr size_t kMaxSuspendedEvents = 256;

bool HasEventCondition(const std::vector<Condition>& conds) {
    return std::any_of(conds.begin(), conds.end(), [](const Condition& c) { return c.kind == ConditionKind::Event; });
}

}  // namespace

const char* ToString(MissionState s) {
    static const char* const kNames[] = {"NotStarted", "Active", "ObjectiveCompleted", "Completed", "Failed", "Suspended"};
    const auto i = static_cast<size_t>(s);
    return i < std::size(kNames) ? kNames[i] : "?";
}

// --- SequencePlayer -------------------------------------------------------------------------------

void SequencePlayer::Start(SequenceRef ref, const std::vector<Action>* actions) {
    ref_ = ref;
    actions_ = actions;
    index_ = 0;
    wait_ = -1;
    ++generation_;
}

bool SequencePlayer::Restore(SequenceRef ref, const std::vector<Action>* actions, u32 index, float wait_left) {
    if (!actions || index > actions->size()) return false;
    Start(ref, actions);
    index_ = index;
    wait_ = wait_left;
    return true;
}

void SequencePlayer::Cancel() {
    actions_ = nullptr;
    ++generation_;
}

bool SequencePlayer::Update(float dt, const std::function<bool(const Action&)>& exec) {
    if (!actions_) return false;
    float left = dt;
    while (actions_ && index_ < actions_->size()) {
        const Action& a = (*actions_)[index_];
        if (a.kind == ActionKind::Wait) {
            if (wait_ < 0) wait_ = a.seconds;
            if (wait_ > left) {
                wait_ -= left;
                return false;
            }
            left -= wait_;
            wait_ = -1;
            ++index_;
            continue;
        }
        // Advance first: once an action has run, a save taken afterwards never runs it again.
        ++index_;
        const u32 gen = generation_;
        if (!exec(a) || gen != generation_) return false;  // the owner stopped or restarted this sequence
    }
    if (!actions_) return false;
    actions_ = nullptr;
    return true;
}

// --- MissionManager -------------------------------------------------------------------------------

bool MissionManager::Add(MissionDef def, std::vector<std::string>* errors) {
    std::vector<std::string> local;
    if (!ValidateMission(def, errors ? *errors : local)) return false;
    if (mission_ && mission_->id == def.id) {
        if (errors) errors->push_back("cannot replace the running mission");
        return false;
    }
    std::erase_if(defs_, [&](const std::unique_ptr<MissionDef>& d) { return d->id == def.id; });
    defs_.push_back(std::make_unique<MissionDef>(std::move(def)));
    return true;
}

const MissionDef* MissionManager::Find(u64 mission_id) const {
    for (const auto& d : defs_) {
        if (d->id == mission_id) return d.get();
    }
    return nullptr;
}

void MissionManager::Attach(GameEventBus* bus, IMissionHost* host) {
    Detach();
    bus_ = bus;
    host_ = host;
    if (bus_) subscription_ = bus_->Subscribe(EventType::None, [this](const GameEvent& e) { OnEvent(e); });
}

void MissionManager::Detach() {
    if (bus_ && subscription_) bus_->Unsubscribe(subscription_);
    bus_ = nullptr;
    subscription_ = 0;
}

const ObjectiveDef* MissionManager::Objective() const {
    if (!mission_ || objective_ < 0 || objective_ >= static_cast<int>(mission_->objectives.size())) return nullptr;
    return &mission_->objectives[static_cast<size_t>(objective_)];
}

bool MissionManager::TriggerFired(std::string_view name) const { return TriggerFireCount(name) > 0; }

u32 MissionManager::TriggerFireCount(std::string_view name) const {
    if (!mission_) return 0;
    for (size_t i = 0; i < mission_->triggers.size() && i < triggers_.size(); ++i) {
        if (mission_->triggers[i].name == name) return triggers_[i].fire_count;
    }
    return 0;
}

size_t MissionManager::RunningSequences() const {
    return static_cast<size_t>(std::count_if(sequences_.begin(), sequences_.end(), [](const Sequence& s) { return s.player.Running(); }));
}

void MissionManager::Post(EventType t, u64 subject, s64 param) {
    if (bus_) bus_->Post(t, subject, param);
}

bool MissionManager::Start(u64 mission_id) {
    const MissionDef* def = Find(mission_id);
    if (!def) return false;
    mission_ = def;
    state_ = MissionState::Active;
    resume_state_ = MissionState::Active;
    objective_ = -1;
    objective_time_ = 0;
    flags_.clear();
    xp_ = 0;
    sequences_.clear();
    suspended_events_.clear();
    triggers_.assign(def->triggers.size(), TriggerState{});
    for (size_t i = 0; i < def->triggers.size(); ++i) triggers_[i].latched.assign(def->triggers[i].when.size(), 0);
    Post(EventType::MissionStarted, def->id);
    if (host_) host_->MissionLog(std::format("mission {} started", def->name));
    BeginObjective(0);
    return true;
}

void MissionManager::BeginObjective(int index) {
    objective_ = index;
    objective_time_ = 0;
    const ObjectiveDef& o = mission_->objectives[static_cast<size_t>(index)];
    complete_latched_.assign(o.complete_when.size(), 0);
    fail_latched_.assign(o.fail_when.size(), 0);
    state_ = MissionState::Active;
    if (host_) host_->ObjectiveChanged(*mission_, &o);
    Post(EventType::ObjectiveStarted, o.id, index);
    ArmTriggers();
    RunSequence({SequenceKind::ObjectiveStart, static_cast<u16>(index)});
}

void MissionManager::ArmTriggers() {
    if (!mission_) return;
    for (size_t i = 0; i < triggers_.size(); ++i) {
        const TriggerDef& t = mission_->triggers[i];
        TriggerState& ts = triggers_[i];
        const bool live = state_ == MissionState::Active || state_ == MissionState::ObjectiveCompleted;
        const bool want = live && !ts.fired && (t.scope < 0 || (t.scope == objective_ && state_ == MissionState::Active));
        if (want && !ts.armed) {
            ts.armed = true;
            ts.armed_time = 0;
            ts.last_satisfied = false;
            std::fill(ts.latched.begin(), ts.latched.end(), u8{0});
        } else if (!want && ts.armed) {
            ts.armed = false;
            std::fill(ts.latched.begin(), ts.latched.end(), u8{0});
        }
    }
}

const std::vector<Action>* MissionManager::ActionsOf(SequenceRef ref) const {
    if (!mission_) return nullptr;
    switch (ref.kind) {
        case SequenceKind::ObjectiveStart:
        case SequenceKind::ObjectiveComplete:
        case SequenceKind::ObjectiveFail: {
            if (ref.index >= mission_->objectives.size()) return nullptr;
            const ObjectiveDef& o = mission_->objectives[ref.index];
            return ref.kind == SequenceKind::ObjectiveStart ? &o.on_start
                   : ref.kind == SequenceKind::ObjectiveComplete ? &o.on_complete
                                                                 : &o.on_fail;
        }
        case SequenceKind::Trigger:
            return ref.index < mission_->triggers.size() ? &mission_->triggers[ref.index].actions : nullptr;
        default:
            return nullptr;
    }
}

void MissionManager::RunSequence(SequenceRef ref) {
    const std::vector<Action>* actions = ActionsOf(ref);
    if (!actions) return;
    Sequence* seq = nullptr;
    for (Sequence& s : sequences_) {
        if (s.player.Ref() == ref || !s.player.Running()) {
            seq = &s;
            break;
        }
    }
    if (!seq) {
        sequences_.emplace_back();
        seq = &sequences_.back();
    }
    seq->player.Start(ref, actions);
    // Run the actions up to the first wait right away (same frame as the event that caused them).
    if (seq->player.Update(0.0f, [this, ref](const Action& a) { return Execute(a, ref); })) SequenceFinished(ref);
}

void MissionManager::SequenceFinished(SequenceRef ref) {
    Post(EventType::SequenceFinished, static_cast<u64>(ref.kind) << 16 | ref.index);
    if (ref.kind == SequenceKind::ObjectiveComplete && state_ == MissionState::ObjectiveCompleted && ref.index == objective_) {
        if (objective_ + 1 < static_cast<int>(mission_->objectives.size())) BeginObjective(objective_ + 1);
        else FinishMission();
    }
}

bool MissionManager::Execute(const Action& a, SequenceRef owner) {
    switch (a.kind) {
        case ActionKind::Hint:
            if (host_) host_->ShowHint(a);
            break;
        case ActionKind::Subtitle:
            if (host_) host_->ShowSubtitle(a);
            break;
        case ActionKind::SetFlag:
            flags_.insert(a.text);
            break;
        case ActionKind::ClearFlag:
            flags_.erase(a.text);
            break;
        case ActionKind::Emit:
            Post(a.event, a.id, a.value);
            break;
        case ActionKind::Enable:
        case ActionKind::Disable:
            if (host_) host_->SetInteractionEnabled(a.id, a.kind == ActionKind::Enable);
            break;
        case ActionKind::PlaceHero:
            if (host_) host_->PlaceHero(a.id);
            break;
        case ActionKind::AwardXp:
            xp_ += a.value;
            break;
        case ActionKind::CompleteObjective:
            CompleteObjective();
            break;
        case ActionKind::CompleteMission:
            FinishMission();
            break;
        case ActionKind::FailMission:
            FailMission();
            break;
        case ActionKind::Log:
            if (host_) host_->MissionLog(a.text);
            break;
        case ActionKind::Wait:
            break;
    }
    // Continue only while the owning sequence is still the one running.
    for (const Sequence& s : sequences_) {
        if (s.player.Ref() == owner && s.player.Running()) return true;
    }
    return false;
}

void MissionManager::CompleteObjective() {
    if (state_ != MissionState::Active || !Objective()) return;
    const ObjectiveDef& o = *Objective();
    for (Sequence& s : sequences_) {
        if (s.player.Ref() == SequenceRef{SequenceKind::ObjectiveStart, static_cast<u16>(objective_)}) s.player.Cancel();
    }
    state_ = MissionState::ObjectiveCompleted;
    Post(EventType::ObjectiveCompleted, o.id, objective_);
    if (host_) host_->MissionLog(std::format("objective {} completed", o.name));
    ArmTriggers();
    RunSequence({SequenceKind::ObjectiveComplete, static_cast<u16>(objective_)});
}

void MissionManager::FailMission() {
    if (state_ != MissionState::Active && state_ != MissionState::ObjectiveCompleted) return;
    for (Sequence& s : sequences_) s.player.Cancel();
    state_ = MissionState::Failed;
    ArmTriggers();
    Post(EventType::MissionFailed, mission_->id, objective_);
    if (host_) host_->MissionLog(std::format("mission {} failed", mission_->name));
    if (Objective()) RunSequence({SequenceKind::ObjectiveFail, static_cast<u16>(objective_)});
}

void MissionManager::FinishMission() {
    if (state_ != MissionState::Active && state_ != MissionState::ObjectiveCompleted) return;
    for (Sequence& s : sequences_) {
        if (s.player.Ref().kind != SequenceKind::Trigger) s.player.Cancel();
    }
    state_ = MissionState::Completed;
    ArmTriggers();
    Post(EventType::MissionCompleted, mission_->id);
    if (host_) {
        host_->ObjectiveChanged(*mission_, nullptr);
        host_->MissionLog(std::format("mission {} completed", mission_->name));
    }
}

bool MissionManager::Retry() {
    if (state_ != MissionState::Failed || !Objective()) return false;
    for (Sequence& s : sequences_) s.player.Cancel();
    BeginObjective(objective_);
    return true;
}

void MissionManager::Suspend() {
    if (state_ != MissionState::Active && state_ != MissionState::ObjectiveCompleted) return;
    resume_state_ = state_;
    state_ = MissionState::Suspended;
    Post(EventType::MissionSuspended, mission_->id);
}

void MissionManager::Resume() {
    if (state_ != MissionState::Suspended) return;
    state_ = resume_state_;
    Post(EventType::MissionResumed, mission_->id);
    std::vector<GameEvent> buffered;
    buffered.swap(suspended_events_);
    for (const GameEvent& e : buffered) HandleEvent(e);
}

void MissionManager::OnEvent(const GameEvent& e) {
    if (state_ == MissionState::Suspended) {
        if (e.type == EventType::MissionSuspended) return;
        if (suspended_events_.size() >= kMaxSuspendedEvents) suspended_events_.erase(suspended_events_.begin());
        suspended_events_.push_back(e);
        return;
    }
    if (state_ != MissionState::Active && state_ != MissionState::ObjectiveCompleted) return;
    HandleEvent(e);
}

void MissionManager::Latch(const std::vector<Condition>& conds, std::vector<u8>& latched, const GameEvent& e) {
    for (size_t i = 0; i < conds.size() && i < latched.size(); ++i) {
        const Condition& c = conds[i];
        if (c.kind == ConditionKind::Event && c.event == e.type && (c.any_subject || c.subject == e.subject)) latched[i] = 1;
    }
}

void MissionManager::HandleEvent(const GameEvent& e) {
    if (!mission_) return;
    if (state_ == MissionState::Active && Objective()) {
        Latch(Objective()->complete_when, complete_latched_, e);
        Latch(Objective()->fail_when, fail_latched_, e);
    }
    for (size_t i = 0; i < triggers_.size(); ++i) {
        if (triggers_[i].armed) Latch(mission_->triggers[i].when, triggers_[i].latched, e);
    }
    Evaluate();
}

bool MissionManager::Satisfied(const std::vector<Condition>& conds, const std::vector<u8>& latched, float time) const {
    if (conds.empty()) return false;
    for (size_t i = 0; i < conds.size(); ++i) {
        const Condition& c = conds[i];
        bool ok = false;
        switch (c.kind) {
            case ConditionKind::Event: ok = i < latched.size() && latched[i]; break;
            case ConditionKind::Flag: ok = flags_.contains(c.flag); break;
            case ConditionKind::NotFlag: ok = !flags_.contains(c.flag); break;
            case ConditionKind::Timer: ok = time >= c.seconds; break;
        }
        if (!ok) return false;
    }
    return true;
}

void MissionManager::Evaluate() {
    if (!mission_) return;
    for (int guard = 0; guard < 16; ++guard) {
        bool changed = false;
        if (state_ == MissionState::Active && Objective()) {
            const ObjectiveDef& o = *Objective();
            if (Satisfied(o.fail_when, fail_latched_, objective_time_)) {
                FailMission();
                changed = true;
            } else if (Satisfied(o.complete_when, complete_latched_, objective_time_)) {
                CompleteObjective();
                changed = true;
            }
        }
        for (size_t i = 0; i < triggers_.size(); ++i) {
            TriggerState& ts = triggers_[i];
            if (!ts.armed) continue;
            const TriggerDef& t = mission_->triggers[i];
            const bool sat = Satisfied(t.when, ts.latched, ts.armed_time);
            const bool fire = sat && (t.once || HasEventCondition(t.when) || !ts.last_satisfied);
            ts.last_satisfied = sat;
            if (!fire) continue;
            ++ts.fire_count;
            std::fill(ts.latched.begin(), ts.latched.end(), u8{0});
            if (t.once) {
                ts.fired = true;
                ts.armed = false;
            } else {
                ts.armed_time = 0;  // periodic timers
            }
            if (host_) host_->MissionLog(std::format("trigger {} fired", t.name));
            RunSequence({SequenceKind::Trigger, static_cast<u16>(i)});
            changed = true;
        }
        if (!changed) break;
    }
}

void MissionManager::Update(float dt) {
    if (!mission_ || state_ == MissionState::Suspended || state_ == MissionState::NotStarted) return;
    if (state_ == MissionState::Active) objective_time_ += dt;
    for (TriggerState& ts : triggers_) {
        if (ts.armed) ts.armed_time += dt;
    }
    const size_t n = sequences_.size();
    for (size_t i = 0; i < n && i < sequences_.size(); ++i) {
        if (!sequences_[i].player.Running()) continue;
        const SequenceRef ref = sequences_[i].player.Ref();
        if (sequences_[i].player.Update(dt, [this, ref](const Action& a) { return Execute(a, ref); })) SequenceFinished(ref);
    }
    std::erase_if(sequences_, [](const Sequence& s) { return !s.player.Running(); });
    Evaluate();
}

// --- Persistence ----------------------------------------------------------------------------------

std::vector<u8> MissionManager::Serialize() const {
    ByteWriter w;
    w.Put(kMissionSaveVersion);
    w.Put(mission_ ? mission_->id : u64{0});
    w.Put(static_cast<u8>(state_));
    w.Put(static_cast<u8>(resume_state_));
    w.Put(static_cast<s32>(objective_));
    w.Put(objective_time_);
    w.PutBytes(complete_latched_);
    w.PutBytes(fail_latched_);
    w.Put(static_cast<u32>(triggers_.size()));
    for (const TriggerState& t : triggers_) {
        w.Put(static_cast<u8>(t.armed));
        w.Put(static_cast<u8>(t.fired));
        w.Put(static_cast<u8>(t.last_satisfied));
        w.Put(t.fire_count);
        w.Put(t.armed_time);
        w.PutBytes(t.latched);
    }
    w.Put(static_cast<u32>(flags_.size()));
    for (const std::string& f : flags_) w.PutString(f);
    w.Put(xp_);
    u32 running = 0;
    for (const Sequence& s : sequences_) running += s.player.Running() ? 1 : 0;
    w.Put(running);
    for (const Sequence& s : sequences_) {
        if (!s.player.Running()) continue;
        w.Put(static_cast<u8>(s.player.Ref().kind));
        w.Put(s.player.Ref().index);
        w.Put(s.player.Index());
        w.Put(s.player.WaitLeft());
    }
    return w.Take();
}

bool MissionManager::Deserialize(std::span<const u8> data, std::string* error) {
    auto fail = [&](std::string m) {
        if (error) *error = std::move(m);
        return false;
    };
    ByteReader r(data);
    u32 version = 0;
    u64 id = 0;
    u8 state = 0, resume = 0;
    s32 objective = -1;
    float otime = 0;
    std::vector<u8> cl, fl;
    r.Get(version);
    if (version != kMissionSaveVersion) return fail(std::format("mission state version {}", version));
    r.Get(id);
    r.Get(state);
    r.Get(resume);
    r.Get(objective);
    r.Get(otime);
    r.GetBytes(cl, 4096);
    r.GetBytes(fl, 4096);
    if (!r.Ok()) return fail("truncated mission state");
    const MissionDef* def = Find(id);
    if (!def) return fail(std::format("mission {:#x} is not loaded", id));
    if (state > static_cast<u8>(MissionState::Suspended) || resume > static_cast<u8>(MissionState::Suspended)) return fail("bad state");
    if (objective < -1 || objective >= static_cast<s32>(def->objectives.size())) return fail("bad objective index");
    if (objective >= 0) {
        const ObjectiveDef& o = def->objectives[static_cast<size_t>(objective)];
        if (cl.size() != o.complete_when.size() || fl.size() != o.fail_when.size()) return fail("condition state does not match the definition");
    }
    u32 tcount = 0;
    r.Get(tcount);
    if (!r.Ok() || tcount != def->triggers.size()) return fail("trigger state does not match the definition");
    std::vector<TriggerState> trig(tcount);
    for (u32 i = 0; i < tcount; ++i) {
        u8 armed = 0, fired = 0, last = 0;
        r.Get(armed);
        r.Get(fired);
        r.Get(last);
        r.Get(trig[i].fire_count);
        r.Get(trig[i].armed_time);
        r.GetBytes(trig[i].latched, 4096);
        trig[i].armed = armed != 0;
        trig[i].fired = fired != 0;
        trig[i].last_satisfied = last != 0;
        if (r.Ok() && trig[i].latched.size() != def->triggers[i].when.size()) return fail("trigger latch state does not match");
    }
    u32 nflags = 0;
    r.Get(nflags);
    if (!r.Ok() || nflags > 4096) return fail("bad flag count");
    std::set<std::string> flags;
    for (u32 i = 0; i < nflags; ++i) {
        std::string f;
        if (!r.GetString(f, 256)) return fail("truncated flags");
        flags.insert(std::move(f));
    }
    s64 xp = 0;
    u32 nseq = 0;
    r.Get(xp);
    r.Get(nseq);
    if (!r.Ok() || nseq > 1024) return fail("bad sequence count");
    struct SavedSeq {
        SequenceRef ref;
        u32 index;
        float wait;
    };
    std::vector<SavedSeq> seqs(nseq);
    for (u32 i = 0; i < nseq; ++i) {
        u8 kind = 0;
        r.Get(kind);
        r.Get(seqs[i].ref.index);
        r.Get(seqs[i].index);
        r.Get(seqs[i].wait);
        if (kind > static_cast<u8>(SequenceKind::Trigger)) return fail("bad sequence kind");
        seqs[i].ref.kind = static_cast<SequenceKind>(kind);
    }
    if (!r.Ok() || !r.AtEnd()) return fail("mission state has trailing or missing bytes");

    // Validate the sequences against the definition before touching anything.
    const MissionDef* previous = mission_;
    mission_ = def;
    for (const SavedSeq& s : seqs) {
        const auto* acts = ActionsOf(s.ref);
        if (!acts || s.index > acts->size()) {
            mission_ = previous;
            return fail("saved sequence does not match the definition");
        }
    }
    // Commit.
    state_ = static_cast<MissionState>(state);
    resume_state_ = static_cast<MissionState>(resume);
    objective_ = objective;
    objective_time_ = otime;
    complete_latched_ = std::move(cl);
    fail_latched_ = std::move(fl);
    triggers_ = std::move(trig);
    flags_ = std::move(flags);
    xp_ = xp;
    suspended_events_.clear();
    sequences_.clear();
    for (const SavedSeq& s : seqs) {
        sequences_.emplace_back();
        sequences_.back().player.Restore(s.ref, ActionsOf(s.ref), s.index, s.wait);
    }
    if (host_) host_->ObjectiveChanged(*mission_, state_ == MissionState::Completed ? nullptr : Objective());
    return true;
}

std::vector<std::string> MissionManager::DebugLines() const {
    std::vector<std::string> out;
    if (!mission_) {
        out.push_back("mission: none");
        return out;
    }
    out.push_back(std::format("mission {}  state {}  xp {}", mission_->name, ToString(state_), xp_));
    if (const ObjectiveDef* o = Objective()) {
        out.push_back(std::format("objective {}/{} {}  ({:.1f} s)", objective_ + 1, mission_->objectives.size(), o->name,
                                  objective_time_));
    }
    std::string trig;
    for (size_t i = 0; i < triggers_.size(); ++i) {
        trig += std::format("{}{}{} ", mission_->triggers[i].name, triggers_[i].armed ? "*" : "",
                            triggers_[i].fire_count ? std::format("x{}", triggers_[i].fire_count) : std::string());
    }
    if (!trig.empty()) out.push_back("triggers: " + trig);
    std::string fl;
    for (const auto& f : flags_) fl += f + " ";
    if (!fl.empty()) out.push_back("flags: " + fl);
    for (const Sequence& s : sequences_) {
        if (!s.player.Running()) continue;
        out.push_back(std::format("sequence {}#{} at action {}{}", static_cast<int>(s.player.Ref().kind), s.player.Ref().index,
                                  s.player.Index(), s.player.WaitLeft() > 0 ? std::format(" (wait {:.1f} s)", s.player.WaitLeft()) : ""));
    }
    return out;
}

}  // namespace sspc::mission
