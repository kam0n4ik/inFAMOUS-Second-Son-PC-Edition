#include "profiles/GameProfile.h"

namespace Launcher {

const QList<GameProfile>& AllProfiles() {
    static const QList<GameProfile> profiles{MakeSecondSonProfile(), MakeFirstLightProfile()};
    return profiles;
}

const GameProfile* FindProfile(const QString& id) {
    for (const auto& p : AllProfiles()) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace Launcher
