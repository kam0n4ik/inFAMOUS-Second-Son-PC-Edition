// inFAMOUS PC Launcher — the TOML of the runtime's files (SecondSonPC.toml / FirstLightPC.toml, the user's
// %LOCALAPPDATA%\<product>\settings.toml): tables of `key = value` with strings, numbers, booleans and arrays.
// Kept as its lines: an edit changes only the lines of the keys it sets or removes, so comments, the order and keys
// the launcher does not know stay exactly as they were (settings\user_store.cpp keeps unknown keys the same way).
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace Launcher {

class TomlDocument {
public:
    struct Entry {
        QString table;
        QString key;
        QVariant value;   // bool | qint64 | double | QString | QVariantList (arrays)
        QString raw;      // the value as written
        int firstLine = 0;
        int lastLine = 0; // (a multi-line array or string)
        int valueColumn = 0;
    };

    /// false + `error` ("line N: ...") if the text is not TOML this reader takes; the document is then empty.
    bool parse(const QByteArray& utf8, QString* error = nullptr);
    QByteArray toUtf8() const;

    bool contains(const QString& table, const QString& key) const;
    QVariant value(const QString& table, const QString& key, const QVariant& fallback = {}) const;
    const QList<Entry>& entries() const { return entries_; }
    QStringList tables() const;

    /// Sets the key: its line is rewritten (a trailing comment kept), or a line is added at the end of its table (the
    /// table is added at the end of the document if it is not there).
    void set(const QString& table, const QString& key, const QVariant& value);
    /// Removes the key's line(s); false if it was not there.
    bool remove(const QString& table, const QString& key);

    /// The TOML text of a value (true, 12, 1.0, "text", ["a", "b"]).
    static QString format(const QVariant& value);
    static QString quote(const QString& s);
    static bool isBareKey(const QString& key);

private:
    int findEntry(const QString& table, const QString& key) const;
    void reindex();

    QStringList lines_;
    QList<Entry> entries_;
    QList<QPair<QString, int>> tableHeaders_; // name -> line index
    QString eol_ = QStringLiteral("\n");
    bool bom_ = false;
};

} // namespace Launcher
