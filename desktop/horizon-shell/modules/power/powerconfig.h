// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class QFileSystemWatcher;

namespace PowerModule {

// Idle timeouts in minutes; 0 = never.
struct IdleMinutes {
    int screenOff = 0;
    int lock = 0;
    int suspend = 0;
    bool operator==(const IdleMinutes &other) const = default;
};

// The two settings files this module reads (it never writes either):
//
//   ~/.config/nexus/power.conf    [Power] Idle…Minutes / …OnBattery, written by
//                                 the Control Center's Power page (format:
//                                 applications/control-center/backends/powerbackend.h)
//   ~/.config/nexus/updater.conf  [Updates] AutoCheck, CheckIntervalHours,
//                                 written by NEXUS Updater
//
// Both are re-read when anything in ~/.config/nexus changes (the writers
// replace the files atomically, so the directory is watched, not the files).
//
// Developer override: NEXUS_POWER_TIME_SCALE=<factor> divides every duration
// this module waits for (idle timeouts, update-check delays) by <factor>;
// 60 turns minutes into seconds. Unset or invalid: 1.
class Config : public QObject
{
    Q_OBJECT

public:
    explicit Config(QObject *parent = nullptr);

    IdleMinutes onAc() const { return m_ac; }
    IdleMinutes onBattery() const { return m_battery; }

    bool updatesAutoCheck() const { return m_autoCheck; }
    int updatesIntervalHours() const { return m_intervalHours; }

    QString powerFile() const;
    QString updaterFile() const;

    static double timeScale();
    // 'ms' of configured time as milliseconds to really wait (at least 1).
    static qint64 scaled(qint64 ms);

    QJsonObject toJson() const;

public Q_SLOTS:
    void reload();

Q_SIGNALS:
    void idleChanged();
    void updatesChanged();

private:
    IdleMinutes m_ac, m_battery;
    bool m_autoCheck = true;
    int m_intervalHours = 24;
    QTimer m_reloadTimer;
    QFileSystemWatcher *m_watcher = nullptr;
};

} // namespace PowerModule
