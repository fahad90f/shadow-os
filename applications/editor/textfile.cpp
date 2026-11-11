// SPDX-License-Identifier: GPL-3.0-or-later
#include "textfile.h"

#include <QCoreApplication>
#include <QStringConverter>
#include <QStringDecoder>
#include <QStringEncoder>

namespace TextFile {

namespace {

QString tr(const char *s) { return QCoreApplication::translate("TextFile", s); }

struct Bom { const char *bytes; int length; const char *encoding; };
// Longest first: the UTF-32LE mark starts with the UTF-16LE one.
const Bom kBoms[] = {
    {"\xFF\xFE\x00\x00", 4, "UTF-32LE"},
    {"\x00\x00\xFE\xFF", 4, "UTF-32BE"},
    {"\xEF\xBB\xBF", 3, "UTF-8"},
    {"\xFF\xFE", 2, "UTF-16LE"},
    {"\xFE\xFF", 2, "UTF-16BE"},
};

QByteArray bomFor(const QString &encoding)
{
    for (const Bom &b : kBoms) {
        if (encoding.compare(QLatin1String(b.encoding), Qt::CaseInsensitive) == 0)
            return QByteArray(b.bytes, b.length);
    }
    return {};
}

bool decodeWith(const QString &encoding, const QByteArray &bytes, QString *out)
{
    QStringDecoder decoder(encoding.toLatin1().constData(), QStringConverter::Flag::Stateless);
    if (!decoder.isValid())
        return false;
    QString text = decoder.decode(bytes);
    if (decoder.hasError())
        return false;
    *out = std::move(text);
    return true;
}

// UTF-16 text without a byte order mark: mostly-ASCII content has a zero in
// every other byte. Looks at the first 64 KiB.
QString guessUtf16(const QByteArray &bytes)
{
    if (bytes.size() < 4 || bytes.size() % 2 != 0)
        return {};
    const qsizetype n = qMin<qsizetype>(bytes.size(), 65536) & ~qsizetype(1);
    qsizetype evenZero = 0, oddZero = 0;
    for (qsizetype i = 0; i < n; i += 2) {
        if (bytes.at(i) == 0) ++evenZero;
        if (bytes.at(i + 1) == 0) ++oddZero;
    }
    const qsizetype pairs = n / 2;
    if (oddZero * 10 >= pairs * 4 && evenZero * 50 <= pairs)
        return QStringLiteral("UTF-16LE");
    if (evenZero * 10 >= pairs * 4 && oddZero * 50 <= pairs)
        return QStringLiteral("UTF-16BE");
    return {};
}

void normaliseLineEndings(Decoded &d)
{
    const QChar *s = d.text.constData();
    const qsizetype n = d.text.size();
    bool needsRewrite = false;
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t c = s[i].unicode();
        if (c == u'\n') {
            ++d.lf;
        } else if (c == u'\r') {
            needsRewrite = true;
            if (i + 1 < n && s[i + 1] == u'\n') { ++d.crlf; ++i; }
            else ++d.cr;
        } else if (c == 0 || c == 0x2029 || c == 0xFDD0 || c == 0xFDD1) {
            d.fragileCharacters = true;
        }
    }
    if (d.crlf >= d.lf && d.crlf >= d.cr && d.crlf > 0) d.lineEnding = QStringLiteral("CRLF");
    else if (d.cr > d.lf && d.cr > d.crlf) d.lineEnding = QStringLiteral("CR");
    else d.lineEnding = QStringLiteral("LF");
    d.mixedLineEndings = int(d.lf > 0) + int(d.crlf > 0) + int(d.cr > 0) > 1;
    if (needsRewrite) {
        QString out;
        out.reserve(n);
        for (qsizetype i = 0; i < n; ++i) {
            if (s[i] == u'\r') {
                out += u'\n';
                if (i + 1 < n && s[i + 1] == u'\n') ++i;
            } else {
                out += s[i];
            }
        }
        d.text = std::move(out);
    }
}

} // namespace

QStringList encodings()
{
    static const QStringList list = [] {
        QStringList out;
        const char *candidates[] = {"UTF-8", "UTF-16LE", "UTF-16BE", "UTF-32LE", "UTF-32BE", "ISO-8859-1",
                                    "windows-1252", "ISO-8859-15", "ISO-8859-2", "windows-1250", "windows-1251",
                                    "KOI8-R", "Shift_JIS", "EUC-JP", "GBK", "GB18030", "Big5", "EUC-KR"};
        for (const char *name : candidates) {
            if (QStringDecoder(name).isValid() && QStringEncoder(name).isValid())
                out << QString::fromLatin1(name);
        }
        return out;
    }();
    return list;
}

bool encodingAvailable(const QString &name)
{
    return encodings().contains(name, Qt::CaseInsensitive);
}

Decoded decode(const QByteArray &bytes, const QString &forcedEncoding)
{
    Decoded d;
    qsizetype skip = 0;
    QString bomEncoding;
    for (const Bom &b : kBoms) {
        if (bytes.startsWith(QByteArrayView(b.bytes, b.length))) {
            bomEncoding = QString::fromLatin1(b.encoding);
            skip = b.length;
            break;
        }
    }

    if (!forcedEncoding.isEmpty()) {
        if (!encodingAvailable(forcedEncoding)) {
            d.error = tr("The encoding %1 is not available on this system.").arg(forcedEncoding);
            return d;
        }
        d.encoding = forcedEncoding;
        d.bom = bomEncoding.compare(forcedEncoding, Qt::CaseInsensitive) == 0;
        if (!decodeWith(forcedEncoding, d.bom ? bytes.mid(skip) : bytes, &d.text)) {
            d.error = tr("The file is not valid %1.").arg(forcedEncoding);
            return d;
        }
    } else if (!bomEncoding.isEmpty()) {
        d.encoding = bomEncoding;
        d.bom = true;
        if (!decodeWith(bomEncoding, bytes.mid(skip), &d.text)) {
            d.error = tr("The file starts with a %1 byte order mark but is not valid %1.").arg(bomEncoding);
            return d;
        }
    } else if (bytes.contains('\0')) {
        const QString utf16 = guessUtf16(bytes);
        if (utf16.isEmpty() || !decodeWith(utf16, bytes, &d.text)) {
            d.error = tr("This looks like a binary file (it contains NUL bytes and is not UTF-16 or UTF-32 text). "
                         "Opening it as text could damage it, so it was not opened.");
            return d;
        }
        d.encoding = utf16;
        d.guessed = true;
    } else if (decodeWith(QStringLiteral("UTF-8"), bytes, &d.text)) {
        d.encoding = QStringLiteral("UTF-8");
    } else {
        // Not UTF-8. Use an 8-bit encoding that maps every byte to a
        // character and back, so the file survives a load/save cycle even
        // when the guess is wrong.
        d.guessed = true;
        for (const QString &candidate : {QStringLiteral("windows-1252"), QStringLiteral("ISO-8859-1")}) {
            QString text;
            if (!encodingAvailable(candidate) || !decodeWith(candidate, bytes, &text))
                continue;
            QStringEncoder encoder(candidate.toLatin1().constData(), QStringConverter::Flag::Stateless);
            const QByteArray back = encoder.encode(text);
            if (!encoder.hasError() && back == bytes) {
                d.text = std::move(text);
                d.encoding = candidate;
                break;
            }
        }
        if (d.encoding.isEmpty()) {
            d.error = tr("The text encoding of this file could not be determined.");
            return d;
        }
    }
    normaliseLineEndings(d);
    return d;
}

Encoded encode(const QString &text, const QString &encoding, bool bom, const QString &lineEnding)
{
    Encoded e;
    QStringEncoder encoder(encoding.toLatin1().constData(), QStringConverter::Flag::Stateless);
    if (!encoder.isValid()) {
        e.error = tr("The encoding %1 is not available on this system.").arg(encoding);
        return e;
    }
    QString converted;
    const QString *source = &text;
    if (lineEnding == QLatin1String("CRLF") || lineEnding == QLatin1String("CR")) {
        converted = text;
        converted.replace(QLatin1Char('\n'), lineEnding == QLatin1String("CRLF") ? QStringLiteral("\r\n") : QStringLiteral("\r"));
        source = &converted;
    }
    QByteArray body = encoder.encode(*source);
    if (encoder.hasError()) {
        e.error = tr("The text contains characters that cannot be written in %1. Choose another encoding (for example UTF-8).").arg(encoding);
        return e;
    }
    if (bom)
        e.bytes = bomFor(encoding);
    e.bytes += body;
    return e;
}

QJsonObject describe(const Decoded &d, qint64 byteCount)
{
    return QJsonObject{
        {QStringLiteral("ok"), d.ok()},
        {QStringLiteral("error"), d.error},
        {QStringLiteral("bytes"), double(byteCount)},
        {QStringLiteral("characters"), double(d.text.size())},
        {QStringLiteral("encoding"), d.encoding},
        {QStringLiteral("bom"), d.bom},
        {QStringLiteral("guessed"), d.guessed},
        {QStringLiteral("lineEnding"), d.lineEnding},
        {QStringLiteral("mixedLineEndings"), d.mixedLineEndings},
        {QStringLiteral("lf"), double(d.lf)},
        {QStringLiteral("crlf"), double(d.crlf)},
        {QStringLiteral("cr"), double(d.cr)},
        {QStringLiteral("fragileCharacters"), d.fragileCharacters},
    };
}

} // namespace TextFile
