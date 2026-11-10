// SPDX-License-Identifier: GPL-3.0-or-later
#include "command.h"

#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>

namespace Nexus {

QProcessEnvironment Command::parseEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LANGUAGE"), QStringLiteral("C"));
    return env;
}

bool Command::available(const QString &program)
{
    if (program.startsWith(QLatin1Char('/')))
        return QFileInfo(program).isExecutable();
    return !QStandardPaths::findExecutable(program).isEmpty();
}

void Command::run(QObject *context, const QString &program, const QStringList &arguments,
                  Callback done, int timeoutMs, const QByteArray &stdinData)
{
    auto *process = new QProcess(context);
    process->setProcessEnvironment(parseEnvironment());
    process->setProgram(program);
    process->setArguments(arguments);

    auto finished = std::make_shared<bool>(false);
    auto finish = [process, done, finished](int code) {
        if (*finished)
            return;
        *finished = true;
        CommandResult r;
        r.exitCode = code;
        r.out = process->readAllStandardOutput();
        r.err = process->readAllStandardError();
        process->deleteLater();
        if (done)
            done(r);
    };

    QObject::connect(process, &QProcess::finished, context,
                     [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? code : -1);
    });
    QObject::connect(process, &QProcess::errorOccurred, context, [finish, process](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            process->setProperty("nexusFailedToStart", true);
            finish(-1);
        }
    });

    if (timeoutMs > 0) {
        QTimer::singleShot(timeoutMs, process, [process, finish] {
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(1000);
                finish(-2);
            }
        });
    }

    process->start();
    if (!stdinData.isEmpty())
        process->write(stdinData);
    process->closeWriteChannel();
}

CommandResult Command::runSync(const QString &program, const QStringList &arguments,
                               int timeoutMs, const QByteArray &stdinData)
{
    CommandResult r;
    QProcess process;
    process.setProcessEnvironment(parseEnvironment());
    process.start(program, arguments);
    if (!process.waitForStarted(5000))
        return r;
    if (!stdinData.isEmpty())
        process.write(stdinData);
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        r.exitCode = -2;
    } else {
        r.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    }
    r.out = process.readAllStandardOutput();
    r.err = process.readAllStandardError();
    return r;
}

} // namespace Nexus
