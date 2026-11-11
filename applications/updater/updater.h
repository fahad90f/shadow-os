// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <pkjob.h>
#include <updatechecker.h>

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

// Back-end of SHADOW Updater.
//
// Data sources
//   updates          PackageKit: RefreshCache, GetUpdates, Resolve, GetDetails, GetUpdateDetail
//   installing       PackageKit: UpdatePackages (simulated first), RepairSystem
//   history          /var/log/apt/history.log*, completed with who started each
//                    PackageKit run (PackageKit GetOldTransactions)
//   restart state    PackageKit RequireRestart, /run/reboot-required(.pkgs), kernels in /boot
//   package health   dpkg --audit, /var/lib/dpkg/updates, dpkg-query
//   sources          /etc/apt/sources.list*, /var/lib/apt/lists (signed Release files)
//   restart          logind (org.freedesktop.login1.Manager.CanReboot / Reboot)
//   settings         ~/.config/shadow/updater.conf
//
// Nothing here runs as root; PackageKit and logind authorise through polkit.
class Updater : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *service READ service CONSTANT)
    Q_PROPERTY(QObject *updates READ updates CONSTANT)
    Q_PROPERTY(QObject *job READ job NOTIFY jobChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged)

    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(bool historyLoading READ historyLoading NOTIFY historyChanged)
    Q_PROPERTY(QString historyError READ historyError NOTIFY historyChanged)
    Q_PROPERTY(bool historyTruncated READ historyTruncated NOTIFY historyChanged)
    Q_PROPERTY(QString transactionsError READ transactionsError NOTIFY historyChanged)

    Q_PROPERTY(QVariantMap health READ health NOTIFY healthChanged)
    Q_PROPERTY(bool healthChecking READ healthChecking NOTIFY healthChanged)
    Q_PROPERTY(bool healthKnown READ healthKnown NOTIFY healthChanged)
    Q_PROPERTY(bool healthy READ healthy NOTIFY healthChanged)

    Q_PROPERTY(QVariantMap reboot READ reboot NOTIFY rebootChanged)
    Q_PROPERTY(bool restartNeeded READ restartNeeded NOTIFY rebootChanged)
    Q_PROPERTY(QString restartLevel READ restartLevel NOTIFY rebootChanged)
    Q_PROPERTY(QString restartReason READ restartReason NOTIFY rebootChanged)
    Q_PROPERTY(QString canReboot READ canReboot NOTIFY rebootChanged)
    Q_PROPERTY(QString canRebootError READ canRebootError NOTIFY rebootChanged)

    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(bool sourcesLoading READ sourcesLoading NOTIFY sourcesChanged)

    Q_PROPERTY(QString lastLog READ lastLog NOTIFY lastLogChanged)
    Q_PROPERTY(QString lastLogTitle READ lastLogTitle NOTIFY lastLogChanged)
    Q_PROPERTY(QString lastLogPath READ lastLogPath CONSTANT)

    Q_PROPERTY(bool autoCheck READ autoCheck WRITE setAutoCheck NOTIFY settingsChanged)
    Q_PROPERTY(int checkIntervalHours READ checkIntervalHours WRITE setCheckIntervalHours NOTIFY settingsChanged)
    Q_PROPERTY(QDateTime lastCheck READ lastCheck NOTIFY settingsChanged)
    Q_PROPERTY(QString lastCheckResult READ lastCheckResult NOTIFY settingsChanged)
    Q_PROPERTY(QString settingsPath READ settingsPath CONSTANT)

public:
    explicit Updater(QObject *parent = nullptr);

    QObject *service() const;
    QObject *updates() { return &m_updates; }
    ShadowPk::UpdateChecker *checker() { return &m_updates; }
    QObject *job() const { return m_job; }
    bool busy() const { return m_job && m_job->active(); }

    QVariantList history() const { return m_history; }
    bool historyLoading() const { return m_historyLoading; }
    QString historyError() const { return m_historyError; }
    bool historyTruncated() const { return m_historyTruncated; }
    // Why PackageKit's own records could not be merged in ("" if they were).
    QString transactionsError() const { return m_transactionsError; }

    QVariantMap health() const { return m_health; }
    bool healthChecking() const { return m_healthChecking; }
    bool healthKnown() const { return !m_health.isEmpty(); }
    bool healthy() const { return m_health.value(QStringLiteral("healthy")).toBool(); }

    QVariantMap reboot() const { return m_reboot; }
    bool restartNeeded() const;
    // "none" | "application" | "session" | "system"
    QString restartLevel() const;
    QString restartReason() const;
    // logind's answer: "yes" | "challenge" | "no" | "na" | "" (not asked / unreachable)
    QString canReboot() const { return m_canReboot; }
    QString canRebootError() const { return m_canRebootError; }

    QVariantList sources() const { return m_sources; }
    bool sourcesLoading() const { return m_sourcesLoading; }

    QString lastLog() const { return m_lastLog; }
    QString lastLogTitle() const { return m_lastLogTitle; }
    QString lastLogPath() const;

    bool autoCheck() const { return m_autoCheck; }
    void setAutoCheck(bool on);
    int checkIntervalHours() const { return m_checkIntervalHours; }
    void setCheckIntervalHours(int hours);
    QDateTime lastCheck() const { return m_lastCheck; }
    QString lastCheckResult() const { return m_lastCheckResult; }
    QString settingsPath() const;

    // First contact with PackageKit and the local facts; called once by the UI.
    Q_INVOKABLE void initialize();
    Q_INVOKABLE void retryService();
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void installAll();
    Q_INVOKABLE void installSecurity();
    Q_INVOKABLE void installSelected();
    Q_INVOKABLE void repair();
    Q_INVOKABLE void dismissJob();
    Q_INVOKABLE void restartNow();
    Q_INVOKABLE void refreshHistory();
    Q_INVOKABLE void refreshTransactions();
    Q_INVOKABLE void refreshHealth();
    Q_INVOKABLE void refreshReboot();
    Q_INVOKABLE void refreshSources();
    Q_INVOKABLE void copyToClipboard(const QString &text) const;
    // Everything a bug report about a failed update needs, as plain text.
    Q_INVOKABLE QString report() const;

    // Record the outcome of a check (also used by `shadow-updater --check`).
    static void storeCheckResult(const ShadowPk::UpdateChecker &checker);
    static QJsonObject settingsJson();

Q_SIGNALS:
    void jobChanged();
    void historyChanged();
    void healthChanged();
    void rebootChanged();
    void sourcesChanged();
    void lastLogChanged();
    void settingsChanged();
    void jobFinished(bool success, const QString &kind);
    void restartFailed(const QString &message);

private:
    void startJob(ShadowPk::Job *job);
    void startUpdate(const QStringList &ids, const QString &title);
    void onJobFinished(bool success);
    void saveLastLog(const ShadowPk::Job *job);
    void loadLastLog();
    void loadSettings();
    void probeLogind();
    void rebuildHistory();

    ShadowPk::UpdateChecker m_updates;
    QPointer<ShadowPk::Job> m_job;
    bool m_initialized = false;

    QVariantList m_history;         // merged, as shown
    QVariantList m_aptHistory;      // /var/log/apt/history.log*
    bool m_historyLoading = false;
    QString m_historyError;
    bool m_historyTruncated = false;
    QVariantList m_transactions;
    bool m_transactionsLoading = false;
    QString m_transactionsError;

    QVariantMap m_health;
    bool m_healthChecking = false;

    QVariantMap m_reboot;
    int m_jobRestartRank = 0;
    bool m_jobRestartSecurity = false;
    QStringList m_jobRestartPackages;
    QString m_canReboot;
    QString m_canRebootError;

    QVariantList m_sources;
    bool m_sourcesLoading = false;

    QString m_lastLog;
    QString m_lastLogTitle;

    bool m_autoCheck = true;
    int m_checkIntervalHours = 24;
    QDateTime m_lastCheck;
    QString m_lastCheckResult;
};
