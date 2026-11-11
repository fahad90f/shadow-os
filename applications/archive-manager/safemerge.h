// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <atomic>

// The security boundary of extraction.
//
// Tools never write into the user's folder. They unpack into a private,
// empty staging directory (mode 0700) created inside the destination; this
// code then walks what really arrived on disk — without ever following a
// symbolic link — and moves into the destination only what is safe:
//
//   * regular files and directories, with set-user-ID, set-group-ID and
//     sticky bits removed;
//   * symbolic links whose target is relative and stays inside the
//     extraction folder;
//   * nothing else: device nodes, FIFOs, sockets, absolute or escaping
//     links are left behind and reported.
//
// Hard-linked files are copied apart first, so a link a tool was tricked
// into creating can never give access to (or modify) a file elsewhere.
// Existing content of the destination is never written through: a
// directory in the archive is merged only into a real directory, never
// into a symbolic link, and existing files are replaced only when the user
// chose that.
namespace SafeMerge {

enum class Conflict { Skip, Overwrite, Rename };

struct Report
{
    int files = 0;
    int directories = 0;
    int links = 0;
    qint64 bytes = 0;
    int skippedExisting = 0;
    int renamed = 0;
    QStringList refused;        // "<path>: <reason>", for the user
    QString error;              // fatal problem; the merge stopped
    QStringList topLevel;       // names created directly in the destination
    bool cancelled = false;
};

// Move the content of stageRoot (optionally only its sub-folder 'subPath')
// into destination. Both must be on the same file system (the stage is
// created inside the destination); files are moved, not copied.
Report merge(const QString &stageRoot, const QString &subPath, const QString &destination,
             Conflict conflict, const std::atomic_bool &cancel);

// Total size of the regular files below 'path' (no symlinks followed).
qint64 treeSize(const QString &path);

} // namespace SafeMerge
