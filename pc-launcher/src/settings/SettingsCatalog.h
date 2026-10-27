// inFAMOUS PC Launcher — the settings the launcher shows. Each one is a setting of the runtime's registry
// (core\src\settings\registry.cpp, experiments.cpp): the same table, key, type, range, choices and labels, written to
// the same user file (%LOCALAPPDATA%\<product>\settings.toml) in the same form. Nothing here exists without a
// mechanism in the runtime (docs\SETTINGS_INTEGRATION.md has the table).
#pragma once

#include <QList>
#include <QString>
#include <QVariant>

namespace Launcher {

enum class SettingType { Bool, Int, Choice, Text };
enum class SettingSection { Display, Rendering, Effects, Game, Experiments };

/// What the computer and the game's folder must have (checked by SettingsManager).
namespace SysCaps {
constexpr quint32 Dlss = 1u << 0;         // GeForce RTX + nvngx_dlss.dll next to the game's exe
constexpr quint32 DlssFrameGen = 1u << 1; // GeForce RTX 40/50 + nvngx_dlssg.dll
constexpr quint32 Reflex = 1u << 2;       // an NVIDIA GPU
constexpr quint32 Fsr = 1u << 3;          // amd_fidelityfx_vk.dll next to the game's exe
} // namespace SysCaps

struct ChoiceDef {
    QString value;
    QString ru, en;
    quint32 needsSys = 0; // this one choice needs it (DLSS among the upscalers)
    quint32 needsGame = 0;
};

struct SettingDef {
    QString id;      // registry id ("display.presentMode", "experiments.SSPC_CP_MODE")
    QString table;   // the .toml table
    QString key;
    SettingSection section = SettingSection::Display;
    QString groupRu, groupEn; // a heading in the section
    SettingType type = SettingType::Bool;
    QList<ChoiceDef> choices;
    double min = 0, max = 0, step = 1;
    QString unit;
    QVariant builtinDefault; // the runtime's default when the product's .toml has no such key (pc_settings.h)
    QString ru, en, helpRu, helpEn;
    quint32 needsGame = 0; // GameCaps
    quint32 needsSys = 0;  // SysCaps
    QString requiresOn;    // another Bool setting that must be on for this one to matter
    QString requiresNot;   // "<id>=<value>": matters only while that setting is not this value
    bool acceptsFrameRate = false;  // any whole 20..1000 from the files (registry AcceptsFrameRate)
    bool acceptsResolution = false; // any "WxH" from the files (registry AcceptsResolution)
    // [Experiments]: the environment variable (the key), its values when on / off, the value shown while unset
    bool experiment = false;
    QString on, off, unsetShows;
    QString warnRu, warnEn; // a known risk, shown next to it
};

const QList<SettingDef>& Catalog();
const SettingDef* FindSetting(const QString& id);

} // namespace Launcher
