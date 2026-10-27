// Detection (docs\GAME_DETECTION.md): fixture folders of both games, by content and never by name; the real Launcher.exe
// with --detect-report from a copy in a game's Launcher\ subfolder, from another working folder and through a .lnk.
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

#include "detection/GameDetector.h"
#include "fixtures.h"

using namespace Launcher;
using Status = DetectionResult::Status;

class TstDetection : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString Dir(const QString& name) { return tmp_.filePath(name); }
    DetectionResult Detect(const QString& dir) { return GameDetector(AllProfiles()).detect(dir); }
    static QString Id(const DetectionResult& r) { return r.game() ? r.game()->profile->id : QString(); }

    /// Launcher.exe --detect-report <file> [--game-root <root>]: the status and the report text.
    QPair<int, QString> RunReport(const QString& exe, const QString& root, const QString& cwd) {
        const QString out = Dir(QStringLiteral("report-%1.txt").arg(++reports_));
        QStringList args{QStringLiteral("--detect-report"), out};
        if (!root.isEmpty()) args << QStringLiteral("--game-root") << root;
        QProcess p;
        p.setWorkingDirectory(cwd);
        p.start(exe, args);
        if (!p.waitForFinished(30000)) return {-1, QStringLiteral("timeout")};
        return {p.exitCode(), QString::fromUtf8(Fixture::Read(out))};
    }
    int reports_ = 0;

    /// A copy of the built Launcher.exe in <root>\Launcher\ (the Qt DLLs come from PATH, as in tools\env.ps1).
    QString CopyLauncher(const QString& root) {
        const QString exe = root + QStringLiteral("/Launcher/Launcher.exe");
        QDir().mkpath(QFileInfo(exe).absolutePath());
        QFile::remove(exe);
        return QFile::copy(QStringLiteral(LAUNCHER_EXE), exe) ? exe : QString();
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        if (!Fixture::HaveSfos()) QSKIP("the games' param.sfo files are not where LAUNCHER_SS_SFO / LAUNCHER_FL_SFO say");
    }

    void secondSonRelease() {
        const QString root = Dir("ss-release");
        QVERIFY(Fixture::MakeGame(root, "SecondSonPC", {.sfo = Fixture::SsSfo()}));
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::Detected);
        QCOMPARE(Id(r), QStringLiteral("second_son"));
        QVERIFY(r.game()->titleConfirmed);
    }

    void firstLightDevTree() {
        const QString root = Dir("fl-dev");
        QVERIFY(Fixture::MakeGame(root, "FirstLightPC", {.exeInBuildBin = true, .sfo = Fixture::FlSfo()}));
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::Detected);
        QCOMPARE(Id(r), QStringLiteral("first_light"));
        QVERIFY(r.game()->exePath.endsWith(QStringLiteral("build/bin/FirstLightPC.exe")));
    }

    void folderNameIsIrrelevant() {
        // a folder called "Second Son" that holds First Light: First Light; spaces and Cyrillic in the path
        const QString root = Dir(QStringLiteral("inFAMOUS Second Son PC Edition/Игры и папка с пробелами"));
        QVERIFY(Fixture::MakeGame(root, "FirstLightPC", {.sfo = Fixture::FlSfo()}));
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::Detected);
        QCOMPARE(Id(r), QStringLiteral("first_light"));
    }

    void noGame() {
        const QString root = Dir("empty/a/b/c");
        QDir().mkpath(root);
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::NotFound);
        QCOMPARE(r.searchedDirs.size(), 3);
    }

    void incompleteFiles() {
        const QString root = Dir("ss-incomplete");
        QVERIFY(Fixture::MakeGame(root, "SecondSonPC", {.eboot = false, .sceModule = false, .sfo = Fixture::SsSfo()}));
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::Incomplete);
        QCOMPARE(Id(r), QStringLiteral("second_son"));
        QCOMPARE(r.game()->missingFiles.size(), 2);
    }

    void exeMissing() {
        const QString root = Dir("fl-noexe");
        QVERIFY(Fixture::MakeGame(root, "FirstLightPC", {.exe = false, .sfo = Fixture::FlSfo()}));
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::Incomplete);
        QVERIFY(!r.game()->exeFound);
    }

    void gameFilesOnly() {
        // only the PS4 game's files: the Title ID names the game, the PC Edition is missing
        const QString root = Dir("fl-gameonly");
        QVERIFY(Fixture::MakeGame(root, "FirstLightPC", {.config = false, .exe = false, .sfo = Fixture::FlSfo()}));
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::Incomplete);
        QCOMPARE(Id(r), QStringLiteral("first_light"));
    }

    void bothGames() {
        const QString root = Dir("both");
        QVERIFY(Fixture::MakeGame(root, "SecondSonPC", {.sfo = Fixture::SsSfo()}));
        QVERIFY(Fixture::MakeGame(root, "FirstLightPC", {}));
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::Ambiguous);
        QCOMPARE(r.candidates.size(), 2);
        QVERIFY(r.game() == nullptr);
    }

    void contradictingSigns() {
        // Second Son's configuration and exe over First Light's files
        const QString root = Dir("conflict");
        QVERIFY(Fixture::MakeGame(root, "SecondSonPC", {.sfo = Fixture::FlSfo()}));
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::Ambiguous);
    }

    void damagedConfig() {
        const QString root = Dir("ss-badtoml");
        QVERIFY(Fixture::MakeGame(root, "SecondSonPC", {.sfo = Fixture::SsSfo()}));
        QVERIFY(Fixture::Write(root + "/SecondSonPC.toml", "[Game\npath = \n"));
        const auto r = Detect(root);
        QCOMPARE(r.status, Status::Incomplete);
        QVERIFY(!r.game()->configValid);
    }

    void launcherSubfolder() {
        const QString root = Dir("ss-sub");
        QVERIFY(Fixture::MakeGame(root, "SecondSonPC", {.sfo = Fixture::SsSfo()}));
        QDir().mkpath(root + "/Launcher");
        const auto r = Detect(root + "/Launcher");
        QCOMPARE(r.status, Status::Detected);
        QCOMPARE(QDir::cleanPath(r.game()->root), QDir::cleanPath(root));
    }

    void workingDirectoryIsIrrelevant() {
        const QString root = Dir("fl-cwd");
        QVERIFY(Fixture::MakeGame(root, "FirstLightPC", {.sfo = Fixture::FlSfo()}));
        const QString ssRoot = Dir("ss-cwd");
        QVERIFY(Fixture::MakeGame(ssRoot, "SecondSonPC", {.sfo = Fixture::SsSfo()}));
        const QString before = QDir::currentPath();
        QDir::setCurrent(ssRoot); // the working folder holds the other game
        const auto r = Detect(root);
        QDir::setCurrent(before);
        QCOMPARE(Id(r), QStringLiteral("first_light"));
    }

    // ---- the real Launcher.exe

    void exeFromLauncherSubfolder() {
        const QString root = Dir(QStringLiteral("Папка с пробелами/FL copy"));
        QVERIFY(Fixture::MakeGame(root, "FirstLightPC", {.sfo = Fixture::FlSfo()}));
        const QString exe = CopyLauncher(root);
        QVERIFY(!exe.isEmpty());
        // started from another working folder (the system folder), no --game-root: the exe's own folder decides
        const auto [status, text] = RunReport(exe, {}, QStringLiteral("C:/Windows/System32"));
        QCOMPARE(status, 0);
        QVERIFY2(text.contains(QStringLiteral("profile=first_light")), qPrintable(text));
    }

    void exeNoGame() {
        const QString root = Dir("nogame-exe");
        QDir().mkpath(root);
        const QString exe = CopyLauncher(root);
        const auto [status, text] = RunReport(exe, {}, root);
        QCOMPARE(status, int(Status::NotFound));
    }

    void exeThroughShortcut() {
        const QString root = Dir(QStringLiteral("ярлык/Second Son"));
        QVERIFY(Fixture::MakeGame(root, "SecondSonPC", {.sfo = Fixture::SsSfo()}));
        const QString exe = CopyLauncher(root);
        const QString out = Dir("report-lnk.txt");
        const QString lnk = Dir("Launcher.lnk");
        // the shortcut sits elsewhere and starts in the user's profile folder
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) || true);
        IShellLinkW* link = nullptr;
        QVERIFY(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))));
        link->SetPath(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(exe).utf16()));
        const QString args = QStringLiteral("--detect-report \"%1\"").arg(QDir::toNativeSeparators(out));
        link->SetArguments(reinterpret_cast<LPCWSTR>(args.utf16()));
        link->SetWorkingDirectory(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(QDir::homePath()).utf16()));
        IPersistFile* file = nullptr;
        QVERIFY(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))));
        QVERIFY(SUCCEEDED(file->Save(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(lnk).utf16()), TRUE)));
        file->Release();
        link->Release();

        const std::wstring lnkPath = QDir::toNativeSeparators(lnk).toStdWString();
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpFile = lnkPath.c_str();
        sei.nShow = SW_HIDE;
        QVERIFY(ShellExecuteExW(&sei));
        QVERIFY(sei.hProcess != nullptr);
        QCOMPARE(WaitForSingleObject(sei.hProcess, 30000), DWORD(WAIT_OBJECT_0));
        DWORD code = 99;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        QCOMPARE(int(code), 0);
        QVERIFY(QString::fromUtf8(Fixture::Read(out)).contains(QStringLiteral("profile=second_son")));
    }
};

QTEST_GUILESS_MAIN(TstDetection)
#include "tst_detection.moc"
