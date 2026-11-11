// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusConnection>
#include <QJsonObject>
#include <QObject>
#include <QVariantMap>

namespace PowerModule {

// The power source and the battery's warning level.
//
// Source: UPower on the system bus — the daemon's OnBattery property, its
// GetCriticalAction() policy, and the display device
// (/org/freedesktop/UPower/devices/DisplayDevice), UPower's own summary of
// all batteries, with its WarningLevel. Followed through PropertiesChanged.
// Without UPower the computer counts as running on external power.
class Source : public QObject
{
    Q_OBJECT

public:
    // org.freedesktop.UPower.Device WarningLevel
    enum Warning { WarningUnknown = 0, WarningNone = 1, WarningDischarging = 2, WarningLow = 3, WarningCritical = 4, WarningAction = 5 };

    explicit Source(QObject *parent = nullptr);

    // True once UPower has answered (or is known to be unreachable).
    bool ready() const { return m_ready; }
    bool available() const { return m_available; }
    QString unavailableReason() const { return m_reason; }
    bool onBattery() const { return m_available && m_onBattery; }
    bool batteryPresent() const { return m_present; }
    int percentage() const { return m_percentage; }
    Warning warningLevel() const { return m_warning; }
    // "HybridSleep", "Hibernate", "PowerOff", "Suspend", "Ignore"; empty when unknown.
    QString criticalAction() const { return m_criticalAction; }

    static QString warningName(Warning level);
    QJsonObject toJson() const;

Q_SIGNALS:
    void readyChanged();
    void onBatteryChanged();
    // The warning level, the percentage or the critical action changed.
    void batteryChanged();

private Q_SLOTS:
    void propertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);

private:
    void load();
    void recompute();
    void gone(const QString &reason);
    void stepDone();

    QDBusConnection m_bus;
    QVariantMap m_daemon, m_display;
    bool m_ready = false;
    bool m_available = false;
    QString m_reason;
    bool m_onBattery = false;
    bool m_present = false;
    int m_percentage = 0;
    Warning m_warning = WarningUnknown;
    QString m_criticalAction;
    quint64 m_generation = 0;
    int m_pending = 0;
};

} // namespace PowerModule
