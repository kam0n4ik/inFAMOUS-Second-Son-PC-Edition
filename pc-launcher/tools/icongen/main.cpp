// icongen - the launcher's .ico from its SVG (assets\brand\launcher.svg): 16..256 px, each a PNG entry (Windows Vista+).
//   icongen <in.svg> <out.ico>
#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc != 3) return 1;
    QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));
    if (!svg.isValid()) return 2;
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    QList<QByteArray> pngs;
    for (const int s : sizes) {
        QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        svg.render(&p, QRectF(0, 0, s, s));
        p.end();
        QByteArray data;
        QBuffer buf(&data);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        pngs << data;
    }
    QByteArray ico;
    const auto u16 = [&](quint16 v) { ico.append(char(v & 0xff)).append(char(v >> 8)); };
    const auto u32 = [&](quint32 v) { u16(quint16(v & 0xffff)); u16(quint16(v >> 16)); };
    u16(0); u16(1); u16(quint16(pngs.size()));
    quint32 offset = 6 + 16 * quint32(pngs.size());
    for (qsizetype i = 0; i < pngs.size(); ++i) {
        const int s = sizes[i];
        ico.append(char(s >= 256 ? 0 : s)).append(char(s >= 256 ? 0 : s)).append(char(0)).append(char(0));
        u16(1); u16(32);
        u32(quint32(pngs[i].size()));
        u32(offset);
        offset += quint32(pngs[i].size());
    }
    for (const auto& p : pngs) ico.append(p);
    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly)) return 3;
    out.write(ico);
    return 0;
}
