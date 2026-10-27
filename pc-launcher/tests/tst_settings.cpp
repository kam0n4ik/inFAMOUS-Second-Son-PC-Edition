// Settings (docs\SETTINGS_INTEGRATION.md): the TOML editor and SettingsManager on a fixture %LOCALAPPDATA% — never
// the player's real settings.toml.
#include <QTemporaryDir>
#include <QtTest>

#include "fixtures.h"
#include "profiles/GameProfile.h"
#include "settings/SettingsManager.h"
#include "settings/TomlDocument.h"

using namespace Launcher;

class TstSettings : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString userFile(const QString& product) { return tmp_.filePath(product + "/settings.toml"); }

    std::unique_ptr<SettingsManager> Open(const char* profileId, const QString& product) {
        auto m = std::make_unique<SettingsManager>();
        m->setRussian(true);
        const QString root = tmp_.filePath("game-" + product);
        QDir().mkpath(root);
        Fixture::Write(root + "/" + product + ".toml", "[Display]\nwidth = 1280\nheight = 720\nshowFps = false\n[Graphics]\nmotionBlur = true\n");
        m->setGame(FindProfile(QString::fromLatin1(profileId)), root + "/" + product + ".toml", root, {}, {});
        return m;
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        // the runtime and the launcher both read %LOCALAPPDATA%: point it at the fixture
        qputenv("LOCALAPPDATA", QDir::toNativeSeparators(tmp_.path()).toLocal8Bit());
        QCOMPARE(QDir::cleanPath(SettingsManager::LocalAppData()), QDir::cleanPath(tmp_.path()));
    }

    void tomlKeepsEverythingElse() {
        const QByteArray text = "# head comment\r\n[Display]\r\nwidth = 1920   # mine\r\nfutureKey = [1, 2]\r\n\r\n[Unknown]\r\nx = 'a'\r\n";
        TomlDocument d;
        QVERIFY(d.parse(text));
        d.set("Display", "width", qint64(2560));
        d.set("Display", "showFps", true);
        d.set("Graphics", "fov", 1.25);
        const QByteArray out = d.toUtf8();
        QVERIFY(out.contains("# head comment\r\n"));
        QVERIFY(out.contains("width = 2560   # mine\r\n"));
        QVERIFY(out.contains("futureKey = [1, 2]"));
        QVERIFY(out.contains("[Unknown]\r\nx = 'a'"));
        TomlDocument back;
        QVERIFY(back.parse(out));
        QCOMPARE(back.value("Display", "showFps").toBool(), true);
        QCOMPARE(back.value("Graphics", "fov").toDouble(), 1.25);
        QVERIFY(d.remove("Display", "width"));
        QVERIFY(!d.toUtf8().contains("width"));
    }

    void tomlRejectsBroken() {
        TomlDocument d;
        QString error;
        QVERIFY(!d.parse("[Display\nwidth = 1\n", &error));
        QVERIFY(!error.isEmpty());
    }

    void saveKeepsUnknownKeysAndComments() {
        const QString file = userFile("SecondSonPC");
        QVERIFY(Fixture::Write(file, "# player's note\n[Display]\nshowFps = true\n[Graphics]\npotatoMode = true\n"));
        auto m = Open("second_son", "SecondSonPC");
        QCOMPARE(QDir::cleanPath(m->userFile()), QDir::cleanPath(file));
        QCOMPARE(m->value("display.showFps").toBool(), true);
        m->setValue("display.showFps", false); // = the product's default: the line goes
        m->setValue("graphics.motionBlur", false);
        QVERIFY2(m->save(), qPrintable(m->lastError()));
        const QByteArray out = Fixture::Read(file);
        QVERIFY(out.contains("# player's note"));
        QVERIFY(out.contains("potatoMode = true"));
        QVERIFY(!out.contains("showFps"));
        QVERIFY(out.contains("motionBlur = false"));
        QVERIFY(QFileInfo::exists(file + ".bak")); // the file as it was
        QVERIFY(Fixture::Read(file + ".bak").contains("showFps = true"));
        QVERIFY(!QFileInfo::exists(file + ".tmp"));
    }

    void reopenReadsWhatWasSaved() {
        auto m = Open("second_son", "SecondSonPC");
        QCOMPARE(m->value("graphics.motionBlur").toBool(), false);
        QVERIFY(m->isModified("graphics.motionBlur"));
        QVERIFY(!m->isModified("display.showFps"));
    }

    void damagedFileIsNotTouched() {
        const QString file = userFile("SecondSonPC");
        const QByteArray broken = "[Display\nshowFps = maybe\n";
        QVERIFY(Fixture::Write(file, broken));
        auto m = Open("second_son", "SecondSonPC");
        m->setValue("display.showFps", true);
        QVERIFY(!m->save());
        QVERIFY(!m->lastError().isEmpty());
        QCOMPARE(Fixture::Read(file), broken);
    }

    void newFileHasTheRuntimeHeader() {
        const QString file = userFile("FirstLightPC");
        QFile::remove(file);
        auto m = Open("first_light", "FirstLightPC");
        m->setValue("display.showFps", true);
        QVERIFY2(m->save(), qPrintable(m->lastError()));
        const QByteArray out = Fixture::Read(file);
        QVERIFY(out.startsWith("# FirstLightPC user settings"));
        QVERIFY(out.contains("[Display]\nshowFps = true"));
    }

    void firstLightHooksAreInactive() {
        auto fl = Open("first_light", "FirstLightPC");
        QVERIFY(!fl->isActive("graphics.motionBlur"));
        QVERIFY(!fl->unavailableReason("graphics.motionBlur").isEmpty());
        QVERIFY(fl->isActive("graphics.fov"));
        QVERIFY(fl->isActive("graphics.antiAliasing"));
        QVERIFY(!fl->isActive("graphics.shadowQuality"));
        QVERIFY(fl->unavailableReason("display.showFps").isEmpty());
        QVERIFY(fl->isActive("display.showFps"));
        auto ss = Open("second_son", "SecondSonPC");
        QVERIFY(ss->isActive("graphics.motionBlur"));
    }

    void experimentsDefaultOff() {
        auto m = Open("second_son", "SecondSonPC");
        QVERIFY(!m->launchEnvironment().isEmpty()); // the standard profile, as play.cmd
        bool aq = false;
        for (const auto& [k, v] : m->launchEnvironment()) aq = aq || (k == "SSPC_FB_ASYNC_COMPUTE" && v != "0");
        QVERIFY(!aq);
    }
};

QTEST_GUILESS_MAIN(TstSettings)
#include "tst_settings.moc"
