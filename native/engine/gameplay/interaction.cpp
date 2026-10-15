#include "gameplay/interaction.h"

#include <algorithm>
#include <cmath>

#include "core/save_file.h"

namespace sspc {
namespace {

constexpr u32 kInteractionSaveVersion = 1;

Quat Conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

}  // namespace

void InteractionSystem::AddInteractable(Interactable i) { items_.push_back(std::move(i)); }
void InteractionSystem::AddVolume(TriggerVolume v) { volumes_.push_back(std::move(v)); }

bool InteractionSystem::SetEnabled(u64 id, bool enabled) {
    bool found = false;
    for (Interactable& i : items_) {
        if (i.id == id) {
            i.enabled = enabled;
            found = true;
        }
    }
    for (TriggerVolume& v : volumes_) {
        if (v.id == id) {
            v.enabled = enabled;
            if (!enabled) v.inside = false;
            found = true;
        }
    }
    return found;
}

bool InteractionSystem::Contains(const TriggerVolume& v, const Vec3& p) {
    const Vec3 local = Rotate(Conjugate(v.rotation), p - v.center);
    return std::abs(local.x) <= v.half_extents.x && std::abs(local.y) <= v.half_extents.y && std::abs(local.z) <= v.half_extents.z;
}

void InteractionSystem::Update(const Vec3& feet, float yaw, bool interact_pressed, GameEventBus& bus, bool silent) {
    // Focus: nearest enabled interactable in range within the facing cone (measured at chest height).
    const Vec3 chest = feet + Vec3{0, 0, 1.2f};
    const Vec3 facing{std::cos(yaw), std::sin(yaw), 0};
    int best = -1;
    float best_d = 1e30f;
    for (size_t k = 0; k < items_.size(); ++k) {
        const Interactable& it = items_[k];
        if (!it.enabled) continue;
        const Vec3 to = it.position - chest;
        const float d = Length(to);
        if (d > it.radius) continue;
        const Vec3 flat = Normalize(Vec3{to.x, to.y, 0});
        const float angle = std::acos(std::clamp(Dot(flat, facing), -1.0f, 1.0f));
        if (d > 0.5f && angle > it.max_angle) continue;
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(k);
        }
    }
    if (best != focused_ && best >= 0 && !silent) bus.Post(EventType::InteractionAvailable, items_[static_cast<size_t>(best)].id);
    focused_ = best;
    if (interact_pressed && focused_ >= 0 && !silent) bus.Post(EventType::InteractionPressed, items_[static_cast<size_t>(focused_)].id);

    const Vec3 probe = feet + Vec3{0, 0, 0.9f};  // the player's body centre
    for (TriggerVolume& v : volumes_) {
        if (!v.enabled) continue;
        const bool in = Contains(v, probe);
        if (in != v.inside && !silent) bus.Post(in ? EventType::PlayerEnteredArea : EventType::PlayerLeftArea, v.id);
        v.inside = in;
    }
}

const Interactable* InteractionSystem::Focused() const {
    return focused_ >= 0 && static_cast<size_t>(focused_) < items_.size() ? &items_[static_cast<size_t>(focused_)] : nullptr;
}

std::vector<u8> InteractionSystem::Serialize() const {
    ByteWriter w;
    w.Put(kInteractionSaveVersion);
    w.Put(static_cast<u32>(items_.size()));
    for (const Interactable& i : items_) {
        w.Put(i.id);
        w.Put(static_cast<u8>(i.enabled));
    }
    w.Put(static_cast<u32>(volumes_.size()));
    for (const TriggerVolume& v : volumes_) {
        w.Put(v.id);
        w.Put(static_cast<u8>(v.enabled));
        w.Put(static_cast<u8>(v.inside));
    }
    return w.Take();
}

bool InteractionSystem::Deserialize(std::span<const u8> data, std::string* error) {
    ByteReader r(data);
    u32 version = 0, ni = 0;
    r.Get(version);
    r.Get(ni);
    if (!r.Ok() || version != kInteractionSaveVersion || ni != items_.size()) {
        if (error) *error = "interaction state does not match the loaded objects";
        return false;
    }
    std::vector<u8> enabled(ni), v_enabled, v_inside;
    for (u32 k = 0; k < ni; ++k) {
        u64 id = 0;
        r.Get(id);
        r.Get(enabled[k]);
        if (r.Ok() && id != items_[k].id) r.Fail();
    }
    u32 nv = 0;
    r.Get(nv);
    if (!r.Ok() || nv != volumes_.size()) {
        if (error) *error = "trigger volume state does not match the loaded volumes";
        return false;
    }
    v_enabled.resize(nv);
    v_inside.resize(nv);
    for (u32 k = 0; k < nv; ++k) {
        u64 id = 0;
        r.Get(id);
        r.Get(v_enabled[k]);
        r.Get(v_inside[k]);
        if (r.Ok() && id != volumes_[k].id) r.Fail();
    }
    if (!r.Ok() || !r.AtEnd()) {
        if (error) *error = "interaction state corrupted";
        return false;
    }
    for (u32 k = 0; k < ni; ++k) items_[k].enabled = enabled[k] != 0;
    for (u32 k = 0; k < nv; ++k) {
        volumes_[k].enabled = v_enabled[k] != 0;
        volumes_[k].inside = v_inside[k] != 0;
    }
    focused_ = -1;
    return true;
}

}  // namespace sspc
