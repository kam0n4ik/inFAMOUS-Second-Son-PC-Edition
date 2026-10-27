// Play (ProcessManager): the stub game as SecondSonPC.exe in a fixture folder — the arguments, the working folder, the
// environment, "running" only once its window exists, the end, an early failure, the errors before a start, and the
// game living on when the launcher goes away. No real game is started.
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <windows.h>

#include "fixtures.h"
#include "launch/ProcessManager.h"
#include "profiles/GameProfile.h"
#include "system/SystemInfo.h"

using namespace Launcher;

class TstProcess : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    int n_ = 0;

    LaunchSpec Spec(const QString& root, QList<QPair<QString, QString>> env) {
        LaunchSpec s;
        s.profile = FindProfile(QStringLiteral("second_son"));
        s.exePath = root + "/SecondSonPC.exe";
        s.workingDir = root;
        s.gameDir = root;
        s.userDataDir = root + "/userdata";
        s.args = {QStringLiteral("--no-launcher")};
        s.env = std::move(env);
        return s;
    }
    QString Game() {
        const QString root = tmp_.filePath(QStringLiteral("игра %1").arg(++n_));
        Fixture::MakeGame(root, "SecondSonPC", {});
        Fixture::Write(root + "/sce_sys/param.sfo", "x");
        return root;
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        if (System::IsProcessRunning(QStringLiteral("SecondSonPC.exe"))) {
            QSKIP("SecondSonPC.exe is running (someone plays): the stub would be refused as a second copy");
        }
    }

    void startRunEnd() {
        const QString root = Game();
        QVERIFY(Fixture::Write(root + "/userdata/home/1/savedata/CUSA00004/s/data.bin", "save"));
        ProcessManager pm;
        pm.setRussian(true);
        QSignalSpy started(&pm, &ProcessManager::started), shown(&pm, &ProcessManager::windowShown),
            finished(&pm, &ProcessManager::finished);
        pm.launch(Spec(root, {{"SSPC_TEST_FLAG", "1"}, {"STUB_TITLE", "inFAMOUS Second Son"}, {"STUB_LIFETIME_MS", "2500"}}));
        QVERIFY(started.wait(15000));
        QCOMPARE(pm.state(), QStringLiteral("starting")); // a process alone is not "running"
        QVERIFY(shown.wait(15000));
        QCOMPARE(pm.state(), QStringLiteral("running"));
        QCOMPARE(pm.message(), QStringLiteral("Игра запущена"));
        QVERIFY(finished.wait(15000));
        QCOMPARE(finished.front().at(0).toInt(), 0);
        QCOMPARE(finished.front().at(2).toBool(), false);
        QCOMPARE(pm.state(), QStringLiteral("idle"));

        const QString report = QString::fromUtf8(Fixture::Read(root + "/stub_report.txt"));
        QVERIFY2(report.contains(QStringLiteral("--no-launcher")), qPrintable(report));
        QVERIFY(report.contains(QStringLiteral("cwd=") + QDir::toNativeSeparators(root)));
        QVERIFY(report.contains(QStringLiteral("env:SSPC_TEST_FLAG=1")));
        // the saves were copied before the start, as play.cmd does
        const QStringList copies = QDir(root + "/userdata/savedata_backups").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(copies.size(), 1);
    }

    void missingExe() {
        const QString root = Game();
        QFile::remove(root + "/SecondSonPC.exe");
        ProcessManager pm;
        pm.setRussian(true);
        QSignalSpy failed(&pm, &ProcessManager::failed);
        pm.launch(Spec(root, {}));
        QVERIFY(failed.wait(10000));
        QCOMPARE(failed.front().at(0).toString(), QStringLiteral("Исполняемый файл игры не найден"));
        QCOMPARE(pm.state(), QStringLiteral("error"));
    }

    void missingGameFiles() {
        const QString root = Game();
        QFile::remove(root + "/eboot.bin");
        ProcessManager pm;
        pm.setRussian(true);
        QSignalSpy failed(&pm, &ProcessManager::failed);
        pm.launch(Spec(root, {}));
        QVERIFY(failed.wait(10000));
        QCOMPARE(failed.front().at(0).toString(), QStringLiteral("Не найдены необходимые игровые файлы"));
        QVERIFY(failed.front().at(1).toString().contains(QStringLiteral("eboot.bin")));
        QVERIFY(!QFileInfo::exists(root + "/stub_report.txt")); // nothing was started
    }

    void earlyFailure() {
        const QString root = Game();
        ProcessManager pm;
        pm.setRussian(true);
        QSignalSpy finished(&pm, &ProcessManager::finished);
        pm.launch(Spec(root, {{"STUB_NO_WINDOW", "1"}, {"STUB_EXIT", "3"}}));
        QVERIFY(finished.wait(15000));
        QCOMPARE(finished.front().at(0).toInt(), 3);
        QCOMPARE(finished.front().at(2).toBool(), true);
        QCOMPARE(pm.state(), QStringLiteral("error"));
        QCOMPARE(pm.message(), QStringLiteral("Произошла ошибка запуска. Откройте журнал для подробностей"));
    }

    void gameOutlivesLauncher() {
        const QString root = Game();
        qint64 pid = 0;
        {
            ProcessManager pm;
            QSignalSpy started(&pm, &ProcessManager::started);
            pm.launch(Spec(root, {{"STUB_LIFETIME_MS", "4000"}}));
            QVERIFY(started.wait(15000));
            pid = pm.pid();
        } // the launcher closes
        const HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
        QVERIFY(h != nullptr);
        QCOMPARE(WaitForSingleObject(h, 500), DWORD(WAIT_TIMEOUT)); // still running
        QCOMPARE(WaitForSingleObject(h, 15000), DWORD(WAIT_OBJECT_0)); // ends by itself
        CloseHandle(h);
    }

    void secondCopyRefused() {
        const QString root = Game();
        ProcessManager a, b;
        b.setRussian(true);
        QSignalSpy started(&a, &ProcessManager::started), failed(&b, &ProcessManager::failed), finished(&a, &ProcessManager::finished);
        a.launch(Spec(root, {{"STUB_LIFETIME_MS", "3000"}}));
        QVERIFY(started.wait(15000));
        LaunchSpec s = Spec(root, {});
        s.backupSaves = false;
        b.launch(s);
        QVERIFY(failed.wait(10000));
        QCOMPARE(failed.front().at(0).toString(), QStringLiteral("Игра уже запущена"));
        QVERIFY(finished.wait(15000));
    }
};

QTEST_MAIN(TstProcess)
#include "tst_process.moc"
