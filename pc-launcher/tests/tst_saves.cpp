// Saves (SaveManager): a fixture userdata folder with saves of both games; backups and a restore that changes only
// the game's own saves and keeps a copy of what it replaces.
#include <QTemporaryDir>
#include <QtTest>

#include "fixtures.h"
#include "saves/SaveManager.h"

using namespace Launcher;

class TstSaves : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString ud() { return tmp_.filePath("userdata"); }
    QString save(const QString& tid, const QString& dir) { return ud() + "/home/1/savedata/" + tid + "/" + dir; }
    const QStringList ss{QStringLiteral("CUSA00004")};

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        QVERIFY(Fixture::Write(save("CUSA00004", "infamousssAuto0") + "/data.bin", "ss one"));
        QVERIFY(Fixture::Write(save("CUSA00004", "infamousssAuto1") + "/data.bin", "ss two"));
        QVERIFY(Fixture::Write(save("CUSA00897", "fl0") + "/data.bin", "first light"));
    }

    void scanSeesOnlyThisGame() {
        const auto list = SaveManager::ScanSaves(ud(), ss);
        QCOMPARE(list.size(), 2);
        for (const auto& s : list) QVERIFY(s.path.contains(QStringLiteral("CUSA00004")));
        QCOMPARE(SaveManager::ScanSaves(ud(), {QStringLiteral("CUSA00897")}).size(), 1);
    }

    void backupAndRestore() {
        QString created, error;
        QVERIFY2(SaveManager::Backup(ud(), QStringLiteral("_launcher"), &created, &error), qPrintable(error));
        QVERIFY(created.contains(QStringLiteral("savedata_backups")));
        QCOMPARE(Fixture::Read(created + "/1/savedata/CUSA00004/infamousssAuto0/data.bin"), QByteArray("ss one"));
        const auto backups = SaveManager::ScanBackups(ud(), ss);
        QCOMPARE(backups.size(), 1);
        QCOMPARE(backups.front().saves, 2);

        // the game saves again; then the copy is restored
        QVERIFY(Fixture::Write(save("CUSA00004", "infamousssAuto0") + "/data.bin", "ss one, later"));
        QVERIFY(Fixture::Write(save("CUSA00897", "fl0") + "/data.bin", "first light, later"));
        QString safety;
        QVERIFY2(SaveManager::Restore(ud(), ss, created, &safety, &error), qPrintable(error));
        QCOMPARE(Fixture::Read(save("CUSA00004", "infamousssAuto0") + "/data.bin"), QByteArray("ss one"));
        // the other game's saves are not touched, the replaced state is in the safety copy, the copy itself stays
        QCOMPARE(Fixture::Read(save("CUSA00897", "fl0") + "/data.bin"), QByteArray("first light, later"));
        QCOMPARE(Fixture::Read(safety + "/1/savedata/CUSA00004/infamousssAuto0/data.bin"), QByteArray("ss one, later"));
        QVERIFY(QFileInfo(created + "/1/savedata/CUSA00004/infamousssAuto0/data.bin").isFile());
        // no leftovers of the swap
        const QStringList left = QDir(ud() + "/home/1/savedata").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(left, QStringList({QStringLiteral("CUSA00004"), QStringLiteral("CUSA00897")}));
    }

    void restoreRefusesForeignFolders() {
        QString safety, error;
        const QString outside = tmp_.filePath("elsewhere");
        QVERIFY(Fixture::Write(outside + "/1/savedata/CUSA00004/x/data.bin", "x"));
        QVERIFY(!SaveManager::Restore(ud(), ss, outside, &safety, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(Fixture::Read(save("CUSA00004", "infamousssAuto0") + "/data.bin"), QByteArray("ss one"));
    }

    void restoreRefusesAnotherGamesCopy() {
        // a copy that holds only First Light's saves cannot be restored as Second Son's
        const QString onlyFl = ud() + "/savedata_backups/20260101_000000";
        QVERIFY(Fixture::Write(onlyFl + "/1/savedata/CUSA00897/fl0/data.bin", "fl"));
        QString safety, error;
        QVERIFY(!SaveManager::Restore(ud(), ss, onlyFl, &safety, &error));
        QCOMPARE(Fixture::Read(save("CUSA00004", "infamousssAuto0") + "/data.bin"), QByteArray("ss one"));
    }
};

QTEST_GUILESS_MAIN(TstSaves)
#include "tst_saves.moc"
