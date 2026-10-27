#include "settings/TomlDocument.h"

#include <QRegularExpression>
#include <QStringDecoder>

#include <cmath>

namespace Launcher {

namespace {

struct ParseError {
    int line;
    QString what;
};

/// A reader over the document's lines that can continue a value on the next line (arrays, multi-line strings).
class Reader {
public:
    explicit Reader(const QStringList& lines) : lines_(lines) {}

    int line = 0;
    int pos = 0;

    bool atEnd() const { return line >= lines_.size(); }
    const QString& cur() const { return lines_[line]; }
    bool eol() const { return atEnd() || pos >= cur().size(); }
    QChar peek(int ahead = 0) const {
        if (atEnd() || pos + ahead >= cur().size()) {
            return QChar();
        }
        return cur()[pos + ahead];
    }
    bool startsWith(const QString& s) const { return !atEnd() && cur().mid(pos, s.size()) == s; }
    void skipSpaces() {
        while (!eol() && (peek() == u' ' || peek() == u'\t')) {
            ++pos;
        }
    }
    void nextLine() {
        ++line;
        pos = 0;
    }
    /// Spaces, comments and line breaks (inside an array).
    void skipBlank() {
        for (;;) {
            skipSpaces();
            if (atEnd()) {
                return;
            }
            if (eol() || peek() == u'#') {
                nextLine();
                continue;
            }
            return;
        }
    }
    [[noreturn]] void fail(const QString& what) const { throw ParseError{line + 1, what}; }

private:
    const QStringList& lines_;
};

bool IsBareChar(QChar c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'_' || c == u'-';
}

QString ReadBasicString(Reader& r) {
    // at the opening quote
    ++r.pos;
    QString out;
    for (;;) {
        if (r.eol()) {
            r.fail(QStringLiteral("unterminated string"));
        }
        const QChar c = r.peek();
        ++r.pos;
        if (c == u'"') {
            return out;
        }
        if (c != u'\\') {
            out += c;
            continue;
        }
        const QChar e = r.peek();
        ++r.pos;
        switch (e.unicode()) {
        case 'b': out += u'\b'; break;
        case 't': out += u'\t'; break;
        case 'n': out += u'\n'; break;
        case 'f': out += u'\f'; break;
        case 'r': out += u'\r'; break;
        case 'e': out += QChar(0x1B); break;
        case '"': out += u'"'; break;
        case '\\': out += u'\\'; break;
        case 'u':
        case 'U': {
            const int n = e == u'u' ? 4 : 8;
            bool ok = false;
            const uint code = r.cur().mid(r.pos, n).toUInt(&ok, 16);
            if (!ok || r.cur().mid(r.pos, n).size() != n) {
                r.fail(QStringLiteral("bad \\u escape"));
            }
            r.pos += n;
            const char32_t cp = code;
            out += QString::fromUcs4(&cp, 1);
            break;
        }
        default:
            r.fail(QStringLiteral("bad escape \\%1").arg(e));
        }
    }
}

QString ReadLiteralString(Reader& r) {
    ++r.pos;
    const int end = r.cur().indexOf(u'\'', r.pos);
    if (end < 0) {
        r.fail(QStringLiteral("unterminated literal string"));
    }
    const QString out = r.cur().mid(r.pos, end - r.pos);
    r.pos = end + 1;
    return out;
}

QString ReadMultiline(Reader& r, bool basic) {
    const QString delim = basic ? QStringLiteral("\"\"\"") : QStringLiteral("'''");
    r.pos += 3;
    if (r.eol()) {
        r.nextLine(); // a newline right after the opening delimiter is trimmed
    }
    QString out;
    for (;;) {
        if (r.atEnd()) {
            r.fail(QStringLiteral("unterminated multi-line string"));
        }
        const int end = r.cur().indexOf(delim, r.pos);
        if (end >= 0) {
            out += r.cur().mid(r.pos, end - r.pos);
            r.pos = end + 3;
            return out; // (escapes of multi-line basic strings are kept as written: the launcher never edits them)
        }
        out += r.cur().mid(r.pos);
        out += u'\n';
        r.nextLine();
    }
}

QVariant ReadValue(Reader& r);

QVariant ReadArray(Reader& r) {
    ++r.pos;
    QVariantList items;
    for (;;) {
        r.skipBlank();
        if (r.atEnd()) {
            r.fail(QStringLiteral("unterminated array"));
        }
        if (r.peek() == u']') {
            ++r.pos;
            return items;
        }
        items.push_back(ReadValue(r));
        r.skipBlank();
        if (r.atEnd()) {
            r.fail(QStringLiteral("unterminated array"));
        }
        if (r.peek() == u',') {
            ++r.pos;
            continue;
        }
        if (r.peek() == u']') {
            ++r.pos;
            return items;
        }
        r.fail(QStringLiteral("',' or ']' expected in an array"));
    }
}

QVariant ReadInlineTable(Reader& r) {
    // kept as text: the runtime's files have none; a key holding one is preserved but not interpreted
    int depth = 0;
    const int start = r.pos;
    while (!r.eol()) {
        const QChar c = r.peek();
        ++r.pos;
        if (c == u'{') {
            ++depth;
        } else if (c == u'}' && --depth == 0) {
            return r.cur().mid(start, r.pos - start);
        }
    }
    r.fail(QStringLiteral("unterminated inline table"));
}

QVariant ReadScalar(Reader& r) {
    const int start = r.pos;
    while (!r.eol()) {
        const QChar c = r.peek();
        if (c == u',' || c == u']' || c == u'}' || c == u'#' || c == u' ' || c == u'\t') {
            break;
        }
        ++r.pos;
    }
    QString tok = r.cur().mid(start, r.pos - start);
    // a date-time with a space ("1979-05-27 07:32:00Z")
    static const QRegularExpression dateRe(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
    if (dateRe.match(tok).hasMatch() && r.peek() == u' ' && r.peek(1).isDigit()) {
        ++r.pos;
        while (!r.eol() && !r.peek().isSpace() && r.peek() != u'#' && r.peek() != u',' && r.peek() != u']') {
            ++r.pos;
        }
        tok = r.cur().mid(start, r.pos - start);
    }
    if (tok.isEmpty()) {
        r.fail(QStringLiteral("a value expected"));
    }
    if (tok == u"true") {
        return true;
    }
    if (tok == u"false") {
        return false;
    }
    QString t = tok;
    t.remove(u'_');
    static const QRegularExpression intRe(QStringLiteral("^[+-]?(0|[1-9]\\d*)$"));
    if (intRe.match(t).hasMatch()) {
        bool ok = false;
        const qint64 v = t.toLongLong(&ok);
        if (ok) {
            return QVariant::fromValue<qint64>(v);
        }
    }
    if (t.startsWith(u"0x") || t.startsWith(u"0o") || t.startsWith(u"0b")) {
        bool ok = false;
        const int base = t[1] == u'x' ? 16 : t[1] == u'o' ? 8 : 2;
        const qint64 v = t.mid(2).toLongLong(&ok, base);
        if (ok) {
            return QVariant::fromValue<qint64>(v);
        }
    }
    static const QRegularExpression floatRe(
        QStringLiteral("^[+-]?(0|[1-9]\\d*)(\\.\\d+)?([eE][+-]?\\d+)?$"));
    if (floatRe.match(t).hasMatch()) {
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (ok) {
            return v;
        }
    }
    if (t == u"inf" || t == u"+inf") {
        return INFINITY;
    }
    if (t == u"-inf") {
        return -INFINITY;
    }
    if (t == u"nan" || t == u"+nan" || t == u"-nan") {
        return NAN;
    }
    static const QRegularExpression dtRe(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}|^\\d{2}:\\d{2}"));
    if (dtRe.match(tok).hasMatch()) {
        return tok; // a date or time: kept as its text
    }
    r.fail(QStringLiteral("unknown value \"%1\"").arg(tok));
}

QVariant ReadValue(Reader& r) {
    if (r.startsWith(QStringLiteral("\"\"\""))) {
        return ReadMultiline(r, true);
    }
    if (r.startsWith(QStringLiteral("'''"))) {
        return ReadMultiline(r, false);
    }
    switch (r.peek().unicode()) {
    case '"':
        return ReadBasicString(r);
    case '\'':
        return ReadLiteralString(r);
    case '[':
        return ReadArray(r);
    case '{':
        return ReadInlineTable(r);
    default:
        return ReadScalar(r);
    }
}

QString ReadKeyPart(Reader& r) {
    if (r.peek() == u'"') {
        return ReadBasicString(r);
    }
    if (r.peek() == u'\'') {
        return ReadLiteralString(r);
    }
    const int start = r.pos;
    while (!r.eol() && IsBareChar(r.peek())) {
        ++r.pos;
    }
    if (r.pos == start) {
        r.fail(QStringLiteral("a key expected"));
    }
    return r.cur().mid(start, r.pos - start);
}

QString ReadKey(Reader& r) {
    QString key = ReadKeyPart(r);
    for (;;) {
        r.skipSpaces();
        if (r.peek() != u'.') {
            return key;
        }
        ++r.pos;
        r.skipSpaces();
        key += u'.' + ReadKeyPart(r);
    }
}

void ExpectLineEnd(Reader& r) {
    r.skipSpaces();
    if (!r.eol() && r.peek() != u'#') {
        r.fail(QStringLiteral("unexpected text after the value"));
    }
}

} // namespace

bool TomlDocument::parse(const QByteArray& utf8In, QString* error) {
    QByteArray utf8 = utf8In;
    bom_ = utf8.startsWith("\xEF\xBB\xBF");
    if (bom_) {
        utf8.remove(0, 3);
    }
    QStringDecoder dec(QStringDecoder::Utf8);
    const QString text = dec.decode(utf8);
    if (dec.hasError()) {
        if (error != nullptr) {
            *error = QStringLiteral("the file is not UTF-8 text");
        }
        lines_.clear();
        entries_.clear();
        tableHeaders_.clear();
        return false;
    }
    eol_ = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n") : QStringLiteral("\n");
    QStringList lines = text.split(u'\n');
    for (auto& l : lines) {
        if (l.endsWith(u'\r')) {
            l.chop(1);
        }
    }
    if (!lines.isEmpty() && lines.back().isEmpty()) {
        lines.removeLast(); // the final line break
    }
    lines_ = lines;
    QList<Entry> entries;
    QList<QPair<QString, int>> headers;
    try {
        Reader r(lines_);
        QString table;
        while (!r.atEnd()) {
            r.skipSpaces();
            if (r.eol() || r.peek() == u'#') {
                r.nextLine();
                continue;
            }
            if (r.peek() == u'[') {
                if (r.peek(1) == u'[') {
                    r.fail(QStringLiteral("arrays of tables are not supported"));
                }
                ++r.pos;
                r.skipSpaces();
                table = ReadKey(r);
                r.skipSpaces();
                if (r.peek() != u']') {
                    r.fail(QStringLiteral("']' expected"));
                }
                ++r.pos;
                ExpectLineEnd(r);
                for (const auto& h : headers) {
                    if (h.first == table) {
                        r.fail(QStringLiteral("table [%1] defined twice").arg(table));
                    }
                }
                headers.push_back({table, r.line});
                r.nextLine();
                continue;
            }
            Entry e;
            e.table = table;
            e.firstLine = r.line;
            e.key = ReadKey(r);
            r.skipSpaces();
            if (r.peek() != u'=') {
                r.fail(QStringLiteral("'=' expected after \"%1\"").arg(e.key));
            }
            ++r.pos;
            r.skipSpaces();
            const int valueLine = r.line, valueCol = r.pos;
            e.valueColumn = valueCol;
            e.value = ReadValue(r);
            if (r.line == valueLine) {
                e.raw = r.cur().mid(valueCol, r.pos - valueCol);
            } else {
                QStringList parts{lines_[valueLine].mid(valueCol)};
                for (int i = valueLine + 1; i < r.line; ++i) {
                    parts << lines_[i];
                }
                parts << r.cur().left(r.pos);
                e.raw = parts.join(u'\n');
            }
            e.lastLine = r.line;
            ExpectLineEnd(r);
            for (const auto& other : entries) {
                if (other.table == e.table && other.key == e.key) {
                    r.fail(QStringLiteral("key \"%1\" defined twice in [%2]").arg(e.key, e.table));
                }
            }
            entries.push_back(e);
            r.nextLine();
        }
    } catch (const ParseError& pe) {
        if (error != nullptr) {
            *error = QStringLiteral("line %1: %2").arg(pe.line).arg(pe.what);
        }
        lines_.clear();
        entries_.clear();
        tableHeaders_.clear();
        return false;
    }
    entries_ = entries;
    tableHeaders_ = headers;
    return true;
}

QByteArray TomlDocument::toUtf8() const {
    QString text = lines_.join(eol_);
    if (!lines_.isEmpty()) {
        text += eol_;
    }
    QByteArray out = text.toUtf8();
    if (bom_) {
        out.prepend("\xEF\xBB\xBF");
    }
    return out;
}

int TomlDocument::findEntry(const QString& table, const QString& key) const {
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_[i].table == table && entries_[i].key == key) {
            return i;
        }
    }
    return -1;
}

bool TomlDocument::contains(const QString& table, const QString& key) const {
    return findEntry(table, key) >= 0;
}

QVariant TomlDocument::value(const QString& table, const QString& key, const QVariant& fallback) const {
    const int i = findEntry(table, key);
    return i >= 0 ? entries_[i].value : fallback;
}

QStringList TomlDocument::tables() const {
    QStringList out;
    for (const auto& h : tableHeaders_) {
        out << h.first;
    }
    return out;
}

bool TomlDocument::isBareKey(const QString& key) {
    if (key.isEmpty()) {
        return false;
    }
    for (const QChar c : key) {
        if (!IsBareChar(c)) {
            return false;
        }
    }
    return true;
}

QString TomlDocument::quote(const QString& s) {
    QString out = QStringLiteral("\"");
    for (const QChar c : s) {
        if (c == u'"' || c == u'\\') {
            out += u'\\';
            out += c;
        } else if (c.unicode() < 0x20 || c.unicode() == 0x7F) {
            out += QStringLiteral("\\u") + QString::number(c.unicode(), 16).rightJustified(4, u'0').toUpper();
        } else {
            out += c;
        }
    }
    return out + u'"';
}

QString TomlDocument::format(const QVariant& v) {
    switch (v.typeId()) {
    case QMetaType::Bool:
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        return QString::number(v.toLongLong());
    case QMetaType::Double:
    case QMetaType::Float: {
        QString s = QString::number(v.toDouble(), 'g', 15);
        if (!s.contains(u'.') && !s.contains(u'e') && !s.contains(u"inf") && !s.contains(u"nan")) {
            s += QStringLiteral(".0"); // 1.0, not 1 (an integer in TOML)
        }
        return s;
    }
    case QMetaType::QStringList: {
        QStringList parts;
        for (const auto& s : v.toStringList()) {
            parts << quote(s);
        }
        return u'[' + parts.join(QStringLiteral(", ")) + u']';
    }
    case QMetaType::QVariantList: {
        QStringList parts;
        for (const auto& item : v.toList()) {
            parts << format(item);
        }
        return u'[' + parts.join(QStringLiteral(", ")) + u']';
    }
    default:
        return quote(v.toString());
    }
}

void TomlDocument::set(const QString& table, const QString& key, const QVariant& value) {
    const QString text = format(value);
    const QString keyText = isBareKey(key) ? key : quote(key);
    const int i = findEntry(table, key);
    if (i >= 0) {
        const Entry& e = entries_[i];
        if (e.firstLine == e.lastLine) {
            QString& line = lines_[e.firstLine];
            const int start = e.valueColumn;
            const int end = start + e.raw.size();
            line = line.left(start) + text + line.mid(end);
        } else {
            QString indent;
            for (const QChar c : lines_[e.firstLine]) {
                if (c != u' ' && c != u'\t') {
                    break;
                }
                indent += c;
            }
            for (int l = e.lastLine; l > e.firstLine; --l) {
                lines_.removeAt(l);
            }
            lines_[e.firstLine] = indent + keyText + QStringLiteral(" = ") + text;
        }
        reindex();
        return;
    }
    const QString newLine = keyText + QStringLiteral(" = ") + text;
    int header = -1;
    for (const auto& h : tableHeaders_) {
        if (h.first == table) {
            header = h.second;
        }
    }
    if (table.isEmpty()) {
        // the root table: before the first header
        int at = 0;
        for (const auto& e : entries_) {
            if (e.table.isEmpty()) {
                at = e.lastLine + 1;
            }
        }
        if (at == 0 && !tableHeaders_.isEmpty()) {
            at = tableHeaders_.front().second;
            lines_.insert(at, QString());
        } else if (at == 0) {
            at = static_cast<int>(lines_.size());
            if (at > 0 && lines_.back().isEmpty()) {
                --at; // (the empty last line of a text that ends with a line break)
            }
        }
        lines_.insert(at, newLine);
        reindex();
        return;
    }
    if (header < 0) {
        if (!lines_.isEmpty() && !lines_.back().trimmed().isEmpty()) {
            lines_ << QString();
        }
        lines_ << u'[' + (isBareKey(table) ? table : quote(table)) + u']' << newLine;
        reindex();
        return;
    }
    int at = header;
    for (const auto& e : entries_) {
        if (e.table == table) {
            at = std::max(at, e.lastLine);
        }
    }
    lines_.insert(at + 1, newLine);
    reindex();
}

bool TomlDocument::remove(const QString& table, const QString& key) {
    const int i = findEntry(table, key);
    if (i < 0) {
        return false;
    }
    const Entry e = entries_[i];
    for (int l = e.lastLine; l >= e.firstLine; --l) {
        lines_.removeAt(l);
    }
    reindex();
    return true;
}

void TomlDocument::reindex() {
    const bool bom = bom_;
    const QString eol = eol_;
    QString error;
    const QByteArray text = toUtf8();
    parse(text, &error); // (always parses: only whole entries were changed)
    bom_ = bom;
    eol_ = eol;
}

} // namespace Launcher
