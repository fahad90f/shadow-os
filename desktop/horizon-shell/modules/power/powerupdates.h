// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QDBusConnection>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <functional>

class QProcess;

namespace PowerModule {

class Config;
class Notifier;

// Periodic update check.
//
// When ~/.config/shadow/updater.conf has Updates/AutoCheck=true (the default)
// and shadow-updater is installed, "shadow-updater --check" is run a few
// minutes after the shell started and then every Updates/CheckIntervalHours.
// The updater prints one JSON object and reports through its exit status:
// 0 nothing to do, 10 updates, 11 security updates, 1 the check failed
// (applications/updater/README.md). 10 and 11 produce a notification with an
// action that opens SHADOW Updater; 0 and 1 produce none.
//
// A check only starts while the network is up: NetworkManager's State is
// "connected" (site or global), or — where NetworkManager is not running — a
// default route exists. Otherwise it is retried every few minutes and as
// soon as NetworkManager reports a connection.
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    UpdateChecker(Config *config, Notifier *notifier, QObject *parent = nullptr);

    // Whether a check would be scheduled at all, and why not.
    static QString updaterProgram();
    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();
    // The user chose the notification's action.
    void openUpdaterRequested();

private Q_SLOTS:
    void networkStateChanged(uint state);

private:
    void reschedule(bool afterCheck);
    void due();
    void withNetwork(const std::function<void(bool up, const QString &how)> &then);
    void startCheck();
    void finished(int exitCode, bool crashed, const QByteArray &output, const QString &startError);

    Config *m_config;
    Notifier *m_notifier;
    QDBusConnection m_system;
    QTimer m_timer;             // the next scheduled check
    QTimer m_retryTimer;        // the network was down when a check was due
    QDateTime m_nextCheck;
    QProcess *m_process = nullptr;
    bool m_waitingForNetwork = false;
    QString m_networkHow;
    bool m_firstDone = false;

    QDateTime m_lastRun;
    int m_lastExitCode = -1;
    QString m_lastResult;       // "none", "updates", "security", "failed", "not-run"
    QString m_lastError;
    int m_lastUpdates = 0, m_lastSecurity = 0;
    int m_runs = 0;
};

} // namespace PowerModule
