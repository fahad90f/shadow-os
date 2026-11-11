// SPDX-License-Identifier: GPL-3.0-or-later
#include "trayutil.h"

#include <QChar>
#include <QList>
#include <QStringList>
#include <QStringView>

namespace {

bool isAsciiLetter(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
}

bool isAsciiDigit(QChar c)
{
    return c.unicode() >= u'0' && c.unicode() <= u'9';
}

// Characters that must not reach a label: control characters, stray
// surrogates, and the explicit bidirectional embeddings/overrides/isolates
// that could make a text read differently from what it says.
bool isUnwanted(char32_t c)
{
    if ((c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF)
        return true;
    switch (QChar::category(c)) {
    case QChar::Other_Control:
    case QChar::Other_Surrogate:
    case QChar::Other_NotAssigned:
        return true;
    default:
        return false;
    }
}

// The character an entity ("amp", "#39", "#x27") stands for, or 0.
char32_t entityValue(QStringView name)
{
    if (name == u"amp") return U'&';
    if (name == u"lt") return U'<';
    if (name == u"gt") return U'>';
    if (name == u"quot") return U'"';
    if (name == u"apos") return U'\'';
    if (name == u"nbsp") return U' ';
    if (name.size() >= 2 && name.front() == u'#') {
        bool ok = false;
        const bool hex = name.at(1) == u'x' || name.at(1) == u'X';
        const uint value = hex ? name.mid(2).toUInt(&ok, 16) : name.mid(1).toUInt(&ok, 10);
        if (ok && value > 0 && value <= 0x10FFFF)
            return char32_t(value);
    }
    return 0;
}

// Whether the text between '<' and '>' is a tag, and if so whether it ends
// a line. "a < b and c > d" is not markup.
bool isTag(QStringView inner, bool *breaksLine)
{
    if (inner.startsWith(u'/'))
        inner = inner.mid(1);
    if (inner.isEmpty() || !isAsciiLetter(inner.front()))
        return false;
    qsizetype end = 0;
    while (end < inner.size() && (isAsciiLetter(inner.at(end)) || isAsciiDigit(inner.at(end))))
        ++end;
    const QString name = inner.left(end).toString().toLower();
    static const QStringList lineTags{QStringLiteral("br"), QStringLiteral("p"), QStringLiteral("div"),
                                      QStringLiteral("li"), QStringLiteral("tr"), QStringLiteral("h1"),
                                      QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4")};
    *breaksLine = lineTags.contains(name);
    return true;
}

} // namespace

bool TrayUtil::isValidBusName(const QString &name)
{
    if (name.isEmpty() || name.size() > TrayLimits::MaxNameLength)
        return false;
    const bool unique = name.startsWith(u':');
    const QList<QStringView> elements = QStringView(name).mid(unique ? 1 : 0).split(u'.');
    if (elements.size() < 2)
        return false;
    for (const QStringView element : elements) {
        if (element.isEmpty())
            return false;
        // Only the elements of unique names (":1.42") may start with a digit.
        if (!unique && isAsciiDigit(element.front()))
            return false;
        for (const QChar c : element) {
            if (!isAsciiLetter(c) && !isAsciiDigit(c) && c != u'_' && c != u'-')
                return false;
        }
    }
    return true;
}

bool TrayUtil::isValidObjectPath(const QString &path)
{
    if (path.size() > TrayLimits::MaxNameLength || !path.startsWith(u'/'))
        return false;
    if (path.size() == 1)
        return true;
    if (path.endsWith(u'/'))
        return false;
    for (qsizetype i = 1; i < path.size(); ++i) {
        const QChar c = path.at(i);
        if (c == u'/') {
            if (path.at(i - 1) == u'/')
                return false;
        } else if (!isAsciiLetter(c) && !isAsciiDigit(c) && c != u'_') {
            return false;
        }
    }
    return true;
}

QString TrayUtil::cleanText(const QString &text, int maxLength)
{
    QString out;
    out.reserve(qMin<qsizetype>(text.size(), maxLength + 1));
    int written = 0;            // characters, not UTF-16 units
    bool pendingSpace = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        char32_t c = text.at(i).unicode();
        qsizetype units = 1;
        if (QChar::isHighSurrogate(c) && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            c = QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
            units = 2;
        }
        const qsizetype start = i;
        i += units - 1;
        if (QChar::isSpace(c)) {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (isUnwanted(c))
            continue;
        if (written + (pendingSpace ? 1 : 0) >= maxLength) {
            out += QChar(0x2026);
            break;
        }
        if (pendingSpace) {
            out += u' ';
            ++written;
            pendingSpace = false;
        }
        out += QStringView(text).mid(start, units);
        ++written;
    }
    return out;
}

QString TrayUtil::plainText(const QString &markup, int maxLength, int maxLines)
{
    // Tags disappear (those that end a line become a line break) and
    // entities become the characters they stand for.
    QString text;
    text.reserve(markup.size());
    for (qsizetype i = 0; i < markup.size(); ++i) {
        const QChar c = markup.at(i);
        if (c == u'<') {
            const qsizetype end = markup.indexOf(u'>', i + 1);
            bool breaksLine = false;
            if (end > i && end - i <= 96 && isTag(QStringView(markup).mid(i + 1, end - i - 1), &breaksLine)) {
                if (breaksLine)
                    text += u'\n';
                i = end;
                continue;
            }
        } else if (c == u'&') {
            const qsizetype end = markup.indexOf(u';', i + 1);
            if (end > i && end - i <= 10) {
                const char32_t value = entityValue(QStringView(markup).mid(i + 1, end - i - 1));
                if (value) {
                    text += QString::fromUcs4(&value, 1);
                    i = end;
                    continue;
                }
            }
        }
        text += c;
    }

    QStringList lines;
    int budget = maxLength;
    const QList<QStringView> rawLines = QStringView(text).split(u'\n');
    for (const QStringView raw : rawLines) {
        if (lines.size() >= maxLines || budget <= 0)
            break;
        const QString line = cleanText(raw.toString(), budget);
        if (line.isEmpty())
            continue;
        budget -= int(line.size());
        lines << line;
    }
    return lines.join(u'\n');
}

QString TrayUtil::menuLabel(const QString &label)
{
    QString out;
    out.reserve(label.size());
    for (qsizetype i = 0; i < label.size(); ++i) {
        const QChar c = label.at(i);
        if (c != u'_') {
            out += c;
        } else if (i + 1 < label.size() && label.at(i + 1) == u'_') {
            out += u'_';        // "__": a literal underscore
            ++i;
        }
        // A single "_" only marks the access key; the key itself follows as
        // ordinary text.
    }
    return cleanText(out, TrayLimits::MaxLabelLength);
}
