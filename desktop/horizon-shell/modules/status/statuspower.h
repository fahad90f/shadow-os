// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

#include <QDBusConnection>
#include <QStringList>

namespace StatusModule {

class DBusProperties;

// Battery charge and state.
//
// Source: UPower on the system bus — the "display device"
// (/org/freedesktop/UPower/devices/DisplayDevice), UPower's own summary of
// all batteries, which is what a status icon is meant to show, plus the
// daemon's OnBattery property. Both are followed through PropertiesChanged.
//
//   present        the computer has a battery (or runs from a UPS)
//   percentage     0–100
//   state          "charging" | "discharging" | "full" | "empty" |
//                  "pending-charge" | "pending-discharge" | "unknown"
//   onBattery      running from the battery, no external power
//   secondsToEmpty / secondsToFull   UPower's estimate, 0 when unknown
//   warningLevel   "none" | "low" | "critical" | "action"
class BatteryBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(bool present READ present NOTIFY changed)
    Q_PROPERTY(int percentage READ percentage NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(bool charging READ charging NOTIFY changed)
    Q_PROPERTY(bool onBattery READ onBattery NOTIFY changed)
    Q_PROPERTY(qlonglong secondsToEmpty READ secondsToEmpty NOTIFY changed)
    Q_PROPERTY(qlonglong secondsToFull READ secondsToFull NOTIFY changed)
    Q_PROPERTY(QString warningLevel READ warningLevel NOTIFY changed)

public:
    explicit BatteryBackend(QObject *parent = nullptr);

    bool present() const { return m_now.present; }
    int percentage() const { return m_now.percentage; }
    QString state() const { return m_now.state; }
    bool charging() const { return m_now.state == QLatin1String("charging"); }
    bool onBattery() const { return m_now.onBattery; }
    qlonglong secondsToEmpty() const { return m_now.secondsToEmpty; }
    qlonglong secondsToFull() const { return m_now.secondsToFull; }
    QString warningLevel() const { return m_now.warningLevel; }

Q_SIGNALS:
    void changed();

protected:
    QJsonObject details() const override;

private:
    struct Snapshot {
        bool present = false;
        int percentage = 0;
        QString state = QStringLiteral("unknown");
        bool onBattery = false;
        qlonglong secondsToEmpty = 0;
        qlonglong secondsToFull = 0;
        QString warningLevel = QStringLiteral("none");
        bool operator==(const Snapshot &other) const = default;
    };

    void recompute();

    QDBusConnection m_bus;
    DBusProperties *m_daemon = nullptr;
    DBusProperties *m_display = nullptr;
    Snapshot m_now;
};

// The power profile (power saver / balanced / performance).
//
// Source: power-profiles-daemon (or a compatible daemon such as tuned-ppd)
// on the system bus, under its current name
// org.freedesktop.UPower.PowerProfiles or, on older versions, net.hadess.PowerProfiles.
// The interface is the same under both names: Profiles, ActiveProfile
// (writable), PerformanceDegraded.
//
//   profiles         the profiles this machine offers, most frugal first
//   activeProfile    "power-saver" | "balanced" | "performance"
//   degradedReason   why "performance" is being held back, when it is
class PowerProfilesBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(QStringList profiles READ profiles NOTIFY changed)
    Q_PROPERTY(QString activeProfile READ activeProfile NOTIFY changed)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY changed)
    Q_PROPERTY(QString degradedReason READ degradedReason NOTIFY changed)

public:
    explicit PowerProfilesBackend(QObject *parent = nullptr);

    QStringList profiles() const { return m_profiles; }
    QString activeProfile() const { return m_active; }
    int activeIndex() const { return int(m_profiles.indexOf(m_active)); }
    QString degradedReason() const { return m_degraded; }

    Q_INVOKABLE void setActiveProfile(const QString &profile);
    // By position in 'profiles'.
    Q_INVOKABLE void setActiveIndex(int index);

Q_SIGNALS:
    // Also emitted when a requested change failed, so that controls showing
    // the profile fall back to the real one.
    void changed();

protected:
    QJsonObject details() const override;

private:
    void probe(int endpoint);
    void endpointChanged(int endpoint);

    QDBusConnection m_bus;
    DBusProperties *m_endpoints[2] = {nullptr, nullptr};
    int m_endpoint = 0;         // the one being asked / in use
    QStringList m_profiles;
    QString m_active;
    QString m_degraded;
};

} // namespace StatusModule
