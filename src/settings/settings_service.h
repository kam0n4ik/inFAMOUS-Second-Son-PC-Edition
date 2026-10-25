// SecondSonPC — the owner of the PC settings while the game runs (docs/PC_SETTINGS_IMPLEMENTATION.md §5): the
// defaults (SecondSonPC.toml), the user's overrides (the user settings file), the current values, what this run
// started with (for "restart required"), availability (capabilities of this run), changes from the menu (checked,
// applied by the subsystems' appliers, saved off-thread). Own code; the bridge only adds appliers.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/config.h"
#include "settings/setting_types.h"
#include "settings/user_store.h"

namespace SecondSon::Settings {

struct Availability {
    bool available = true;
    std::string_view why_en, why_ru; // not available: why
};

enum class SetResult : u8 { Ok, Unchanged, Invalid, Unavailable };

class Service {
public:
    /// `defaults`: SecondSonPC.toml as parsed (no user file); `overrides`: the user file's; `started`: the
    /// configuration this run uses (defaults + overrides + command line); `file`: where changes are saved (empty:
    /// not saved - automated runs, self-tests); `capabilities`: Capability bits of this run.
    void Init(const App::Config& defaults, Overrides overrides, const App::Config& started,
              std::filesystem::path file, u32 capabilities, KeptKeys kept = {});
    bool Initialized() const;
    void SetCapabilities(u32 capabilities);
    u32 Capabilities() const;

    App::Config Current() const;
    Value Get(const Desc& desc) const;
    Value Default(const Desc& desc) const;  // SecondSonPC.toml's value
    Value Started(const Desc& desc) const;  // this run's value at start
    bool IsOverridden(const Desc& desc) const;
    bool NeedsRestart(const Desc& desc) const; // Restart setting changed since the start
    /// How a change of `desc` takes effect in this run: its `apply`, but Restart for a Live setting whose
    /// `live_needs` this run lacks (the vertical sync with the bridge's presenter).
    Apply EffectiveApply(const Desc& desc) const;
    bool AnyNeedsRestart() const;
    Availability Available(const Desc& desc) const;

    /// A change from the menu: checked (availability, type, range), stored as an override (dropped when it equals
    /// the default), saved, then the appliers are called (Live settings only) on this thread.
    SetResult Set(const Desc& desc, const Value& value, std::string* error = nullptr);
    /// Several settings as one change (window width + height): all or nothing.
    SetResult SetMany(std::span<const std::pair<const Desc*, Value>> changes, std::string* error = nullptr);
    void Reset(const Desc& desc);
    void ResetCategory(Category category);
    Overrides CurrentOverrides() const;

    using Applier = std::function<void(const App::Config& current, std::span<const Desc* const> changed)>;
    /// Called after every Live change, outside the service's lock, on the thread that made it.
    void AddApplier(Applier applier);

    /// Waits until the changes are on disk. true: nothing pending.
    bool Flush(u32 timeout_ms);
    std::filesystem::path File() const;
    u64 Saves() const;
    std::string LastSaveError() const;

private:
    SetResult Commit(std::span<const std::pair<const Desc*, Value>> changes, std::string* error);
    void SaveLocked(); // mutex held

    mutable std::mutex mutex_;
    bool initialized_ = false;
    App::Config defaults_;
    App::Config current_;
    App::Config started_;
    Overrides overrides_;
    KeptKeys kept_; // keys of the user file this version does not take: written back unchanged
    u32 capabilities_ = 0;
    std::unique_ptr<AsyncWriter> writer_;
    std::vector<Applier> appliers_;
};

Service& Instance();

/// Start-up: the user's overrides into a configuration (each through its setter). Returns how many.
size_t ApplyOverrides(App::Config& config, const Overrides& overrides);

} // namespace SecondSon::Settings
