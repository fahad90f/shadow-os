// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusnetwork.h"

#include "statusdbus.h"

#include <QJsonArray>
#include <QStringDecoder>

namespace StatusModule {

namespace {

const QString kService = QStringLiteral("org.freedesktop.NetworkManager");
const QString kPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString kManager = QStringLiteral("org.freedesktop.NetworkManager");
const QString kDevice = QStringLiteral("org.freedesktop.NetworkManager.Device");
const QString kWireless = QStringLiteral("org.freedesktop.NetworkManager.Device.Wireless");
const QString kActive = QStringLiteral("org.freedesktop.NetworkManager.Connection.Active");
const QString kAccessPoint = QStringLiteral("org.freedesktop.NetworkManager.AccessPoint");

// Values of NetworkManager's D-Bus enumerations that matter here.
constexpr int kStateAsleep = 10;                // NMState
constexpr int kStateConnecting = 40;
constexpr int kStateConnectedLocal = 50;
constexpr int kDeviceTypeWifi = 2;              // NMDeviceType
constexpr int kDeviceStateActivated = 100;      // NMDeviceState
constexpr int kActiveStateActivating = 1;       // NMActiveConnectionState
constexpr int kActiveStateActivated = 2;

QString stateOf(int nmState)
{
    if (nmState >= kStateConnectedLocal)
        return QStringLiteral("connected");
    if (nmState == kStateConnecting)
        return QStringLiteral("connecting");
    if (nmState == kStateAsleep)
        return QStringLiteral("asleep");
    return QStringLiteral("disconnected");      // unknown, disconnected, disconnecting
}

QString connectivityOf(int connectivity)
{
    switch (connectivity) {     // NMConnectivityState
    case 1: return QStringLiteral("none");
    case 2: return QStringLiteral("portal");
    case 3: return QStringLiteral("limited");
    case 4: return QStringLiteral("full");
    default: return QStringLiteral("unknown");
    }
}

// The medium of a connection type (connection.type of the profile); empty
// for virtual ones (loopback, bridges, tunnels) that only ride on another.
QString kindOf(const QString &type)
{
    if (type == QLatin1String("802-3-ethernet") || type == QLatin1String("pppoe"))
        return QStringLiteral("wired");
    if (type == QLatin1String("802-11-wireless"))
        return QStringLiteral("wifi");
    if (type == QLatin1String("gsm") || type == QLatin1String("cdma"))
        return QStringLiteral("mobile");
    if (type == QLatin1String("bluetooth"))
        return QStringLiteral("bluetooth");
    return {};
}

int rankOf(const QString &kind)
{
    if (kind == QLatin1String("wired"))
        return 4;
    if (kind == QLatin1String("wifi"))
        return 3;
    if (kind == QLatin1String("mobile"))
        return 2;
    return 1;
}

bool isVpnType(const QString &type)
{
    return type == QLatin1String("vpn") || type == QLatin1String("wireguard");
}

QString decodeSsid(const QByteArray &raw)
{
    // An SSID is up to 32 arbitrary bytes; nearly all are UTF-8.
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString text = utf8(raw);
    return utf8.hasError() ? QString::fromLatin1(raw) : text;
}

} // namespace

NetworkBackend::NetworkBackend(QObject *parent)
    : Backend(parent), m_bus(QDBusConnection::systemBus())
{
    m_manager = new DBusProperties(m_bus, kService, kPath, kManager, this);
    connect(m_manager, &DBusProperties::changed, this, &NetworkBackend::sync);
    dbusWatchService(m_bus, kService, this, [this] { m_manager->load(); }, [this] { m_manager->clear(); });
    m_manager->load();
}

DBusProperties *NetworkBackend::track(const QString &path, const QString &interface)
{
    auto *properties = new DBusProperties(m_bus, kService, path, interface, this);
    connect(properties, &DBusProperties::changed, this, &NetworkBackend::sync);
    properties->load();
    return properties;
}

void NetworkBackend::drop(DBusProperties *properties)
{
    if (!properties)
        return;
    properties->disconnect(this);
    properties->deleteLater();
}

// Brings the set of tracked objects (devices, active connections, the access
// point in use) in line with what the manager object lists, then derives the
// values shown. Runs after every change of any tracked object.
void NetworkBackend::sync()
{
    if (!m_manager->isValid()) {
        for (const Device &entry : std::as_const(m_devices)) {
            drop(entry.device);
            drop(entry.wireless);
        }
        m_devices.clear();
        for (DBusProperties *connection : std::as_const(m_connections))
            drop(connection);
        m_connections.clear();
        drop(m_accessPoint);
        m_accessPoint = nullptr;
        if (!m_manager->isLoaded())
            return;
        if (m_now != Snapshot()) {
            m_now = Snapshot();
            Q_EMIT changed();
        }
        setAvailable(false, m_bus.isConnected() ? tr("NetworkManager is not running.")
                                                : tr("The system bus is not available."));
        return;
    }

    const QStringList devicePaths = dbusObjectPaths(m_manager->value(QStringLiteral("Devices")));
    for (auto it = m_devices.begin(); it != m_devices.end();) {
        if (devicePaths.contains(it.key())) {
            ++it;
            continue;
        }
        drop(it->device);
        drop(it->wireless);
        it = m_devices.erase(it);
    }
    for (const QString &path : devicePaths) {
        Device &entry = m_devices[path];
        if (!entry.device)
            entry.device = track(path, kDevice);
        if (!entry.wireless && entry.device->isValid()
            && entry.device->integer(QStringLiteral("DeviceType")) == kDeviceTypeWifi)
            entry.wireless = track(path, kWireless);
    }

    const QStringList activePaths = dbusObjectPaths(m_manager->value(QStringLiteral("ActiveConnections")));
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        if (activePaths.contains(it.key())) {
            ++it;
            continue;
        }
        drop(it.value());
        it = m_connections.erase(it);
    }
    for (const QString &path : activePaths) {
        if (!m_connections.contains(path))
            m_connections.insert(path, track(path, kActive));
    }

    // The access point of the first Wi-Fi device that is using one.
    QString accessPointPath;
    for (const Device &entry : std::as_const(m_devices)) {
        if (!entry.wireless || !entry.wireless->isValid())
            continue;
        accessPointPath = dbusObjectPath(entry.wireless->value(QStringLiteral("ActiveAccessPoint")));
        if (!accessPointPath.isEmpty())
            break;
    }
    if (accessPointPath != (m_accessPoint ? m_accessPoint->path() : QString())) {
        drop(m_accessPoint);
        m_accessPoint = accessPointPath.isEmpty() ? nullptr : track(accessPointPath, kAccessPoint);
    }

    bool settled = !m_accessPoint || m_accessPoint->isLoaded();
    for (const Device &entry : std::as_const(m_devices))
        settled = settled && entry.device->isLoaded() && (!entry.wireless || entry.wireless->isLoaded());
    for (const DBusProperties *connection : std::as_const(m_connections))
        settled = settled && connection->isLoaded();
    recompute(settled);
}

void NetworkBackend::recompute(bool settled)
{
    Snapshot s;
    const int nmState = int(m_manager->integer(QStringLiteral("State")));
    s.state = stateOf(nmState);
    s.connectivity = connectivityOf(int(m_manager->integer(QStringLiteral("Connectivity"))));
    s.wifiEnabled = m_manager->boolean(QStringLiteral("WirelessEnabled"));
    s.wifiHardwareEnabled = m_manager->boolean(QStringLiteral("WirelessHardwareEnabled"), true);

    for (const Device &entry : std::as_const(m_devices)) {
        if (!entry.wireless)
            continue;
        s.wifiAvailable = true;
        if (!m_accessPoint || !m_accessPoint->isValid() || !entry.wireless->isValid())
            continue;
        if (dbusObjectPath(entry.wireless->value(QStringLiteral("ActiveAccessPoint"))) != m_accessPoint->path())
            continue;
        s.wifiSsid = decodeSsid(m_accessPoint->value(QStringLiteral("Ssid")).toByteArray());
        s.wifiStrength = qBound(0, int(m_accessPoint->integer(QStringLiteral("Strength"))), 100);
        s.wifiConnected = entry.device->integer(QStringLiteral("State")) == kDeviceStateActivated;
    }

    // What carries the traffic: the connection NetworkManager names as the
    // primary one if it is a physical medium; otherwise (nothing primary, or
    // a tunnel) the best physical connection that is up or coming up.
    const QString primaryPath = dbusObjectPath(m_manager->value(QStringLiteral("PrimaryConnection")));
    int bestRank = -1;
    QString bestKind, bestName;
    for (auto it = m_connections.cbegin(); it != m_connections.cend(); ++it) {
        const DBusProperties *connection = it.value();
        if (!connection->isValid())
            continue;
        const QString type = connection->string(QStringLiteral("Type"));
        const int activeState = int(connection->integer(QStringLiteral("State")));
        if (connection->boolean(QStringLiteral("Vpn")) || isVpnType(type)) {
            if (activeState == kActiveStateActivated && !s.vpnActive) {
                s.vpnActive = true;
                s.vpnName = connection->string(QStringLiteral("Id"));
            }
            continue;
        }
        const QString kind = kindOf(type);
        if (kind.isEmpty() || (activeState != kActiveStateActivated && activeState != kActiveStateActivating))
            continue;
        const int rank = rankOf(kind) + (activeState == kActiveStateActivated ? 10 : 0)
                         + (it.key() == primaryPath ? 100 : 0);
        if (rank > bestRank) {
            bestRank = rank;
            bestKind = kind;
            bestName = connection->string(QStringLiteral("Id"));
        }
    }
    if (s.state == QLatin1String("connected") || s.state == QLatin1String("connecting")) {
        s.primaryKind = bestKind;
        s.primaryName = bestName;
        if (bestKind.isEmpty() && s.state == QLatin1String("connected")) {
            // Connected through something that is not a physical medium of
            // its own (a bridge, a tunnel).
            s.primaryKind = QStringLiteral("other");
            if (const DBusProperties *primary = m_connections.value(primaryPath); primary && primary->isValid())
                s.primaryName = primary->string(QStringLiteral("Id"));
        }
    }

    if (s != m_now) {
        m_now = s;
        Q_EMIT changed();
    }
    if (settled || isReady())
        setAvailable(true);
}

void NetworkBackend::setWifiEnabled(bool enabled)
{
    if (!isAvailable())
        return;
    m_manager->set(QStringLiteral("WirelessEnabled"), enabled, [this, enabled](const QString &error) {
        if (error.isEmpty())
            return;
        Q_EMIT failed(enabled ? tr("Wi-Fi could not be switched on: %1").arg(error)
                              : tr("Wi-Fi could not be switched off: %1").arg(error));
    });
}

QJsonObject NetworkBackend::details() const
{
    QJsonObject json;
    if (!isAvailable())
        return json;
    json.insert(QStringLiteral("version"), m_manager->string(QStringLiteral("Version")));
    json.insert(QStringLiteral("nmState"), int(m_manager->integer(QStringLiteral("State"))));
    json.insert(QStringLiteral("state"), m_now.state);
    json.insert(QStringLiteral("connectivity"), m_now.connectivity);
    json.insert(QStringLiteral("primary"), QJsonObject{
        {QStringLiteral("kind"), m_now.primaryKind},
        {QStringLiteral("name"), m_now.primaryName},
        {QStringLiteral("type"), m_manager->string(QStringLiteral("PrimaryConnectionType"))},
    });
    json.insert(QStringLiteral("wifi"), QJsonObject{
        {QStringLiteral("available"), m_now.wifiAvailable},
        {QStringLiteral("enabled"), m_now.wifiEnabled},
        {QStringLiteral("hardwareEnabled"), m_now.wifiHardwareEnabled},
        {QStringLiteral("connected"), m_now.wifiConnected},
        {QStringLiteral("ssid"), m_now.wifiSsid},
        {QStringLiteral("strength"), m_now.wifiStrength},
    });
    json.insert(QStringLiteral("vpn"), QJsonObject{
        {QStringLiteral("active"), m_now.vpnActive},
        {QStringLiteral("name"), m_now.vpnName},
    });
    QJsonArray connections;
    for (const DBusProperties *connection : std::as_const(m_connections)) {
        if (!connection->isValid())
            continue;
        connections.append(QJsonObject{
            {QStringLiteral("id"), connection->string(QStringLiteral("Id"))},
            {QStringLiteral("type"), connection->string(QStringLiteral("Type"))},
            {QStringLiteral("state"), int(connection->integer(QStringLiteral("State")))},
            {QStringLiteral("vpn"), connection->boolean(QStringLiteral("Vpn"))},
        });
    }
    json.insert(QStringLiteral("activeConnections"), connections);
    json.insert(QStringLiteral("deviceCount"), int(m_devices.size()));
    return json;
}

} // namespace StatusModule
