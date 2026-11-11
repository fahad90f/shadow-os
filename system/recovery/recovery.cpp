// SPDX-License-Identifier: GPL-3.0-or-later
#include "recovery.h"

#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTimer>

#include <cstring>

namespace {

// pkexec(1): 126 — not authorised or the dialog was dismissed; 127 — the
// authorisation could not be obtained or the program could not be run.
constexpr int kPkexecDismissed = 126;
constexpr int kPkexecFailed = 127;
constexpr int kMaxLogChars = 400000;

void wipe(QByteArray &bytes)
{
    if (!bytes.isEmpty())
        explicit_bzero(bytes.data(), size_t(bytes.size()));
    bytes.clear();
}

QString lastLines(const QByteArray &text, int count)
{
    const QStringList lines = QString::fromUtf8(text).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.mid(qMax(0, lines.size() - count)).join(QLatin1Char('\n'));
}

} // namespace

RecoveryOptions &RecoveryOptions::instance()
{
    static RecoveryOptions options;
    return options;
}

Recovery::Recovery(QObject *parent)
    : QObject(parent)
{
    const RecoveryOptions &o = RecoveryOptions::instance();
    if (!o.testMode()) {
        if (!QFileInfo(o.enginePath).isFile())
            m_engineProblem = tr("The recovery engine %1 is not installed (package “nexus-recovery”).").arg(o.enginePath);
        else if (!o.runningAsRoot && !QFileInfo(RecoveryOptions::pkexecPath()).isExecutable())
            m_engineProblem = tr("%1 is missing (package “pkexec”), so NEXUS Recovery cannot ask for administrator "
                                 "rights to run its engine.").arg(RecoveryOptions::pkexecPath());
    }
    queryRestart();
}

// ---- data ---------------------------------------------------------------------------

QVariantList Recovery::installations() const { return m_scan.value(QLatin1String("installations")).toArray().toVariantList(); }
QVariantList Recovery::lockedVolumes() const { return m_scan.value(QLatin1String("locked_volumes")).toArray().toVariantList(); }
QVariantList Recovery::inactiveLvm() const { return m_scan.value(QLatin1String("inactive_lvm")).toArray().toVariantList(); }
QVariantList Recovery::unreadable() const { return m_scan.value(QLatin1String("unreadable")).toArray().toVariantList(); }

QVariantMap Recovery::current() const
{
    const QJsonArray all = m_scan.value(QLatin1String("installations")).toArray();
    for (const QJsonValue &value : all) {
        if (value.toObject().value(QLatin1String("id")).toString() == m_currentId && !m_currentId.isEmpty())
            return value.toObject().toVariantMap();
    }
    return {};
}

bool Recovery::terminalAvailable() const
{
    return !QStandardPaths::findExecutable(QStringLiteral("nexus-terminal")).isEmpty();
}

QString Recovery::shellCommand() const
{
    if (m_currentId.isEmpty())
        return {};
    return QStringLiteral("sudo nexus-recovery-engine shell --target %1 --exec").arg(m_currentId);
}

// ---- the engine ---------------------------------------------------------------------

void Recovery::appendLog(const QString &text)
{
    m_log += text;
    if (!text.endsWith(QLatin1Char('\n')))
        m_log += QLatin1Char('\n');
    if (m_log.size() > kMaxLogChars)
        m_log = m_log.right(kMaxLogChars / 2);
    Q_EMIT logChanged();
}

void Recovery::clearLog()
{
    m_log.clear();
    Q_EMIT logChanged();
}

void Recovery::copyLog() const
{
    QGuiApplication::clipboard()->setText(m_log);
}

void Recovery::setResult(const QString &action, const QString &status, const QString &message, const QString &detail)
{
    if (action.isEmpty())
        return;
    m_results.insert(action, QVariantMap{{QStringLiteral("status"), status}, {QStringLiteral("message"), message},
                                         {QStringLiteral("detail"), detail}});
    Q_EMIT resultsChanged();
    Q_EMIT actionFinished(action, status != QLatin1String("failed"), message);
}

QString Recovery::errorText(const QJsonObject &result, QString *hint)
{
    const QJsonObject error = result.value(QLatin1String("error")).toObject();
    if (hint)
        *hint = error.value(QLatin1String("hint")).toString();
    return error.value(QLatin1String("message")).toString();
}

void Recovery::run(Job job)
{
    const RecoveryOptions &o = RecoveryOptions::instance();
    QString problem = m_engineProblem;
    if (o.testMode())
        problem = tr("Test mode (--scan-file): the engine is not run and nothing is changed.");
    else if (m_process)
        problem = tr("Another step is still running.");
    if (!problem.isEmpty()) {
        wipe(job.secret);
        const auto done = job.done;
        QTimer::singleShot(0, this, [done, problem] { if (done) done(false, {}, problem); });
        return;
    }

    m_job = std::move(job);
    m_buffer.clear();
    m_jobResult = {};
    m_haveResult = false;
    m_viaPkexec = !o.runningAsRoot;
    m_process = new QProcess(this);
    if (m_viaPkexec) {
        m_process->setProgram(RecoveryOptions::pkexecPath());
        m_process->setArguments(QStringList{o.enginePath} + m_job.arguments);
    } else {
        m_process->setProgram(o.enginePath);
        m_process->setArguments(m_job.arguments);
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    m_process->setProcessEnvironment(env);
    m_busyAction = m_job.action;
    m_busyTitle = m_job.title;
    if (!m_job.quiet)
        appendLog(QStringLiteral("\n== %1").arg(m_job.title));

    connect(m_process, &QProcess::readyReadStandardOutput, this, &Recovery::readOutput);
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        finishJob(exitCode, status, QString());
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_process) {
            const QString message = tr("%1 could not be started: %2").arg(m_process->program(), m_process->errorString());
            // errorOccurred can fire inside start(); finish after it has returned.
            QTimer::singleShot(0, this, [this, message] { finishJob(-1, QProcess::NormalExit, message); });
        }
    });
    Q_EMIT busyChanged();
    m_process->start();
    if (m_process->state() != QProcess::NotRunning) {
        if (!m_job.secret.isEmpty())
            m_process->write(m_job.secret + '\n');
        m_process->closeWriteChannel();
    }
    wipe(m_job.secret);
}

void Recovery::readOutput()
{
    if (!m_process)
        return;
    m_buffer += m_process->readAllStandardOutput();
    qsizetype newline;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        handleLine(line);
    }
}

void Recovery::handleLine(const QByteArray &line)
{
    const QJsonObject object = QJsonDocument::fromJson(line).object();
    const QString event = object.value(QLatin1String("event")).toString();
    if (event == QLatin1String("result")) {
        m_jobResult = object;
        m_haveResult = true;
        const QString log = object.value(QLatin1String("log")).toString();
        if (!log.isEmpty() && log != m_engineLogFile)
            m_engineLogFile = log;
        return;
    }
    if (m_job.quiet)
        return;
    if (event == QLatin1String("step")) {
        appendLog(QStringLiteral("-- %1\n   $ %2%3").arg(object.value(QLatin1String("title")).toString(),
                                                         object.value(QLatin1String("command")).toString(),
                                                         object.value(QLatin1String("dry_run")).toBool() ? tr("   (not run)") : QString()));
    } else if (event == QLatin1String("output")) {
        appendLog(QStringLiteral("   ") + object.value(QLatin1String("line")).toString());
    } else if (event == QLatin1String("step-done")) {
        const QString status = object.value(QLatin1String("status")).toString();
        const int code = object.value(QLatin1String("exit")).toInt();
        appendLog(status == QLatin1String("ok") ? tr("   done")
                  : status == QLatin1String("skipped") ? tr("   exit status %1 (not required)").arg(code)
                  : tr("   FAILED (exit status %1)").arg(code));
    } else if (event.isEmpty() && !line.trimmed().isEmpty()) {
        appendLog(QString::fromUtf8(line));     // not JSON: show it rather than lose it
    }
}

void Recovery::finishJob(int exitCode, QProcess::ExitStatus status, const QString &startError)
{
    if (!m_process)
        return;
    readOutput();
    if (!m_buffer.trimmed().isEmpty())
        handleLine(m_buffer);
    const QByteArray errors = m_process->readAllStandardError();
    m_process->deleteLater();
    m_process = nullptr;

    const bool ok = m_haveResult && m_jobResult.value(QLatin1String("ok")).toBool();
    QString problem;
    if (!ok) {
        if (!startError.isEmpty()) {
            problem = startError;
        } else if (m_haveResult) {
            QString hint;
            problem = errorText(m_jobResult, &hint);
            if (!hint.isEmpty())
                problem += QLatin1Char(' ') + hint;
            if (problem.trimmed().isEmpty())
                problem = tr("The step did not succeed; see the output below.");
        } else if (m_viaPkexec && exitCode == kPkexecDismissed) {
            problem = tr("Administrator rights were not granted: the authentication dialog was dismissed or this "
                         "account is not allowed to run NEXUS Recovery. Nothing was changed.");
        } else if (m_viaPkexec && exitCode == kPkexecFailed) {
            problem = tr("Administrator rights could not be obtained (pkexec exit status 127): the password was "
                         "wrong, no authentication agent is running in this session, or the engine could not be "
                         "executed. Nothing was changed.");
        } else if (status == QProcess::CrashExit) {
            problem = tr("The recovery engine stopped unexpectedly.") + QLatin1Char(' ') + lastLines(errors, 4);
        } else {
            problem = tr("The recovery engine ended without a result (exit status %1).").arg(exitCode)
                      + QLatin1Char(' ') + lastLines(errors, 4);
        }
        if (!m_job.quiet)
            appendLog(QStringLiteral("!! ") + problem);
    }
    const Job job = std::move(m_job);
    const QJsonObject result = m_jobResult;
    m_job = Job();
    m_busyAction.clear();
    m_busyTitle.clear();
    Q_EMIT logChanged();
    Q_EMIT busyChanged();
    if (job.done)
        job.done(ok, result, problem);
}

// ---- scan ---------------------------------------------------------------------------

void Recovery::applyScan(const QJsonObject &result)
{
    m_scan = result;
    m_scanned = true;
    m_scanError.clear();
    m_scanHint.clear();
    const QJsonArray all = result.value(QLatin1String("installations")).toArray();
    bool stillThere = false;
    QStringList nexus;
    for (const QJsonValue &value : all) {
        const QJsonObject installation = value.toObject();
        stillThere = stillThere || installation.value(QLatin1String("id")).toString() == m_currentId;
        if (installation.value(QLatin1String("is_nexus")).toBool())
            nexus << installation.value(QLatin1String("id")).toString();
    }
    if (!stillThere)
        m_currentId = nexus.size() == 1 ? nexus.constFirst() : QString();    // the obvious choice, or none
    Q_EMIT scanChanged();
    Q_EMIT currentChanged();
}

void Recovery::scanThen(const std::function<void()> &then)
{
    const RecoveryOptions &o = RecoveryOptions::instance();
    if (o.testMode()) {
        QFile file(o.scanFile);
        QJsonObject result;
        if (file.open(QIODevice::ReadOnly)) {
            // A recorded engine output: its last line is the result.
            const QList<QByteArray> lines = file.readAll().trimmed().split('\n');
            result = QJsonDocument::fromJson(lines.constLast()).object();
        }
        if (result.contains(QLatin1String("installations"))) {
            applyScan(result);
        } else {
            m_scanError = tr("%1 is not a recorded scan result.").arg(o.scanFile);
            m_scanned = true;
            Q_EMIT scanChanged();
        }
        Q_EMIT scanFinished();
        if (then)
            then();
        return;
    }
    m_scanning = true;
    Q_EMIT scanningChanged();
    Job job;
    job.title = tr("Look for installed systems");
    job.arguments = {QStringLiteral("scan")};
    job.quiet = true;
    job.done = [this, then](bool ok, const QJsonObject &result, const QString &problem) {
        m_scanning = false;
        if (ok) {
            applyScan(result);
        } else {
            m_scanError = problem;
            m_scanHint.clear();
            m_scanned = true;
            Q_EMIT scanChanged();
        }
        Q_EMIT scanningChanged();
        Q_EMIT scanFinished();
        if (then)
            then();
    };
    run(std::move(job));
}

void Recovery::rescan()
{
    if (busy() || m_scanning)
        return;
    scanThen({});
}

void Recovery::select(const QString &id)
{
    if (busy() || id == m_currentId)
        return;
    const QJsonArray all = m_scan.value(QLatin1String("installations")).toArray();
    for (const QJsonValue &value : all) {
        const QJsonObject installation = value.toObject();
        if (installation.value(QLatin1String("id")).toString() == id && installation.value(QLatin1String("is_nexus")).toBool()) {
            m_currentId = id;
            m_results.clear();
            Q_EMIT resultsChanged();
            Q_EMIT currentChanged();
            return;
        }
    }
}

// ---- prerequisites ------------------------------------------------------------------

bool Recovery::ready(const QString &action)
{
    QString problem;
    if (busy())
        problem = tr("Another step is still running.");
    else if (m_currentId.isEmpty() || current().isEmpty())
        problem = tr("Choose an installation first.");
    if (problem.isEmpty())
        return true;
    setResult(action, QStringLiteral("failed"), problem);
    return false;
}

void Recovery::whenMounted(const QString &action, const std::function<void()> &then)
{
    const QVariantMap installation = current();
    if (installation.value(QStringLiteral("running")).toBool()
        || !installation.value(QStringLiteral("mounted_by_recovery")).toString().isEmpty()) {
        then();
        return;
    }
    Job job;
    job.action = action;
    job.title = tr("Mount the installation");
    job.arguments = {QStringLiteral("mount"), QStringLiteral("--target"), m_currentId};
    job.done = [this, action, then](bool ok, const QJsonObject &result, const QString &problem) {
        if (!ok) {
            setResult(action, QStringLiteral("failed"), tr("The installation could not be mounted. %1").arg(problem));
            return;
        }
        appendLog(tr("   mounted at %1").arg(result.value(QLatin1String("mountpoint")).toString()));
        const QJsonArray warnings = result.value(QLatin1String("warnings")).toArray();
        for (const QJsonValue &warning : warnings)
            appendLog(tr("   warning: %1").arg(warning.toString()));
        scanThen(then);
    };
    run(std::move(job));
}

void Recovery::whenUnmounted(const QString &action, const std::function<void()> &then)
{
    if (current().value(QStringLiteral("mounted_by_recovery")).toString().isEmpty()) {
        then();
        return;
    }
    Job job;
    job.action = action;
    job.title = tr("Unmount the installation");
    job.arguments = {QStringLiteral("umount"), QStringLiteral("--target"), m_currentId};
    job.done = [this, action, then](bool ok, const QJsonObject &, const QString &problem) {
        if (!ok) {
            setResult(action, QStringLiteral("failed"), tr("The installation could not be unmounted. %1").arg(problem));
            scanThen({});
            return;
        }
        scanThen(then);
    };
    run(std::move(job));
}

void Recovery::mountCurrent()
{
    if (!ready(QStringLiteral("mount")))
        return;
    whenMounted(QStringLiteral("mount"), [this] {
        setResult(QStringLiteral("mount"), QStringLiteral("ok"),
                  tr("Mounted at %1.").arg(current().value(QStringLiteral("mounted_by_recovery")).toString()));
    });
}

void Recovery::unmountCurrent()
{
    if (!ready(QStringLiteral("mount")))
        return;
    whenUnmounted(QStringLiteral("mount"), [this] {
        setResult(QStringLiteral("mount"), QStringLiteral("ok"), tr("The installation is unmounted."));
    });
}

// ---- actions ------------------------------------------------------------------------

void Recovery::unlock(const QString &device, const QString &passphrase)
{
    if (busy())
        return;
    Job job;
    job.action = QStringLiteral("unlock");
    job.title = tr("Unlock the encrypted volume %1").arg(device);
    job.arguments = {QStringLiteral("unlock"), QStringLiteral("--device"), device};
    job.secret = passphrase.toUtf8();
    if (job.secret.isEmpty() || job.secret.contains('\n')) {
        setResult(job.action, QStringLiteral("failed"), tr("Enter the passphrase of the encrypted volume."));
        return;
    }
    job.done = [this, device](bool ok, const QJsonObject &result, const QString &problem) {
        if (!ok) {
            setResult(QStringLiteral("unlock"), QStringLiteral("failed"), problem);
            return;
        }
        const int found = int(result.value(QLatin1String("installations")).toArray().size());
        scanThen([this, device, found] {
            setResult(QStringLiteral("unlock"), found > 0 ? QStringLiteral("ok") : QStringLiteral("warning"),
                      found > 0 ? tr("%1 is unlocked.").arg(device)
                                : tr("%1 is unlocked, but no installed system was found on it.").arg(device));
        });
    };
    run(std::move(job));
}

void Recovery::activateLvm(const QString &volumeGroup)
{
    if (busy())
        return;
    Job job;
    job.action = QStringLiteral("lvm");
    job.title = tr("Activate the volume group %1").arg(volumeGroup);
    job.arguments = {QStringLiteral("activate-lvm"), QStringLiteral("--volume-group"), volumeGroup};
    job.done = [this, volumeGroup](bool ok, const QJsonObject &, const QString &problem) {
        if (!ok) {
            setResult(QStringLiteral("lvm"), QStringLiteral("failed"), problem);
            return;
        }
        scanThen([this, volumeGroup] {
            setResult(QStringLiteral("lvm"), QStringLiteral("ok"), tr("The volume group %1 is active.").arg(volumeGroup));
        });
    };
    run(std::move(job));
}

void Recovery::checkFilesystems(bool repair)
{
    const QString action = QStringLiteral("fsck");
    if (!ready(action))
        return;
    if (current().value(QStringLiteral("running")).toBool()) {
        setResult(action, QStringLiteral("failed"),
                  tr("The file systems of the running system are in use and cannot be checked from here. "
                     "Start the computer from the NEXUS OS medium and choose Recovery."));
        return;
    }
    whenUnmounted(action, [this, action, repair] {
        Job job;
        job.action = action;
        job.title = repair ? tr("Check and repair the file systems") : tr("Check the file systems");
        job.arguments = {QStringLiteral("fsck"), QStringLiteral("--target"), m_currentId};
        if (repair)
            job.arguments << QStringLiteral("--repair");
        job.done = [this, action, repair](bool ok, const QJsonObject &result, const QString &problem) {
            const QJsonArray filesystems = result.value(QLatin1String("filesystems")).toArray();
            if (filesystems.isEmpty()) {
                setResult(action, QStringLiteral("failed"), problem);
                return;
            }
            QStringList clean, repaired, broken, skipped;
            for (const QJsonValue &value : filesystems) {
                const QJsonObject fs = value.toObject();
                const QString name = QStringLiteral("%1 (%2)").arg(fs.value(QLatin1String("device")).toString(),
                                                                   fs.value(QLatin1String("mountpoint")).toString());
                const QString status = fs.value(QLatin1String("status")).toString();
                if (status == QLatin1String("clean")) clean << name;
                else if (status == QLatin1String("repaired")) repaired << name;
                else if (status == QLatin1String("errors") || status == QLatin1String("repair-failed")) broken << name;
                else skipped << name + QStringLiteral(": ") + fs.value(QLatin1String("detail")).toString();
            }
            QStringList parts;
            if (!broken.isEmpty())
                parts << (repair ? tr("Could not be repaired: %1.") : tr("Errors found on %1.")).arg(broken.join(QStringLiteral(", ")));
            if (!repaired.isEmpty())
                parts << tr("Repaired: %1.").arg(repaired.join(QStringLiteral(", ")));
            if (!clean.isEmpty())
                parts << tr("No errors: %1.").arg(clean.join(QStringLiteral(", ")));
            if (!skipped.isEmpty())
                parts << tr("Not checked: %1.").arg(skipped.join(QStringLiteral("; ")));
            m_results.insert(action, QVariantMap{
                {QStringLiteral("status"), !broken.isEmpty() ? QStringLiteral("failed") : !skipped.isEmpty() ? QStringLiteral("warning") : QStringLiteral("ok")},
                {QStringLiteral("message"), parts.join(QLatin1Char(' '))},
                {QStringLiteral("detail"), QString()},
                {QStringLiteral("errorsFound"), !broken.isEmpty() && !repair},
            });
            Q_EMIT resultsChanged();
            Q_EMIT actionFinished(action, ok, parts.join(QLatin1Char(' ')));
            scanThen({});
        };
        run(std::move(job));
    });
}

void Recovery::repairBoot()
{
    const QString action = QStringLiteral("boot");
    if (!ready(action))
        return;
    whenMounted(action, [this, action] {
        Job job;
        job.action = action;
        job.title = tr("Repair the boot loader");
        job.arguments = {QStringLiteral("repair-boot"), QStringLiteral("--target"), m_currentId};
        job.done = [this, action](bool ok, const QJsonObject &result, const QString &problem) {
            QStringList warnings;
            const QJsonArray list = result.value(QLatin1String("warnings")).toArray();
            for (const QJsonValue &warning : list)
                warnings << warning.toString();
            if (!ok) {
                const QString step = result.value(QLatin1String("failed_step")).toString();
                setResult(action, QStringLiteral("failed"),
                          step.isEmpty() ? problem : tr("The step “%1” failed; the steps after it were not run. See the output.").arg(step));
            } else {
                setResult(action, warnings.isEmpty() ? QStringLiteral("ok") : QStringLiteral("warning"),
                          tr("The initial RAM disks, the boot loader and the boot menu were rebuilt.")
                              + (warnings.isEmpty() ? QString() : QLatin1Char(' ') + warnings.join(QLatin1Char(' '))));
            }
            scanThen({});
        };
        run(std::move(job));
    });
}

void Recovery::repairPackages()
{
    const QString action = QStringLiteral("packages");
    if (!ready(action))
        return;
    whenMounted(action, [this, action] {
        Job job;
        job.action = action;
        job.title = tr("Repair the package system");
        job.arguments = {QStringLiteral("repair-packages"), QStringLiteral("--target"), m_currentId};
        job.done = [this, action](bool ok, const QJsonObject &result, const QString &problem) {
            const QString step = result.value(QLatin1String("failed_step")).toString();
            if (ok)
                setResult(action, QStringLiteral("ok"), tr("Interrupted installations were finished and dependencies are consistent."));
            else
                setResult(action, QStringLiteral("failed"),
                          step.isEmpty() ? problem : tr("The step “%1” failed. See the output.").arg(step));
            scanThen({});
        };
        run(std::move(job));
    });
}

QString Recovery::passwordProblem(const QString &password, const QString &repeated) const
{
    if (password.isEmpty())
        return tr("Enter a new password.");
    if (password.contains(QLatin1Char('\n')) || password.contains(QLatin1Char('\r')))
        return tr("The password must be a single line.");
    if (password.toUtf8().size() > 1024)
        return tr("The password is too long.");
    if (password != repeated)
        return tr("The two entries are not the same.");
    return {};
}

void Recovery::resetPassword(const QString &user, const QString &password)
{
    const QString action = QStringLiteral("password");
    if (!ready(action))
        return;
    const QString problem = passwordProblem(password, password);
    if (!problem.isEmpty()) {
        setResult(action, QStringLiteral("failed"), problem);
        return;
    }
    // Held here only until the engine is started (after a possible mount).
    auto secret = std::make_shared<QByteArray>(password.toUtf8());
    whenMounted(action, [this, action, user, secret] {
        Job job;
        job.action = action;
        job.title = tr("Set a new password for %1").arg(user);
        job.arguments = {QStringLiteral("reset-password"), QStringLiteral("--target"), m_currentId,
                         QStringLiteral("--user"), user};
        job.secret = *secret;
        wipe(*secret);
        job.done = [this, action, user](bool ok, const QJsonObject &, const QString &failure) {
            setResult(action, ok ? QStringLiteral("ok") : QStringLiteral("failed"),
                      ok ? tr("%1 has a new password.").arg(user) : failure);
            scanThen({});
        };
        run(std::move(job));
    });
}

QString Recovery::suggestedLogFile() const
{
    return QDir::homePath() + QStringLiteral("/nexus-recovery-logs-")
           + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QStringLiteral(".tar.gz");
}

void Recovery::saveLogs(const QString &file)
{
    const QString action = QStringLiteral("logs");
    if (!ready(action))
        return;
    QString path = file.trimmed();
    if (path.startsWith(QStringLiteral("~/")))
        path = QDir::homePath() + path.mid(1);
    if (!path.endsWith(QStringLiteral(".tar.gz")))
        path += QStringLiteral(".tar.gz");
    QString problem;
    if (!QDir::isAbsolutePath(path))
        problem = tr("Give the file with its full path, for example %1.").arg(suggestedLogFile());
    else if (!QFileInfo(QFileInfo(path).absolutePath()).isDir())
        problem = tr("The folder %1 does not exist.").arg(QFileInfo(path).absolutePath());
    else if (QFileInfo::exists(path))
        problem = tr("%1 already exists; choose another name.").arg(path);
    if (!problem.isEmpty()) {
        setResult(action, QStringLiteral("failed"), problem);
        return;
    }
    whenMounted(action, [this, action, path] {
        Job job;
        job.action = action;
        job.title = tr("Collect the logs of the installation");
        job.arguments = {QStringLiteral("logs"), QStringLiteral("--target"), m_currentId, QStringLiteral("--output"), path};
        job.done = [this, action, path](bool ok, const QJsonObject &result, const QString &failure) {
            if (ok) {
                appendLog(tr("   %1 files, %2 bytes").arg(result.value(QLatin1String("files")).toInt())
                                                      .arg(result.value(QLatin1String("size")).toInteger()));
                const QJsonArray missing = result.value(QLatin1String("not_included")).toArray();
                for (const QJsonValue &value : missing)
                    appendLog(tr("   not included: %1").arg(value.toString()));
            }
            setResult(action, ok ? QStringLiteral("ok") : QStringLiteral("failed"),
                      ok ? tr("Saved to %1 (%2 files).").arg(path).arg(result.value(QLatin1String("files")).toInt()) : failure);
            scanThen({});
        };
        run(std::move(job));
    });
}

void Recovery::openTerminal()
{
    const QString action = QStringLiteral("terminal");
    if (!ready(action))
        return;
    if (current().value(QStringLiteral("running")).toBool()) {
        setResult(action, QStringLiteral("failed"), tr("This is the running system; any terminal is already inside it."));
        return;
    }
    if (!terminalAvailable()) {
        setResult(action, QStringLiteral("failed"), tr("NEXUS Terminal (nexus-terminal) is not installed."));
        return;
    }
    whenMounted(action, [this, action] {
        const RecoveryOptions &o = RecoveryOptions::instance();
        // The shell is started by the engine (the one program polkit lets
        // this application run as root), inside the terminal.
        QStringList arguments{QStringLiteral("-e")};
        if (!o.runningAsRoot)
            arguments << RecoveryOptions::pkexecPath();
        arguments << o.enginePath << QStringLiteral("shell") << QStringLiteral("--target") << m_currentId << QStringLiteral("--exec");
        const bool started = QProcess::startDetached(QStringLiteral("nexus-terminal"), arguments);
        appendLog(QStringLiteral("\n== %1\n   $ nexus-terminal %2").arg(tr("Open a terminal in the installed system"), arguments.join(QLatin1Char(' '))));
        setResult(action, started ? QStringLiteral("ok") : QStringLiteral("failed"),
                  started ? tr("A terminal with a root shell in the installed system was opened. Close it before unmounting.")
                          : tr("NEXUS Terminal could not be started."));
    });
}

// ---- restart ------------------------------------------------------------------------

void Recovery::queryRestart()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        m_restartProblem = tr("The system bus is not reachable, so the computer cannot be restarted from here.");
        return;
    }
    const QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"), QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("CanReboot"));
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        if (reply.isError()) {
            m_restartProblem = tr("logind did not answer (%1), so the computer cannot be restarted from here.").arg(reply.error().message());
        } else if (reply.value() == QLatin1String("yes") || reply.value() == QLatin1String("challenge")) {
            m_canRestart = true;
            m_restartProblem.clear();
        } else {
            m_restartProblem = tr("Restarting is not allowed for this session (logind: %1).").arg(reply.value());
        }
        Q_EMIT canRestartChanged();
    });
}

void Recovery::restart()
{
    if (busy() || !m_canRestart)
        return;
    // Unmount every installation Recovery mounted, close what it unlocked,
    // then hand over to logind. Any failure stops the restart.
    QStringList mounted;
    const QJsonArray all = m_scan.value(QLatin1String("installations")).toArray();
    for (const QJsonValue &value : all) {
        if (!value.toObject().value(QLatin1String("mounted_by_recovery")).toString().isEmpty())
            mounted << value.toObject().value(QLatin1String("id")).toString();
    }
    auto reboot = [this] {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.login1"),
            QStringLiteral("/org/freedesktop/login1"), QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("Reboot"));
        call << true;
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            if (w->isError())
                setResult(QStringLiteral("restart"), QStringLiteral("failed"), tr("The restart was refused: %1").arg(w->error().message()));
        });
    };
    auto lockThenReboot = [this, reboot] {
        Job job;
        job.action = QStringLiteral("restart");
        job.title = tr("Close the encrypted volumes that were unlocked here");
        job.arguments = {QStringLiteral("lock")};
        job.done = [this, reboot](bool ok, const QJsonObject &, const QString &problem) {
            if (!ok) {
                setResult(QStringLiteral("restart"), QStringLiteral("failed"), problem);
                scanThen({});
                return;
            }
            reboot();
        };
        run(std::move(job));
    };
    auto next = std::make_shared<std::function<void(QStringList)>>();
    *next = [this, next, lockThenReboot](QStringList remaining) {
        if (remaining.isEmpty()) {
            lockThenReboot();
            return;
        }
        Job job;
        job.action = QStringLiteral("restart");
        job.title = tr("Unmount the installation");
        job.arguments = {QStringLiteral("umount"), QStringLiteral("--target"), remaining.takeFirst()};
        job.done = [this, next, remaining](bool ok, const QJsonObject &, const QString &problem) {
            if (!ok) {
                setResult(QStringLiteral("restart"), QStringLiteral("failed"),
                          tr("The installation could not be unmounted, so the computer was not restarted. %1").arg(problem));
                scanThen({});
                return;
            }
            (*next)(remaining);
        };
        run(std::move(job));
    };
    (*next)(mounted);
}

// ---- diagnostics --------------------------------------------------------------------

QJsonObject Recovery::statusJson() const
{
    const RecoveryOptions &o = RecoveryOptions::instance();
    return QJsonObject{
        {QStringLiteral("engine"), o.enginePath},
        {QStringLiteral("engineInstalled"), QFileInfo(o.enginePath).isFile()},
        {QStringLiteral("engineProblem"), m_engineProblem},
        {QStringLiteral("pkexec"), QFileInfo(RecoveryOptions::pkexecPath()).isExecutable()},
        {QStringLiteral("runningAsRoot"), o.runningAsRoot},
        {QStringLiteral("testMode"), o.testMode()},
        {QStringLiteral("terminalAvailable"), terminalAvailable()},
        {QStringLiteral("liveSession"), QFileInfo::exists(QStringLiteral("/run/nexus/live-session"))},
    };
}

QJsonObject Recovery::scanJson() const
{
    return QJsonObject{
        {QStringLiteral("ok"), m_scanned && m_scanError.isEmpty() && m_engineProblem.isEmpty()},
        {QStringLiteral("error"), m_engineProblem.isEmpty() ? m_scanError : m_engineProblem},
        {QStringLiteral("scan"), m_scan},
    };
}
