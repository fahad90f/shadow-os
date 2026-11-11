// SPDX-License-Identifier: GPL-3.0-or-later
#include "notificationtext.h"

#include <QDir>
#include <QUrl>

namespace NotificationText {

namespace {

// Characters that are removed wherever they appear: explicit bidirectional
// embeddings, overrides and isolates (they can make text read differently
// from what was sent), the byte-order mark and the interlinear-annotation /
// object-replacement block.
bool isDropped(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069) || u == 0xFEFF
           || (u >= 0xFFF9 && u <= 0xFFFC);
}

bool isControl(QChar c)
{
    return c.category() == QChar::Other_Control;
}

bool isAsciiLetter(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
}

bool isAsciiDigit(QChar c)
{
    const char16_t u = c.unicode();
    return u >= u'0' && u <= u'9';
}

// Decodes the character reference starting at in[at] == '&'. Returns the
// index after it, or -1 if it is not one of the references understood here
// (the ampersand is then ordinary text).
qsizetype decodeEntity(QStringView in, qsizetype at, QString *decoded)
{
    const qsizetype limit = qMin(in.size(), at + 12);
    qsizetype end = -1;
    for (qsizetype i = at + 1; i < limit; ++i) {
        const QChar c = in.at(i);
        if (c == u';') {
            end = i;
            break;
        }
        if (!isAsciiLetter(c) && !isAsciiDigit(c) && c != u'#')
            return -1;
    }
    if (end < 0 || end == at + 1)
        return -1;
    const QStringView name = in.mid(at + 1, end - at - 1);

    if (name.startsWith(u'#')) {
        bool ok = false;
        uint code = 0;
        if (name.size() > 1 && (name.at(1) == u'x' || name.at(1) == u'X'))
            code = name.mid(2).toUInt(&ok, 16);
        else
            code = name.mid(1).toUInt(&ok, 10);
        if (!ok || code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return -1;
        const char32_t character = code;
        *decoded = QString::fromUcs4(&character, 1);
        return end + 1;
    }

    struct Named { QLatin1StringView name; char16_t value; };
    static const Named named[] = {
        {QLatin1StringView("amp"), u'&'}, {QLatin1StringView("lt"), u'<'}, {QLatin1StringView("gt"), u'>'},
        {QLatin1StringView("quot"), u'"'}, {QLatin1StringView("apos"), u'\''}, {QLatin1StringView("nbsp"), u' '},
    };
    for (const Named &entity : named) {
        if (name == entity.name) {
            *decoded = QString(QChar(entity.value));
            return end + 1;
        }
    }
    return -1;
}

QString decodeEntities(QStringView in)
{
    QString out;
    out.reserve(in.size());
    qsizetype i = 0;
    while (i < in.size()) {
        if (in.at(i) == u'&') {
            QString decoded;
            const qsizetype next = decodeEntity(in, i, &decoded);
            if (next > 0) {
                out += decoded;
                i = next;
                continue;
            }
        }
        out += in.at(i++);
    }
    return out;
}

// The value of attribute 'wanted' in the attribute part of a tag
// (name="value", name='value' or name=value), entities decoded.
QString attributeValue(QStringView attributes, QLatin1StringView wanted)
{
    const qsizetype n = attributes.size();
    qsizetype p = 0;
    while (p < n) {
        while (p < n && (attributes.at(p).isSpace() || attributes.at(p) == u'/'))
            ++p;
        const qsizetype nameStart = p;
        while (p < n && !attributes.at(p).isSpace() && attributes.at(p) != u'=' && attributes.at(p) != u'/')
            ++p;
        const QStringView name = attributes.mid(nameStart, p - nameStart);
        while (p < n && attributes.at(p).isSpace())
            ++p;
        QStringView value;
        if (p < n && attributes.at(p) == u'=') {
            ++p;
            while (p < n && attributes.at(p).isSpace())
                ++p;
            if (p < n && (attributes.at(p) == u'"' || attributes.at(p) == u'\'')) {
                const QChar quote = attributes.at(p++);
                const qsizetype valueStart = p;
                while (p < n && attributes.at(p) != quote)
                    ++p;
                value = attributes.mid(valueStart, p - valueStart);
                if (p < n)
                    ++p;
            } else {
                const qsizetype valueStart = p;
                while (p < n && !attributes.at(p).isSpace())
                    ++p;
                value = attributes.mid(valueStart, p - valueStart);
            }
        }
        if (name.isEmpty() && value.isEmpty()) {
            ++p;            // stray character; never loop in place
            continue;
        }
        if (name.compare(wanted, Qt::CaseInsensitive) == 0)
            return decodeEntities(value);
    }
    return {};
}

// Builds the sanitised body. Text arrives character by character; tags are
// opened lazily in front of the next visible character, so the output never
// contains an empty or dangling element.
class BodyWriter
{
public:
    void character(QChar c)
    {
        if (m_truncated)
            return;
        if (m_visible >= MaxBodyLength) {
            m_truncated = true;
            return;
        }
        // Elements that end here are closed before the pending line break
        // or space, so that those are not part of a link or of bold text.
        if (!tagsCurrent())
            closeTags();
        flushPending();
        syncTags();
        switch (c.unicode()) {
        case u'&': m_styled += QLatin1StringView("&amp;"); break;
        case u'<': m_styled += QLatin1StringView("&lt;"); break;
        case u'>': m_styled += QLatin1StringView("&gt;"); break;
        case u'"': m_styled += QLatin1StringView("&quot;"); break;
        default: m_styled += c;
        }
        m_plain += c;
        ++m_visible;
    }

    void text(QStringView s)
    {
        for (const QChar c : s) {
            if (c == u'\n')
                lineBreak();
            else if (c.isSpace() || isControl(c))
                space();
            else if (!isDropped(c))
                character(c);
        }
    }

    void space()
    {
        if (m_visible > 0 && m_breaks == 0)
            m_space = true;
    }

    // An explicit line break (a new line in the text, <br>). At most one
    // empty line is kept between paragraphs.
    void lineBreak()
    {
        m_space = false;
        if (m_visible > 0 && m_breaks < 2)
            ++m_breaks;
    }

    // The start or end of a block element: the text continues on a new line.
    void blockBoundary()
    {
        m_space = false;
        if (m_visible > 0 && m_breaks < 1)
            m_breaks = 1;
    }

    void bold(bool open) { m_bold = qMax(0, m_bold + (open ? 1 : -1)); }
    void italic(bool open) { m_italic = qMax(0, m_italic + (open ? 1 : -1)); }
    void underline(bool open) { m_underline = qMax(0, m_underline + (open ? 1 : -1)); }

    void openLink(const QString &href)
    {
        m_link = -1;
        const QString target = linkTarget(href);
        if (target.isEmpty())
            return;
        int index = int(m_links.indexOf(target));
        if (index < 0) {
            if (m_links.size() >= MaxLinks)
                return;
            index = int(m_links.size());
            m_links.append(target);
        }
        m_link = index;
    }
    void closeLink() { m_link = -1; }

    bool full() const { return m_truncated; }
    void markTruncated() { m_truncated = true; }

    Body finish()
    {
        if (m_truncated && m_visible > 0) {
            // Not in the middle of a surrogate pair.
            if (m_plain.back().isHighSurrogate()) {
                m_plain.chop(1);
                m_styled.chop(1);
            }
            syncTags();
            m_styled += QChar(0x2026);
            m_plain += QChar(0x2026);
        }
        closeTags();
        Body body;
        body.styled = m_styled;
        body.plain = m_plain;
        body.links = m_links;
        body.truncated = m_truncated;
        return body;
    }

private:
    void flushPending()
    {
        for (; m_breaks > 0; --m_breaks) {
            m_styled += QLatin1StringView("<br>");
            m_plain += u'\n';
            ++m_visible;
        }
        if (m_space) {
            m_styled += u' ';
            m_plain += u' ';
            ++m_visible;
            m_space = false;
        }
    }

    void closeTags()
    {
        if (m_openUnderline) m_styled += QLatin1StringView("</u>");
        if (m_openItalic) m_styled += QLatin1StringView("</i>");
        if (m_openBold) m_styled += QLatin1StringView("</b>");
        if (m_openLink >= 0) m_styled += QLatin1StringView("</a>");
        m_openUnderline = m_openItalic = m_openBold = false;
        m_openLink = -1;
    }

    bool tagsCurrent() const
    {
        return m_openLink == m_link && m_openBold == (m_bold > 0) && m_openItalic == (m_italic > 0)
            && m_openUnderline == (m_underline > 0);
    }

    void syncTags()
    {
        const bool wantBold = m_bold > 0, wantItalic = m_italic > 0, wantUnderline = m_underline > 0;
        if (tagsCurrent())
            return;
        closeTags();
        if (m_link >= 0) {
            m_styled += QLatin1StringView("<a href=\"") + QString::number(m_link) + QLatin1StringView("\">");
            m_openLink = m_link;
        }
        if (wantBold) { m_styled += QLatin1StringView("<b>"); m_openBold = true; }
        if (wantItalic) { m_styled += QLatin1StringView("<i>"); m_openItalic = true; }
        if (wantUnderline) { m_styled += QLatin1StringView("<u>"); m_openUnderline = true; }
    }

    QString m_styled, m_plain;
    QStringList m_links;
    int m_visible = 0;
    int m_breaks = 0;
    bool m_space = false;
    bool m_truncated = false;
    int m_bold = 0, m_italic = 0, m_underline = 0, m_link = -1;
    bool m_openBold = false, m_openItalic = false, m_openUnderline = false;
    int m_openLink = -1;
};

bool isBlockElement(QStringView name)
{
    static const QLatin1StringView blocks[] = {
        QLatin1StringView("p"), QLatin1StringView("div"), QLatin1StringView("li"), QLatin1StringView("ul"),
        QLatin1StringView("ol"), QLatin1StringView("tr"), QLatin1StringView("table"), QLatin1StringView("hr"),
        QLatin1StringView("pre"), QLatin1StringView("blockquote"), QLatin1StringView("h1"), QLatin1StringView("h2"),
        QLatin1StringView("h3"), QLatin1StringView("h4"), QLatin1StringView("h5"), QLatin1StringView("h6"),
    };
    for (const QLatin1StringView block : blocks) {
        if (name == block)
            return true;
    }
    return false;
}

// Elements whose content is not text meant for the reader.
bool isHiddenElement(QStringView name)
{
    return name == QLatin1StringView("script") || name == QLatin1StringView("style")
           || name == QLatin1StringView("head") || name == QLatin1StringView("title");
}

// Handles the tag starting at in[at] == '<'. Returns the index after it, or
// -1 if this is not a tag (the '<' is then ordinary text).
qsizetype parseTag(QStringView in, qsizetype at, BodyWriter &out)
{
    const qsizetype n = in.size();
    qsizetype p = at + 1;
    if (p >= n)
        return -1;

    // Comments, declarations and processing instructions carry no text.
    if (in.at(p) == u'!' || in.at(p) == u'?') {
        if (in.mid(p).startsWith(QLatin1StringView("!--"))) {
            const qsizetype end = in.indexOf(QLatin1StringView("-->"), p + 3);
            return end < 0 ? n : end + 3;
        }
        const qsizetype end = in.indexOf(u'>', p);
        return end < 0 ? -1 : end + 1;
    }

    bool closing = false;
    if (in.at(p) == u'/') {
        closing = true;
        ++p;
    }
    const qsizetype nameStart = p;
    while (p < n && p - nameStart < 16 && (isAsciiLetter(in.at(p)) || isAsciiDigit(in.at(p))))
        ++p;
    if (p == nameStart || p >= n || !isAsciiLetter(in.at(nameStart)))
        return -1;
    if (!in.at(p).isSpace() && in.at(p) != u'/' && in.at(p) != u'>')
        return -1;
    const QString name = in.mid(nameStart, p - nameStart).toString().toLower();

    // The attributes run to the closing '>'; a quoted value may contain one.
    const qsizetype attributesStart = p;
    const qsizetype limit = qMin(n, p + 4096);
    QChar quote;
    while (p < limit) {
        const QChar c = in.at(p);
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
        } else if (c == u'"' || c == u'\'') {
            quote = c;
        } else if (c == u'>') {
            break;
        }
        ++p;
    }
    if (p >= limit)
        return -1;
    const QStringView attributes = in.mid(attributesStart, p - attributesStart);
    qsizetype next = p + 1;

    if (name == QLatin1StringView("b") || name == QLatin1StringView("strong")) {
        out.bold(!closing);
    } else if (name == QLatin1StringView("i") || name == QLatin1StringView("em")) {
        out.italic(!closing);
    } else if (name == QLatin1StringView("u")) {
        out.underline(!closing);
    } else if (name == QLatin1StringView("a")) {
        if (closing)
            out.closeLink();
        else
            out.openLink(attributeValue(attributes, QLatin1StringView("href")));
    } else if (name == QLatin1StringView("br")) {
        out.lineBreak();
    } else if (name == QLatin1StringView("img")) {
        // Images in the body are not supported; the alternative text is.
        if (!closing)
            out.text(attributeValue(attributes, QLatin1StringView("alt")));
    } else if (isBlockElement(name)) {
        out.blockBoundary();
    } else if (isHiddenElement(name) && !closing) {
        const QString endTag = QLatin1StringView("</") + name;
        const qsizetype end = in.indexOf(endTag, next, Qt::CaseInsensitive);
        if (end < 0)
            return n;
        const qsizetype close = in.indexOf(u'>', end);
        next = close < 0 ? n : close + 1;
    }
    // Every other element is dropped; its text content is kept.
    return next;
}

} // namespace

QString singleLine(const QString &text, int maxLength)
{
    QString out;
    if (maxLength <= 0)
        return out;
    // Look at a bounded prefix only: megabytes of white space must not cost
    // megabytes of work.
    const qsizetype scanLimit = qMin(text.size(), qsizetype(maxLength) * 8 + 256);
    out.reserve(qMin(scanLimit, qsizetype(maxLength) + 1));
    bool pendingSpace = false;
    bool truncated = scanLimit < text.size();
    for (qsizetype i = 0; i < scanLimit; ++i) {
        const QChar c = text.at(i);
        if (isDropped(c))
            continue;
        if (c.isSpace() || isControl(c)) {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (out.size() + (pendingSpace ? 1 : 0) >= maxLength) {
            truncated = true;
            break;
        }
        if (pendingSpace) {
            out += u' ';
            pendingSpace = false;
        }
        out += c;
    }
    if (truncated && !out.isEmpty()) {
        if (out.back().isHighSurrogate())
            out.chop(1);
        out += QChar(0x2026);
    }
    return out;
}

Body sanitizeBody(const QString &raw)
{
    const qsizetype n = qMin(raw.size(), qsizetype(MaxBodyInput));
    const QStringView in = QStringView(raw).left(n);
    BodyWriter out;

    qsizetype i = 0;
    while (i < n && !out.full()) {
        const QChar c = in.at(i);
        if (c == u'<') {
            const qsizetype next = parseTag(in, i, out);
            if (next < 0) {
                out.character(c);
                ++i;
            } else {
                i = next;
            }
        } else if (c == u'&') {
            QString decoded;
            const qsizetype next = decodeEntity(in, i, &decoded);
            if (next < 0) {
                out.character(c);
                ++i;
            } else {
                out.text(decoded);
                i = next;
            }
        } else if (c == u'\n') {
            out.lineBreak();
            ++i;
        } else if (c.isSpace() || isControl(c)) {
            out.space();
            ++i;
        } else if (isDropped(c)) {
            ++i;
        } else {
            out.character(c);
            ++i;
        }
    }
    if (n < raw.size())
        out.markTruncated();
    return out.finish();
}

QString linkTarget(const QString &href)
{
    const QString trimmed = href.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > MaxLinkLength)
        return {};
    for (const QChar c : trimmed) {
        if (c.isSpace() || isControl(c) || isDropped(c))
            return {};
    }
    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1StringView("http") || scheme == QLatin1StringView("https")) {
        // "https://bank.example@evil.example/" shows one host and opens another.
        if (url.host().isEmpty() || !url.userInfo().isEmpty())
            return {};
    } else if (scheme == QLatin1StringView("mailto")) {
        if (url.path().isEmpty())
            return {};
    } else {
        return {};
    }
    QString encoded = QString::fromLatin1(url.toEncoded());
    // Characters with a meaning inside an attribute value.
    encoded.replace(u'\'', QLatin1StringView("%27"));
    encoded.replace(u'"', QLatin1StringView("%22"));
    encoded.replace(u'<', QLatin1StringView("%3C"));
    encoded.replace(u'>', QLatin1StringView("%3E"));
    return encoded;
}

QString iconName(const QString &value)
{
    if (value.isEmpty() || value.size() > 128 || value.startsWith(u'.'))
        return {};
    for (const QChar c : value) {
        if (!isAsciiLetter(c) && !isAsciiDigit(c) && c != u'-' && c != u'_' && c != u'.' && c != u'+')
            return {};
    }
    return value;
}

bool looksLikeUrl(const QString &value)
{
    // scheme ":" — RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
    const qsizetype colon = value.indexOf(u':');
    if (colon < 1 || colon > 32)
        return false;
    if (!isAsciiLetter(value.at(0)))
        return false;
    for (qsizetype i = 1; i < colon; ++i) {
        const QChar c = value.at(i);
        if (!isAsciiLetter(c) && !isAsciiDigit(c) && c != u'+' && c != u'-' && c != u'.')
            return false;
    }
    return true;
}

QString localFilePath(const QString &value)
{
    if (value.isEmpty() || value.size() > 4096 || value.contains(QChar(u'\0')))
        return {};
    if (value.startsWith(u'/'))
        return QDir::cleanPath(value);
    if (value.startsWith(QLatin1StringView("file:"), Qt::CaseInsensitive)) {
        const QUrl url(value);
        if (!url.isValid() || url.scheme().toLower() != QLatin1StringView("file"))
            return {};
        // "file://server/share/…" would reach out to another machine.
        if (!url.host().isEmpty() && url.host().toLower() != QLatin1StringView("localhost"))
            return {};
        const QString path = url.path(QUrl::FullyDecoded);
        if (!path.startsWith(u'/') || path.contains(QChar(u'\0')))
            return {};
        return QDir::cleanPath(path);
    }
    return {};
}

} // namespace NotificationText
