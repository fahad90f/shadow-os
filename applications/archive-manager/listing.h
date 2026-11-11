// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// Parsers for the tools' listings. The result is used for display, for the
// size estimate and for choosing which entries to ask a tool for — never as
// a security decision: what may be written to disk is decided on the real,
// extracted files (see safemerge.h).
namespace Listing {

enum class Kind { File, Directory, SymLink, HardLink, Device, Fifo, Other };

struct Entry
{
    QString rawPath;        // exactly as stored in the archive
    QStringList parts;      // display path: components without "", "." and leading "/"
    Kind kind = Kind::File;
    qint64 size = 0;
    qint64 packedSize = -1; // -1: not reported by the format
    QDateTime modified;
    QString linkTarget;
    QString mode;           // "-rwxr-xr-x" where known
    bool encrypted = false;
    QStringList hazards;    // why this entry is not extracted as stored (shown as a warning)
};

struct Result
{
    QList<Entry> entries;
    bool headerEncrypted = false;   // 7z with encrypted file names
    QString typeName;               // what the tool calls the format
};

// Output of: 7z l -slt -sccUTF-8 <archive>
Result parseSevenZip(const QByteArray &output);
// Output of: bsdtar -tv --numeric-owner -f <archive>   (C locale)
Result parseBsdTar(const QByteArray &output);

// Fill parts and hazards from rawPath/kind/linkTarget/mode.
void classify(Entry &entry);
QString kindName(Kind kind);

} // namespace Listing
