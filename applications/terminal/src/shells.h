// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// Which shells exist on this system and which one is the user's.
//
// Sources: /etc/shells (the system's list of valid login shells) and the
// passwd database entry of the current user (getpwuid_r), i.e. the same data
// chsh(1) and login(1) work with.
namespace Shells {

struct Shell
{
    QString path;        // as listed in /etc/shells
    QString name;        // file name, e.g. "bash"
    QString canonical;   // symlinks resolved
    bool executable = false;
};

// Every entry of /etc/shells, in file order.
QList<Shell> listed();

// Entries that can be started, one per distinct program (Debian lists both
// /bin/bash and /usr/bin/bash, which are the same file).
QList<Shell> available();

// The user's login shell from the passwd database. Falls back to $SHELL and
// finally /bin/sh when the entry is missing or not executable; 'source'
// reports which one was used ("passwd", "environment" or "fallback").
QString loginShell(QString *source = nullptr);

// The user's home directory: $HOME, else the passwd database entry.
QString homeDirectory();

bool isExecutable(const QString &path);

// Human-readable name of a shell program: "Bash", "Zsh", "Fish", ...
QString displayName(const QString &path);

// Split a command line into an argument vector with the quoting rules of a
// POSIX shell ('...', "...", backslash) — without running a shell. Nothing is
// expanded. An unclosed quote yields an empty list and *ok == false.
QStringList splitCommand(const QString &commandLine, bool *ok = nullptr);

// Resolve argv[0] of a command to an executable file: absolute and relative
// paths are checked directly, bare names are searched in $PATH. Returns an
// empty string if there is no such program.
QString resolveProgram(const QString &program);

// Data behind "--dump shells".
QJsonObject toJson();

} // namespace Shells
