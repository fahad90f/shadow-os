// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Byte-level side of the editor: turning a file's bytes into text and back
// without losing anything. Used by the document (GUI) and by the diagnostic
// command line (--probe, --resave), so both go through the same code.
namespace TextFile {

struct Decoded
{
    QString text;            // line endings normalised to '\n'
    QString encoding;        // codec name, e.g. "UTF-8", "UTF-16LE", "windows-1252"
    bool bom = false;        // the file started with a byte order mark
    QString lineEnding;      // "LF" | "CRLF" | "CR" (the dominant one)
    bool mixedLineEndings = false;
    bool guessed = false;    // encoding is a fallback guess (not UTF-8, no BOM)
    bool fragileCharacters = false; // contains characters the text widget cannot keep (see README)
    QString error;           // non-empty: the file was not decoded
    qint64 lf = 0, crlf = 0, cr = 0;
    bool ok() const { return error.isEmpty(); }
};

struct Encoded
{
    QByteArray bytes;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Encodings offered in the UI (only those the Qt build can really convert).
QStringList encodings();
bool encodingAvailable(const QString &name);

// forcedEncoding empty → detect: BOM, then UTF-16 without BOM, then UTF-8,
// then a lossless 8-bit fallback. Binary data is refused.
Decoded decode(const QByteArray &bytes, const QString &forcedEncoding = QString());

// text uses '\n'. Fails (instead of writing '?') when a character cannot be
// represented in the encoding.
Encoded encode(const QString &text, const QString &encoding, bool bom, const QString &lineEnding);

QJsonObject describe(const Decoded &d, qint64 byteCount);

} // namespace TextFile
