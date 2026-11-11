// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <command.h>

#include <QDBusMessage>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class QDBusObjectPath;

// A connection profile as NetworkManager transfers it: a{sa{sv}}.
using NmSettings = QMap<QString, QVariantMap>;
// GetManagedObjects(): a{oa{sa{sv}}}.
using NmObjects = QMap<QDBusObjectPath, NmSettings>;

// NetworkManager front-end: devices, connection profiles, Wi-Fi and VPN,
// through NetworkManager's D-Bus API on the system bus
// (org.freedesktop.NetworkManager). Authorisation is NetworkManager's own
// polkit policy — this application holds no privilege.
//
//   reading    org.freedesktop.DBus.ObjectManager.GetManagedObjects for the
//              manager, devices, access points, IP configurations and active
//              connections; Settings.Connection.GetSettings per profile
//   changing   ActivateConnection, AddAndActivateConnection,
//              DeactivateConnection, Device.Disconnect,
//              Device.Wireless.RequestScan, Settings.Connection.Update/Delete,
//              the WirelessEnabled property
//   following  PropertiesChanged, InterfacesAdded/Removed and the StateChanged
//              signals of devices and active connections
//
// Two things have no D-Bus method and go through nmcli, NetworkManager's own
// client, which implements them on top of the same API: importing a VPN
// configuration file (the import code lives in nmcli and the VPN plug-ins)
// and answering a request for a secret that is not stored in the profile
// (nmcli acts as the secret agent for that one activation).
//
// Secrets: a new Wi-Fi network's passphrase is part of the profile sent to
// NetworkManager over the system bus; a secret for an existing profile is
// written to nmcli's standard input (`passwd-file /dev/stdin`). Neither
// appears in an argument vector, a log or a settings file of this application.
class Nm : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // "unknown" until the first answer, then "running", "not-running" (the
    // daemon is not started), "unavailable" (it cannot be reached; see
    // statusText) or "missing" (nmcli is not installed).
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool running READ running NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString version READ version NOTIFY generalChanged)
    Q_PROPERTY(QString state READ state NOTIFY generalChanged)
    Q_PROPERTY(QString connectivity READ connectivity NOTIFY generalChanged)
    Q_PROPERTY(bool networkingEnabled READ networkingEnabled NOTIFY generalChanged)
    Q_PROPERTY(QString wifiHardware READ wifiHardware NOTIFY generalChanged)   // "enabled" | "disabled" | "missing"
    Q_PROPERTY(bool wifiEnabled READ wifiEnabled NOTIFY generalChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList wifiDevices READ wifiDevices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList connections READ connections NOTIFY connectionsChanged)
    Q_PROPERTY(QVariantList savedWifi READ savedWifi NOTIFY connectionsChanged)
    Q_PROPERTY(QVariantList vpnConnections READ vpnConnections NOTIFY connectionsChanged)
    Q_PROPERTY(int activeVpnCount READ activeVpnCount NOTIFY connectionsChanged)
    Q_PROPERTY(QVariantList wifiNetworks READ wifiNetworks NOTIFY wifiNetworksChanged)
    Q_PROPERTY(QStringList vpnPlugins READ vpnPlugins NOTIFY vpnPluginsChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    // Devices, profile UUIDs, SSIDs or file paths an operation is running on.
    Q_PROPERTY(QStringList busyTargets READ busyTargets NOTIFY busyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit Nm(QObject *parent = nullptr);

    bool active() const { return m_active; }
    void setActive(bool active);
    QString status() const { return m_status; }
    bool running() const { return m_status == QLatin1String("running"); }
    QString statusText() const { return m_statusText; }
    QString version() const { return m_version; }
    QString state() const { return m_state; }
    QString connectivity() const { return m_connectivity; }
    bool networkingEnabled() const { return m_networking; }
    QString wifiHardware() const { return m_wifiHardware; }
    bool wifiEnabled() const { return m_wifiEnabled; }
    QVariantList devices() const { return m_devices; }
    QVariantList wifiDevices() const;
    QVariantList connections() const { return m_connections; }
    QVariantList savedWifi() const;
    QVariantList vpnConnections() const;
    int activeVpnCount() const;
    QVariantList wifiNetworks() const { return m_wifiNetworks; }
    QStringList vpnPlugins() const { return m_vpnPlugins; }
    bool scanning() const { return m_scanning; }
    QStringList busyTargets() const { return m_busyTargets; }
    bool busy() const { return !m_busyTargets.isEmpty(); }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void refreshWifi(bool rescan);
    QVariantMap deviceByName(const QString &name) const;
    QVariantMap connectionByUuid(const QString &uuid) const;

    // ---- devices -------------------------------------------------------------
    Q_INVOKABLE void connectDevice(const QString &interface);
    Q_INVOKABLE void disconnectDevice(const QString &interface);

    // ---- connection profiles ---------------------------------------------------
    Q_INVOKABLE void activateConnection(const QString &uuid);
    // Retry an activation that asked for a secret (see secretsRequired()).
    Q_INVOKABLE void activateWithSecret(const QString &uuid, const QString &secret);
    Q_INVOKABLE void deactivateConnection(const QString &uuid);
    Q_INVOKABLE void deleteConnection(const QString &uuid);
    Q_INVOKABLE void setAutoconnect(const QString &uuid, bool autoconnect);

    // ---- Wi-Fi -------------------------------------------------------------------
    Q_INVOKABLE void setWifiEnabled(bool enabled);
    // Empty if 'password' can be a WPA passphrase or key, else what is wrong with it.
    Q_INVOKABLE QString wifiPasswordError(const QString &password) const;
    // Connect to a network that has no saved profile yet. 'password' is empty
    // for open networks.
    Q_INVOKABLE void connectWifi(const QString &ssid, const QString &interface, const QString &password, bool hidden);

    // ---- VPN ---------------------------------------------------------------------
    // "wireguard", "openvpn" or "" judging from the file's content and name.
    Q_INVOKABLE QString sniffVpnType(const QString &path) const;
    bool vpnPluginInstalled(const QString &type) const;
    Q_INVOKABLE void importVpn(const QString &type, const QString &path);

    // ---- IP and DNS configuration ---------------------------------------------------
    // Validate an edit of a profile and describe what it would change, without
    // touching anything. 'config' may contain "ipv4" and "ipv6" maps (method,
    // addresses, gateway, dns, search, ignoreAutoDns) and "dnsOverTls"; absent
    // keys keep their current value. Result: ok, errors[], warnings[],
    // changes[{property,label,from,to}], normalized (the resulting settings), active, device.
    Q_INVOKABLE QVariantMap planIpChange(const QString &uuid, const QVariantMap &config) const;
    // Re-validate, update the profile (Settings.Connection.Update) and, if asked and the profile
    // is active, re-activate it so the change takes effect.
    Q_INVOKABLE void applyIpChange(const QString &uuid, const QVariantMap &config, bool reactivate);

    QJsonObject dumpGeneral() const;
    QJsonObject dumpWifi() const;
    QJsonObject dumpConnections() const;
    QJsonObject dumpVpn() const;

Q_SIGNALS:
    void activeChanged();
    void statusChanged();
    void generalChanged();
    void devicesChanged();
    void connectionsChanged();
    void wifiNetworksChanged();
    void vpnPluginsChanged();
    void scanningChanged();
    void busyChanged();
    void refreshed();
    // Result of a user-initiated operation, for a toast.
    void operationFinished(const QString &operation, const QString &target, bool ok, const QString &message);
    // An activation needs a secret that is not stored; ask and call activateWithSecret().
    void secretsRequired(const QString &uuid, const QString &name);

public Q_SLOTS:
    void scheduleRefresh();

private Q_SLOTS:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);
    void onProfileUpdated();
    void onActiveStateChanged(uint state, uint reason, const QDBusMessage &message);
    void onDeviceStateChanged(uint newState, uint oldState, uint reason, const QDBusMessage &message);

private:
    using Done = std::function<void(const Shadow::CommandResult &)>;
    using Reply = std::function<void(const QDBusMessage &)>;

    // An activation in progress, by the path of its active-connection object.
    struct Activation
    {
        QString operation, target, name;
        QString successText, noSecretsText;
        QString createdProfile;         // deleted again if the activation fails
        QStringList devices;
        bool askForSecret = false;      // on "no secrets": emit secretsRequired() instead of failing
        bool keepProfile = false;
        QTimer *timeout = nullptr;
    };

    void call(const QString &path, const QString &interface, const QString &method, const QVariantList &arguments,
              Reply done, int timeoutMs = 20000);
    bool ensureRunning(const QString &operation, const QString &target);
    void trackActivation(const QString &path, const Activation &activation);
    void activationStarted(const QDBusMessage &reply, int activeArgument, Activation activation);
    void finishActivation(const QString &path, bool ok, uint activeReason);
    void simpleResult(const QString &operation, const QString &target, const QDBusMessage &reply, const QString &successText);
    void updateProfile(const QString &profile, std::function<void(NmSettings &)> mutate, Reply done);
    void finishScan(const QString &error);
    QVariantMap properties(const QString &path, const QString &interface) const;
    QString devicePath(const QString &interfaceName) const;
    QString profilePath(const QString &uuid) const;
    QString activePath(const QString &uuid) const;
    void buildGeneral();
    void buildDevices();
    void buildConnections();
    void buildWifiNetworks();
    void nmcli(const QStringList &arguments, Done done, int timeoutMs = 20000, const QByteArray &stdinData = {});
    void operation(const QString &name, const QString &target, const QStringList &arguments, int timeoutMs,
                   const QByteArray &stdinData = {}, Done after = {});
    void failOperation(const QString &name, const QString &target, const QString &message);
    void beginBusy(const QString &target);
    void endBusy(const QString &target);
    void setStatus(const QString &status, const QString &text);
    void stepDone();
    void finishRefresh();
    void loadVpnPlugins();
    void clearData();
    static QString errorText(const Shadow::CommandResult &r);

    bool m_active = false;
    bool m_refreshing = false;
    bool m_refreshQueued = false;
    int m_pending = 0;
    QString m_nmcli;
    QString m_status = QStringLiteral("unknown");
    QString m_statusText;
    QString m_version, m_state, m_connectivity, m_wifiHardware;
    bool m_networking = false, m_wifiEnabled = false;
    QVariantList m_devices, m_connections, m_wifiNetworks;
    QStringList m_vpnPlugins;
    bool m_scanning = false;
    QStringList m_busyTargets;

    // NetworkManager's object tree as of the last refresh, by object path,
    // and the content of each profile (fetched once, again after "Updated").
    QHash<QString, NmSettings> m_objects;
    QHash<QString, NmSettings> m_settingsCache;
    QHash<QString, Activation> m_activations;
    QHash<QString, uint> m_deviceFailure;       // device path → reason of its last failure
    bool m_scanReportDue = false;
    QTimer m_scanTimer;

    QTimer m_debounce;
    QTimer m_slowTimer;
};
