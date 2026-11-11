// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

#include <QDBusConnection>
#include <QHash>

namespace StatusModule {

class DBusProperties;

// Network state for the panel indicator and the Wi-Fi tile.
//
// Source: NetworkManager on the system bus (org.freedesktop.NetworkManager),
// read with org.freedesktop.DBus.Properties and followed through
// PropertiesChanged; nothing is polled.
//
//   manager object      State, Connectivity, PrimaryConnection(Type),
//                       ActiveConnections, Devices, WirelessEnabled,
//                       WirelessHardwareEnabled, Version
//   active connections  Id, Type, Vpn, State, Default
//   devices             DeviceType, State; Wi-Fi devices: ActiveAccessPoint
//   access point in use Ssid, Strength
//
// Switching Wi-Fi writes the manager's WirelessEnabled property; polkit
// (org.freedesktop.NetworkManager.enable-disable-wifi) decides whether the
// user may.
class NetworkBackend : public Backend
{
    Q_OBJECT
    // "disconnected" | "connecting" | "connected" | "asleep" (networking is switched off)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    // "unknown" | "none" | "portal" | "limited" | "full"
    Q_PROPERTY(QString connectivity READ connectivity NOTIFY changed)
    // What carries the traffic: "wired" | "wifi" | "mobile" | "bluetooth" | "other" | "" (nothing)
    Q_PROPERTY(QString primaryKind READ primaryKind NOTIFY changed)
    Q_PROPERTY(QString primaryName READ primaryName NOTIFY changed)
    Q_PROPERTY(bool wifiAvailable READ wifiAvailable NOTIFY changed)
    Q_PROPERTY(bool wifiEnabled READ wifiEnabled NOTIFY changed)
    Q_PROPERTY(bool wifiHardwareEnabled READ wifiHardwareEnabled NOTIFY changed)
    Q_PROPERTY(bool wifiConnected READ wifiConnected NOTIFY changed)
    Q_PROPERTY(QString wifiSsid READ wifiSsid NOTIFY changed)
    Q_PROPERTY(int wifiStrength READ wifiStrength NOTIFY changed)
    Q_PROPERTY(bool vpnActive READ vpnActive NOTIFY changed)
    Q_PROPERTY(QString vpnName READ vpnName NOTIFY changed)

public:
    explicit NetworkBackend(QObject *parent = nullptr);

    QString state() const { return m_now.state; }
    QString connectivity() const { return m_now.connectivity; }
    QString primaryKind() const { return m_now.primaryKind; }
    QString primaryName() const { return m_now.primaryName; }
    bool wifiAvailable() const { return m_now.wifiAvailable; }
    bool wifiEnabled() const { return m_now.wifiEnabled; }
    bool wifiHardwareEnabled() const { return m_now.wifiHardwareEnabled; }
    bool wifiConnected() const { return m_now.wifiConnected; }
    QString wifiSsid() const { return m_now.wifiSsid; }
    int wifiStrength() const { return m_now.wifiStrength; }
    bool vpnActive() const { return m_now.vpnActive; }
    QString vpnName() const { return m_now.vpnName; }

    Q_INVOKABLE void setWifiEnabled(bool enabled);

Q_SIGNALS:
    void changed();

protected:
    QJsonObject details() const override;

private:
    struct Device {
        DBusProperties *device = nullptr;
        DBusProperties *wireless = nullptr;     // only for Wi-Fi devices
    };
    struct Snapshot {
        QString state = QStringLiteral("disconnected");
        QString connectivity = QStringLiteral("unknown");
        QString primaryKind;
        QString primaryName;
        bool wifiAvailable = false;
        bool wifiEnabled = false;
        bool wifiHardwareEnabled = false;
        bool wifiConnected = false;
        QString wifiSsid;
        int wifiStrength = 0;
        bool vpnActive = false;
        QString vpnName;
        bool operator==(const Snapshot &other) const = default;
    };

    DBusProperties *track(const QString &path, const QString &interface);
    void drop(DBusProperties *properties);
    void sync();
    void recompute(bool settled);

    QDBusConnection m_bus;
    DBusProperties *m_manager = nullptr;
    QHash<QString, Device> m_devices;
    QHash<QString, DBusProperties *> m_connections;
    DBusProperties *m_accessPoint = nullptr;
    Snapshot m_now;
};

} // namespace StatusModule
