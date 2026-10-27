// inFAMOUS PC Launcher — a PS4 param.sfo (sce_sys\param.sfo of the game, sce_sys\param.sfo of a save): the
// "\0PSF" key table read-only. Strings are UTF-8 (Latin-1 if they are not valid UTF-8).
#pragma once

#include <QMap>
#include <QString>
#include <QVariant>

namespace Launcher {

struct ParamSfo {
    bool valid = false;
    QString error;
    QMap<QString, QVariant> values; // KEY -> QString | quint32

    QString text(const QString& key) const { return values.value(key).toString(); }
    QString titleId() const { return text(QStringLiteral("TITLE_ID")); }

    static ParamSfo parse(const QByteArray& data);
    static ParamSfo load(const QString& path);
};

} // namespace Launcher
