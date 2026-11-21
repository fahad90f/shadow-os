// SPDX-License-Identifier: GPL-3.0-or-later
#include "speechtext.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

namespace {

// Block-level kind of one Markdown line.
enum class LineKind { Unknown, Blank, Paragraph, Quote, ListItem, Heading, FenceOpen, Table, Rule };

// A line held back longer than this is cut at a clause boundary anyway, so
// a reply without punctuation still starts speaking.
constexpr int kLongSentence = 260;

QString codeOmitted() { return QCoreApplication::translate("SpeechText", "(code omitted)"); }
QString aLink() { return QCoreApplication::translate("SpeechText", "a link"); }

bool isTerminator(QChar c)
{
    return c == u'.' || c == u'!' || c == u'?' || c == QChar(0x2026); // …
}

bool isCloser(QChar c)
{
    // Characters that may follow a sentence end and still belong to it.
    return c == u'"' || c == u'\'' || c == QChar(0x201D) || c == QChar(0x2019) || c == u')'
        || c == u']' || c == u'*' || c == u'_';
}

bool hasSpeakable(const QString &s)
{
    for (const QChar c : s)
        if (c.isLetterOrNumber())
            return true;
    return false;
}

// "e.g. this", "Dr. Smith", "J. R. R. Tolkien": a full stop that does not end
// a sentence. 'dot' is the index of the '.' in 'text'.
bool isAbbreviation(QStringView text, qsizetype dot)
{
    static const QSet<QString> kAbbrev = {
        QStringLiteral("e.g"), QStringLiteral("i.e"), QStringLiteral("eg"), QStringLiteral("ie"),
        QStringLiteral("etc"), QStringLiteral("vs"), QStringLiteral("mr"), QStringLiteral("mrs"),
        QStringLiteral("ms"), QStringLiteral("dr"), QStringLiteral("st"), QStringLiteral("no"),
        QStringLiteral("approx"), QStringLiteral("fig"), QStringLiteral("cf"), QStringLiteral("jr"),
        QStringLiteral("sr"), QStringLiteral("inc"), QStringLiteral("ltd"), QStringLiteral("a.m"),
        QStringLiteral("p.m"), QStringLiteral("u.s"), QStringLiteral("ca"),
    };
    qsizetype b = dot;
    while (b > 0 && (text.at(b - 1).isLetter() || text.at(b - 1) == u'.'))
        --b;
    const QStringView raw = text.mid(b, dot - b);
    if (raw.isEmpty())
        return false; // "in 2024. Then" — a number ends the sentence
    if (raw.size() == 1)
        return raw.at(0) != u'I'; // an initial ("J. Smith"), but "… so did I. Then"
    return kAbbrev.contains(raw.toString().toLower());
}

// Positions just past each complete sentence in 'text' (the index of the
// whitespace that follows it). With 'raw' the text is still Markdown: no cut
// inside a code span, a link or an HTML tag.
QList<qsizetype> sentenceCuts(QStringView text, bool raw)
{
    QList<qsizetype> cuts;
    bool inCode = false;
    int brackets = 0, parens = 0, angles = 0;
    auto track = [&](QChar c) {
        if (!raw)
            return;
        if (c == u'`') {
            inCode = !inCode;
            return;
        }
        if (inCode)
            return;
        switch (c.unicode()) {
        case '[': ++brackets; break;
        case ']': brackets = qMax(0, brackets - 1); break;
        case '(': ++parens; break;
        case ')': parens = qMax(0, parens - 1); break;
        case '<': ++angles; break;
        case '>': angles = qMax(0, angles - 1); break;
        default: break;
        }
    };
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        track(c);
        if (!isTerminator(c) || inCode)
            continue;
        qsizetype end = i; // the last terminator of a run like "?!" or "..."
        while (end + 1 < n && isTerminator(text.at(end + 1)))
            ++end;
        qsizetype j = end; // then closing quotes/brackets that belong to the sentence
        while (j + 1 < n && isCloser(text.at(j + 1))) {
            ++j;
            track(text.at(j));
        }
        const bool singleDot = end == i && text.at(i) == u'.';
        i = j;
        if (j + 1 >= n || !text.at(j + 1).isSpace())
            continue; // "3.5", "file.txt", or nothing after it yet
        if (raw && (inCode || brackets > 0 || parens > 0 || angles > 0))
            continue;
        if (singleDot && isAbbreviation(text, end))
            continue;
        cuts.append(j + 1);
    }
    return cuts;
}

QString collapse(const QString &s)
{
    return s.simplified();
}

// Classify a complete line; 'content' gets the text after the block marker.
LineKind classify(const QString &line, QString *content, QChar *fenceChar = nullptr, int *fenceLen = nullptr)
{
    static const QRegularExpression rule(QStringLiteral("^\\s*([-*_])(\\s*\\1){2,}\\s*$"));
    static const QRegularExpression heading(QStringLiteral("^\\s{0,3}(#{1,6})(?:\\s+(.*?))?\\s*#*\\s*$"));
    static const QRegularExpression quote(QStringLiteral("^\\s*(?:>\\s?)+"));
    static const QRegularExpression bullet(QStringLiteral("^\\s*(?:[-*+\\x{2022}]|\\d{1,3}[.)])\\s+"));
    static const QRegularExpression tableSep(
        QStringLiteral("^\\s*\\|?\\s*:?-{2,}:?\\s*(\\|\\s*:?-{2,}:?\\s*)+\\|?\\s*$"));
    static const QRegularExpression emphasisLine(QStringLiteral("^\\s*(\\*\\*|__)(.+?)\\1:?\\s*$"));

    const QString t = line.trimmed();
    *content = line;
    if (t.isEmpty())
        return LineKind::Blank;
    if (t.startsWith(QLatin1String("```")) || t.startsWith(QLatin1String("~~~"))) {
        if (fenceChar)
            *fenceChar = t.at(0);
        if (fenceLen) {
            int k = 0;
            while (k < t.size() && t.at(k) == t.at(0))
                ++k;
            *fenceLen = k;
        }
        return LineKind::FenceOpen;
    }
    if (t.startsWith(u'|') || tableSep.match(t).hasMatch())
        return LineKind::Table;
    if (rule.match(t).hasMatch())
        return LineKind::Rule;
    QRegularExpressionMatch m = heading.match(line);
    if (m.hasMatch()) {
        *content = m.captured(2);
        return LineKind::Heading;
    }
    m = emphasisLine.match(line); // "**Summary**" on a line of its own reads as a heading
    if (m.hasMatch()) {
        *content = m.captured(2);
        return LineKind::Heading;
    }
    m = bullet.match(line);
    if (m.hasMatch()) {
        *content = line.mid(m.capturedEnd());
        return LineKind::ListItem;
    }
    m = quote.match(line);
    if (m.hasMatch()) {
        *content = line.mid(m.capturedEnd());
        return LineKind::Quote;
    }
    return LineKind::Paragraph;
}

// Classify the start of a line that is still streaming. Unknown = not enough
// text yet to tell (e.g. "1" could become "1. item" or "100 files").
LineKind classifyPrefix(const QString &partial, qsizetype *markerLen)
{
    *markerLen = 0;
    qsizetype i = 0;
    while (i < partial.size() && (partial.at(i) == u' ' || partial.at(i) == u'\t'))
        ++i;
    if (i >= partial.size())
        return LineKind::Unknown;
    const QChar c = partial.at(i);
    const QStringView rest = QStringView(partial).mid(i);
    if (c == u'|' || c == u'#')
        return LineKind::Unknown; // table row / heading: wait for the whole line
    if (c == u'`' || c == u'~') {
        if (rest.size() < 3)
            return rest.size() >= 2 && rest.at(1) != c ? LineKind::Paragraph : LineKind::Unknown;
        if (rest.at(1) == c && rest.at(2) == c)
            return LineKind::Unknown; // a code fence
        return LineKind::Paragraph;   // inline code at the start of a sentence
    }
    if (c == u'>') {
        qsizetype k = i;
        while (k < partial.size() && (partial.at(k) == u'>' || partial.at(k) == u' '))
            ++k;
        *markerLen = k;
        return LineKind::Quote;
    }
    if (c == u'-' || c == u'*' || c == u'+' || c == u'_' || c == QChar(0x2022)) {
        if (rest.size() < 2)
            return LineKind::Unknown;
        if (rest.at(1) == u' ' && c != u'_') {
            // "- item" — unless it turns into a rule ("- - -"): wait for a letter.
            qsizetype k = i + 1;
            while (k < partial.size() && partial.at(k) == u' ')
                ++k;
            if (k >= partial.size())
                return LineKind::Unknown;
            if (partial.at(k) == c)
                return LineKind::Unknown;
            *markerLen = k;
            return LineKind::ListItem;
        }
        // "---", "***", "___" (a rule), "**Heading**" alone on its line, or a
        // paragraph that starts in bold: text after the closing marker
        // settles it as a paragraph; otherwise wait for the whole line.
        if (rest.at(1) == c) {
            static const QRegularExpression emphasisThenText(QStringLiteral("^(\\*\\*|__)[^*_]+\\1\\s*\\S"));
            return emphasisThenText.matchView(rest).hasMatch() ? LineKind::Paragraph : LineKind::Unknown;
        }
        return LineKind::Paragraph;
    }
    if (c.isDigit()) {
        qsizetype k = i;
        while (k < partial.size() && partial.at(k).isDigit())
            ++k;
        if (k >= partial.size())
            return LineKind::Unknown;
        if ((partial.at(k) == u'.' || partial.at(k) == u')') && k - i <= 3) {
            if (k + 1 >= partial.size())
                return LineKind::Unknown;
            if (partial.at(k + 1) == u' ') {
                qsizetype m = k + 1;
                while (m < partial.size() && partial.at(m) == u' ')
                    ++m;
                *markerLen = m;
                return LineKind::ListItem;
            }
        }
        return LineKind::Paragraph;
    }
    return LineKind::Paragraph;
}

} // namespace

// --- SpeechText ---------------------------------------------------------------

QString SpeechText::stripInline(const QString &line)
{
    static const QRegularExpression image(QStringLiteral("!\\[([^\\]]*)\\]\\((?:[^()]|\\([^)]*\\))*\\)"));
    static const QRegularExpression link(QStringLiteral("\\[([^\\]]+)\\]\\((?:[^()]|\\([^)]*\\))*\\)"));
    static const QRegularExpression refLink(QStringLiteral("\\[([^\\]]+)\\]\\[[^\\]]*\\]"));
    static const QRegularExpression footnote(QStringLiteral("\\[\\^[^\\]]+\\]"));
    static const QRegularExpression autoLink(QStringLiteral("<(?:(?:https?|ftp|mailto):[^>\\s]+|[^>\\s@]+@[^>\\s]+)>"));
    static const QRegularExpression htmlTag(QStringLiteral("</?[A-Za-z][^>]*>"));
    static const QRegularExpression url(
        QStringLiteral("\\b(?:https?://|ftp://|www\\.)[^\\s<>()\\[\\]]*[^\\s<>()\\[\\].,;:!?'\"]"));
    static const QRegularExpression code(QStringLiteral("(`+)(.+?)\\1"));
    static const QRegularExpression strong(QStringLiteral("\\*\\*|__|~~"));
    static const QRegularExpression emOpen(QStringLiteral("(^|[\\s(\\[{\"'])[*_](?=\\S)"));
    static const QRegularExpression emClose(QStringLiteral("(?<=\\S)[*_](?=$|[\\s)\\]}.,;:!?\"'])"));
    static const QRegularExpression escape(QStringLiteral("\\\\([\\\\`*_{}\\[\\]()#+\\-.!|>~])"));
    static const QRegularExpression snake(QStringLiteral("(?<=\\w)_(?=\\w)"));

    QString s = line;
    // Code spans first: their content is kept verbatim (as words), and nothing
    // inside them may be taken for emphasis or a link. They are swapped for
    // placeholders and put back at the end.
    QStringList spans;
    {
        QString out;
        qsizetype last = 0;
        auto it = code.globalMatch(s);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += QStringView(s).mid(last, m.capturedStart() - last);
            QString inner = m.captured(2).trimmed();
            inner.replace(u'_', u' ');
            spans << inner;
            out += QStringLiteral("\x01%1\x02").arg(spans.size() - 1);
            last = m.capturedEnd();
        }
        out += QStringView(s).mid(last);
        s = out;
    }
    s.replace(image, QStringLiteral("\\1"));
    s.replace(link, QStringLiteral("\\1"));
    s.replace(refLink, QStringLiteral("\\1"));
    s.remove(footnote);
    s.replace(autoLink, aLink());
    s.replace(htmlTag, QStringLiteral(" "));
    s.replace(url, aLink());
    s.remove(strong);
    s.replace(emOpen, QStringLiteral("\\1"));
    s.remove(emClose);
    s.replace(escape, QStringLiteral("\\1"));
    s.replace(snake, QStringLiteral(" "));
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    s.replace(QChar(0x2192), QStringLiteral(" to ")); // →
    for (int i = 0; i < spans.size(); ++i)
        s.replace(QStringLiteral("\x01%1\x02").arg(i), spans.at(i));
    return collapse(s);
}

QString SpeechText::forSpeech(const QString &markdown)
{
    SentenceStream stream;
    QStringList all = stream.push(markdown);
    all += stream.flush();
    return all.join(u' ');
}

// --- SentenceStream ---------------------------------------------------------------

void SentenceStream::reset()
{
    m_line.clear();
    m_lineContinues = false;
    m_lineKind = 0;
    m_para.clear();
    m_inFence = false;
    m_fenceChar = QChar();
    m_fenceLen = 0;
}

QStringList SentenceStream::push(const QString &chunk)
{
    QStringList out;
    if (chunk.isEmpty())
        return out;
    QString text = chunk;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(u'\r', u'\n');
    m_line += text;
    qsizetype nl;
    while ((nl = m_line.indexOf(u'\n')) >= 0) {
        const QString line = m_line.left(nl);
        m_line.remove(0, nl + 1);
        completeLine(line, out);
        m_lineContinues = false;
        m_lineKind = 0;
    }
    earlySentences(out);
    return out;
}

QStringList SentenceStream::flush()
{
    QStringList out;
    if (!m_line.isEmpty()) {
        const QString line = m_line;
        m_line.clear();
        completeLine(line, out);
    }
    flushParagraph(out, false);
    reset();
    return out;
}

void SentenceStream::appendPara(const QString &text)
{
    if (text.isEmpty())
        return;
    if (!m_para.isEmpty() && !m_para.back().isSpace())
        m_para += u' ';
    m_para += text;
}

void SentenceStream::takeSentences(QStringList &out, bool endIsBoundary)
{
    qsizetype start = 0;
    QList<qsizetype> cuts = sentenceCuts(m_para, false);
    if (endIsBoundary) {
        // The text so far is known to end where it ends (a line ended, or it
        // was cut at a sentence end): a sentence that ends there is complete,
        // although nothing follows it yet.
        qsizetype e = m_para.size();
        while (e > 0 && m_para.at(e - 1).isSpace())
            --e;
        qsizetype k = e;
        while (k > 0 && isCloser(m_para.at(k - 1)))
            --k;
        const bool ends = k > 0 && isTerminator(m_para.at(k - 1))
            && !(m_para.at(k - 1) == u'.' && (k < 2 || m_para.at(k - 2) != u'.') && isAbbreviation(m_para, k - 1));
        if (ends && (cuts.isEmpty() || cuts.last() < e))
            cuts.append(e);
    }
    for (const qsizetype cut : cuts) {
        const QString sentence = collapse(m_para.mid(start, cut - start));
        if (hasSpeakable(sentence))
            out << sentence;
        start = cut;
    }
    m_para.remove(0, start);
    // Without punctuation for a long stretch: cut at a clause boundary (or a
    // word boundary) so speech can start; the voice will just pause there.
    while (m_para.size() > kLongSentence) {
        qsizetype cut = -1;
        for (const QLatin1String sep : {QLatin1String(", "), QLatin1String("; "), QLatin1String(": "),
                                        QLatin1String(" - ")}) {
            const qsizetype p = m_para.lastIndexOf(sep, kLongSentence);
            if (p > kLongSentence / 3 && p > cut)
                cut = p + 1;
        }
        if (cut < 0) {
            const qsizetype p = m_para.lastIndexOf(u' ', kLongSentence);
            cut = p > kLongSentence / 3 ? p : kLongSentence;
        }
        const QString part = collapse(m_para.left(cut));
        if (hasSpeakable(part))
            out << part;
        m_para.remove(0, cut);
    }
    if (m_para.trimmed().isEmpty())
        m_para.clear();
}

void SentenceStream::flushParagraph(QStringList &out, bool finalPunctuation)
{
    takeSentences(out);
    QString tail = collapse(m_para);
    m_para.clear();
    if (!hasSpeakable(tail))
        return;
    if (finalPunctuation) {
        const QChar last = tail.back();
        if (!isTerminator(last) && !isCloser(last) && last != u':' && last != u';' && last != u',')
            tail += u'.';
    }
    out << tail;
}

void SentenceStream::completeLine(const QString &line, QStringList &out)
{
    if (m_inFence) {
        // A closing fence: the same character, at least as long as the opening one.
        const QString t = line.trimmed();
        int k = 0;
        while (k < t.size() && t.at(k) == m_fenceChar)
            ++k;
        if (k >= m_fenceLen && k == t.size())
            m_inFence = false;
        return;
    }
    QString content;
    LineKind kind;
    if (m_lineContinues) {
        kind = static_cast<LineKind>(m_lineKind);
        content = line;
    } else {
        kind = classify(line, &content, &m_fenceChar, &m_fenceLen);
    }
    switch (kind) {
    case LineKind::Blank:
    case LineKind::Rule:
    case LineKind::Table:
        flushParagraph(out, false);
        break;
    case LineKind::FenceOpen:
        flushParagraph(out, false);
        out << codeOmitted();
        m_inFence = true;
        break;
    case LineKind::Heading:
    case LineKind::ListItem:
        if (!m_lineContinues)
            flushParagraph(out, false);
        appendPara(SpeechText::stripInline(content));
        flushParagraph(out, true);
        break;
    case LineKind::Unknown:
    case LineKind::Quote:
    case LineKind::Paragraph:
        appendPara(SpeechText::stripInline(content));
        takeSentences(out, true);
        break;
    }
}

void SentenceStream::earlySentences(QStringList &out)
{
    if (m_inFence || m_line.isEmpty())
        return;
    if (!m_lineContinues) {
        qsizetype marker = 0;
        const LineKind kind = classifyPrefix(m_line, &marker);
        if (kind == LineKind::Unknown)
            return; // a table row, heading, fence or rule — or too little text to tell
        if (kind == LineKind::ListItem)
            flushParagraph(out, false); // a list starts: the paragraph before it is complete
        m_lineKind = static_cast<int>(kind);
        m_line.remove(0, marker);
        m_lineContinues = true;
    }
    const QList<qsizetype> cuts = sentenceCuts(m_line, true);
    qsizetype cut = cuts.isEmpty() ? -1 : cuts.last();
    if (cut < 0 && m_line.size() > 2 * kLongSentence) {
        // A very long line with no sentence end yet: hand over everything up
        // to the last word boundary outside code/links; takeSentences() cuts it.
        bool inCode = false;
        int depth = 0;
        for (qsizetype i = 0; i < m_line.size(); ++i) {
            const QChar c = m_line.at(i);
            if (c == u'`')
                inCode = !inCode;
            else if (!inCode && (c == u'[' || c == u'(' || c == u'<'))
                ++depth;
            else if (!inCode && (c == u']' || c == u')' || c == u'>'))
                depth = qMax(0, depth - 1);
            else if (c == u' ' && !inCode && depth == 0)
                cut = i;
        }
    }
    if (cut <= 0)
        return;
    appendPara(SpeechText::stripInline(m_line.left(cut)));
    m_line.remove(0, cut);
    takeSentences(out, !cuts.isEmpty()); // cut at a sentence end: all of it is complete
}
