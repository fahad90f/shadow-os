// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "secret.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

// The wizard's connection to the installation engine
// (system/installer/engine, /usr/sbin/shadow-installer-engine).
//
// The engine is a root-only program with three verbs. This class starts it
// with an argument vector — through pkexec (polkit action
// org.shadow.installer.run) when the installer runs as an ordinary user —
// and turns its output into properties:
//
//   probe                       → probe (JSON report of the machine)
//   plan --answers -            → plan (exact operations + SHA-256)
//   install --answers - --confirm <hash>
//                               → JSON-lines events: step, progress, log,
//                                 error, done; exit 0 / 1 / 2 / 3 (plan changed)
//
// The answer document is written to the engine's standard input; it is the
// only place the user's password and the encryption passphrase ever go. A
// copy WITHOUT those two values is kept, mode 0600, in a private directory
// under $XDG_RUNTIME_DIR as a record of what was requested.
//
// Every way the engine can fail — not installed, pkexec missing, the
// authentication dialog dismissed, non-zero exit, output that is not JSON,
// no answer within the time limit, death by signal — ends in a state with a
// message; nothing waits forever.
class Installer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // ---- how the installer was started ------------------------------------
    Q_PROPERTY(bool testMode READ testMode CONSTANT)
    Q_PROPERTY(bool simulation READ simulation CONSTANT)
    Q_PROPERTY(bool liveSession READ liveSession CONSTANT)
    Q_PROPERTY(QString startStep READ startStep CONSTANT)
    Q_PROPERTY(QVariantMap preset READ preset CONSTANT)
    Q_PROPERTY(QString probeFile READ probeFile CONSTANT)
    Q_PROPERTY(QString enginePath READ enginePath CONSTANT)
    // Empty when the destructive phase may run; otherwise why it is refused.
    Q_PROPERTY(QString installBlockedReason READ installBlockedReason CONSTANT)

    // ---- probe -----------------------------------------------------------
    // "idle" | "running" | "ready" | "failed"
    Q_PROPERTY(QString probeState READ probeState NOTIFY probeChanged)
    Q_PROPERTY(QVariantMap probe READ probe NOTIFY probeChanged)
    Q_PROPERTY(QString probeError READ probeError NOTIFY probeChanged)
    Q_PROPERTY(QString probeErrorDetail READ probeErrorDetail NOTIFY probeChanged)
    Q_PROPERTY(bool scanningSystems READ scanningSystems NOTIFY probeChanged)

    // ---- plan --------------------------------------------------------------
    Q_PROPERTY(QString planState READ planState NOTIFY planChanged)
    Q_PROPERTY(QVariantMap plan READ plan NOTIFY planChanged)
    Q_PROPERTY(QString planError READ planError NOTIFY planChanged)
    Q_PROPERTY(QString planHint READ planHint NOTIFY planChanged)
    Q_PROPERTY(QString answersFile READ answersFile NOTIFY planChanged)

    // ---- install -----------------------------------------------------------
    // "idle" | "running" | "succeeded" | "failed" | "plan-changed"
    Q_PROPERTY(QString installState READ installState NOTIFY installChanged)
    Q_PROPERTY(QVariantList steps READ steps NOTIFY installChanged)     // [{ id, title, state }]
    Q_PROPERTY(int stepIndex READ stepIndex NOTIFY installChanged)
    Q_PROPERTY(int stepTotal READ stepTotal NOTIFY installChanged)
    Q_PROPERTY(double percent READ percent NOTIFY installChanged)       // -1: the engine gave none
    Q_PROPERTY(QString progressMessage READ progressMessage NOTIFY installChanged)
    Q_PROPERTY(int elapsedSeconds READ elapsedSeconds NOTIFY elapsedChanged)
    Q_PROPERTY(QString logText READ logText NOTIFY logChanged)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY installChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY installChanged)
    Q_PROPERTY(QString errorHint READ errorHint NOTIFY installChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY installChanged)
    Q_PROPERTY(QString engineLogPath READ engineLogPath NOTIFY installChanged)
    Q_PROPERTY(QString doneMessage READ doneMessage NOTIFY installChanged)
    Q_PROPERTY(QString restartError READ restartError NOTIFY restartChanged)
    Q_PROPERTY(bool restarting READ restarting NOTIFY restartChanged)

    // ---- secrets (only whether they are set) ---------------------------------
    Q_PROPERTY(bool passwordSet READ passwordSet NOTIFY secretsChanged)
    Q_PROPERTY(bool passphraseSet READ passphraseSet NOTIFY secretsChanged)

    // ---- tool profiles -------------------------------------------------------
    Q_PROPERTY(QVariantList profiles READ profiles CONSTANT)
    Q_PROPERTY(QString profilesDir READ profilesDir CONSTANT)

public:
    explicit Installer(QObject *parent = nullptr);
    ~Installer() override;

    bool testMode() const;
    bool simulation() const;
    bool liveSession() const;
    QString startStep() const;
    QVariantMap preset() const { return m_preset; }
    QString probeFile() const;
    QString enginePath() const;
    QString installBlockedReason() const;

    QString probeState() const { return m_probeState; }
    QVariantMap probe() const { return m_probe; }
    QString probeError() const { return m_probeError; }
    QString probeErrorDetail() const { return m_probeErrorDetail; }
    bool scanningSystems() const { return m_scanningSystems; }

    QString planState() const { return m_planState; }
    QVariantMap plan() const { return m_plan; }
    QString planError() const { return m_planError; }
    QString planHint() const { return m_planHint; }
    QString answersFile() const { return m_answersFile; }

    QString installState() const { return m_installState; }
    QVariantList steps() const { return m_steps; }
    int stepIndex() const { return m_stepIndex; }
    int stepTotal() const { return m_stepTotal; }
    double percent() const { return m_percent; }
    QString progressMessage() const { return m_progressMessage; }
    int elapsedSeconds() const { return m_elapsedSeconds; }
    QString logText() const { return m_log.join(QLatin1Char('\n')); }
    QStringList warnings() const { return m_warnings; }
    QString errorMessage() const { return m_errorMessage; }
    QString errorHint() const { return m_errorHint; }
    QString errorCode() const { return m_errorCode; }
    QString engineLogPath() const { return m_engineLogPath; }
    QString doneMessage() const { return m_doneMessage; }
    QString restartError() const { return m_restartError; }
    bool restarting() const { return m_restarting; }

    bool passwordSet() const { return !m_password.isEmpty(); }
    bool passphraseSet() const { return !m_passphrase.isEmpty(); }

    QVariantList profiles() const { return m_profiles; }
    QString profilesDir() const;

    // Ask the engine to describe the machine. 'otherSystems' also runs the
    // operating-system scan (slow; mounts foreign filesystems read-only).
    Q_INVOKABLE void runProbe(bool otherSystems = false);

    // The secrets leave QML through these two calls and are never readable again.
    Q_INVOKABLE void setPassword(const QString &password);
    Q_INVOKABLE void setPassphrase(const QString &passphrase);
    Q_INVOKABLE void clearSecrets();

    // 'answers' is the answer file WITHOUT user.password and storage.passphrase.
    Q_INVOKABLE void requestPlan(const QVariantMap &answers);
    Q_INVOKABLE void discardPlan();
    // Carries out the plan whose hash is 'confirmedHash' — the one on screen.
    Q_INVOKABLE bool startInstall(const QString &confirmedHash);
    // After "plan-changed" or a failure: back to a state from which a new plan can be made.
    Q_INVOKABLE void resetInstall();

    Q_INVOKABLE void restart();
    Q_INVOKABLE void copyLog();
    Q_INVOKABLE void copyText(const QString &text);

    // Input rules of the engine (validation.h).
    Q_INVOKABLE QString userNameProblem(const QString &name) const;
    Q_INVOKABLE QString fullNameProblem(const QString &name) const;
    Q_INVOKABLE QString hostNameProblem(const QString &name) const;
    Q_INVOKABLE QString passwordProblem(const QString &password) const;
    Q_INVOKABLE QString passphraseProblem(const QString &passphrase) const;
    Q_INVOKABLE QVariantMap passwordFacts(const QString &password, const QStringList &personalWords) const;
    Q_INVOKABLE QString suggestUserName(const QString &fullName) const;

    // Used by main(): --preset and --dump.
    void loadPreset(const QString &path);
    QJsonObject environmentJson() const;
    QJsonArray profilesJson() const;

Q_SIGNALS:
    void probeChanged();
    void planChanged();
    void installChanged();
    void elapsedChanged();
    void logChanged();
    void secretsChanged();
    void restartChanged();
    // The secrets were wiped; text fields that still show them must clear.
    void secretsScrubbed();

private:
    struct Outcome
    {
        bool started = true;
        bool timedOut = false;
        bool crashed = false;
        int exitCode = -1;
        QString startError;
        QByteArray out;
        QByteArray err;
    };
    struct Invocation
    {
        QString program;
        QStringList arguments;
        bool viaPkexec = false;
        QString problem;        // non-empty: cannot even be attempted
    };

    Invocation engineCommand(const QStringList &engineArguments) const;
    QProcess *createProcess(const Invocation &invocation);
    void runOnce(const QStringList &engineArguments, QByteArray stdinDocument, int timeoutMs,
                 std::function<void(const Outcome &, bool viaPkexec)> done);
    static void abandon(QProcess *process);
    // A sentence for an outcome that is not "exit 0"; 'detail' gets the engine's own words.
    QString describeFailure(const Outcome &outcome, bool viaPkexec, QString *detail, QString *hint = nullptr) const;

    void finishProbe(const Outcome &outcome, bool viaPkexec, bool otherSystems);
    bool acceptProbe(const QByteArray &json, QString *problem);
    void setProbeFailure(const QString &error, const QString &detail);

    QByteArray buildStdinDocument() const;
    bool writeAnswersRecord();
    void setPlanFailure(const QString &error, const QString &hint);

    void readInstallOutput();
    void handleEvent(const QJsonObject &event);
    void installFinished(int exitCode, QProcess::ExitStatus status);
    void endInstall(const QString &state);
    void appendLog(const QString &line);
    void markActiveStep(const QString &state);
    void loadProfiles();

    QVariantMap m_preset;

    QString m_probeState = QStringLiteral("idle");
    QVariantMap m_probe;
    QString m_probeError;
    QString m_probeErrorDetail;
    bool m_scanningSystems = false;
    quint64 m_probeSerial = 0;

    QString m_planState = QStringLiteral("idle");
    QVariantMap m_plan;
    QString m_planError;
    QString m_planHint;
    QJsonObject m_answers;          // without secrets
    QString m_answersFile;
    std::unique_ptr<QTemporaryDir> m_privateDir;
    quint64 m_planSerial = 0;

    QString m_installState = QStringLiteral("idle");
    QPointer<QProcess> m_installProcess;
    bool m_installViaPkexec = false;
    QByteArray m_installBuffer;
    QVariantList m_steps;
    int m_stepIndex = 0;
    int m_stepTotal = 0;
    double m_percent = -1;
    QString m_progressMessage;
    int m_elapsedSeconds = 0;
    QElapsedTimer m_elapsed;
    QTimer m_elapsedTick;
    QStringList m_log;
    QStringList m_warnings;
    QString m_errorMessage;
    QString m_errorHint;
    QString m_errorCode;
    QString m_engineLogPath;
    QString m_doneMessage;
    bool m_sawDone = false;
    bool m_sawEvent = false;
    QString m_restartError;
    bool m_restarting = false;

    Secret m_password;
    Secret m_passphrase;

    QVariantList m_profiles;
};
