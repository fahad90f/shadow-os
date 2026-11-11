// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

// How this run was started. Filled in once by main().
struct RecoveryOptions
{
    // The engine. Production: /usr/sbin/nexus-recovery-engine, run as root
    // through pkexec (polkit action org.nexus.recovery.run).
    QString enginePath;
    // TEST MODE: the list of installations comes from this recorded scan
    // result. No engine is ever started in this mode (refused in C++).
    QString scanFile;
    bool runningAsRoot = false;     // effective uid 0: the engine is started directly

    bool testMode() const { return !scanFile.isEmpty(); }
    static RecoveryOptions &instance();
    static QString defaultEnginePath() { return QStringLiteral("/usr/sbin/nexus-recovery-engine"); }
    static QString pkexecPath() { return QStringLiteral("/usr/bin/pkexec"); }
};

// Front-end of nexus-recovery-engine.
//
// Everything that examines or changes a disk is done by the engine, one
// sub-command per step, started through pkexec with an argument vector;
// secrets (LUKS passphrase, new password) are written to its standard input
// and wiped here. The engine prints one JSON object per line: "step",
// "output" and "step-done" while it works (shown live in logText) and a
// final "result".
//
// One engine run at a time. Actions that need the installation mounted mount
// it first (the engine's own "mount" step, visible in the log); the file
// system check needs it unmounted and unmounts it first.
class Recovery : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // Why the engine cannot be used at all (not installed, no pkexec); empty when it can.
    Q_PROPERTY(QString engineProblem READ engineProblem CONSTANT)
    Q_PROPERTY(bool testMode READ testMode CONSTANT)
    Q_PROPERTY(bool liveSession READ liveSession NOTIFY scanChanged)

    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(bool scanned READ scanned NOTIFY scanChanged)
    Q_PROPERTY(QString scanError READ scanError NOTIFY scanChanged)
    Q_PROPERTY(QString scanHint READ scanHint NOTIFY scanChanged)
    // The engine's scan result: see nexus_recovery/scan.py.
    Q_PROPERTY(QVariantList installations READ installations NOTIFY scanChanged)
    Q_PROPERTY(QVariantList lockedVolumes READ lockedVolumes NOTIFY scanChanged)
    Q_PROPERTY(QVariantList inactiveLvm READ inactiveLvm NOTIFY scanChanged)
    Q_PROPERTY(QVariantList unreadable READ unreadable NOTIFY scanChanged)

    Q_PROPERTY(QString currentId READ currentId NOTIFY currentChanged)
    Q_PROPERTY(QVariantMap current READ current NOTIFY currentChanged)

    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyAction READ busyAction NOTIFY busyChanged)
    Q_PROPERTY(QString busyTitle READ busyTitle NOTIFY busyChanged)
    Q_PROPERTY(QString logText READ logText NOTIFY logChanged)
    Q_PROPERTY(QString engineLogFile READ engineLogFile NOTIFY logChanged)
    // action → { status: "ok" | "failed" | "warning", message, detail }
    Q_PROPERTY(QVariantMap results READ results NOTIFY resultsChanged)

    Q_PROPERTY(bool terminalAvailable READ terminalAvailable CONSTANT)
    Q_PROPERTY(bool canRestart READ canRestart NOTIFY canRestartChanged)
    Q_PROPERTY(QString restartProblem READ restartProblem NOTIFY canRestartChanged)

public:
    explicit Recovery(QObject *parent = nullptr);

    QString engineProblem() const { return m_engineProblem; }
    bool testMode() const { return RecoveryOptions::instance().testMode(); }
    bool liveSession() const { return m_scan.value(QLatin1String("live_session")).toBool(); }
    bool scanning() const { return m_scanning; }
    bool scanned() const { return m_scanned; }
    QString scanError() const { return m_scanError; }
    QString scanHint() const { return m_scanHint; }
    QVariantList installations() const;
    QVariantList lockedVolumes() const;
    QVariantList inactiveLvm() const;
    QVariantList unreadable() const;
    QString currentId() const { return m_currentId; }
    QVariantMap current() const;
    bool busy() const { return m_process != nullptr; }
    QString busyAction() const { return m_busyAction; }
    QString busyTitle() const { return m_busyTitle; }
    QString logText() const { return m_log; }
    QString engineLogFile() const { return m_engineLogFile; }
    QVariantMap results() const { return m_results; }
    bool terminalAvailable() const;
    bool canRestart() const { return m_canRestart; }
    QString restartProblem() const { return m_restartProblem; }

    Q_INVOKABLE void rescan();
    Q_INVOKABLE void select(const QString &id);
    Q_INVOKABLE void unlock(const QString &device, const QString &passphrase);
    Q_INVOKABLE void activateLvm(const QString &volumeGroup);
    Q_INVOKABLE void mountCurrent();
    Q_INVOKABLE void unmountCurrent();
    Q_INVOKABLE void checkFilesystems(bool repair);
    Q_INVOKABLE void repairBoot();
    Q_INVOKABLE void repairPackages();
    Q_INVOKABLE void resetPassword(const QString &user, const QString &password);
    // Why 'password' cannot be used (empty when it can). Checked again by resetPassword().
    Q_INVOKABLE QString passwordProblem(const QString &password, const QString &repeated) const;
    Q_INVOKABLE QString suggestedLogFile() const;
    Q_INVOKABLE void saveLogs(const QString &file);
    Q_INVOKABLE void openTerminal();
    // Unmounts and locks what Recovery opened, then asks logind to reboot.
    Q_INVOKABLE void restart();
    Q_INVOKABLE void copyLog() const;
    Q_INVOKABLE void clearLog();

    QJsonObject statusJson() const;
    QJsonObject scanJson() const;

Q_SIGNALS:
    void scanningChanged();
    void scanChanged();
    void scanFinished();
    void currentChanged();
    void busyChanged();
    void logChanged();
    void resultsChanged();
    void canRestartChanged();
    // For a toast: an action ended.
    void actionFinished(const QString &action, bool ok, const QString &message);

private:
    struct Job {
        QString action;             // key in results ("boot", "packages", "fsck", ...); empty for housekeeping
        QString title;
        QStringList arguments;
        QByteArray secret;          // written to the engine's stdin, then wiped
        bool quiet = false;         // do not write to the visible log (scan)
        // ok = the engine printed a result with ok: true
        std::function<void(bool ok, const QJsonObject &result, const QString &problem)> done;
    };

    void run(Job job);
    void readOutput();
    void handleLine(const QByteArray &line);
    void finishJob(int exitCode, QProcess::ExitStatus status, const QString &startError);
    void appendLog(const QString &text);
    void setResult(const QString &action, const QString &status, const QString &message, const QString &detail = {});
    void applyScan(const QJsonObject &result);
    void scanThen(const std::function<void()> &then);
    // Runs 'then' once the current installation is mounted (mounting it if needed).
    void whenMounted(const QString &action, const std::function<void()> &then);
    void whenUnmounted(const QString &action, const std::function<void()> &then);
    bool ready(const QString &action);
    static QString errorText(const QJsonObject &result, QString *hint = nullptr);
    void queryRestart();

    QString m_engineProblem;
    QJsonObject m_scan;
    bool m_scanning = false;
    bool m_scanned = false;
    QString m_scanError, m_scanHint;
    QString m_currentId;

    QProcess *m_process = nullptr;
    Job m_job;
    QByteArray m_buffer;
    QJsonObject m_jobResult;
    bool m_haveResult = false;
    bool m_viaPkexec = false;
    QString m_busyAction, m_busyTitle;
    QString m_log;
    QString m_engineLogFile;
    QVariantMap m_results;
    bool m_canRestart = false;
    QString m_restartProblem;
};
