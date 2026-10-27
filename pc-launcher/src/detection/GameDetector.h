// inFAMOUS PC Launcher — which game the launcher stands in (docs\GAME_DETECTION.md). Never by a folder's name:
// the folder of Launcher.exe (and up to two folders above it, for an install in <game>\Launcher\), the product's
// configuration file there, its executable, the game's sce_sys\param.sfo TITLE_ID and the game's own files.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "detection/ParamSfo.h"
#include "profiles/GameProfile.h"

namespace Launcher {

struct Evidence {
    QString profileId; // "" = not of one game
    bool positive = true;
    QString textRu, textEn;
};

/// One game found (fully or in part) in one folder.
struct Candidate {
    const GameProfile* profile = nullptr;
    QString root;       // the folder of the .toml (the runtime's RootDir), or where the game's files are
    QString configPath; // <root>\<Product>.toml ("" if missing)
    QString gameDir;    // [Game] path of the .toml, or the root
    QString exePath;    // "" if missing
    QString userDataDir, logsDir, shaderCacheDir;
    ParamSfo sfo;
    bool configFound = false;
    bool configValid = false;
    QString configError;
    bool exeFound = false;
    bool titleConfirmed = false;
    QString foreignTitle;   // param.sfo of another supported game (a conflict)
    QStringList missingFiles; // the game's files that are not there
    QList<Evidence> evidence;

    bool gameFilesOk() const { return titleConfirmed && missingFiles.isEmpty(); }
    bool complete() const { return configFound && configValid && exeFound && gameFilesOk(); }
};

struct DetectionResult {
    enum class Status { Detected, Incomplete, Ambiguous, NotFound };
    Status status = Status::NotFound;
    QString startDir;
    QStringList searchedDirs;
    QList<Candidate> candidates; // Ambiguous: each game found; otherwise the chosen one first
    QList<Evidence> evidence;    // everything checked, in order (the details of the "not found" screen)
    int chosen = -1;

    const Candidate* game() const { return chosen >= 0 && chosen < candidates.size() ? &candidates[chosen] : nullptr; }
    QString report(bool ru) const;
};

class GameDetector {
public:
    explicit GameDetector(const QList<GameProfile>& profiles) : profiles_(profiles) {}

    /// `startDir`: the folder of Launcher.exe. Looks there, then up to `levelsUp` folders above it.
    DetectionResult detect(const QString& startDir, int levelsUp = 2) const;

    /// Everything about one game in one folder (also used when the user resolves an ambiguity).
    Candidate inspect(const GameProfile& profile, const QString& root) const;

private:
    const QList<GameProfile>& profiles_;
};

/// The launcher's own folder: GetModuleFileNameW, never the working directory.
QString LauncherDir();

} // namespace Launcher
