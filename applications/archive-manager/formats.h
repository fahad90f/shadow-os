// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>

// What the application knows about each archive format: which tool reads
// it, whether it can be created or changed, and what encryption is real.
namespace Formats {

enum class Engine { None, SevenZip, BsdTar };

struct Format
{
    QString id;                 // "zip", "7z", "tar", "tar.gz", "tar.bz2", "tar.xz", "tar.zst", "cpio", "iso", "rar"
    QString label;
    Engine engine = Engine::None;
    bool canCreate = false;
    bool canModify = false;     // add and delete entries
    bool canEncrypt = false;    // password protection when creating
    QString cipher;             // what the password really does (shown in the UI)
    QStringList extensions;     // first one is used when creating
    bool valid() const { return engine != Engine::None; }
};

const QList<Format> &all();
Format byId(const QString &id);

// Identify by content (magic numbers), using the file name only to tell a
// compressed tar from a compressed single file. 'problem' explains a
// failure (unreadable file, unknown format, plain compressed file).
Format detect(const QString &path, QString *problem);

// For a new archive: the format its file name asks for.
Format fromFileName(const QString &name);

// "report" → "report.tar.gz": strips a known archive extension.
QString baseName(const QString &fileName);

bool toolAvailable(Engine engine);
QString toolName(Engine engine);
QJsonArray describe();          // for --dump formats

} // namespace Formats
