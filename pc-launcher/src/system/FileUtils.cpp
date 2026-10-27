#include "system/FileUtils.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

namespace Launcher::Files {

namespace {

std::wstring W(const QString& s) {
    return QDir::toNativeSeparators(s).toStdWString();
}

QString Win32Error(const char* what) {
    const DWORD code = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    QString text = buf != nullptr ? QString::fromWCharArray(buf).trimmed() : QString();
    LocalFree(buf);
    return QStringLiteral("%1: %2 (%3)").arg(QLatin1String(what), text).arg(code);
}

} // namespace

bool IsReparsePoint(const QString& path) {
    const DWORD attr = GetFileAttributesW(W(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

DirStats Measure(const QString& dir) {
    DirStats s;
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        return s;
    }
    // (QDirIterator does not follow links unless asked: FollowSymlinks is not given)
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (fi.isSymLink() || fi.isJunction()) {
            continue;
        }
        s.bytes += fi.size();
        ++s.files;
        s.newestMs = std::max(s.newestMs, fi.lastModified().toMSecsSinceEpoch());
    }
    return s;
}

bool WriteAtomic(const QString& file, const QByteArray& text, QString* error) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    const std::wstring path = W(file), tmp = path + L".tmp", bak = path + L".bak";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = Win32Error("creating the temporary file");
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, text.constData(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == static_cast<DWORD>(text.size()) && FlushFileBuffers(h);
    if (!ok && error) {
        *error = Win32Error("writing the temporary file");
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (ReplaceFileW(path.c_str(), tmp.c_str(), bak.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            return true;
        }
        CopyFileW(path.c_str(), bak.c_str(), FALSE); // ReplaceFile can refuse while another program holds the file
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = Win32Error("replacing the file");
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool CopyTree(const QString& from, const QString& to, QString* error, qint64* bytes) {
    if (!QFileInfo(from).isDir()) {
        if (error) *error = QStringLiteral("not a folder: %1").arg(QDir::toNativeSeparators(from));
        return false;
    }
    if (!QDir().mkpath(to)) {
        if (error) *error = QStringLiteral("cannot create %1").arg(QDir::toNativeSeparators(to));
        return false;
    }
    const QDir src(from);
    QDirIterator it(from, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (fi.isSymLink() || fi.isJunction()) {
            continue;
        }
        const QString target = QDir(to).filePath(src.relativeFilePath(fi.absoluteFilePath()));
        if (fi.isDir()) {
            if (!QDir().mkpath(target)) {
                if (error) *error = QStringLiteral("cannot create %1").arg(QDir::toNativeSeparators(target));
                return false;
            }
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        if (!CopyFileW(W(fi.absoluteFilePath()).c_str(), W(target).c_str(), FALSE)) {
            if (error) *error = Win32Error("copying a file") + u' ' + QDir::toNativeSeparators(fi.absoluteFilePath());
            return false;
        }
        if (bytes) *bytes += fi.size();
    }
    return true;
}

bool IsInside(const QString& root, const QString& path) {
    const QString r = QDir::cleanPath(QFileInfo(root).absoluteFilePath()).toLower();
    const QString p = QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
    return p == r || p.startsWith(r.endsWith(u'/') ? r : r + u'/');
}

bool RemoveTreeInside(const QString& root, const QString& dir, QString* error) {
    const QString r = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    const QString d = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    if (r.isEmpty() || d.compare(r, Qt::CaseInsensitive) == 0 || !IsInside(r, d)) {
        if (error) *error = QStringLiteral("refused: %1 is not inside %2").arg(QDir::toNativeSeparators(d), QDir::toNativeSeparators(r));
        return false;
    }
    if (!QFileInfo::exists(d)) {
        return true;
    }
    if (IsReparsePoint(d)) {
        // a link: only the link goes
        if (!RemoveDirectoryW(W(d).c_str()) && !DeleteFileW(W(d).c_str())) {
            if (error) *error = Win32Error("removing a link");
            return false;
        }
        return true;
    }
    QDir qd(d);
    const auto entries = qd.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : entries) {
        const QString p = fi.absoluteFilePath();
        if (fi.isDir() && !IsReparsePoint(p)) {
            if (!RemoveTreeInside(r, p, error)) {
                return false;
            }
        } else if (fi.isDir()) {
            if (!RemoveDirectoryW(W(p).c_str())) {
                if (error) *error = Win32Error("removing a link") + u' ' + QDir::toNativeSeparators(p);
                return false;
            }
        } else {
            SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(W(p).c_str())) {
                if (error) *error = Win32Error("deleting a file") + u' ' + QDir::toNativeSeparators(p);
                return false;
            }
        }
    }
    if (!RemoveDirectoryW(W(d).c_str())) {
        if (error) *error = Win32Error("removing a folder") + u' ' + QDir::toNativeSeparators(d);
        return false;
    }
    return true;
}

bool Reveal(const QString& path) {
    const QFileInfo fi(path);
    if (!fi.exists()) {
        return false;
    }
    if (fi.isDir()) {
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", W(fi.absoluteFilePath()).c_str(), nullptr,
                                                        nullptr, SW_SHOWNORMAL)) > 32;
    }
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(W(fi.absoluteFilePath()).c_str());
    if (pidl == nullptr) {
        return false;
    }
    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    return SUCCEEDED(hr);
}

QString Tail(const QString& file, qint64 maxBytes, int maxLines) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const qint64 size = f.size();
    const qint64 from = std::max<qint64>(0, size - maxBytes);
    f.seek(from);
    QByteArray data = f.read(size - from);
    if (from > 0) {
        const qsizetype nl = data.indexOf('\n');
        if (nl >= 0) {
            data.remove(0, nl + 1);
        }
    }
    QStringDecoder dec(QStringDecoder::Utf8);
    QString text = dec.decode(data);
    QStringList lines = text.split(u'\n');
    if (lines.size() > maxLines) {
        lines = lines.mid(lines.size() - maxLines);
    }
    for (auto& l : lines) {
        if (l.endsWith(u'\r')) l.chop(1);
    }
    return lines.join(u'\n');
}

QString HumanSize(qint64 bytes, bool ru) {
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1024.0) {
        return QStringLiteral("%1 %2").arg(QString::number(mb / 1024.0, 'f', 2).replace(u'.', ru ? u',' : u'.'),
                                           ru ? QStringLiteral("ГБ") : QStringLiteral("GB"));
    }
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 %2").arg(QString::number(mb, 'f', mb < 10 ? 1 : 0).replace(u'.', ru ? u',' : u'.'),
                                           ru ? QStringLiteral("МБ") : QStringLiteral("MB"));
    }
    if (bytes >= 1024) {
        return QStringLiteral("%1 %2").arg(bytes / 1024).arg(ru ? QStringLiteral("КБ") : QStringLiteral("KB"));
    }
    return QStringLiteral("%1 %2").arg(bytes).arg(ru ? QStringLiteral("Б") : QStringLiteral("B"));
}

} // namespace Launcher::Files
