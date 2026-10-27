// inFAMOUS PC Launcher — a supported game and everything the launcher needs to know about it.
// The launcher has no `if (game == ...)`: every difference between the games lives in one of these structures
// (SecondSonProfile.cpp, FirstLightProfile.cpp). The values are those of the PC runtime's own sources
// (core\src\app\product.h, core\src\launcher\launch.cpp, the products' .toml and play.cmd); docs\ARCHITECTURE.md.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace Launcher {

struct EnvVar {
    QString name;
    QString value;
};

/// What the runtime can do for this game. Settings carry the capability they need (SettingsCatalog.cpp); a setting
/// whose capability the profile lacks is shown inactive with the profile's reason.
namespace GameCaps {
constexpr quint32 EngineHooks = 1u << 0;        // Second Son's engine hooks install on this eboot (FOV, TAA, effects)
constexpr quint32 RenderSwitch = 1u << 1;       // the engine's render size switch: render scale, upscalers, HUD layout
constexpr quint32 ResolutionPatch = 1u << 2;    // the game's display size patch: resolution and aspect ratio
constexpr quint32 FreeFrameRate = 1u << 3;      // frame rates above 60 checked against the game's clock
constexpr quint32 CameraHooks = 1u << 4;        // the game's own FOV and anti-aliasing sites (render_resolution.cpp)
} // namespace GameCaps

struct GameProfile {
    QString id;                // "second_son": theme, assets, launcher state
    QString productId;         // "SecondSonPC": exe, .toml, log, %LOCALAPPDATA% folder (product.h kProduct.name)
    QString gameName;          // "inFAMOUS: Second Son"
    QString shortName;         // "Second Son"
    QString brand;             // "SECOND SON"
    QString windowTitle;       // the game window's title (product.h kWindowTitle)
    QStringList titleIds;      // param.sfo TITLE_ID values of this game (checked against the real files)
    QString exeName;           // "SecondSonPC.exe"
    QString configFileName;    // "SecondSonPC.toml"
    QStringList exeSearchDirs; // relative to the root (the folder of the .toml): "." (a player's install), "build/bin"
    QString logFileName;       // "SecondSonPC.log"
    QString themeId;           // "second_son" | "first_light"
    QStringList artCandidates; // the background, relative to the game folder (the game's own pictures)
    QString defaultUserData = QStringLiteral("userdata");
    QString defaultLogs = QStringLiteral("logs");
    QString defaultShaderCache = QStringLiteral("cache/shaders");
    QStringList launchArgs;      // after the exe: "--no-launcher" (the game at once, not its built-in launcher)
    QList<EnvVar> standardEnv;   // launch.cpp kStandard / play.cmd
    QList<EnvVar> experimentalEnv; // launch.cpp kExperimental / play_experimental.cmd
    quint32 caps = 0;
    QString hooksReasonRu, hooksReasonEn;       // why a setting that needs GameCaps::EngineHooks does nothing here
    QString renderSwitchReasonRu, renderSwitchReasonEn;
    QString frameRateReasonRu, frameRateReasonEn;
    QStringList unverifiedSettings;             // implemented for this game but not yet checked in play
    QString unverifiedReasonRu, unverifiedReasonEn;

    bool has(quint32 cap) const { return (caps & cap) == cap; }
};

GameProfile MakeSecondSonProfile();
GameProfile MakeFirstLightProfile();

/// Every supported game, in a fixed order.
const QList<GameProfile>& AllProfiles();
const GameProfile* FindProfile(const QString& id);

} // namespace Launcher
