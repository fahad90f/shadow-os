// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>

// Updates page: how this computer looks for and installs updates.
//
// * ~/.config/nexus/updater.conf — the settings file of NEXUS Updater
//   (applications/updater/README.md). Read and written: Updates/AutoCheck,
//   Updates/CheckIntervalHours. Read only: the result of the last check
//   (LastCheck/Time, Updates, Security, Error, RefreshError), which
//   "nexus-updater --check" records. "Check now" runs exactly that command.
// * unattended-upgrades: installed when /usr/bin/unattended-upgrade exists;
//   the effective APT::Periodic values come from "apt-config dump" (all of
//   apt.conf.d, not just one file), with /etc/apt/apt.conf.d/20auto-upgrades
//   parsed directly when apt-config is missing. Changed through
//   "pkexec /usr/libexec/nexus/updates-helper set-auto-upgrades <0|1> <0|1>"
//   (polkit action org.nexus.controlcenter.updates.configure).
// * /run/reboot-required for the restart notice.
//
// Test hook (development only): NEXUS_CC_HELPER_DIR replaces /usr/libexec/nexus.
class UpdatesBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool autoCheck READ autoCheck WRITE setAutoCheck NOTIFY settingsChanged)
    Q_PROPERTY(int checkIntervalHours READ checkIntervalHours WRITE setCheckIntervalHours NOTIFY settingsChanged)
    Q_PROPERTY(QString settingsFile READ settingsFile CONSTANT)
    Q_PROPERTY(bool lastCheckKnown READ lastCheckKnown NOTIFY settingsChanged)
    Q_PROPERTY(QDateTime lastCheck READ lastCheck NOTIFY settingsChanged)
    Q_PROPERTY(int lastUpdates READ lastUpdates NOTIFY settingsChanged)
    Q_PROPERTY(int lastSecurity READ lastSecurity NOTIFY settingsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY settingsChanged)
    Q_PROPERTY(QString lastRefreshError READ lastRefreshError NOTIFY settingsChanged)
    Q_PROPERTY(bool updaterAvailable READ updaterAvailable CONSTANT)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(bool rebootRequired READ rebootRequired NOTIFY systemChanged)

    Q_PROPERTY(bool systemReady READ systemReady NOTIFY systemChanged)
    Q_PROPERTY(bool unattendedInstalled READ unattendedInstalled NOTIFY systemChanged)
    Q_PROPERTY(bool periodicKnown READ periodicKnown NOTIFY systemChanged)
    Q_PROPERTY(QString periodicSource READ periodicSource NOTIFY systemChanged)
    Q_PROPERTY(QString updatePackageLists READ updatePackageLists NOTIFY systemChanged)
    Q_PROPERTY(QString unattendedUpgrade READ unattendedUpgrade NOTIFY systemChanged)
    Q_PROPERTY(bool listsEnabled READ listsEnabled NOTIFY systemChanged)
    Q_PROPERTY(bool unattendedEnabled READ unattendedEnabled NOTIFY systemChanged)
    // "enabled" | "disabled" | … as systemctl is-enabled prints it; empty when systemd is not running.
    Q_PROPERTY(QString timerState READ timerState NOTIFY systemChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit UpdatesBackend(QObject *parent = nullptr);

    bool autoCheck() const { return m_autoCheck; }
    void setAutoCheck(bool on);
    int checkIntervalHours() const { return m_interval; }
    void setCheckIntervalHours(int hours);
    QString settingsFile() const;
    bool lastCheckKnown() const { return m_lastCheck.isValid(); }
    QDateTime lastCheck() const { return m_lastCheck; }
    int lastUpdates() const { return m_lastUpdates; }
    int lastSecurity() const { return m_lastSecurity; }
    QString lastError() const { return m_lastError; }
    QString lastRefreshError() const { return m_lastRefreshError; }
    bool updaterAvailable() const;
    bool checking() const { return m_checking; }
    bool rebootRequired() const;

    bool systemReady() const { return m_systemReady; }
    bool unattendedInstalled() const;
    bool periodicKnown() const { return m_periodicKnown; }
    QString periodicSource() const { return m_periodicSource; }
    QString updatePackageLists() const { return m_updateLists; }
    QString unattendedUpgrade() const { return m_unattended; }
    bool listsEnabled() const { return isOn(m_updateLists); }
    bool unattendedEnabled() const { return isOn(m_unattended); }
    QString timerState() const { return m_timerState; }
    bool busy() const { return m_busy; }

    Q_INVOKABLE void reload();
    Q_INVOKABLE void checkNow();
    Q_INVOKABLE bool openUpdater() const;
    Q_INVOKABLE void setAutomatic(bool updateLists, bool unattendedUpgrade);

    QJsonObject toJson() const;

Q_SIGNALS:
    void settingsChanged();
    void systemChanged();
    void checkingChanged();
    void busyChanged();
    void failed(const QString &message);
    void succeeded(const QString &message);

private:
    static bool isOn(const QString &value) { return !value.isEmpty() && value != QLatin1String("0"); }
    void loadSettings();
    void loadSystem();
    void applyPeriodic(const QString &text, const QString &source);

    bool m_autoCheck = true;
    int m_interval = 24;
    QDateTime m_lastCheck;
    int m_lastUpdates = 0, m_lastSecurity = 0;
    QString m_lastError, m_lastRefreshError;
    bool m_checking = false;
    bool m_systemReady = false;
    bool m_periodicKnown = false;
    QString m_periodicSource, m_updateLists, m_unattended, m_timerState;
    bool m_busy = false;
    int m_pending = 0;
    QFileSystemWatcher m_watcher;
};
