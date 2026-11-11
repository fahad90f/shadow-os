// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "packagemodel.h"
#include "pkquery.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>

#include <Transaction>

#include <functional>

namespace ShadowPk {

// What a transaction would do, grouped the way people read it. Built from
// the packages a *simulated* PackageKit transaction reports.
//   { install: [..], remove: [..], update: [..], reinstall: [..], downgrade: [..],
//     obsolete: [..], untrusted: [..], additional: [..], downloadSize: <bytes>,
//     installCount, removeCount, … }
// Each list entry is { packageId, name, version, arch, repo, repoText, summary }.
// "additional" lists the packages to install or update that the user did not
// ask for by name (dependencies).
QVariantMap summarizeChanges(const QList<PackageEntry> &simulated, qulonglong downloadSize,
                             const QStringList &requestedNames);

// One operation that changes the system: install, remove, update, refresh,
// repair, enable/disable a repository.
//
// A Job drives the PackageKit transactions such an operation needs and keeps
// the user in control at every point where PackageKit asks a question:
//
//   simulate (what will change?)            phase "simulating"
//     -> untrusted packages?                prompt "untrusted"  (danger; never accepted silently)
//     -> removals / explicit confirmation   prompt "confirm"
//   run for real                            phase "running"
//     -> licence agreement                  prompt "eula"       -> AcceptEula, then run again
//     -> unknown signing key                prompt "key"        -> InstallSignature, then run again
//     -> another medium needed              prompt "media"      -> run again
//     -> package not from a signed source   prompt "untrusted"  -> run again without only-trusted
//   finished                                result "success" | "failed" | "cancelled"
//
// Declining any prompt ends the job as "cancelled"; nothing is installed.
class Job : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString kind READ kind CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString phase READ phase NOTIFY phaseChanged)
    Q_PROPERTY(bool active READ active NOTIFY phaseChanged)
    Q_PROPERTY(QString status READ status NOTIFY progressChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY progressChanged)
    Q_PROPERTY(int percentage READ percentage NOTIFY progressChanged)
    Q_PROPERTY(bool cancellable READ cancellable NOTIFY progressChanged)
    Q_PROPERTY(double downloadSizeRemaining READ downloadSizeRemaining NOTIFY progressChanged)
    Q_PROPERTY(double downloadRate READ downloadRate NOTIFY progressChanged)
    Q_PROPERTY(int remainingTime READ remainingTime NOTIFY progressChanged)
    Q_PROPERTY(QString currentItem READ currentItem NOTIFY progressChanged)
    Q_PROPERTY(QObject *items READ itemsObject CONSTANT)
    Q_PROPERTY(QString promptKind READ promptKind NOTIFY promptChanged)
    Q_PROPERTY(QVariantMap prompt READ prompt NOTIFY promptChanged)
    Q_PROPERTY(QVariantMap changes READ changes NOTIFY changesChanged)
    Q_PROPERTY(QString result READ result NOTIFY phaseChanged)
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY phaseChanged)
    Q_PROPERTY(QString errorTitle READ errorTitle NOTIFY phaseChanged)
    Q_PROPERTY(QString errorDetails READ errorDetails NOTIFY phaseChanged)
    Q_PROPERTY(bool repairRequired READ repairRequired NOTIFY phaseChanged)
    Q_PROPERTY(bool untrustedAccepted READ untrustedAccepted NOTIFY phaseChanged)
    Q_PROPERTY(QString restart READ restart NOTIFY restartChanged)
    Q_PROPERTY(bool restartForSecurity READ restartForSecurity NOTIFY restartChanged)
    Q_PROPERTY(QStringList restartPackages READ restartPackages NOTIFY restartChanged)
    Q_PROPERTY(QString logText READ logText NOTIFY logChanged)

public:
    enum class Confirm { Never, IfRemovals, Always };
    using Starter = std::function<PackageKit::Transaction *(PackageKit::Transaction::TransactionFlags)>;

    // kind: "install" | "remove" | "update" | "refresh" | "repair" | "repository"
    Job(const QString &kind, const QString &title, Starter starter, QObject *parent = nullptr);
    ~Job() override;

    // Simulate before running (operations that take TransactionFlags). Default: off.
    void setSimulateFirst(bool on) { m_simulateFirst = on; }
    void setConfirm(Confirm confirm) { m_confirm = confirm; }
    // Package names the user asked for, so dependencies can be told apart.
    void setRequestedNames(const QStringList &names) { m_requestedNames = names; }

    QString kind() const { return m_kind; }
    QString title() const { return m_title; }
    // "idle" | "simulating" | "waiting" (for the user) | "running" | "finished"
    QString phase() const { return m_phase; }
    bool active() const { return m_phase != QLatin1String("idle") && m_phase != QLatin1String("finished"); }
    QString status() const;
    QString statusText() const;
    int percentage() const { return m_percentage; }
    bool cancellable() const;
    double downloadSizeRemaining() const { return static_cast<double>(m_downloadRemaining); }
    double downloadRate() const { return m_downloadRate; }
    int remainingTime() const { return m_remainingTime; }
    QString currentItem() const { return m_currentItem; }
    PackageModel *items() { return &m_items; }
    QObject *itemsObject() { return &m_items; }
    QString promptKind() const { return m_promptKind; }
    QVariantMap prompt() const { return m_prompt; }
    QVariantMap changes() const { return m_changes; }
    // "" while active, then "success" | "failed" | "cancelled"
    QString result() const { return m_result; }
    QString errorKey() const { return m_errorKey; }
    QString errorTitle() const { return m_errorTitle; }
    QString errorDetails() const { return m_errorDetails; }
    bool repairRequired() const { return m_repairRequired; }
    bool untrustedAccepted() const { return m_allowUntrusted; }
    // "none" | "application" | "session" | "system"
    QString restart() const;
    bool restartForSecurity() const { return m_restartSecurity; }
    QStringList restartPackages() const { return m_restartPackages; }
    QString logText() const { return m_log.join(QLatin1Char('\n')); }
    QJsonObject toJson() const;

    Q_INVOKABLE void start();
    // Cancel the running transaction if PackageKit allows it, or abandon a pending question.
    Q_INVOKABLE void cancel();
    // Answer the pending prompt. Rejecting ends the job without changing the system.
    Q_INVOKABLE void answer(bool accept);

Q_SIGNALS:
    void phaseChanged();
    void progressChanged();
    void promptChanged();
    void changesChanged();
    void restartChanged();
    void logChanged();
    void finished(bool success);

private:
    void log(const QString &line);
    void setPhase(const QString &phase);
    void runSimulation();
    void simulationDone(const QueryResult &result);
    void continueAfterSimulation();
    void runReal();
    void attach(PackageKit::Transaction *transaction);
    void transactionFinished(PackageKit::Transaction::Exit exit, uint runtime);
    void runAuxiliary(PackageKit::Transaction *transaction, const QString &what);
    void raisePrompt(const QString &kind, const QVariantMap &prompt);
    void raiseNextEula();
    void resume();
    void recordError(PackageKit::Transaction::Error error, const QString &details);
    void finishJob(const QString &result);
    void resetProgress();
    void updateFromTransaction();

    QString m_kind;
    QString m_title;
    Starter m_starter;
    bool m_simulateFirst = false;
    Confirm m_confirm = Confirm::Never;
    QStringList m_requestedNames;

    QString m_phase = QStringLiteral("idle");
    QPointer<PackageKit::Transaction> m_transaction;
    QPointer<Query> m_query;
    bool m_resumeWithSimulation = false;
    bool m_simulated = false;
    bool m_confirmed = false;
    bool m_allowUntrusted = false;
    bool m_cancelRequested = false;

    PackageKit::Transaction::Status m_status = PackageKit::Transaction::StatusUnknown;
    int m_percentage = -1;
    bool m_allowCancel = false;
    qulonglong m_downloadRemaining = 0;
    double m_downloadRate = 0;
    int m_remainingTime = 0;
    QElapsedTimer m_rateTimer;
    qulonglong m_rateLastRemaining = 0;
    QString m_currentItem;
    PackageModel m_items;

    QString m_promptKind;
    QVariantMap m_prompt;
    QVariantList m_pendingEulas;
    QVariantMap m_pendingKey;
    QVariantMap m_pendingMedia;
    QVariantMap m_changes;

    QString m_result;
    bool m_hasError = false;
    QString m_errorKey;
    QString m_errorTitle;
    QString m_errorDetails;
    bool m_repairRequired = false;
    int m_restartRank = 0;
    bool m_restartSecurity = false;
    QStringList m_restartPackages;
    QStringList m_log;
};

// The operations both applications offer, with the right simulation and
// confirmation policy each. The caller owns the job and calls start().
namespace Jobs {
// Install package IDs (and their dependencies). Asks before removing anything.
Job *install(const QStringList &packageIds, const QString &title, QObject *parent);
// Remove package IDs and what depends on them — always after showing the full list.
Job *remove(const QStringList &packageIds, const QString &title, QObject *parent);
// Update package IDs. Asks before removing anything.
Job *update(const QStringList &packageIds, const QString &title, QObject *parent);
// Refresh the software lists (apt update).
Job *refresh(bool force, QObject *parent);
// PackageKit RepairSystem: finish interrupted dpkg runs and fix broken dependencies.
Job *repair(QObject *parent);
// Enable or disable a configured repository.
Job *repoEnable(const QString &repoId, bool enable, const QString &title, QObject *parent);
} // namespace Jobs

} // namespace ShadowPk
