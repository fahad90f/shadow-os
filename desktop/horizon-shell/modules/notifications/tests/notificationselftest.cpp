// SPDX-License-Identifier: GPL-3.0-or-later
//
// Self test of NotificationText: what clients send must come out bounded
// and harmless. Exit code 0 if every check holds.
#include "notificationtext.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTextStream>

using namespace NotificationText;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what, const QString &detail = {})
{
    ++checks;
    if (ok)
        return;
    ++failures;
    QTextStream(stderr) << "FAIL: " << what << (detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail) << '\n';
}

static void equal(const QString &actual, const QString &expected, const char *what)
{
    check(actual == expected, what, QStringLiteral("got \"%1\", expected \"%2\"").arg(actual, expected));
}

// The styled output may contain nothing but escaped text and the tags
// <b> <i> <u> <br> <a href="N">, properly nested and closed.
static bool onlySafeMarkup(const QString &styled, QString *why)
{
    static const QRegularExpression tag(QStringLiteral("<(/?)(b|i|u|br|a)( href=\"\\d+\")?>"));
    QStringList open;
    qsizetype i = 0;
    while (i < styled.size()) {
        const QChar c = styled.at(i);
        if (c == u'<') {
            const QRegularExpressionMatch m = tag.match(styled, i, QRegularExpression::NormalMatch,
                                                        QRegularExpression::AnchorAtOffsetMatchOption);
            if (!m.hasMatch()) {
                *why = QStringLiteral("unexpected tag at %1: %2").arg(i).arg(styled.mid(i, 30));
                return false;
            }
            const QString name = m.captured(2);
            if (name != QLatin1String("br")) {
                if (m.captured(1).isEmpty()) {
                    if ((name == QLatin1String("a")) != !m.captured(3).isEmpty()) {
                        *why = QStringLiteral("attribute where none belongs at %1").arg(i);
                        return false;
                    }
                    open << name;
                } else {
                    if (open.isEmpty() || open.takeLast() != name) {
                        *why = QStringLiteral("badly nested </%1> at %2").arg(name).arg(i);
                        return false;
                    }
                }
            }
            i += m.capturedLength();
            continue;
        }
        if (c == u'>' || c == u'"') {
            *why = QStringLiteral("unescaped %1 at %2").arg(c).arg(i);
            return false;
        }
        if (c == u'&') {
            static const QRegularExpression entity(QStringLiteral("&(amp|lt|gt|quot);"));
            const QRegularExpressionMatch m = entity.match(styled, i, QRegularExpression::NormalMatch,
                                                           QRegularExpression::AnchorAtOffsetMatchOption);
            if (!m.hasMatch()) {
                *why = QStringLiteral("unescaped & at %1").arg(i);
                return false;
            }
            i += m.capturedLength();
            continue;
        }
        ++i;
    }
    if (!open.isEmpty()) {
        *why = QStringLiteral("unclosed <%1>").arg(open.constLast());
        return false;
    }
    return true;
}

static Body safe(const QString &raw, const char *what)
{
    const Body body = sanitizeBody(raw);
    QString why;
    check(onlySafeMarkup(body.styled, &why), what, why + QStringLiteral(" in: ") + body.styled.left(200));
    check(body.plain.size() <= MaxBodyLength + 2, what, QStringLiteral("plain text too long"));
    check(body.links.size() <= MaxLinks, what, QStringLiteral("too many links"));
    return body;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- single-line fields ------------------------------------------------
    equal(singleLine(QStringLiteral("  Hello \n\t world  "), 100), QStringLiteral("Hello world"), "white space collapses");
    equal(singleLine(QStringLiteral("<b>x</b>"), 100), QStringLiteral("<b>x</b>"), "single line keeps markup as text");
    equal(singleLine(QStringLiteral("abcdef"), 4), QStringLiteral("abcd\u2026"), "single line is cut");
    equal(singleLine(QStringLiteral("a\u202Eb\u2066c\uFEFF"), 10), QStringLiteral("abc"), "bidi controls are dropped");
    equal(singleLine(QStringLiteral("a\u0000b\u0007c\u001b[31m"), 20), QStringLiteral("a b c [31m"), "control characters become spaces");
    equal(singleLine(QString(1 << 22, u' ') + QStringLiteral("x"), 10), QString(), "megabytes of white space");
    check(singleLine(QString(1 << 22, u'x'), 200).size() == 201, "megabyte summary is cut to 200 + ellipsis");
    equal(singleLine(QStringLiteral("ab\U0001F600"), 3), QStringLiteral("ab\u2026"), "no cut inside a surrogate pair");

    // ---- body: the markup subset -------------------------------------------------
    equal(sanitizeBody(QStringLiteral("plain text")).styled, QStringLiteral("plain text"), "plain body");
    equal(sanitizeBody(QStringLiteral("<b>bold</b> <i>it</i> <u>un</u>")).styled,
          QStringLiteral("<b>bold</b> <i>it</i> <u>un</u>"), "b i u");
    equal(sanitizeBody(QStringLiteral("a\nb")).styled, QStringLiteral("a<br>b"), "new line");
    equal(sanitizeBody(QStringLiteral("a<br/>b<BR>c")).styled, QStringLiteral("a<br>b<br>c"), "br");
    equal(sanitizeBody(QStringLiteral("1 < 2 && 3 > 2")).styled, QStringLiteral("1 &lt; 2 &amp;&amp; 3 &gt; 2"), "comparison signs");
    equal(sanitizeBody(QStringLiteral("&lt;b&gt;x&lt;/b&gt; &amp;amp; &#60;i&#62; &#x3c;u&#x3E;")).styled,
          QStringLiteral("&lt;b&gt;x&lt;/b&gt; &amp;amp; &lt;i&gt; &lt;u&gt;"), "entities never become markup");
    equal(sanitizeBody(QStringLiteral("say \"hi\"")).styled, QStringLiteral("say &quot;hi&quot;"), "quotes are escaped");
    equal(sanitizeBody(QStringLiteral("<b>unclosed")).styled, QStringLiteral("<b>unclosed</b>"), "unclosed tag is closed");
    equal(sanitizeBody(QStringLiteral("</b></i>text</a>")).styled, QStringLiteral("text"), "stray closing tags");
    equal(sanitizeBody(QStringLiteral("<b></b><i></i>")).styled, QString(), "empty elements leave nothing");

    // ---- links ----------------------------------------------------------------------------
    {
        const Body body = safe(QStringLiteral("<a href=\"https://example.org/a?b=1&amp;c=2\">link</a> tail"), "link");
        equal(body.styled, QStringLiteral("<a href=\"0\">link</a> tail"), "link is an index");
        check(body.links == QStringList{QStringLiteral("https://example.org/a?b=1&c=2")}, "link target", body.links.join(u' '));
    }
    for (const char *bad : {"javascript:alert(1)", "file:///etc/passwd", "data:text/html,<script>", "ftp://x/y",
                            "https://user:pw@evil.example/", "https://bank.example@evil.example/", "//evil.example/x",
                            "relative/path", "", "http://", "vbscript:x", " java\tscript:alert(1)",
                            "https://exa mple.org/", "smb://server/share", "mailto:"}) {
        const QString href = QString::fromLatin1(bad);
        check(linkTarget(href).isEmpty(), "bad link target refused", href);
        const Body body = safe(QStringLiteral("<a href=\"%1\">text</a>").arg(href), "bad link");
        equal(body.styled, QStringLiteral("text"), "bad link leaves its text only");
        check(body.links.isEmpty(), "bad link not stored", href);
    }
    equal(linkTarget(QStringLiteral("mailto:someone@example.org")), QStringLiteral("mailto:someone@example.org"), "mailto");
    check(!linkTarget(QStringLiteral("https://example.org/\"onclick=\"x")).contains(u'"'), "quotes in links are encoded");
    {
        QString many;
        for (int i = 0; i < 50; ++i)
            many += QStringLiteral("<a href=\"https://example.org/%1\">l%1</a> ").arg(i);
        const Body body = safe(many, "many links");
        check(body.links.size() == MaxLinks, "links are capped");
    }
    {
        // A line break caused by a block element after a link is not part of the link.
        const Body body = safe(QStringLiteral("<a href=\"https://example.org/\">good</a><table><tr><td>cell</td></tr></table>"),
                               "link then block");
        equal(body.styled, QStringLiteral("<a href=\"0\">good</a><br>cell"), "link closed before the line break");
    }

    // ---- what must never get through ---------------------------------------------------------
    {
        const Body body = safe(QStringLiteral("<img src=\"http://evil.example/x.png\" alt=\"ALT\"> after"), "img");
        equal(body.styled, QStringLiteral("ALT after"), "img leaves its alt text");
        check(!body.styled.contains(QLatin1String("evil")), "img source is gone");
    }
    equal(safe(QStringLiteral("a<script>alert(1)</script>b<style>x{}</style>c"), "script").styled, QStringLiteral("abc"),
          "script and style content is dropped");
    equal(safe(QStringLiteral("a<script>never closed"), "open script").styled, QStringLiteral("a"), "unclosed script");
    equal(safe(QStringLiteral("x<!-- comment <b> -->y<?pi?>z<!DOCTYPE html>"), "comment").styled, QStringLiteral("xyz"),
          "comments and declarations");
    equal(safe(QStringLiteral("<font size=\"99\" color=\"red\">f</font><span style=\"x\">s</span>"), "font").styled,
          QStringLiteral("fs"), "unknown elements keep their text only");
    equal(safe(QStringLiteral("<b onclick=\"x\" style='y'>t</b>"), "attributes").styled, QStringLiteral("<b>t</b>"),
          "attributes are dropped");
    equal(safe(QStringLiteral("<a href='https://example.org/' onmouseover=\"x\">t</a>"), "a attributes").styled,
          QStringLiteral("<a href=\"0\">t</a>"), "only href survives");
    equal(safe(QStringLiteral("<p>one</p><p>two</p><div>three</div>"), "blocks").styled,
          QStringLiteral("one<br>two<br>three"), "block elements become line breaks");
    equal(safe(QStringLiteral("a\n\n\n\n\nb"), "blank lines").styled, QStringLiteral("a<br><br>b"), "blank lines collapse");
    equal(safe(QStringLiteral("<<b>>x"), "angle soup").plain, QStringLiteral("<>x"), "angle soup");
    equal(safe(QStringLiteral("abc\u202Edef"), "rlo").plain, QStringLiteral("abcdef"), "bidi override dropped from body");
    safe(QStringLiteral("<a href=\"https://example.org/\"><b>x</a></b>y<i>"), "misnested");
    safe(QStringLiteral("<b <i <u <a href= <<<< &&&& &#xFFFFFFFF; &#0; &bogus; <"), "garbage");
    safe(QStringLiteral("<a href=\"") + QString(20000, u'x'), "unterminated attribute");

    // ---- size and time bounds -----------------------------------------------------------------
    {
        QElapsedTimer timer;
        timer.start();
        const Body big = safe(QString(8 << 20, u'w'), "8 MB body");
        check(big.truncated && big.plain.size() == MaxBodyLength + 1, "huge body is cut");
        const Body nested = safe(QStringLiteral("<b>").repeated(100000) + QStringLiteral("deep"), "deep nesting");
        check(nested.plain.isEmpty() || nested.plain.size() <= MaxBodyLength + 1, "deep nesting bounded");
        safe(QStringLiteral("<").repeated(1 << 20), "a million <");
        safe(QStringLiteral("&").repeated(1 << 20), "a million &");
        safe(QStringLiteral("<a href=\"").repeated(200000), "many unterminated links");
        safe(QString(1 << 20, u'\n'), "a million new lines");
        check(timer.elapsed() < 2000, "hostile bodies are handled quickly",
              QStringLiteral("%1 ms").arg(timer.elapsed()));
    }

    // ---- icons and files ---------------------------------------------------------------------
    equal(iconName(QStringLiteral("dialog-information")), QStringLiteral("dialog-information"), "icon name");
    check(iconName(QStringLiteral("../../etc/passwd")).isEmpty(), "icon name with a path");
    check(iconName(QStringLiteral("http://x/y.png")).isEmpty(), "icon name that is a URL");
    check(iconName(QStringLiteral("a b")).isEmpty(), "icon name with a space");
    equal(localFilePath(QStringLiteral("/usr/share/x.png")), QStringLiteral("/usr/share/x.png"), "absolute path");
    equal(localFilePath(QStringLiteral("file:///usr/share/a%20b.png")), QStringLiteral("/usr/share/a b.png"), "file URL");
    equal(localFilePath(QStringLiteral("/a/../b")), QStringLiteral("/b"), "path is cleaned");
    for (const char *remote : {"http://example.org/x.png", "https://example.org/x.png", "file://server/share/x.png",
                               "ftp://x/y", "smb://x/y", "relative.png", "data:image/png;base64,AAAA", "~/x.png", ""}) {
        check(localFilePath(QString::fromLatin1(remote)).isEmpty(), "not a local file", QString::fromLatin1(remote));
    }
    check(looksLikeUrl(QStringLiteral("http://x")) && looksLikeUrl(QStringLiteral("data:x"))
          && !looksLikeUrl(QStringLiteral("dialog-information")) && !looksLikeUrl(QStringLiteral("/a:b")), "URL detection");

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
