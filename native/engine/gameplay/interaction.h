// SecondSonPC native runtime - interactable objects and trigger volumes.
//
// Interactables are points with a use radius (e.g. the USE_BUTTON of the cannery billboard, radius
// 200 cm in its data); the nearest enabled one in range that the player faces gets the focus, and
// pressing interact posts InteractionPressed. Trigger volumes are oriented boxes; crossing their
// boundary posts PlayerEnteredArea / PlayerLeftArea. Both only report events - what they mean is up to
// the mission definition.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "gameplay/events.h"

namespace sspc {

struct Interactable {
    u64 id = 0;
    std::string name;
    Vec3 position;            // world, metres
    float radius = 2.0f;      // metres
    float max_angle = 1.2f;   // radians between the player's facing and the direction to the object
    u64 prompt_key = 0;       // text key hash of the prompt (e.g. NameHash("u_button_graffiti"))
    bool enabled = false;
};

struct TriggerVolume {
    u64 id = 0;
    std::string name;
    Vec3 center;          // world, metres
    Quat rotation;
    Vec3 half_extents{1, 1, 1};
    bool enabled = true;
    bool inside = false;
};

class InteractionSystem {
public:
    void AddInteractable(Interactable i);
    void AddVolume(TriggerVolume v);
    bool SetEnabled(u64 id, bool enabled);  // interactables and volumes
    // Recomputes focus and volume occupancy for the player; posts events on changes. With
    // `silent` the occupancy is updated without events (after loading a save or teleporting).
    void Update(const Vec3& player_feet, float player_yaw, bool interact_pressed, GameEventBus& bus, bool silent = false);

    const Interactable* Focused() const;
    const std::vector<Interactable>& Interactables() const { return items_; }
    const std::vector<TriggerVolume>& Volumes() const { return volumes_; }
    static bool Contains(const TriggerVolume& v, const Vec3& p);

    std::vector<u8> Serialize() const;
    bool Deserialize(std::span<const u8> data, std::string* error);

private:
    std::vector<Interactable> items_;
    std::vector<TriggerVolume> volumes_;
    int focused_ = -1;
};

}  // namespace sspc
