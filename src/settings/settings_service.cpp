// SecondSonPC — the owner of the PC settings while the game runs (see settings_service.h).
#include "settings/settings_service.h"

#include <algorithm>

#include "base/log.h"
#include "settings/registry.h"

namespace SecondSon::Settings {

namespace {

constexpr std::string_view kWhyEn = "Not available in this run (see the start options).";
constexpr std::string_view kWhyRu = "Недоступно в этом режиме запуска.";

Apply Effective(const Desc& d, u32 capabilities) {
    if (d.apply == Apply::Live && d.live_needs != 0 && (capabilities & d.live_needs) != d.live_needs) {
        return Apply::Restart;
    }
    return d.apply;
}

} // namespace

void Service::Init(const App::Config& defaults, Overrides overrides, const App::Config& started,
                   std::filesystem::path file, u32 capabilities, KeptKeys kept) {
    std::scoped_lock lock{mutex_};
    defaults_ = defaults;
    overrides_ = std::move(overrides);
    kept_ = std::move(kept);
    current_ = started;
    started_ = started;
    capabilities_ = capabilities;
    writer_ = file.empty() ? nullptr : std::make_unique<AsyncWriter>(std::move(file));
    initialized_ = true;
}

bool Service::Initialized() const {
    std::scoped_lock lock{mutex_};
    return initialized_;
}

void Service::SetCapabilities(u32 capabilities) {
    std::scoped_lock lock{mutex_};
    capabilities_ = capabilities;
}

u32 Service::Capabilities() const {
    std::scoped_lock lock{mutex_};
    return capabilities_;
}

App::Config Service::Current() const {
    std::scoped_lock lock{mutex_};
    return current_;
}

Value Service::Get(const Desc& desc) const {
    std::scoped_lock lock{mutex_};
    return desc.get != nullptr ? desc.get(current_, desc) : Value{false};
}

Value Service::Default(const Desc& desc) const {
    std::scoped_lock lock{mutex_};
    return desc.get != nullptr ? desc.get(defaults_, desc) : Value{false};
}

Value Service::Started(const Desc& desc) const {
    std::scoped_lock lock{mutex_};
    return desc.get != nullptr ? desc.get(started_, desc) : Value{false};
}

bool Service::IsOverridden(const Desc& desc) const {
    std::scoped_lock lock{mutex_};
    return overrides_.contains(desc.id);
}

bool Service::NeedsRestart(const Desc& desc) const {
    std::scoped_lock lock{mutex_};
    return Effective(desc, capabilities_) == Apply::Restart && desc.get != nullptr &&
           !Equal(desc.get(current_, desc), desc.get(started_, desc));
}

Apply Service::EffectiveApply(const Desc& desc) const {
    std::scoped_lock lock{mutex_};
    return Effective(desc, capabilities_);
}

bool Service::AnyNeedsRestart() const {
    return std::ranges::any_of(All(), [this](const Desc& d) { return NeedsRestart(d); });
}

Availability Service::Available(const Desc& desc) const {
    std::scoped_lock lock{mutex_};
    if (desc.apply == Apply::Unavailable) {
        return {false, desc.why_en, desc.why_ru};
    }
    if ((capabilities_ & desc.needs) != desc.needs) {
        return {false, desc.why_en.empty() ? kWhyEn : desc.why_en, desc.why_ru.empty() ? kWhyRu : desc.why_ru};
    }
    return {};
}

SetResult Service::Set(const Desc& desc, const Value& value, std::string* error) {
    const std::pair<const Desc*, Value> one{&desc, value};
    return Commit({&one, 1}, error);
}

SetResult Service::SetMany(std::span<const std::pair<const Desc*, Value>> changes, std::string* error) {
    return Commit(changes, error);
}

SetResult Service::Commit(std::span<const std::pair<const Desc*, Value>> changes, std::string* error) {
    std::vector<const Desc*> changed;
    App::Config snapshot;
    std::vector<Applier> appliers;
    {
        // Availability and validation first, for all of them: all or nothing.
        for (const auto& [d, v] : changes) {
            if (!Available(*d).available || d->set == nullptr || d->table.empty()) {
                if (error != nullptr) {
                    *error = std::string(d->id) + ": not available";
                }
                return SetResult::Unavailable;
            }
            std::string why;
            if (!Validate(*d, v, why)) {
                if (error != nullptr) {
                    *error = std::string(d->id) + ": " + why;
                }
                return SetResult::Invalid;
            }
        }
        std::scoped_lock lock{mutex_};
        for (const auto& [d, v] : changes) {
            if (Equal(d->get(current_, *d), v)) {
                continue;
            }
            d->set(current_, *d, v);
            if (Equal(d->get(defaults_, *d), v)) {
                overrides_.erase(std::string(d->id)); // back to SecondSonPC.toml's value: nothing to keep
            } else {
                overrides_.insert_or_assign(std::string(d->id), v);
            }
            changed.push_back(d);
            SSPC_LOG(App, Info, "setting {} = {}{}", d->id, Describe(v),
                     Effective(*d, capabilities_) == Apply::Restart ? " (from the next start)" : "");
        }
        if (changed.empty()) {
            return SetResult::Unchanged;
        }
        SaveLocked();
        snapshot = current_;
        appliers = appliers_;
        std::erase_if(changed, [this](const Desc* d) { return Effective(*d, capabilities_) != Apply::Live; });
    }
    if (!changed.empty()) {
        for (const auto& apply : appliers) {
            apply(snapshot, changed);
        }
    }
    return SetResult::Ok;
}

void Service::Reset(const Desc& desc) {
    if (desc.get == nullptr || desc.table.empty()) {
        return;
    }
    Set(desc, Default(desc));
}

void Service::ResetCategory(Category category) {
    std::vector<std::pair<const Desc*, Value>> changes;
    for (const auto& d : All()) {
        if (d.category == category && d.get != nullptr && !d.table.empty() && Available(d).available) {
            changes.emplace_back(&d, Default(d));
        }
    }
    if (!changes.empty()) {
        SetMany(changes);
    }
}

Overrides Service::CurrentOverrides() const {
    std::scoped_lock lock{mutex_};
    return overrides_;
}

void Service::AddApplier(Applier applier) {
    std::scoped_lock lock{mutex_};
    appliers_.push_back(std::move(applier));
}

void Service::SaveLocked() {
    if (writer_ != nullptr) {
        writer_->Save(SerializeUserFile(overrides_, kept_));
    }
}

bool Service::Flush(u32 timeout_ms) {
    AsyncWriter* writer;
    {
        std::scoped_lock lock{mutex_};
        writer = writer_.get();
    }
    return writer == nullptr || writer->Flush(timeout_ms);
}

std::filesystem::path Service::File() const {
    std::scoped_lock lock{mutex_};
    return writer_ != nullptr ? writer_->File() : std::filesystem::path{};
}

u64 Service::Saves() const {
    std::scoped_lock lock{mutex_};
    return writer_ != nullptr ? writer_->Writes() : 0;
}

std::string Service::LastSaveError() const {
    std::scoped_lock lock{mutex_};
    return writer_ != nullptr ? writer_->LastError() : std::string{};
}

Service& Instance() {
    static Service service;
    return service;
}

size_t ApplyOverrides(App::Config& config, const Overrides& overrides) {
    size_t n = 0;
    for (const auto& [id, value] : overrides) {
        const Desc* d = Find(id);
        if (d != nullptr && d->set != nullptr) {
            d->set(config, *d, value);
            ++n;
        }
    }
    return n;
}

} // namespace SecondSon::Settings
