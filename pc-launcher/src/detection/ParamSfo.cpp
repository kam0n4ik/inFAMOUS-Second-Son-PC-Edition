#include "detection/ParamSfo.h"

#include <QFile>
#include <QStringDecoder>
#include <QtEndian>

namespace Launcher {

namespace {

quint32 U32(const QByteArray& d, qsizetype at) {
    return qFromLittleEndian<quint32>(d.constData() + at);
}
quint16 U16(const QByteArray& d, qsizetype at) {
    return qFromLittleEndian<quint16>(d.constData() + at);
}

QString DecodeText(QByteArray raw) {
    const qsizetype nul = raw.indexOf('\0');
    if (nul >= 0) {
        raw.truncate(nul);
    }
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString s = utf8.decode(raw);
    if (utf8.hasError()) {
        s = QString::fromLatin1(raw);
    }
    return s;
}

} // namespace

ParamSfo ParamSfo::parse(const QByteArray& d) {
    ParamSfo out;
    if (d.size() < 20 || d.left(4) != QByteArray("\0PSF", 4)) {
        out.error = QStringLiteral("not a param.sfo (no \\0PSF header)");
        return out;
    }
    const quint32 keyTable = U32(d, 8), dataTable = U32(d, 12), count = U32(d, 16);
    if (count > 4096 || 20 + qsizetype(count) * 16 > d.size() || keyTable > quint32(d.size()) ||
        dataTable > quint32(d.size())) {
        out.error = QStringLiteral("damaged param.sfo header");
        return out;
    }
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype e = 20 + qsizetype(i) * 16;
        const quint16 keyOff = U16(d, e), fmt = U16(d, e + 2);
        const quint32 len = U32(d, e + 4), dataOff = U32(d, e + 12);
        const qsizetype k = qsizetype(keyTable) + keyOff;
        if (k >= d.size()) {
            continue;
        }
        const qsizetype kEnd = d.indexOf('\0', k);
        const QString key = QString::fromLatin1(d.mid(k, (kEnd < 0 ? d.size() : kEnd) - k));
        const qsizetype v = qsizetype(dataTable) + dataOff;
        if (v < 0 || v + qsizetype(len) > d.size()) {
            continue;
        }
        if (fmt == 0x0404 && len >= 4) {
            out.values.insert(key, U32(d, v));
        } else if (fmt == 0x0204) {
            out.values.insert(key, DecodeText(d.mid(v, len)));
        } else {
            out.values.insert(key, d.mid(v, len).toHex());
        }
    }
    out.valid = true;
    return out;
}

ParamSfo ParamSfo::load(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        ParamSfo out;
        out.error = f.errorString();
        return out;
    }
    if (f.size() > 1024 * 1024) {
        ParamSfo out;
        out.error = QStringLiteral("too large for a param.sfo");
        return out;
    }
    return parse(f.readAll());
}

} // namespace Launcher
