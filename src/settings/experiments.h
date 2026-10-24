// SecondSonPC — the experiments page of the settings menu: the runtime's SSPC_* switches (what play.cmd and
// play_experimental.cmd turn on, and the measured R8-R10 experiments that are off for players) as settings a player
// can flip. Each is read once by its subsystem at start, so every one applies from the next start. The launcher's
// environment is the default; the user's choice is stored in the user settings file ([Experiments], the variable's
// name as the key) and put into the environment at start, before any subsystem reads it. Own code.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "settings/setting_types.h"
#include "settings/user_store.h"

namespace SecondSon::Settings {

struct Experiment {
    std::string_view env;                // the environment variable the subsystem reads
    std::string_view group_en, group_ru; // the menu's header above it
    Type type = Type::Bool;              // Bool or Choice
    std::string_view on, off;            // Bool: the variable's value when on / off
    std::span<const Choice> choices;     // Choice: the values (one may be "": the variable unset)
    std::string_view unset_shows;        // Choice: the value shown while the variable is not set
    std::string_view also_env, also_on;  // Bool: a second variable set with it (on: also_on; off: unset)
    std::string_view requires_env;       // another Bool experiment that must be on for this one to matter
    std::string_view en, ru;
    std::string_view help_en, help_ru;
};

std::span<const Experiment> Experiments();
const Experiment* FindExperiment(std::string_view env);
/// The setting's id: "experiments.<variable>".
std::string ExperimentId(std::string_view env);

/// Start-up, before the defaults are taken: the launcher's values of every experiment's variable into the config.
void ReadExperiments(App::Config& config);
/// Start-up, after the user's overrides are applied: the experiments the user changed into this process's
/// environment (CRT and Win32 alike), before any subsystem reads them. Returns "NAME=value" of each, for the log.
std::vector<std::string> ExportExperiments(const App::Config& config, const Overrides& overrides);

} // namespace SecondSon::Settings
