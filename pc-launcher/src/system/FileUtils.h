// inFAMOUS PC Launcher — file operations with the guarantees the launcher promises: atomic writes with a backup,
// copies that report what failed, and removal only inside a given folder (never through a junction or a link).
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace Launcher::Files {

struct DirStats {
    qint64 bytes = 0;
    qint64 files = 0;
    qint64 newestMs = 0; // last write, ms since epoch (0: none)
};

/// Sizes of the regular files below `dir` (links and junctions are not followed).
DirStats Measure(const QString& dir);

/// `text` to `file` atomically: <file>.tmp written and flushed, then it replaces the file; the previous version is
/// kept as <file>.bak (as settings\user_store.cpp WriteFileAtomic). false + `error`: the old file is untouched.
bool WriteAtomic(const QString& file, const QByteArray& text, QString* error);

/// Copies the tree `from` into `to` (created). false + `error` on the first failure (what was copied stays).
bool CopyTree(const QString& from, const QString& to, QString* error, qint64* bytes = nullptr);

/// True if `path` is `root` or inside it (after resolving "..", case-insensitive like Windows).
bool IsInside(const QString& root, const QString& path);

/// Is `path` a junction, symbolic link or other reparse point?
bool IsReparsePoint(const QString& path);

/// Removes the tree `dir`, which must be strictly inside `root`. Links/junctions inside are removed as links, their
/// targets never entered. false + `error` if `dir` is not inside `root` or something could not be removed.
bool RemoveTreeInside(const QString& root, const QString& dir, QString* error);

/// Opens a folder (or a file's folder, the file selected) in Explorer.
bool Reveal(const QString& path);

/// The last `maxBytes` of a text file, from a line start.
QString Tail(const QString& file, qint64 maxBytes, int maxLines);

QString HumanSize(qint64 bytes, bool ru);

} // namespace Launcher::Files
