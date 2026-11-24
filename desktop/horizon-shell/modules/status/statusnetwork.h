// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

#include <QDBusConnection>
#include <QHash>
#include <QVariantList>

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
//   active connections  Id, Uuid, Type, Vpn, State, Default
//   settings object     ListConnections, NewConnection, ConnectionRemoved;
//                       GetSettings of each saved connection (its
//                       "connection" group only: id, uuid, type, timestamp),
//                       to list the VPN and WireGuard profiles
//   devices             DeviceType, State; Wi-Fi devices: ActiveAccessPoint
//   access point in use Ssid, Strength
//
// Switching Wi-Fi writes the manager's WirelessEnabled property; polkit
// (org.freedesktop.NetworkManager.enable-disable-wifi) decides whether the
// user may. Connecting a VPN profile is the manager's ActivateConnection,
// disconnecting its DeactivateConnection (polkit:
// org.freedesktop.NetworkManager.network-control). A profile whose password
// is not saved cannot be connected from here: the shell is not a secret
// agent, and NetworkManager's refusal is shown as it is.
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
    // A VPN or WireGuard connection is up.
    Q_PROPERTY(bool vpnActive READ vpnActive NOTIFY changed)
    Q_PROPERTY(QString vpnName READ vpnName NOTIFY changed)
    // One is coming up; vpnName is its name then.
    Q_PROPERTY(bool vpnConnecting READ vpnConnecting NOTIFY changed)
    // The saved profiles, the most recently used first: [{uuid, name, kind ("vpn" | "wireguard")}].
    Q_PROPERTY(QVariantList vpnProfiles READ vpnProfiles NOTIFY vpnProfilesChanged)

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
    bool vpnConnecting() const { return m_now.vpnConnecting; }
    QVariantList vpnProfiles() const { return m_vpnProfiles; }

    Q_INVOKABLE void setWifiEnabled(bool enabled);
    // Connects the profile with this uuid; "" means the most recently used.
    Q_INVOKABLE void connectVpn(const QString &uuid = QString());
    // Takes down every VPN connection that is up or coming up.
    Q_INVOKABLE void disconnectVpn();
    // Reads the saved profiles again (they are also followed as they are
    // added and removed; this picks up a changed name or order of use).
    Q_INVOKABLE void reloadVpnProfiles();

Q_SIGNALS:
    void changed();
    void vpnProfilesChanged();

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
        bool vpnConnecting = false;
        QString vpnName;
        bool operator==(const Snapshot &other) const = default;
    };
    struct VpnProfile {
        QString path;       // the saved connection's object path
        QString uuid;
        QString name;
        QString kind;
        qulonglong lastUsed = 0;
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
    QList<VpnProfile> m_profiles;
    QVariantList m_vpnProfiles;
    int m_profileGeneration = 0;

private Q_SLOTS:
    void onSavedConnectionsChanged();
};

} // namespace StatusModule
