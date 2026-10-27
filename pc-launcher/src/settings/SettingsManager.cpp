#include "settings/SettingsManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include "profiles/GameProfile.h"
#include "system/FileUtils.h"

namespace Launcher {

namespace {

const QString kWindowMode = QStringLiteral("display.windowMode");
const QString kWindowSize = QStringLiteral("display.windowSize");
const QString kLaunchProfile = QStringLiteral("launch.profile");

/// Rows the launcher shows through a combined row (as the runtime's own menu does).
bool HiddenRow(const QString& id) {
    return id == QLatin1String("display.fullscreen") || id == QLatin1String("display.fullscreenMode") ||
           id == QLatin1String("display.width") || id == QLatin1String("display.height");
}

/// The keys the runtime's registry knows besides the launcher's (core\src\settings\registry.cpp), for the file check.
const QSet<QString>& KnownKeys() {
    static const QSet<QString> keys = [] {
        QSet<QString> k;
        for (const char* s : {"Display.fullscreen", "Display.fullscreenMode", "Display.width", "Display.height",
                              "Display.monitor", "Display.presentMode", "Display.showFps", "Display.frameRateLimit",
                              "Display.resolution", "Display.aspectRatio", "Display.hudSafeArea", "Display.hudArea",
                              "Graphics.fsr", "Graphics.sharpening", "Graphics.sharpness", "Graphics.smokeQuality",
                              "Graphics.particleResolution", "Graphics.renderScale", "Graphics.fov", "Graphics.motionBlur",
                              "Graphics.motionBlurStrength", "Graphics.reflectionQuality", "Graphics.rainShadows",
                              "Graphics.anisotropy", "Graphics.antiAliasing", "Graphics.shadowQuality",
                              "Graphics.textureQuality", "Graphics.upscaler", "Graphics.upscaleQuality",
                              "Graphics.upscaleSharpness", "Graphics.frameGeneration", "Graphics.frameGenMultiplier",
                              "Graphics.reflex", "Audio.volume", "Audio.device", "Input.mouseSensitivity", "Input.invertY",
                              "Input.mouseDeadzone", "Input.captureMouse", "Input.motionSensitivity", "Input.sideways",
                              "Gamepad.vibration", "Gamepad.cameraSensitivity", "Gamepad.invertCameraX",
                              "Gamepad.invertCameraY", "Gamepad.menuButton", "Interface.language", "Interface.scale",
                              "Interface.prompts", "ShaderCache.precompile", "ShaderCache.showProgress",
                              "ShaderCache.predictAll", "ShaderCache.maxSizeMiB", "ShaderCache.precompileThreads",
                              "Language.system", "System.enterButton"}) {
            k.insert(QString::fromLatin1(s));
        }
        for (const char* e :
             {"SSPC_CP_MODE", "SSPC_CITY_PERFORMANCE", "SSPC_REA_MERGE_RUNS", "SSPC_CP_ROUND_NODRAIN",
              "SSPC_P411_ASYNC_ACQUIRE", "SSPC_P53_FRONTEND_THREAD", "SSPC_CP_ASYNC_WRITES", "SSPC_CP_ASYNC_FLIP",
              "SSPC_CP_RING", "SSPC_NATIVE_GBUFFER", "SSPC_VK_EMIT_THREAD", "SSPC_VK_EMIT_BATCH", "SSPC_R11_EARLY_SUBMIT",
              "SSPC_R11_ASYNC_SUBMIT", "SSPC_ASYNC_PIPELINES", "SSPC_R9_PCACHE", "SSPC_R9_HEAP_WINDOW", "SSPC_VRAM_ADAPT",
              "SSPC_PIPE_PREFETCH", "SSPC_R12_LIGHT_LIST_KEEP", "SSPC_FB_LAZY_WB", "SSPC_REA_HEAP_READS",
              "SSPC_REA_DEFER_WAIT_B", "SSPC_FB_ASYNC_COMPUTE", "SSPC_R10_AQ_MAIN_FREE", "SSPC_R11_AQ_IMAGES",
              "SSPC_R11_AQ_ORDER_EPOCH", "SSPC_R11_FAST_LABEL", "SSPC_R11_LABEL_WAIT", "SSPC_R10_SCHED", "SSPC_R10_SPREAD",
              "SSPC_R10_RUN_PS", "SSPC_R10_RUN_PS_MATERIAL", "SSPC_R10_NATIVE_DRAWS", "SSPC_R12_PSORT", "SSPC_R10_AJM",
              "SSPC_R10_SYSRT", "SSPC_R8_CP_PROFILE_CSV", "SSPC_R12_WEATHER", "SSPC_R12_REFL_STATS", "SSPC_R12_REFL_DIAG",
              "SSPC_R12_REFL_HOOK", "SSPC_R12_RAIN_STATS", "SSPC_R12_RAIN_DIST", "SSPC_R12_RAIN_CYCLE",
              "SSPC_R12_RAIN_DIAG", "SSPC_R12_RAIN_HOOK", "SSPC_R12_LIGHT_CENSUS", "SSPC_R12_WRITER_DETAIL",
              "SSPC_R12_READERS_OF", "SSPC_R12_LAZY_TRACE"}) {
            k.insert(QStringLiteral("Experiments.") + QString::fromLatin1(e));
        }
        return k;
    }();
    return keys;
}

bool SameValue(const QVariant& a, const QVariant& b) {
    if (!a.isValid() || !b.isValid()) {
        return a.isValid() == b.isValid();
    }
    if (a.typeId() == QMetaType::Bool || b.typeId() == QMetaType::Bool) {
        return a.toBool() == b.toBool() && a.typeId() == b.typeId();
    }
    if (a.typeId() == QMetaType::QString || b.typeId() == QMetaType::QString) {
        return a.toString() == b.toString();
    }
    return a.toLongLong() == b.toLongLong();
}

bool IsInt(const QVariant& v) {
    return v.typeId() == QMetaType::LongLong || v.typeId() == QMetaType::Int || v.typeId() == QMetaType::Double;
}

bool ParseSize(const QString& s, int* w, int* h) {
    static const QRegularExpression re(QStringLiteral("^(\\d{3,5})x(\\d{3,5})$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) {
        return false;
    }
    *w = m.captured(1).toInt();
    *h = m.captured(2).toInt();
    return true;
}

QString SizeLabel(const QString& s) {
    int w = 0, h = 0;
    return ParseSize(s, &w, &h) ? QStringLiteral("%1 × %2").arg(w).arg(h) : s;
}

} // namespace

SettingsManager::SettingsManager(QObject* parent) : QObject(parent) {}

QString SettingsManager::LocalAppData() {
    const QString v = qEnvironmentVariable("LOCALAPPDATA");
    return v.isEmpty() ? QDir::homePath() + QStringLiteral("/AppData/Local") : QDir::fromNativeSeparators(v);
}

QString SettingsManager::UserFileFor(const QString& productId) {
    return LocalAppData() + u'/' + productId + QStringLiteral("/settings.toml");
}

QString SettingsManager::launcherStateFile() const {
    return profile_ == nullptr ? QString() : LocalAppData() + u'/' + profile_->productId + QStringLiteral("/launcher.toml");
}

void SettingsManager::setGame(const GameProfile* profile, const QString& productToml, const QString& exeDir,
                              const QList<GpuInfo>& gpus, const QList<DisplayInfo>& displays) {
    profile_ = profile;
    productFile_ = productToml;
    exeDir_ = exeDir;
    gpus_ = gpus;
    displays_ = displays;
    userFile_ = profile ? UserFileFor(profile->productId) : QString();
    // what the computer and the game's folder offer (as the built-in launcher's Caps)
    sysCaps_ = 0;
    const QDir dir(exeDir_);
    const auto has = [&](const char* dll) { return !exeDir_.isEmpty() && QFileInfo(dir.filePath(QLatin1String(dll))).isFile(); };
    if (System::HasNvidiaRtx(gpus_) && has("nvngx_dlss.dll")) sysCaps_ |= SysCaps::Dlss;
    if (System::HasNvidiaRtx40Plus(gpus_) && has("nvngx_dlssg.dll")) sysCaps_ |= SysCaps::DlssFrameGen;
    if (System::HasNvidia(gpus_)) sysCaps_ |= SysCaps::Reflex;
    if (has("amd_fidelityfx_vk.dll")) sysCaps_ |= SysCaps::Fsr;
    draft_.clear();
    loadFiles();
    buildSections();
    emit structureChanged();
    emit launchProfileChanged();
    ++revision_;
    emit valuesChanged();
}

void SettingsManager::setRussian(bool ru) {
    if (ru_ == ru) return;
    ru_ = ru;
    buildSections();
    emit structureChanged();
    ++revision_;
    emit valuesChanged();
}

void SettingsManager::setLocked(bool locked, const QString& reason) {
    locked_ = locked;
    lockReason_ = reason;
    emit editableChanged();
    if (!locked) {
        reload(); // the game may have written the file (its own settings screen)
    }
}

void SettingsManager::loadFiles() {
    defaults_ = TomlDocument();
    user_ = TomlDocument();
    userFileOk_ = true;
    fileProblem_.clear();
    if (!productFile_.isEmpty()) {
        QFile f(productFile_);
        if (f.open(QIODevice::ReadOnly)) {
            defaults_.parse(f.readAll());
        }
    }
    if (!userFile_.isEmpty() && QFileInfo::exists(userFile_)) {
        QFile f(userFile_);
        QString error;
        if (!f.open(QIODevice::ReadOnly)) {
            userFileOk_ = false;
            error = f.errorString();
        } else if (!user_.parse(f.readAll(), &error)) {
            userFileOk_ = false;
        }
        if (!userFileOk_) {
            fileProblem_ = ru_ ? QStringLiteral("Файл настроек %1 не читается (%2). Лаунчер его не перезапишет; игра при "
                                                "запуске отложит его как .corrupt и начнёт с настроек по умолчанию.")
                                     .arg(QDir::toNativeSeparators(userFile_), error)
                               : QStringLiteral("The settings file %1 cannot be read (%2). The launcher will not overwrite "
                                                "it; at its start the game sets it aside as .corrupt and uses the defaults.")
                                     .arg(QDir::toNativeSeparators(userFile_), error);
            user_ = TomlDocument();
        }
    }
    // the launch profile (launcher.toml of the built-in launcher: profile = "standard" | "experimental")
    launchProfile_ = QStringLiteral("standard");
    QFile lf(launcherStateFile());
    if (lf.open(QIODevice::ReadOnly)) {
        TomlDocument st;
        if (st.parse(lf.readAll()) && st.value({}, QStringLiteral("profile")).toString() == QLatin1String("experimental")) {
            launchProfile_ = QStringLiteral("experimental");
        }
    }
}

void SettingsManager::reload() {
    loadFiles();
    buildSections();
    emit structureChanged();
    emit launchProfileChanged();
    ++revision_;
    emit valuesChanged();
}

QList<QPair<QString, QString>> SettingsManager::launchEnvironment() const {
    QList<QPair<QString, QString>> env;
    if (profile_ == nullptr) return env;
    for (const auto& e : profile_->standardEnv) env.append({e.name, e.value});
    if (launchProfile_ == QLatin1String("experimental")) {
        for (const auto& e : profile_->experimentalEnv) env.append({e.name, e.value});
    }
    return env;
}

void SettingsManager::setLaunchProfile(const QString& profile) {
    const QString p = profile == QLatin1String("experimental") ? profile : QStringLiteral("standard");
    if (p == launchProfile_ || locked_ || profile_ == nullptr) return;
    // launcher.toml is the built-in launcher's: only `profile` changes, `closeAfterLaunch` and the rest stay
    TomlDocument st;
    QFile f(launcherStateFile());
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        if (!st.parse(f.readAll())) st = TomlDocument();
        f.close();
    }
    if (!f.exists()) {
        st.parse(QStringLiteral("# %1 launcher: the launch profile and what the launcher does after a start\n")
                     .arg(profile_->productId).toUtf8());
    }
    st.set({}, QStringLiteral("profile"), p);
    QString error;
    if (!Files::WriteAtomic(launcherStateFile(), st.toUtf8(), &error)) {
        lastError_ = error;
        emit saved(false, error);
        return;
    }
    launchProfile_ = p;
    emit launchProfileChanged();
    buildSections(); // experiment defaults follow the profile
    emit structureChanged();
    ++revision_;
    emit valuesChanged();
}

QVariant SettingsManager::fileDefault(const SettingDef& d) const {
    if (d.experiment) {
        QString env;
        bool found = false;
        for (const auto& [name, value] : launchEnvironment()) {
            if (name == d.key) {
                env = value;
                found = true;
            }
        }
        if (d.type == SettingType::Bool) return found && env == d.on;
        return found ? env : d.unsetShows;
    }
    if (defaults_.contains(d.table, d.key)) {
        const QVariant v = defaults_.value(d.table, d.key);
        if (d.type == SettingType::Bool && v.typeId() == QMetaType::Bool) return v;
        if (d.type == SettingType::Int && IsInt(v)) return v.toLongLong();
        if ((d.type == SettingType::Choice || d.type == SettingType::Text) && v.typeId() == QMetaType::QString) return v;
    }
    return d.builtinDefault;
}

QVariant SettingsManager::defaultValue(const QString& id) const {
    if (id == kWindowMode) {
        return defaultValue(QStringLiteral("display.fullscreen")).toBool()
                   ? (defaultValue(QStringLiteral("display.fullscreenMode")).toString() == QLatin1String("exclusive")
                          ? QStringLiteral("exclusive") : QStringLiteral("borderless"))
                   : QStringLiteral("windowed");
    }
    if (id == kWindowSize) {
        return QStringLiteral("%1x%2").arg(defaultValue(QStringLiteral("display.width")).toLongLong())
            .arg(defaultValue(QStringLiteral("display.height")).toLongLong());
    }
    const SettingDef* d = FindSetting(id);
    return d ? fileDefault(*d) : QVariant();
}

QVariant SettingsManager::rawSaved(const SettingDef& d, QString* issue) const {
    if (!user_.contains(d.table, d.key)) {
        return fileDefault(d);
    }
    const QVariant v = user_.value(d.table, d.key);
    auto bad = [&](const QString& ru, const QString& en) {
        if (issue) *issue = ru_ ? ru : en;
        return fileDefault(d);
    };
    switch (d.type) {
    case SettingType::Bool:
        if (v.typeId() != QMetaType::Bool) return bad(QStringLiteral("в файле не true/false"), QStringLiteral("not true/false in the file"));
        return v;
    case SettingType::Int: {
        if (!IsInt(v)) return bad(QStringLiteral("в файле не число"), QStringLiteral("not a number in the file"));
        const qint64 n = v.toLongLong();
        if (n < d.min || n > d.max) {
            return bad(QStringLiteral("%1 вне диапазона %2…%3").arg(n).arg(d.min).arg(d.max),
                       QStringLiteral("%1 is outside %2..%3").arg(n).arg(d.min).arg(d.max));
        }
        return n;
    }
    case SettingType::Choice: {
        if (v.typeId() != QMetaType::QString) return bad(QStringLiteral("в файле не строка"), QStringLiteral("not a string in the file"));
        const QString s = v.toString();
        for (const auto& c : d.choices) {
            if (c.value == s) return s;
        }
        bool ok = false;
        const int n = s.toInt(&ok);
        int w = 0, h = 0;
        if ((d.acceptsFrameRate && ok && n >= 20 && n <= 1000) || (d.acceptsResolution && ParseSize(s, &w, &h))) {
            return s;
        }
        return bad(QStringLiteral("значение «%1» рантайм не принимает").arg(s), QStringLiteral("the runtime does not take \"%1\"").arg(s));
    }
    case SettingType::Text:
        if (v.typeId() != QMetaType::QString) return bad(QStringLiteral("в файле не строка"), QStringLiteral("not a string in the file"));
        return v;
    }
    return v;
}

QVariant SettingsManager::savedValue(const QString& id) const {
    if (id == kWindowMode) {
        return savedValue(QStringLiteral("display.fullscreen")).toBool()
                   ? (savedValue(QStringLiteral("display.fullscreenMode")).toString() == QLatin1String("exclusive")
                          ? QStringLiteral("exclusive") : QStringLiteral("borderless"))
                   : QStringLiteral("windowed");
    }
    if (id == kWindowSize) {
        return QStringLiteral("%1x%2").arg(savedValue(QStringLiteral("display.width")).toLongLong())
            .arg(savedValue(QStringLiteral("display.height")).toLongLong());
    }
    const SettingDef* d = FindSetting(id);
    return d ? rawSaved(*d, nullptr) : QVariant();
}

QVariant SettingsManager::current(const QString& id) const {
    if (id == kWindowMode) {
        const bool fs = current(QStringLiteral("display.fullscreen")).toBool();
        return fs ? (current(QStringLiteral("display.fullscreenMode")).toString() == QLatin1String("exclusive")
                         ? QStringLiteral("exclusive") : QStringLiteral("borderless"))
                  : QStringLiteral("windowed");
    }
    if (id == kWindowSize) {
        return QStringLiteral("%1x%2").arg(current(QStringLiteral("display.width")).toLongLong())
            .arg(current(QStringLiteral("display.height")).toLongLong());
    }
    if (id == kLaunchProfile) return launchProfile_;
    const auto it = draft_.constFind(id);
    if (it != draft_.constEnd()) return *it;
    return savedValue(id);
}

QVariant SettingsManager::value(const QString& id) const {
    return current(id);
}

void SettingsManager::setRaw(const QString& id, const QVariant& v) {
    if (SameValue(v, savedValue(id))) {
        draft_.remove(id);
    } else {
        draft_.insert(id, v);
    }
}

void SettingsManager::setValue(const QString& id, const QVariant& valueIn) {
    if (locked_ || profile_ == nullptr) return;
    if (id == kLaunchProfile) {
        setLaunchProfile(valueIn.toString());
        return;
    }
    if (id == kWindowMode) {
        const QString m = valueIn.toString();
        if (m == QLatin1String("windowed")) {
            setRaw(QStringLiteral("display.fullscreen"), false);
        } else if (m == QLatin1String("borderless")) {
            setRaw(QStringLiteral("display.fullscreen"), true);
            setRaw(QStringLiteral("display.fullscreenMode"), QStringLiteral("borderless"));
        }
    } else if (id == kWindowSize) {
        int w = 0, h = 0;
        if (ParseSize(valueIn.toString(), &w, &h)) {
            setRaw(QStringLiteral("display.width"), qint64(std::clamp(w, 640, 7680)));
            setRaw(QStringLiteral("display.height"), qint64(std::clamp(h, 360, 4320)));
        }
    } else {
        const SettingDef* d = FindSetting(id);
        if (d == nullptr) return;
        QVariant v;
        switch (d->type) {
        case SettingType::Bool: v = valueIn.toBool(); break;
        case SettingType::Int: {
            qint64 n = qRound64(valueIn.toDouble());
            n = std::clamp<qint64>(n, qint64(d->min), qint64(d->max));
            v = n;
            break;
        }
        case SettingType::Choice:
        case SettingType::Text: v = valueIn.toString(); break;
        }
        setRaw(id, v);
    }
    ++revision_;
    emit valuesChanged();
}

QString SettingsManager::availability(const SettingDef& d) const {
    if (profile_ == nullptr) return {};
    if (d.needsGame != 0 && !profile_->has(d.needsGame)) {
        if ((d.needsGame & GameCaps::EngineHooks) && !profile_->has(GameCaps::EngineHooks)) {
            return ru_ ? profile_->hooksReasonRu : profile_->hooksReasonEn;
        }
        if ((d.needsGame & GameCaps::RenderSwitch) && !profile_->has(GameCaps::RenderSwitch)) {
            return ru_ ? profile_->renderSwitchReasonRu : profile_->renderSwitchReasonEn;
        }
        return ru_ ? QStringLiteral("Для этой игры рантайм этого не поддерживает.") : QStringLiteral("The runtime does not support it for this game.");
    }
    if (d.needsSys != 0 && (sysCaps_ & d.needsSys) != d.needsSys) {
        if (d.needsSys & SysCaps::DlssFrameGen) {
            return ru_ ? QStringLiteral("Нужна GeForce RTX 40/50 и nvngx_dlssg.dll рядом с exe игры.")
                       : QStringLiteral("Needs a GeForce RTX 40/50 and nvngx_dlssg.dll next to the game's exe.");
        }
        if (d.needsSys & SysCaps::Reflex) {
            return ru_ ? QStringLiteral("Нужна видеокарта NVIDIA.") : QStringLiteral("Needs an NVIDIA GPU.");
        }
        return ru_ ? QStringLiteral("Не поддерживается этим компьютером.") : QStringLiteral("Not supported by this computer.");
    }
    return {};
}

QString SettingsManager::choiceReason(const ChoiceDef& c) const {
    if (profile_ == nullptr) return {};
    if (c.needsGame != 0 && !profile_->has(c.needsGame)) {
        if (c.needsGame & GameCaps::FreeFrameRate) return ru_ ? profile_->frameRateReasonRu : profile_->frameRateReasonEn;
        return ru_ ? profile_->hooksReasonRu : profile_->hooksReasonEn;
    }
    if (c.needsSys != 0 && (sysCaps_ & c.needsSys) != c.needsSys) {
        if (c.needsSys & SysCaps::Dlss) {
            return ru_ ? QStringLiteral("Нужна GeForce RTX и nvngx_dlss.dll рядом с exe игры.")
                       : QStringLiteral("Needs a GeForce RTX and nvngx_dlss.dll next to the game's exe.");
        }
        if (c.needsSys & SysCaps::DlssFrameGen) {
            return ru_ ? QStringLiteral("Нужна GeForce RTX 40/50 и nvngx_dlssg.dll рядом с exe игры.")
                       : QStringLiteral("Needs a GeForce RTX 40/50 and nvngx_dlssg.dll next to the game's exe.");
        }
        if (c.needsSys & SysCaps::Fsr) {
            return ru_ ? QStringLiteral("Нет amd_fidelityfx_vk.dll рядом с exe игры.")
                       : QStringLiteral("amd_fidelityfx_vk.dll is not next to the game's exe.");
        }
    }
    return {};
}

bool SettingsManager::isActive(const QString& id) const {
    return unavailableReason(id).isEmpty() && inactiveReason(id).isEmpty();
}

QString SettingsManager::unavailableReason(const QString& id) const {
    const SettingDef* d = FindSetting(id);
    return d ? availability(*d) : QString();
}

QString SettingsManager::inactiveReason(const QString& id) const {
    if (id == kWindowSize) {
        return current(kWindowMode).toString() == QLatin1String("windowed")
                   ? QString()
                   : (ru_ ? QStringLiteral("Размер окна действует в оконном режиме.") : QStringLiteral("The window size applies when windowed."));
    }
    const SettingDef* d = FindSetting(id);
    if (d == nullptr) return {};
    const auto requirement = [&](const QString& spec, bool mustEqual) -> QString {
        if (spec.isEmpty()) return {};
        const qsizetype eq = spec.indexOf(u'=');
        const QString other = eq < 0 ? spec : spec.left(eq);
        const SettingDef* od = FindSetting(other);
        const QString name = od ? label(*od) : other;
        if (eq < 0) {
            return current(other).toBool() ? QString()
                                           : (ru_ ? QStringLiteral("Действует, когда включено «%1».").arg(name)
                                                  : QStringLiteral("Applies while \"%1\" is on.").arg(name));
        }
        const QString want = spec.mid(eq + 1);
        const bool equal = current(other).toString() == want;
        if (mustEqual && !equal) {
            QString wantLabel = want;
            if (od) for (const auto& c : od->choices) if (c.value == want) wantLabel = ru_ ? c.ru : c.en;
            return ru_ ? QStringLiteral("Действует, когда «%1» — %2.").arg(name, wantLabel)
                       : QStringLiteral("Applies while \"%1\" is %2.").arg(name, wantLabel);
        }
        if (!mustEqual && equal) {
            return ru_ ? QStringLiteral("Не действует при текущем значении «%1».").arg(name)
                       : QStringLiteral("Does not apply with the current \"%1\".").arg(name);
        }
        return {};
    };
    QString r = requirement(d->requiresOn, true);
    if (r.isEmpty()) r = requirement(d->requiresNot, false);
    return r;
}

bool SettingsManager::isModified(const QString& id) const {
    if (id == kLaunchProfile) return launchProfile_ != QLatin1String("standard");
    return !SameValue(current(id), defaultValue(id));
}

bool SettingsManager::isPending(const QString& id) const {
    if (id == kWindowMode) return draft_.contains(QStringLiteral("display.fullscreen")) || draft_.contains(QStringLiteral("display.fullscreenMode"));
    if (id == kWindowSize) return draft_.contains(QStringLiteral("display.width")) || draft_.contains(QStringLiteral("display.height"));
    return draft_.contains(id);
}

QString SettingsManager::fileIssue(const QString& id) const {
    QStringList ids{id};
    if (id == kWindowMode) ids = {QStringLiteral("display.fullscreen"), QStringLiteral("display.fullscreenMode")};
    if (id == kWindowSize) ids = {QStringLiteral("display.width"), QStringLiteral("display.height")};
    for (const auto& one : ids) {
        const SettingDef* d = FindSetting(one);
        QString issue;
        if (d) rawSaved(*d, &issue);
        if (!issue.isEmpty()) return issue;
    }
    return {};
}

QString SettingsManager::valueText(const QString& id) const {
    const QVariant v = current(id);
    const SettingDef* d = FindSetting(id);
    if (d && d->type == SettingType::Choice) {
        for (const auto& c : d->choices) if (c.value == v.toString()) return ru_ ? c.ru : c.en;
    }
    if (v.typeId() == QMetaType::Bool) return v.toBool() ? (ru_ ? QStringLiteral("Вкл.") : QStringLiteral("On")) : (ru_ ? QStringLiteral("Выкл.") : QStringLiteral("Off"));
    return v.toString();
}

QVariantMap SettingsManager::rowFor(const SettingDef& d) const {
    QVariantMap row;
    row[QStringLiteral("id")] = d.id;
    row[QStringLiteral("label")] = label(d);
    row[QStringLiteral("help")] = ru_ ? d.helpRu : d.helpEn;
    row[QStringLiteral("warn")] = ru_ ? d.warnRu : d.warnEn;
    row[QStringLiteral("unit")] = d.unit == QLatin1String("MiB") ? (ru_ ? QStringLiteral("МиБ") : d.unit) : d.unit;
    row[QStringLiteral("min")] = d.min;
    row[QStringLiteral("max")] = d.max;
    row[QStringLiteral("step")] = d.step;
    row[QStringLiteral("experiment")] = d.experiment;
    row[QStringLiteral("envName")] = d.experiment ? d.key : QString();
    row[QStringLiteral("fileKey")] = QStringLiteral("[%1] %2").arg(d.table, d.key);
    QString type;
    switch (d.type) {
    case SettingType::Bool: type = QStringLiteral("switch"); break;
    case SettingType::Int: type = QStringLiteral("slider"); break;
    case SettingType::Choice:
    case SettingType::Text: type = QStringLiteral("choice"); break;
    }
    row[QStringLiteral("type")] = type;
    const QString reason = availability(d);
    row[QStringLiteral("available")] = reason.isEmpty();
    row[QStringLiteral("reason")] = reason;
    const bool unverified = profile_ && profile_->unverifiedSettings.contains(d.id);
    row[QStringLiteral("unverified")] = unverified;
    row[QStringLiteral("unverifiedReason")] = unverified ? (ru_ ? profile_->unverifiedReasonRu : profile_->unverifiedReasonEn) : QString();

    QVariantList choices;
    QSet<QString> seen;
    const auto add = [&](const QString& value, const QString& text, const QString& why) {
        if (seen.contains(value)) return;
        seen.insert(value);
        choices << QVariantMap{{QStringLiteral("value"), value}, {QStringLiteral("label"), text},
                               {QStringLiteral("enabled"), why.isEmpty()}, {QStringLiteral("reason"), why}};
    };
    if (d.id == QLatin1String("display.monitor")) {
        QString primary;
        for (const auto& m : displays_) if (m.primary) primary = m.name;
        add(QString(), ru_ ? QStringLiteral("Основной монитор%1").arg(primary.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(primary))
                           : QStringLiteral("Primary display%1").arg(primary.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(primary)),
            {});
        for (const auto& m : displays_) {
            add(m.name, QStringLiteral("%1 — %2 × %3, %4 %5").arg(m.name).arg(m.width).arg(m.height).arg(m.refreshHz)
                            .arg(ru_ ? QStringLiteral("Гц") : QStringLiteral("Hz")), {});
        }
        const QString saved = savedValue(d.id).toString();
        if (!saved.isEmpty()) {
            add(saved, ru_ ? QStringLiteral("%1 (не подключён — откроется на основном)").arg(saved)
                           : QStringLiteral("%1 (not connected: the primary display is used)").arg(saved), {});
        }
    } else {
        for (const auto& c : d.choices) add(c.value, ru_ ? c.ru : c.en, choiceReason(c));
        if (d.type == SettingType::Choice) {
            const QString saved = savedValue(d.id).toString();
            if (!saved.isEmpty() && !seen.contains(saved)) {
                add(saved, ru_ ? QStringLiteral("%1 (из файла)").arg(d.acceptsResolution ? SizeLabel(saved) : saved)
                               : QStringLiteral("%1 (from the file)").arg(d.acceptsResolution ? SizeLabel(saved) : saved), {});
            }
        }
    }
    row[QStringLiteral("choices")] = choices;
    return row;
}

void SettingsManager::buildSections() {
    sections_.clear();
    if (profile_ == nullptr) return;
    struct SectionInfo { SettingSection s; const char* id; QString ru, en; const char* icon; };
    const QList<SectionInfo> infos = {
        {SettingSection::Display, "display", QStringLiteral("Экран"), QStringLiteral("Display"), "monitor"},
        {SettingSection::Rendering, "rendering", QStringLiteral("Рендеринг"), QStringLiteral("Rendering"), "layers"},
        {SettingSection::Effects, "effects", QStringLiteral("Эффекты"), QStringLiteral("Effects"), "sparkles"},
        {SettingSection::Game, "game", QStringLiteral("Игра"), QStringLiteral("Game"), "gamepad"},
        {SettingSection::Experiments, "experiments", QStringLiteral("Экспериментальные"), QStringLiteral("Experimental"), "flask"},
    };
    for (const auto& info : infos) {
        QVariantList groups;
        QString groupTitle;
        QVariantList rows;
        int available = 0, total = 0;
        const auto flush = [&] {
            if (!rows.isEmpty()) {
                groups << QVariantMap{{QStringLiteral("title"), groupTitle}, {QStringLiteral("rows"), rows}};
            }
            rows.clear();
        };
        if (info.s == SettingSection::Experiments) {
            groupTitle = ru_ ? QStringLiteral("Профиль запуска") : QStringLiteral("Launch profile");
            QVariantMap row;
            row[QStringLiteral("id")] = kLaunchProfile;
            row[QStringLiteral("type")] = QStringLiteral("choice");
            row[QStringLiteral("label")] = ru_ ? QStringLiteral("Профиль переменных запуска") : QStringLiteral("Launch variables profile");
            row[QStringLiteral("help")] = ru_ ? QStringLiteral("Те же профили, что во встроенном лаунчере игры (launcher.toml): «Стандартный» = play.cmd, "
                                                              "«Экспериментальный» = play_experimental.cmd (линия async compute).")
                                              : QStringLiteral("The same profiles as the game's built-in launcher (launcher.toml): Standard = play.cmd, "
                                                               "Experimental = play_experimental.cmd (the async compute line).");
            row[QStringLiteral("warn")] = ru_ ? QStringLiteral("Экспериментальный профиль включает линию AQ, которая в прошлом приводила к потере устройства.")
                                              : QStringLiteral("The experimental profile turns on the AQ line, which has caused device loss before.");
            row[QStringLiteral("available")] = true;
            row[QStringLiteral("fileKey")] = QStringLiteral("launcher.toml: profile");
            row[QStringLiteral("choices")] = QVariantList{
                QVariantMap{{QStringLiteral("value"), QStringLiteral("standard")}, {QStringLiteral("label"), ru_ ? QStringLiteral("Стандартный (рекомендуется)") : QStringLiteral("Standard (recommended)")}, {QStringLiteral("enabled"), true}},
                QVariantMap{{QStringLiteral("value"), QStringLiteral("experimental")}, {QStringLiteral("label"), ru_ ? QStringLiteral("Экспериментальный") : QStringLiteral("Experimental")}, {QStringLiteral("enabled"), true}}};
            rows << row;
            flush();
        }
        for (const auto& d : Catalog()) {
            if (d.section != info.s || HiddenRow(d.id)) continue;
            const QString g = ru_ ? d.groupRu : d.groupEn;
            if (!g.isEmpty() && g != groupTitle) {
                flush();
                groupTitle = g;
            }
            QVariantMap row = rowFor(d);
            ++total;
            if (row.value(QStringLiteral("available")).toBool()) ++available;
            rows << row;
            if (d.id == QLatin1String("display.monitor")) {
                // the window mode and size: combined rows over fullscreen/fullscreenMode and width/height
                QVariantMap mode;
                mode[QStringLiteral("id")] = kWindowMode;
                mode[QStringLiteral("type")] = QStringLiteral("choice");
                mode[QStringLiteral("label")] = ru_ ? QStringLiteral("Режим окна") : QStringLiteral("Window mode");
                mode[QStringLiteral("help")] = ru_ ? FindSetting(QStringLiteral("display.fullscreen"))->helpRu : FindSetting(QStringLiteral("display.fullscreen"))->helpEn;
                mode[QStringLiteral("available")] = true;
                mode[QStringLiteral("fileKey")] = QStringLiteral("[Display] fullscreen, fullscreenMode");
                QVariantList mc{
                    QVariantMap{{QStringLiteral("value"), QStringLiteral("windowed")}, {QStringLiteral("label"), ru_ ? QStringLiteral("В окне") : QStringLiteral("Windowed")}, {QStringLiteral("enabled"), true}},
                    QVariantMap{{QStringLiteral("value"), QStringLiteral("borderless")}, {QStringLiteral("label"), ru_ ? QStringLiteral("Полноэкранный без рамки") : QStringLiteral("Borderless full screen")}, {QStringLiteral("enabled"), true}}};
                if (savedValue(kWindowMode).toString() == QLatin1String("exclusive")) {
                    mc << QVariantMap{{QStringLiteral("value"), QStringLiteral("exclusive")}, {QStringLiteral("label"), ru_ ? QStringLiteral("Эксклюзивный (из файла)") : QStringLiteral("Exclusive (from the file)")}, {QStringLiteral("enabled"), true}};
                }
                mode[QStringLiteral("choices")] = mc;
                rows << mode;
                ++total; ++available;

                QVariantMap size;
                size[QStringLiteral("id")] = kWindowSize;
                size[QStringLiteral("type")] = QStringLiteral("choice");
                size[QStringLiteral("label")] = ru_ ? QStringLiteral("Размер окна") : QStringLiteral("Window size");
                size[QStringLiteral("help")] = ru_ ? QStringLiteral("Размер окна игры в оконном режиме (игра запоминает и размер, изменённый мышью).")
                                                   : QStringLiteral("The game window's size when windowed (the game also keeps a size set with the mouse).");
                size[QStringLiteral("available")] = true;
                size[QStringLiteral("fileKey")] = QStringLiteral("[Display] width, height");
                int maxW = 0, maxH = 0;
                for (const auto& m : displays_) { maxW = std::max(maxW, m.width); maxH = std::max(maxH, m.height); }
                QVariantList sc;
                QSet<QString> seen;
                for (const char* s : {"1280x720", "1366x768", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160"}) {
                    int w = 0, h = 0;
                    ParseSize(QString::fromLatin1(s), &w, &h);
                    if (maxW > 0 && (w > maxW || h > maxH)) continue;
                    seen.insert(QString::fromLatin1(s));
                    sc << QVariantMap{{QStringLiteral("value"), QString::fromLatin1(s)}, {QStringLiteral("label"), SizeLabel(QString::fromLatin1(s))}, {QStringLiteral("enabled"), true}};
                }
                const QString saved = savedValue(kWindowSize).toString();
                if (!seen.contains(saved)) {
                    sc << QVariantMap{{QStringLiteral("value"), saved}, {QStringLiteral("label"), SizeLabel(saved) + (ru_ ? QStringLiteral(" (из файла)") : QStringLiteral(" (from the file)"))}, {QStringLiteral("enabled"), true}};
                }
                size[QStringLiteral("choices")] = sc;
                rows << size;
                ++total; ++available;
            }
        }
        flush();
        if (info.s == SettingSection::Experiments) {
            total += 1; available += 1;
        }
        sections_ << QVariantMap{{QStringLiteral("id"), QString::fromLatin1(info.id)},
                                 {QStringLiteral("title"), ru_ ? info.ru : info.en},
                                 {QStringLiteral("icon"), QString::fromLatin1(info.icon)},
                                 {QStringLiteral("groups"), groups},
                                 {QStringLiteral("available"), available},
                                 {QStringLiteral("total"), total}};
    }
}

bool SettingsManager::save() {
    lastError_.clear();
    if (profile_ == nullptr) return false;
    if (locked_) {
        lastError_ = lockReason_;
        emit saved(false, lastError_);
        return false;
    }
    if (draft_.isEmpty()) {
        emit saved(true, {});
        return true;
    }
    // the file as it is now (the game's own settings screen may have written it since it was read)
    TomlDocument doc;
    QFile f(userFile_);
    if (f.exists()) {
        QString error;
        if (!f.open(QIODevice::ReadOnly) || !doc.parse(f.readAll(), &error)) {
            lastError_ = ru_ ? QStringLiteral("Файл %1 повреждён (%2) — изменения не записаны, файл не тронут.")
                                   .arg(QDir::toNativeSeparators(userFile_), error.isEmpty() ? f.errorString() : error)
                             : QStringLiteral("%1 is damaged (%2): nothing was written, the file is untouched.")
                                   .arg(QDir::toNativeSeparators(userFile_), error.isEmpty() ? f.errorString() : error);
            emit saved(false, lastError_);
            return false;
        }
        f.close();
    } else {
        doc.parse(QStringLiteral("# %1 user settings, written by the PC settings (the game's Settings screen) and by inFAMOUS PC "
                                 "Launcher.\n# They override %2 (the defaults); delete a line to use its value again.\n")
                      .arg(profile_->productId, profile_->configFileName).toUtf8());
    }
    for (auto it = draft_.constBegin(); it != draft_.constEnd(); ++it) {
        const SettingDef* d = FindSetting(it.key());
        if (d == nullptr) continue;
        if (SameValue(*it, fileDefault(*d))) {
            doc.remove(d->table, d->key); // the default: the line goes, as the runtime's "delete a line" rule
        } else {
            doc.set(d->table, d->key, *it);
        }
    }
    // the result must read back as what was meant
    TomlDocument check;
    QString error;
    const QByteArray text = doc.toUtf8();
    if (!check.parse(text, &error)) {
        lastError_ = QStringLiteral("internal: the written text does not parse (%1); nothing was written").arg(error);
        emit saved(false, lastError_);
        return false;
    }
    if (!Files::WriteAtomic(userFile_, text, &error)) {
        lastError_ = (ru_ ? QStringLiteral("Не удалось записать %1: %2") : QStringLiteral("Could not write %1: %2"))
                         .arg(QDir::toNativeSeparators(userFile_), error);
        emit saved(false, lastError_);
        return false;
    }
    draft_.clear();
    reload();
    emit saved(true, QDir::toNativeSeparators(userFile_));
    return true;
}

void SettingsManager::revert() {
    draft_.clear();
    ++revision_;
    emit valuesChanged();
}

void SettingsManager::resetSection(const QString& sectionId) {
    if (locked_) return;
    for (const auto& s : sections_) {
        const QVariantMap sm = s.toMap();
        if (sm.value(QStringLiteral("id")).toString() != sectionId) continue;
        for (const auto& g : sm.value(QStringLiteral("groups")).toList()) {
            for (const auto& r : g.toMap().value(QStringLiteral("rows")).toList()) {
                const QString id = r.toMap().value(QStringLiteral("id")).toString();
                if (id == kLaunchProfile) continue;
                if (id == kWindowMode) {
                    setRaw(QStringLiteral("display.fullscreen"), defaultValue(QStringLiteral("display.fullscreen")));
                    setRaw(QStringLiteral("display.fullscreenMode"), defaultValue(QStringLiteral("display.fullscreenMode")));
                } else if (id == kWindowSize) {
                    setRaw(QStringLiteral("display.width"), defaultValue(QStringLiteral("display.width")));
                    setRaw(QStringLiteral("display.height"), defaultValue(QStringLiteral("display.height")));
                } else if (r.toMap().value(QStringLiteral("available")).toBool()) {
                    setRaw(id, defaultValue(id));
                }
            }
        }
    }
    ++revision_;
    emit valuesChanged();
}

QString SettingsManager::checkUserFile() const {
    QStringList out;
    if (!userFileOk_) return fileProblem_;
    if (!QFileInfo::exists(userFile_)) {
        return ru_ ? QStringLiteral("Файла пользовательских настроек нет — игра использует %1.").arg(profile_ ? profile_->configFileName : QString())
                   : QStringLiteral("No user settings file: the game uses %1.").arg(profile_ ? profile_->configFileName : QString());
    }
    static const QSet<QString> tables = {QStringLiteral("Display"), QStringLiteral("Graphics"), QStringLiteral("Audio"),
                                         QStringLiteral("Input"), QStringLiteral("Gamepad"), QStringLiteral("Interface"),
                                         QStringLiteral("ShaderCache"), QStringLiteral("Language"), QStringLiteral("System"),
                                         QStringLiteral("Keyboard"), QStringLiteral("Experiments")};
    for (const auto& e : user_.entries()) {
        const QString full = e.table + u'.' + e.key;
        if (e.table == QLatin1String("Keyboard")) continue;
        if (!tables.contains(e.table)) {
            out << (ru_ ? QStringLiteral("[%1] %2 — таблица, которую рантайм не читает (строка сохранена)")
                        : QStringLiteral("[%1] %2: a table the runtime does not read (the line is kept)")).arg(e.table, e.key);
            continue;
        }
        if (!KnownKeys().contains(full)) {
            out << (ru_ ? QStringLiteral("[%1] %2 — неизвестный ключ (рантайм его пропустит, строка сохранена)")
                        : QStringLiteral("[%1] %2: an unknown key (the runtime skips it, the line is kept)")).arg(e.table, e.key);
            continue;
        }
        for (const auto& d : Catalog()) {
            if (d.table == e.table && d.key == e.key) {
                QString issue;
                rawSaved(d, &issue);
                if (!issue.isEmpty()) out << QStringLiteral("[%1] %2 — %3").arg(e.table, e.key, issue);
            }
        }
    }
    return out.join(u'\n');
}

} // namespace Launcher
