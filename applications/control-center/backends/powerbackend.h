// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace NetHw {
class RowModel;
}

// Power & Battery page.
//
// Sources (system bus unless noted)
//   org.freedesktop.UPower          OnBattery, LidIsPresent, EnumerateDevices,
//                                   org.freedesktop.UPower.Device of every device
//                                   (batteries, line power, peripherals) and of
//                                   the aggregate DisplayDevice
//   org.freedesktop.UPower.PowerProfiles (power-profiles-daemon ≥ 0.20) or the
//   older net.hadess.PowerProfiles  Profiles, ActiveProfile (read/write),
//                                   PerformanceDegraded, ActiveProfileHolds
//   org.freedesktop.login1          Manager.CanSuspend / CanHibernate, the
//                                   effective HandleLidSwitch* settings, and
//                                   Session.SetBrightness on the caller's session
//   /sys/class/backlight/*          current and maximum panel brightness
//   /etc/systemd/logind.conf (+ drop-ins)   lid actions when logind is unreachable
//
// ---------------------------------------------------------------------------
// ~/.config/nexus/power.conf — idle behaviour, honoured by the NEXUS HORIZON
// shell. This back-end is the only writer; the shell only reads.
//
//   [Power]
//   IdleScreenOffMinutes=10            minutes without input until the screens
//                                      are switched off
//   IdleLockMinutes=15                 … until the session is locked
//   IdleSuspendMinutes=0               … until the computer is suspended
//   IdleScreenOffMinutesOnBattery=5    the same three, used instead while the
//   IdleLockMinutesOnBattery=10        computer runs on battery
//   IdleSuspendMinutesOnBattery=20     (org.freedesktop.UPower OnBattery = true)
//
//   * INI syntax, UTF-8, one [Power] group — QSettings::NativeFormat with
//     organisation "nexus" and application "power", exactly like
//     appearance.conf. A system-wide default may be installed as
//     /etc/xdg/nexus/power.conf; QSettings falls back to it per key.
//   * Every value is a whole number of minutes from 0 to 1440; 0 means never.
//   * A key that is missing (or not a number in range) has the default shown
//     above. Unknown keys must be ignored by readers.
//   * The file is replaced atomically (write to a temporary file, rename) on
//     every change, so readers should watch the directory ~/.config/nexus and
//     re-read the file when it changes (see Nexus::Appearance::watch()).
//   * The lid switch is not configured here: it is handled by systemd-logind
//     (HandleLidSwitch… in logind.conf) and only displayed on this page.
// ---------------------------------------------------------------------------
class PowerBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

    // UPower
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(bool onBattery READ onBattery NOTIFY batteryChanged)
    Q_PROPERTY(bool hasBattery READ hasBattery NOTIFY batteryChanged)
    Q_PROPERTY(QVariantMap battery READ battery NOTIFY batteryChanged)
    Q_PROPERTY(QAbstractItemModel *batteries READ batteries CONSTANT)
    Q_PROPERTY(int batteryCount READ batteryCount NOTIFY batteryChanged)
    Q_PROPERTY(QAbstractItemModel *peripherals READ peripherals CONSTANT)
    Q_PROPERTY(int peripheralCount READ peripheralCount NOTIFY batteryChanged)
    Q_PROPERTY(bool lidPresent READ lidPresent NOTIFY batteryChanged)

    // power profiles
    Q_PROPERTY(bool profilesAvailable READ profilesAvailable NOTIFY profilesChanged)
    Q_PROPERTY(QString profilesStatus READ profilesStatus NOTIFY profilesChanged)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QString activeProfile READ activeProfile NOTIFY profilesChanged)
    Q_PROPERTY(QString performanceDegraded READ performanceDegraded NOTIFY profilesChanged)
    Q_PROPERTY(QString performanceDegradedText READ performanceDegradedText NOTIFY profilesChanged)
    Q_PROPERTY(QVariantList profileHolds READ profileHolds NOTIFY profilesChanged)

    // brightness
    Q_PROPERTY(bool brightnessAvailable READ brightnessAvailable NOTIFY brightnessChanged)
    Q_PROPERTY(int brightnessPercent READ brightnessPercent NOTIFY brightnessChanged)
    Q_PROPERTY(QString brightnessDevice READ brightnessDevice NOTIFY brightnessChanged)

    // logind
    Q_PROPERTY(bool logindAvailable READ logindAvailable NOTIFY logindChanged)
    Q_PROPERTY(QString canSuspend READ canSuspend NOTIFY logindChanged)
    Q_PROPERTY(QString canHibernate READ canHibernate NOTIFY logindChanged)
    Q_PROPERTY(bool suspendUsable READ suspendUsable NOTIFY logindChanged)
    Q_PROPERTY(QString lidAction READ lidAction NOTIFY logindChanged)
    Q_PROPERTY(QString lidActionExternalPower READ lidActionExternalPower NOTIFY logindChanged)
    Q_PROPERTY(QString lidActionDocked READ lidActionDocked NOTIFY logindChanged)
    Q_PROPERTY(QString lidActionSource READ lidActionSource NOTIFY logindChanged)

    // ~/.config/nexus/power.conf
    Q_PROPERTY(int idleScreenOffMinutes READ idleScreenOffMinutes NOTIFY settingsChanged)
    Q_PROPERTY(int idleLockMinutes READ idleLockMinutes NOTIFY settingsChanged)
    Q_PROPERTY(int idleSuspendMinutes READ idleSuspendMinutes NOTIFY settingsChanged)
    Q_PROPERTY(int idleScreenOffMinutesOnBattery READ idleScreenOffMinutesOnBattery NOTIFY settingsChanged)
    Q_PROPERTY(int idleLockMinutesOnBattery READ idleLockMinutesOnBattery NOTIFY settingsChanged)
    Q_PROPERTY(int idleSuspendMinutesOnBattery READ idleSuspendMinutesOnBattery NOTIFY settingsChanged)
    Q_PROPERTY(QString settingsFile READ settingsFile CONSTANT)

public:
    explicit PowerBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }

    // "ok", "no-system-bus" or "upower-unavailable".
    QString state() const { return m_state; }
    bool available() const { return m_state == QLatin1String("ok"); }
    QString statusMessage() const { return m_statusMessage; }
    bool onBattery() const { return m_onBattery; }
    bool hasBattery() const { return m_hasBattery; }
    // The aggregate battery UPower computes for display: percentage, state,
    // stateText, timeToEmpty, timeToFull (seconds), energyRate (W), icon.
    QVariantMap battery() const { return m_battery; }
    QAbstractItemModel *batteries() const;
    int batteryCount() const;
    QAbstractItemModel *peripherals() const;
    int peripheralCount() const;
    bool lidPresent() const { return m_lidPresent; }

    bool profilesAvailable() const { return m_profilesAvailable; }
    QString profilesStatus() const { return m_profilesStatus; }
    QVariantList profiles() const { return m_profiles; }
    QString activeProfile() const { return m_activeProfile; }
    QString performanceDegraded() const { return m_performanceDegraded; }
    QString performanceDegradedText() const;
    QVariantList profileHolds() const { return m_profileHolds; }

    bool brightnessAvailable() const { return !m_backlightName.isEmpty() && m_backlightMax > 0; }
    int brightnessPercent() const;
    QString brightnessDevice() const { return m_backlightName; }

    bool logindAvailable() const { return m_logindAvailable; }
    QString canSuspend() const { return m_canSuspend; }
    QString canHibernate() const { return m_canHibernate; }
    bool suspendUsable() const;
    QString lidAction() const { return m_lidAction; }
    QString lidActionExternalPower() const { return m_lidActionExternalPower; }
    QString lidActionDocked() const { return m_lidActionDocked; }
    QString lidActionSource() const { return m_lidActionSource; }

    int idleScreenOffMinutes() const { return m_idle.value(QStringLiteral("IdleScreenOffMinutes")); }
    int idleLockMinutes() const { return m_idle.value(QStringLiteral("IdleLockMinutes")); }
    int idleSuspendMinutes() const { return m_idle.value(QStringLiteral("IdleSuspendMinutes")); }
    int idleScreenOffMinutesOnBattery() const { return m_idle.value(QStringLiteral("IdleScreenOffMinutesOnBattery")); }
    int idleLockMinutesOnBattery() const { return m_idle.value(QStringLiteral("IdleLockMinutesOnBattery")); }
    int idleSuspendMinutesOnBattery() const { return m_idle.value(QStringLiteral("IdleSuspendMinutesOnBattery")); }
    QString settingsFile() const;

    Q_INVOKABLE void setActiveProfile(const QString &profile);
    Q_INVOKABLE void setBrightness(int percent);
    // key: one of the six Idle… keys of power.conf; minutes: 0 (never) – 1440.
    Q_INVOKABLE bool setIdleMinutes(const QString &key, int minutes);
    // Human-readable name of a logind action ("suspend" → "Suspend").
    Q_INVOKABLE QString actionText(const QString &action) const;
    Q_INVOKABLE QString capabilityText(const QString &answer) const;

    QJsonObject toJson() const;

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void batteryChanged();
    void profilesChanged();
    void brightnessChanged();
    void logindChanged();
    void settingsChanged();
    void failed(const QString &message);

private Q_SLOTS:
    void scheduleUPowerReload();
    void scheduleProfilesReload();

private:
    void reloadUPower();
    void publishUPower(const QVariantMap &daemon, const QMap<QString, QVariantMap> &devices);
    void clearUPower(const QString &state, const QString &message);
    void reloadProfiles(int candidate = 0);
    void reloadLogind();
    void readLidActionsFromFiles();
    void readBacklight();
    void loadSettings();
    void stepDone();

    bool m_ready = false;
    int m_pendingSteps = 0;

    QString m_state;
    QString m_statusMessage;
    bool m_onBattery = false;
    bool m_hasBattery = false;
    bool m_lidPresent = false;
    QVariantMap m_battery;
    QString m_daemonVersion;
    QVariantList m_linePower;
    NetHw::RowModel *m_batteries = nullptr;
    NetHw::RowModel *m_peripherals = nullptr;
    quint64 m_upowerGeneration = 0;
    QTimer m_upowerTimer;

    bool m_profilesAvailable = false;
    QString m_profilesStatus;
    QString m_profilesService, m_profilesPath, m_profilesInterface;
    QVariantList m_profiles;
    QString m_activeProfile;
    QString m_performanceDegraded;
    QVariantList m_profileHolds;
    QString m_profilesVersion;
    QTimer m_profilesTimer;

    QString m_backlightName;
    QString m_backlightType;
    int m_backlightMax = 0;
    int m_backlightValue = 0;
    QTimer m_backlightTimer;

    bool m_logindAvailable = false;
    QString m_canSuspend = QStringLiteral("unknown");
    QString m_canHibernate = QStringLiteral("unknown");
    QString m_lidAction, m_lidActionExternalPower, m_lidActionDocked, m_lidActionSource;

    QMap<QString, int> m_idle;
};
