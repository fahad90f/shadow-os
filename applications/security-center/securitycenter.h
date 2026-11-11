// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <qqmlregistration.h>

// Back-end of SHADOW Security Center.
//
// It owns no security knowledge of its own: every state shown by the UI is a
// field of the JSON report printed by shadow-security-status, and every change
// to the firewall is one invocation of shadow-firewall. Both tools are run
// through pkexec with an argument vector (polkit actions
// org.shadow.security.read-status and org.shadow.firewall.manage), always
// asynchronously.
//
// Developer override (build chroot, tests): the tools can be called directly,
// without pkexec, from another location — see configure() and README.md.
class SecurityCenter : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // The merged report: one key per section, exactly as the tool printed it.
    Q_PROPERTY(QJsonObject report READ report NOTIFY reportChanged)
    // section name → ISO time (UTC) of the run that produced it
    Q_PROPERTY(QVariantMap sectionTimes READ sectionTimes NOTIFY reportChanged)
    Q_PROPERTY(QString generated READ generated NOTIFY reportChanged)        // last complete report
    Q_PROPERTY(bool hasReport READ hasReport NOTIFY reportChanged)
    Q_PROPERTY(bool privileged READ privileged NOTIFY reportChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)              // a run of the status tool is in progress
    Q_PROPERTY(bool fullRefreshRunning READ fullRefreshRunning NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)                 // why the last run produced no report
    Q_PROPERTY(bool autoRefresh READ autoRefresh WRITE setAutoRefresh NOTIFY autoRefreshChanged)

    Q_PROPERTY(bool firewallBusy READ firewallBusy NOTIFY firewallBusyChanged)
    Q_PROPERTY(QString firewallAction READ firewallAction NOTIFY firewallBusyChanged)
    Q_PROPERTY(QString firewallError READ firewallError NOTIFY firewallErrorChanged)

    Q_PROPERTY(bool direct READ direct CONSTANT)                 // developer override: tools run without pkexec
    Q_PROPERTY(QString statusTool READ statusTool CONSTANT)
    Q_PROPERTY(QString firewallTool READ firewallTool CONSTANT)
    Q_PROPERTY(bool updaterInstalled READ updaterInstalled CONSTANT)
    Q_PROPERTY(QString initialPage READ initialPage CONSTANT)

public:
    explicit SecurityCenter(QObject *parent = nullptr);

    // Called once from main() before any instance exists.
    //   toolsDir   directory holding the tools (either flat, or the repository
    //              layout <dir>/audit/shadow-security-status and
    //              <dir>/firewall/shadow-firewall); empty → /usr/sbin through pkexec
    static void configure(const QString &toolsDir, const QString &initialPage);
    static QStringList sectionNames();          // the tool's sections, in display order
    static QJsonObject toolsJson();             // for --dump tools

    QJsonObject report() const { return m_report; }
    QJsonObject reportJson() const { return m_report; }
    QVariantMap sectionTimes() const { return m_sectionTimes; }
    QString generated() const { return m_generated; }
    bool hasReport() const { return !m_report.isEmpty(); }
    bool privileged() const { return m_privileged; }
    bool loading() const { return m_running; }
    bool fullRefreshRunning() const { return m_running && m_runningFull; }
    QString error() const { return m_error; }
    bool autoRefresh() const { return m_autoRefresh; }
    void setAutoRefresh(bool on);
    bool firewallBusy() const { return m_firewallBusy; }
    QString firewallAction() const { return m_firewallAction; }
    QString firewallError() const { return m_firewallError; }
    bool direct() const;
    QString statusTool() const;
    QString firewallTool() const;
    bool updaterInstalled() const;
    QString initialPage() const;

    // Run the status tool for all sections (and the recommendations).
    Q_INVOKABLE void refresh();
    // Only the sections that are cheap to compute (used by the slow timer).
    Q_INVOKABLE void refreshCheap();
    // Specific sections (used by --dump <section>).
    void refreshSections(const QStringList &sections);

    // ---- firewall (shadow-firewall) ------------------------------------------
    // "" if valid, otherwise the reason — the same rules shadow-firewall applies.
    Q_INVOKABLE QString portError(const QString &port) const;
    Q_INVOKABLE QString nameError(const QString &name) const;
    Q_INVOKABLE void firewallEnable();
    Q_INVOKABLE void firewallDisable();
    Q_INVOKABLE void firewallReload();
    Q_INVOKABLE void firewallReset();
    Q_INVOKABLE void firewallAllow(const QString &port, const QString &protocol, const QString &name);
    Q_INVOKABLE void firewallRemove(const QString &port, const QString &protocol);
    Q_INVOKABLE void clearFirewallError();

    // ---- report export ----------------------------------------------------------
    Q_INVOKABLE QString reportAsJson() const;
    Q_INVOKABLE QString reportAsText() const;
    Q_INVOKABLE void copyText(const QString &text) const;
    Q_INVOKABLE QString defaultSavePath(const QString &format) const;   // format: "json" | "text"
    Q_INVOKABLE bool fileExists(const QString &path) const;
    // Returns "" on success, otherwise why the file could not be written.
    Q_INVOKABLE QString saveReport(const QString &path, const QString &format) const;

    Q_INVOKABLE bool openUpdater() const;
    // ISO-8601 UTC time of the report → local date and time for display.
    Q_INVOKABLE QString localTime(const QString &iso, bool withDate = true) const;

Q_SIGNALS:
    void reportChanged();
    void loadingChanged();
    void errorChanged();
    void autoRefreshChanged();
    void firewallBusyChanged();
    void firewallErrorChanged();
    // One run of the status tool ended (successfully or not).
    void refreshFinished(bool ok);
    // A firewall action ended; message is the tool's own output or error.
    void firewallFinished(bool ok, const QString &action, const QString &message);

private:
    void startRun(const QStringList &sections, bool full);
    void runFirewall(const QString &action, const QStringList &arguments);
    void setError(const QString &error);
    static QString describeFailure(const QString &tool, int exitCode, const QString &stderrText, bool viaPkexec);

    QJsonObject m_report;
    QVariantMap m_sectionTimes;
    QString m_generated;
    QString m_error;
    QString m_firewallAction;
    QString m_firewallError;
    QTimer m_timer;
    bool m_privileged = false;
    bool m_running = false;
    bool m_runningFull = false;
    bool m_pendingFull = false;
    bool m_pendingCheap = false;
    bool m_autoRefresh = false;
    bool m_firewallBusy = false;
};
