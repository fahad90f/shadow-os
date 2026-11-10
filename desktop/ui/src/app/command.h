// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <functional>

namespace Nexus {

struct CommandResult
{
    int exitCode = -1;          // process exit code; -1 if it could not be started, -2 on timeout
    QByteArray out;
    QByteArray err;
    bool ok() const { return exitCode == 0; }
    QString outText() const { return QString::fromUtf8(out); }
    QString errText() const { return QString::fromUtf8(err); }
};

// Thin, safe wrapper around QProcess for the system back-ends.
//
// Commands are always executed with an argument vector — never through a
// shell — so values that originate from the UI (SSIDs, file names, package
// names) cannot be interpreted as shell syntax.
class Command
{
public:
    using Callback = std::function<void(const CommandResult &)>;

    // Asynchronous: 'done' is invoked on context's thread; nothing happens if
    // context is destroyed first.
    static void run(QObject *context, const QString &program, const QStringList &arguments,
                    Callback done, int timeoutMs = 30000, const QByteArray &stdinData = {});

    // Synchronous: only for worker threads or commands known to return at once.
    static CommandResult runSync(const QString &program, const QStringList &arguments,
                                 int timeoutMs = 10000, const QByteArray &stdinData = {});

    // True if 'program' resolves in PATH (or is an existing absolute path).
    static bool available(const QString &program);

    // Environment for child tools: C locale so output parsing is stable.
    static QProcessEnvironment parseEnvironment();
};

} // namespace Nexus
