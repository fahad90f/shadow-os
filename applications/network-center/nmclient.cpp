// SPDX-License-Identifier: GPL-3.0-or-later
#include "nmclient.h"

#include "netutil.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace {

QString typeLabel(const QString &type)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("802-3-ethernet"), QStringLiteral("Ethernet")},
        {QStringLiteral("802-11-wireless"), QStringLiteral("Wi-Fi")},
        {QStringLiteral("vpn"), QStringLiteral("VPN")},
        {QStringLiteral("wireguard"), QStringLiteral("WireGuard")},
        {QStringLiteral("loopback"), QStringLiteral("Loopback")},
        {QStringLiteral("bridge"), QStringLiteral("Bridge")},
        {QStringLiteral("bond"), QStringLiteral("Bond")},
        {QStringLiteral("vlan"), QStringLiteral("VLAN")},
        {QStringLiteral("tun"), QStringLiteral("TUN/TAP")},
        {QStringLiteral("gsm"), QStringLiteral("Mobile broadband")},
        {QStringLiteral("cdma"), QStringLiteral("Mobile broadband")},
        {QStringLiteral("bluetooth"), QStringLiteral("Bluetooth")},
        {QStringLiteral("pppoe"), QStringLiteral("PPPoE")},
        {QStringLiteral("dummy"), QStringLiteral("Dummy")},
        {QStringLiteral("veth"), QStringLiteral("Virtual Ethernet")},
    };
    return labels.value(type, type);
}

QString vpnLabel(const QString &type, const QString &service)
{
    if (type == QLatin1String("wireguard"))
        return QStringLiteral("WireGuard");
    const QString shortName = service.section(QLatin1Char('.'), -1);
    static const QHash<QString, QString> labels = {
        {QStringLiteral("openvpn"), QStringLiteral("OpenVPN")},
        {QStringLiteral("vpnc"), QStringLiteral("Cisco VPNC")},
        {QStringLiteral("openconnect"), QStringLiteral("OpenConnect")},
        {QStringLiteral("l2tp"), QStringLiteral("L2TP")},
        {QStringLiteral("pptp"), QStringLiteral("PPTP")},
        {QStringLiteral("strongswan"), QStringLiteral("IPsec (strongSwan)")},
        {QStringLiteral("libreswan"), QStringLiteral("IPsec (Libreswan)")},
        {QStringLiteral("fortisslvpn"), QStringLiteral("Fortinet SSL VPN")},
    };
    return shortName.isEmpty() ? QStringLiteral("VPN") : labels.value(shortName, shortName);
}

QString dotName(const QString &raw)
{
    if (raw == QLatin1String("0") || raw == QLatin1String("no"))
        return QStringLiteral("no");
    if (raw == QLatin1String("1") || raw == QLatin1String("opportunistic"))
        return QStringLiteral("opportunistic");
    if (raw == QLatin1String("2") || raw == QLatin1String("yes"))
        return QStringLiteral("yes");
    return QStringLiteral("default");
}

QString dotNumber(const QString &name)
{
    if (name == QLatin1String("no")) return QStringLiteral("0");
    if (name == QLatin1String("opportunistic")) return QStringLiteral("1");
    if (name == QLatin1String("yes")) return QStringLiteral("2");
    return QStringLiteral("-1");
}

// A list typed by the user: separated by commas, semicolons or white space.
QStringList userList(const QVariant &value)
{
    QStringList raw;
    if (value.typeId() == QMetaType::QStringList || value.typeId() == QMetaType::QVariantList)
        raw = value.toStringList();
    else
        raw = value.toString().split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    QStringList out;
    for (const QString &item : std::as_const(raw)) {
        const QString t = item.trimmed();
        if (!t.isEmpty())
            out << t;
    }
    return out;
}

QString shown(const QStringList &list)
{
    return list.isEmpty() ? QObject::tr("(none)") : list.join(QStringLiteral(", "));
}

QString shown(const QString &value)
{
    return value.isEmpty() ? QObject::tr("(none)") : value;
}

// nmcli's passwd-file format is "setting.property:value" per line; the value
// is stripped and C-style escapes are expanded, so a literal backslash and
// leading or trailing blanks must be escaped to survive unchanged.
QByteArray passwdFileValue(const QString &secret)
{
    QString v = secret;
    v.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    auto octal = [](QChar c) { return c == QLatin1Char('\t') ? QStringLiteral("\\011") : QStringLiteral("\\040"); };
    int lead = 0;
    while (lead < v.size() && (v.at(lead) == QLatin1Char(' ') || v.at(lead) == QLatin1Char('\t')))
        ++lead;
    QString head;
    for (int i = 0; i < lead; ++i)
        head += octal(v.at(i));
    v = head + v.mid(lead);
    int trail = 0;
    while (trail < v.size() && (v.at(v.size() - 1 - trail) == QLatin1Char(' ') || v.at(v.size() - 1 - trail) == QLatin1Char('\t')))
        ++trail;
    QString tail;
    for (int i = int(v.size()) - trail; i < v.size(); ++i)
        tail += octal(v.at(i));
    v = v.left(v.size() - trail) + tail;
    return v.toUtf8();
}

bool hasControlCharacters(const QString &s)
{
    for (const QChar c : s) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return true;
    }
    return false;
}


// ---- D-Bus plumbing ---------------------------------------------------------------

// Calls that change something may make NetworkManager ask polkit, and polkit
// the user, before they return; give the user time to type a password.
const int kAuthTimeoutMs = 120000;

const QString kNm = QStringLiteral("org.freedesktop.NetworkManager");
const QString kNmPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString kSettingsPath = QStringLiteral("/org/freedesktop/NetworkManager/Settings");
const QString kIfDevice = QStringLiteral("org.freedesktop.NetworkManager.Device");
const QString kIfWireless = QStringLiteral("org.freedesktop.NetworkManager.Device.Wireless");
const QString kIfWired = QStringLiteral("org.freedesktop.NetworkManager.Device.Wired");
const QString kIfAccessPoint = QStringLiteral("org.freedesktop.NetworkManager.AccessPoint");
const QString kIfActive = QStringLiteral("org.freedesktop.NetworkManager.Connection.Active");
const QString kIfSettingsConnection = QStringLiteral("org.freedesktop.NetworkManager.Settings.Connection");
const QString kIfIp4 = QStringLiteral("org.freedesktop.NetworkManager.IP4Config");
const QString kIfIp6 = QStringLiteral("org.freedesktop.NetworkManager.IP6Config");
const QString kIfProperties = QStringLiteral("org.freedesktop.DBus.Properties");

QVariant objectPath(const QString &path)
{
    return QVariant::fromValue(QDBusObjectPath(path.isEmpty() ? QStringLiteral("/") : path));
}

QString pathOf(const QVariant &v)
{
    const QString p = qdbus_cast<QDBusObjectPath>(v).path();
    return p == QLatin1String("/") ? QString() : p;
}

QStringList pathsOf(const QVariant &v)
{
    QStringList out;
    const QList<QDBusObjectPath> list = qdbus_cast<QList<QDBusObjectPath>>(v);
    for (const QDBusObjectPath &p : list)
        out << p.path();
    return out;
}

// What a failed call says, without the D-Bus error name.
QString dbusError(const QDBusMessage &reply)
{
    const QString name = reply.errorName();
    if (name == QLatin1String("org.freedesktop.DBus.Error.NoReply") || name == QLatin1String("org.freedesktop.DBus.Error.Timeout"))
        return QObject::tr("NetworkManager did not answer in time.");
    if (name == QLatin1String("org.freedesktop.DBus.Error.AccessDenied")
        || name == QLatin1String("org.freedesktop.NetworkManager.PermissionDenied")
        || name.endsWith(QLatin1String(".PermissionDenied")))
        return QObject::tr("Not authorised: %1").arg(reply.errorMessage());
    QString text = Net::cleanMessage(reply.errorMessage());
    // "No suitable device found for this connection (device lo not available because …)":
    // the list of every device that does not fit is noise.
    if (text.startsWith(QLatin1String("No suitable device found")) && text.contains(QLatin1String(" (")))
        text = text.left(text.indexOf(QLatin1String(" ("))) + QLatin1Char('.');
    return text.isEmpty() ? name : text;
}

// ---- NetworkManager's enumerations as nmcli words -----------------------------------

QString managerStateName(uint state)
{
    switch (state) {
    case 10: return QStringLiteral("asleep");
    case 20: return QStringLiteral("disconnected");
    case 30: return QStringLiteral("disconnecting");
    case 40: return QStringLiteral("connecting");
    case 50: return QStringLiteral("connected (local only)");
    case 60: return QStringLiteral("connected (site only)");
    case 70: return QStringLiteral("connected");
    default: return QStringLiteral("unknown");
    }
}

QString connectivityName(uint connectivity)
{
    switch (connectivity) {
    case 1: return QStringLiteral("none");
    case 2: return QStringLiteral("portal");
    case 3: return QStringLiteral("limited");
    case 4: return QStringLiteral("full");
    default: return QStringLiteral("unknown");
    }
}

QString deviceTypeName(uint type)
{
    static const QHash<uint, QString> names = {
        {1, QStringLiteral("ethernet")}, {2, QStringLiteral("wifi")}, {5, QStringLiteral("bt")},
        {6, QStringLiteral("olpc-mesh")}, {7, QStringLiteral("wimax")}, {8, QStringLiteral("modem")},
        {9, QStringLiteral("infiniband")}, {10, QStringLiteral("bond")}, {11, QStringLiteral("vlan")},
        {12, QStringLiteral("adsl")}, {13, QStringLiteral("bridge")}, {14, QStringLiteral("generic")},
        {15, QStringLiteral("team")}, {16, QStringLiteral("tun")}, {17, QStringLiteral("iptunnel")},
        {18, QStringLiteral("macvlan")}, {19, QStringLiteral("vxlan")}, {20, QStringLiteral("veth")},
        {21, QStringLiteral("macsec")}, {22, QStringLiteral("dummy")}, {23, QStringLiteral("ppp")},
        {24, QStringLiteral("ovs-interface")}, {25, QStringLiteral("ovs-port")}, {26, QStringLiteral("ovs-bridge")},
        {27, QStringLiteral("wpan")}, {28, QStringLiteral("6lowpan")}, {29, QStringLiteral("wireguard")},
        {30, QStringLiteral("wifi-p2p")}, {31, QStringLiteral("vrf")}, {32, QStringLiteral("loopback")},
        {33, QStringLiteral("hsr")},
    };
    return names.value(type, QStringLiteral("unknown"));
}

QString deviceStateText(uint state)
{
    switch (state) {
    case 10: return QObject::tr("unmanaged");
    case 20: return QObject::tr("unavailable");
    case 30: return QObject::tr("disconnected");
    case 40: return QObject::tr("connecting (prepare)");
    case 50: return QObject::tr("connecting (configuring)");
    case 60: return QObject::tr("connecting (need authentication)");
    case 70: return QObject::tr("connecting (getting IP configuration)");
    case 80: return QObject::tr("connecting (checking IP connectivity)");
    case 90: return QObject::tr("connecting (starting secondary connections)");
    case 100: return QObject::tr("connected");
    case 110: return QObject::tr("deactivating");
    case 120: return QObject::tr("connection failed");
    default: return QObject::tr("unknown");
    }
}

// NMDeviceStateReason: the ones a user can act upon; the rest by number.
QString deviceReasonText(uint reason)
{
    switch (reason) {
    case 0: case 1: return QString();
    case 2: return QObject::tr("the device is now managed");
    case 3: return QObject::tr("the device is now unmanaged");
    case 4: return QObject::tr("the configuration failed");
    case 5: return QObject::tr("the IP configuration could not be reserved (no address available, or a timeout)");
    case 6: return QObject::tr("the IP configuration expired");
    case 7: return QObject::tr("secrets were required, but not provided");
    case 8: return QObject::tr("the 802.1X supplicant disconnected");
    case 9: return QObject::tr("the 802.1X supplicant configuration failed");
    case 10: return QObject::tr("the 802.1X supplicant failed");
    case 11: return QObject::tr("the 802.1X supplicant took too long to authenticate");
    case 15: return QObject::tr("the DHCP client failed to start");
    case 16: return QObject::tr("the DHCP client reported an error");
    case 17: return QObject::tr("the DHCP client failed");
    case 35: return QObject::tr("the necessary firmware for the device may be missing");
    case 36: return QObject::tr("the device was removed");
    case 37: return QObject::tr("NetworkManager went to sleep");
    case 38: return QObject::tr("the connection profile was removed");
    case 39: return QObject::tr("disconnected by the user");
    case 40: return QObject::tr("the cable is unplugged or the link went down");
    case 41: return QObject::tr("the existing configuration of the device was taken over");
    case 42: return QObject::tr("the supplicant is now available");
    case 43: return QObject::tr("the modem could not be found");
    case 53: return QObject::tr("the Wi-Fi network could not be found");
    case 54: return QObject::tr("a secondary connection of the base connection failed");
    case 60: return QObject::tr("a new activation was started");
    case 62: return QObject::tr("the IP address is already in use on the network");
    case 67: return QObject::tr("the peer could not be found");
    default: return QObject::tr("reason %1").arg(reason);
    }
}

// NMActiveConnectionStateReason.
QString activeReasonText(uint reason)
{
    switch (reason) {
    case 2: return QObject::tr("it was disconnected by the user");
    case 3: return QObject::tr("the device was disconnected");
    case 4: return QObject::tr("the service providing the VPN stopped");
    case 5: return QObject::tr("the IP configuration is not valid");
    case 6: return QObject::tr("the connection attempt timed out");
    case 7: return QObject::tr("the VPN service did not start in time");
    case 8: return QObject::tr("the VPN service failed to start");
    case 9: return QObject::tr("secrets were required, but not provided");
    case 10: return QObject::tr("authentication to the server failed");
    case 11: return QObject::tr("the connection profile was deleted");
    case 12: return QObject::tr("the connection it depends on failed");
    case 13: return QObject::tr("the device could not be created");
    case 14: return QObject::tr("the device was removed");
    default: return QString();
    }
}

QString activeStateName(uint state)
{
    switch (state) {
    case 1: return QStringLiteral("activating");
    case 2: return QStringLiteral("activated");
    case 3: return QStringLiteral("deactivating");
    default: return QString();
    }
}

int wifiChannel(uint mhz)
{
    if (mhz == 2484)
        return 14;
    if (mhz >= 2412 && mhz <= 2472)
        return int(mhz - 2407) / 5;
    if (mhz >= 5955 && mhz <= 7115)
        return int(mhz - 5950) / 5;
    if (mhz >= 5000 && mhz <= 5900)
        return int(mhz - 5000) / 5;
    return 0;
}

// NM80211ApSecurityFlags
enum : uint { KeyMgmtPsk = 0x100, KeyMgmt8021x = 0x200, KeyMgmtSae = 0x400, KeyMgmtOwe = 0x800, KeyMgmtOweTm = 0x1000,
              KeyMgmtEapSuiteB = 0x2000 };

// The words nmcli uses in its SECURITY column.
QString apSecurity(uint flags, uint wpa, uint rsn)
{
    QStringList words;
    if ((flags & 0x1) && wpa == 0 && rsn == 0)
        words << QStringLiteral("WEP");
    if (wpa != 0)
        words << QStringLiteral("WPA1");
    if (rsn & (KeyMgmtPsk | KeyMgmt8021x))
        words << QStringLiteral("WPA2");
    if (rsn & KeyMgmtSae)
        words << QStringLiteral("WPA3");
    if (rsn & (KeyMgmtOwe | KeyMgmtOweTm))
        words << QStringLiteral("OWE");
    if ((wpa & KeyMgmt8021x) || (rsn & (KeyMgmt8021x | KeyMgmtEapSuiteB)))
        words << QStringLiteral("802.1X");
    return words.join(QLatin1Char(' '));
}

QString ipv4FromUint(uint networkOrder)
{
    char text[INET_ADDRSTRLEN] = {};
    return inet_ntop(AF_INET, &networkOrder, text, sizeof text) ? QString::fromLatin1(text) : QString();
}

QString ipv6FromBytes(const QByteArray &bytes)
{
    char text[INET6_ADDRSTRLEN] = {};
    if (bytes.size() != 16 || !inet_ntop(AF_INET6, bytes.constData(), text, sizeof text))
        return {};
    return QString::fromLatin1(text);
}

// "address-data" / "AddressData": aa{sv} with "address" and "prefix".
QStringList addressList(const QVariant &value)
{
    QStringList out;
    const QList<QVariantMap> list = qdbus_cast<QList<QVariantMap>>(value);
    for (const QVariantMap &entry : list) {
        const QString address = entry.value(QStringLiteral("address")).toString();
        if (!address.isEmpty())
            out << QStringLiteral("%1/%2").arg(address).arg(entry.value(QStringLiteral("prefix")).toUInt());
    }
    return out;
}

// DNS servers of an ipv4/ipv6 setting: "dns-data" (as, NetworkManager 1.42+)
// or the legacy binary "dns" (au for IPv4, aay for IPv6).
QStringList settingDns(const QVariantMap &setting, int family)
{
    if (setting.contains(QStringLiteral("dns-data")))
        return qdbus_cast<QStringList>(setting.value(QStringLiteral("dns-data")));
    QStringList out;
    if (!setting.contains(QStringLiteral("dns")))
        return out;
    if (family == 4) {
        const QList<uint> list = qdbus_cast<QList<uint>>(setting.value(QStringLiteral("dns")));
        for (const uint a : list)
            out << ipv4FromUint(a);
    } else {
        const QList<QByteArray> list = qdbus_cast<QList<QByteArray>>(setting.value(QStringLiteral("dns")));
        for (const QByteArray &a : list)
            out << ipv6FromBytes(a);
    }
    out.removeAll(QString());
    return out;
}

} // namespace

Nm::Nm(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<NmSettings>();
    qDBusRegisterMetaType<NmObjects>();
    qDBusRegisterMetaType<QList<QVariantMap>>();
    qDBusRegisterMetaType<QList<QByteArray>>();     // aay: IPv6 DNS servers of a profile
    qDBusRegisterMetaType<QList<uint>>();           // au: IPv4 DNS servers of a profile

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(400);
    connect(&m_debounce, &QTimer::timeout, this, &Nm::refresh);
    m_slowTimer.setInterval(15000);
    connect(&m_slowTimer, &QTimer::timeout, this, &Nm::refresh);
    m_scanTimer.setSingleShot(true);
    m_scanTimer.setInterval(15000);
    connect(&m_scanTimer, &QTimer::timeout, this, [this] { finishScan(QString()); });

    // Change notifications. NetworkManager publishes every object (devices,
    // access points, IP configurations, active connections, profiles) under
    // one object manager, so three subscriptions cover all of them.
    QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        auto *watcher = new QDBusServiceWatcher(kNm, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] {
            m_settingsCache.clear();
            scheduleRefresh();
        });
        const QString root = QStringLiteral("/org/freedesktop");
        const QString objectManager = QStringLiteral("org.freedesktop.DBus.ObjectManager");
        bus.connect(kNm, root, objectManager, QStringLiteral("InterfacesAdded"), this, SLOT(scheduleRefresh()));
        bus.connect(kNm, root, objectManager, QStringLiteral("InterfacesRemoved"), this, SLOT(scheduleRefresh()));
        bus.connect(kNm, QString(), kIfProperties, QStringLiteral("PropertiesChanged"), this,
                    SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));
        bus.connect(kNm, QString(), kIfSettingsConnection, QStringLiteral("Updated"), this, SLOT(onProfileUpdated()));
        bus.connect(kNm, QString(), kIfActive, QStringLiteral("StateChanged"), this,
                    SLOT(onActiveStateChanged(uint,uint,QDBusMessage)));
        bus.connect(kNm, QString(), kIfDevice, QStringLiteral("StateChanged"), this,
                    SLOT(onDeviceStateChanged(uint,uint,uint,QDBusMessage)));
    }
    loadVpnPlugins();
}

void Nm::onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &)
{
    // Signal strength and traffic counters change all the time; they are
    // picked up by the regular refresh and must not cause one each.
    if (interface.endsWith(QLatin1String(".Statistics")))
        return;
    if (interface == kIfAccessPoint) {
        bool relevant = false;
        for (auto it = changed.cbegin(); it != changed.cend(); ++it)
            relevant = relevant || (it.key() != QLatin1String("Strength") && it.key() != QLatin1String("LastSeen"));
        if (!relevant)
            return;
    }
    if (m_scanning && interface == kIfWireless && changed.contains(QStringLiteral("LastScan"))) {
        finishScan(QString());
        return;
    }
    scheduleRefresh();
}

void Nm::onProfileUpdated()
{
    m_settingsCache.clear();
    scheduleRefresh();
}

void Nm::call(const QString &path, const QString &interface, const QString &method, const QVariantList &arguments,
              Reply done, int timeoutMs)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kNm, path, interface, method);
    message.setArguments(arguments);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, timeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (done)
            done(w->reply());
    });
}

void Nm::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active) {
        refresh();
        m_slowTimer.start();
    } else {
        m_slowTimer.stop();
        m_debounce.stop();
    }
    Q_EMIT activeChanged();
}

void Nm::scheduleRefresh()
{
    if (m_active && !m_debounce.isActive())
        m_debounce.start();
}

void Nm::nmcli(const QStringList &arguments, Done done, int timeoutMs, const QByteArray &stdinData)
{
    Nexus::Command::run(this, m_nmcli, arguments, std::move(done), timeoutMs, stdinData);
}

QString Nm::errorText(const Nexus::CommandResult &r)
{
    if (r.exitCode == -2)
        return tr("NetworkManager did not answer in time.");
    if (r.exitCode == -1)
        return tr("nmcli could not be started.");
    // nmcli reports "Error: …" on stderr, sometimes followed by a journalctl hint.
    QStringList lines;
    const QString text = Net::cleanMessage(r.errText().isEmpty() ? r.outText() : r.errText());
    const QStringList raw = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString line : raw) {
        line = line.trimmed();
        if (line.startsWith(QLatin1String("Hint:")) || line.startsWith(QLatin1String("Warning: password for")))
            continue;
        if (line.startsWith(QLatin1String("Error: ")))
            line = line.mid(7);
        if (!line.isEmpty() && !lines.contains(line))
            lines << line;
    }
    return lines.isEmpty() ? tr("nmcli failed with exit code %1.").arg(r.exitCode) : lines.join(QLatin1Char(' '));
}

void Nm::setStatus(const QString &status, const QString &text)
{
    if (status == m_status && text == m_statusText)
        return;
    m_status = status;
    m_statusText = text;
    Q_EMIT statusChanged();
}

void Nm::clearData()
{
    if (!m_devices.isEmpty()) {
        m_devices.clear();
        Q_EMIT devicesChanged();
    }
    if (!m_connections.isEmpty()) {
        m_connections.clear();
        Q_EMIT connectionsChanged();
    }
    if (!m_wifiNetworks.isEmpty()) {
        m_wifiNetworks.clear();
        Q_EMIT wifiNetworksChanged();
    }
    if (!m_version.isEmpty() || !m_state.isEmpty() || !m_connectivity.isEmpty() || !m_wifiHardware.isEmpty()
        || m_networking || m_wifiEnabled) {
        m_version.clear();
        m_state.clear();
        m_connectivity.clear();
        m_wifiHardware.clear();
        m_networking = m_wifiEnabled = false;
        Q_EMIT generalChanged();
    }
}

void Nm::refresh()
{
    if (m_refreshing) {
        m_refreshQueued = true;
        return;
    }
    m_refreshing = true;
    m_pending = 1;
    auto unusable = [this](const QString &status, const QString &text) {
        setStatus(status, text);
        m_objects.clear();
        m_settingsCache.clear();
        clearData();
        stepDone();
    };

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        const QString detail = bus.lastError().message();
        // Reported from the event loop, like every other outcome.
        QMetaObject::invokeMethod(this, [unusable, detail] {
            unusable(QStringLiteral("unavailable"),
                     detail.isEmpty() ? tr("NetworkManager cannot be reached: the system message bus is not available.")
                                      : tr("NetworkManager cannot be reached: the system message bus is not available (%1).").arg(detail));
        }, Qt::QueuedConnection);
        return;
    }

    // Ask the bus first: calling NetworkManager directly would only time out.
    QDBusMessage owner = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                                        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
    owner << kNm;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(owner, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, unusable](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusMessage reply = w->reply();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            unusable(QStringLiteral("unavailable"), tr("NetworkManager cannot be reached: %1").arg(dbusError(reply)));
            return;
        }
        if (!reply.arguments().value(0).toBool()) {
            if (Net::findTool(QStringLiteral("NetworkManager")).isEmpty())
                unusable(QStringLiteral("missing"), tr("NetworkManager is not installed (package network-manager)."));
            else
                unusable(QStringLiteral("not-running"), tr("The NetworkManager service is not running."));
            return;
        }
        call(QStringLiteral("/org/freedesktop"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
             QStringLiteral("GetManagedObjects"), {}, [this, unusable](const QDBusMessage &objects) {
            if (objects.type() == QDBusMessage::ErrorMessage) {
                unusable(QStringLiteral("unavailable"), tr("NetworkManager cannot be reached: %1").arg(dbusError(objects)));
                return;
            }
            m_objects.clear();
            const NmObjects all = qdbus_cast<NmObjects>(objects.arguments().value(0));
            for (auto it = all.cbegin(); it != all.cend(); ++it)
                m_objects.insert(it.key().path(), it.value());
            setStatus(QStringLiteral("running"), QString());

            // The content of a profile is not a property; it is fetched once
            // and again after the profile's Updated signal.
            const QStringList known = m_settingsCache.keys();
            for (const QString &path : known) {
                if (!m_objects.contains(path))
                    m_settingsCache.remove(path);
            }
            for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
                if (!it.value().contains(kIfSettingsConnection) || m_settingsCache.contains(it.key()))
                    continue;
                ++m_pending;
                const QString path = it.key();
                call(path, kIfSettingsConnection, QStringLiteral("GetSettings"), {}, [this, path](const QDBusMessage &settings) {
                    // A profile deleted in between simply stays unknown.
                    if (settings.type() != QDBusMessage::ErrorMessage)
                        m_settingsCache.insert(path, qdbus_cast<NmSettings>(settings.arguments().value(0)));
                    stepDone();
                });
            }
            stepDone();
        });
    });
}

void Nm::stepDone()
{
    if (--m_pending > 0)
        return;
    QMetaObject::invokeMethod(this, [this] { finishRefresh(); }, Qt::QueuedConnection);
}

void Nm::finishRefresh()
{
    if (running()) {
        buildGeneral();
        buildDevices();
        buildConnections();
        buildWifiNetworks();
    }
    m_refreshing = false;
    Q_EMIT refreshed();
    if (m_refreshQueued) {
        m_refreshQueued = false;
        refresh();
    }
}

QVariantMap Nm::properties(const QString &path, const QString &interface) const
{
    return m_objects.value(path).value(interface);
}

QString Nm::devicePath(const QString &interfaceName) const
{
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        const auto device = it.value().constFind(kIfDevice);
        if (device != it.value().cend() && device->value(QStringLiteral("Interface")).toString() == interfaceName)
            return it.key();
    }
    return {};
}

QString Nm::profilePath(const QString &uuid) const
{
    for (auto it = m_settingsCache.cbegin(); it != m_settingsCache.cend(); ++it) {
        if (it.value().value(QStringLiteral("connection")).value(QStringLiteral("uuid")).toString() == uuid)
            return it.key();
    }
    return {};
}

QString Nm::activePath(const QString &uuid) const
{
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        const auto active = it.value().constFind(kIfActive);
        if (active != it.value().cend() && active->value(QStringLiteral("Uuid")).toString() == uuid)
            return it.key();
    }
    return {};
}

void Nm::buildGeneral()
{
    const QVariantMap p = properties(kNmPath, kNm);
    bool hasWifi = false;
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it)
        hasWifi = hasWifi || it.value().contains(kIfWireless);
    const QString version = p.value(QStringLiteral("Version")).toString();
    const QString state = managerStateName(p.value(QStringLiteral("State")).toUInt());
    const QString connectivity = connectivityName(p.value(QStringLiteral("Connectivity")).toUInt());
    const QString wifiHardware = !hasWifi ? QStringLiteral("missing")
        : p.value(QStringLiteral("WirelessHardwareEnabled")).toBool() ? QStringLiteral("enabled") : QStringLiteral("disabled");
    const bool networking = p.value(QStringLiteral("NetworkingEnabled")).toBool();
    const bool wifiEnabled = p.value(QStringLiteral("WirelessEnabled")).toBool();
    if (version != m_version || state != m_state || connectivity != m_connectivity || wifiHardware != m_wifiHardware
        || networking != m_networking || wifiEnabled != m_wifiEnabled) {
        m_version = version;
        m_state = state;
        m_connectivity = connectivity;
        m_wifiHardware = wifiHardware;
        m_networking = networking;
        m_wifiEnabled = wifiEnabled;
        Q_EMIT generalChanged();
    }
}

void Nm::buildDevices()
{
    QVariantList devices;
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        if (!it.value().contains(kIfDevice))
            continue;
        const QVariantMap d = it.value().value(kIfDevice);
        const QString name = d.value(QStringLiteral("Interface")).toString();
        if (name.isEmpty())
            continue;
        QVariantMap m;
        m.insert(QStringLiteral("name"), name);
        m.insert(QStringLiteral("type"), deviceTypeName(d.value(QStringLiteral("DeviceType")).toUInt()));
        const uint state = d.value(QStringLiteral("State")).toUInt();
        m.insert(QStringLiteral("stateCode"), int(state));
        m.insert(QStringLiteral("stateText"), deviceStateText(state));
        uint reasonState = 0, reason = 0;
        const QVariant stateReason = d.value(QStringLiteral("StateReason"));
        if (stateReason.canConvert<QDBusArgument>()) {
            const QDBusArgument a = stateReason.value<QDBusArgument>();
            a.beginStructure();
            a >> reasonState >> reason;
            a.endStructure();
        }
        m.insert(QStringLiteral("reasonCode"), int(reason));
        m.insert(QStringLiteral("reasonText"), deviceReasonText(reason));
        const QVariantMap active = properties(pathOf(d.value(QStringLiteral("ActiveConnection"))), kIfActive);
        m.insert(QStringLiteral("connection"), active.value(QStringLiteral("Id")).toString());
        m.insert(QStringLiteral("connectionUuid"), active.value(QStringLiteral("Uuid")).toString());
        m.insert(QStringLiteral("managed"), d.value(QStringLiteral("Managed")).toBool());
        m.insert(QStringLiteral("mac"), d.value(QStringLiteral("HwAddress")).toString());
        m.insert(QStringLiteral("driver"), d.value(QStringLiteral("Driver")).toString());
        // Wired: Mbit/s; wireless: kbit/s. Zero means "not known".
        QString speed;
        const uint wired = it.value().value(kIfWired).value(QStringLiteral("Speed")).toUInt();
        const uint wireless = it.value().value(kIfWireless).value(QStringLiteral("Bitrate")).toUInt();
        if (wired > 0)
            speed = tr("%1 Mbit/s").arg(wired);
        else if (wireless > 0)
            speed = tr("%1 Mbit/s").arg(wireless / 1000);
        m.insert(QStringLiteral("speed"), speed);

        for (const int family : {4, 6}) {
            const QVariantMap ip = properties(pathOf(d.value(family == 4 ? QStringLiteral("Ip4Config") : QStringLiteral("Ip6Config"))),
                                              family == 4 ? kIfIp4 : kIfIp6);
            const QString n = QString::number(family);
            m.insert(QStringLiteral("addresses") + n, addressList(ip.value(QStringLiteral("AddressData"))));
            m.insert(QStringLiteral("gateway") + n, ip.value(QStringLiteral("Gateway")).toString());
            QStringList dns;
            if (family == 4) {
                const QList<QVariantMap> servers = qdbus_cast<QList<QVariantMap>>(ip.value(QStringLiteral("NameserverData")));
                for (const QVariantMap &server : servers)
                    dns << server.value(QStringLiteral("address")).toString();
            } else {
                const QList<QByteArray> servers = qdbus_cast<QList<QByteArray>>(ip.value(QStringLiteral("Nameservers")));
                for (const QByteArray &server : servers)
                    dns << ipv6FromBytes(server);
            }
            dns.removeAll(QString());
            m.insert(QStringLiteral("dns") + n, dns);
            // Domains: announced by DHCP; Searches: the search list in effect.
            QStringList domains = qdbus_cast<QStringList>(ip.value(QStringLiteral("Domains")));
            const QStringList searches = qdbus_cast<QStringList>(ip.value(QStringLiteral("Searches")));
            for (const QString &domain : searches) {
                if (!domains.contains(domain))
                    domains << domain;
            }
            m.insert(QStringLiteral("domains") + n, domains);
        }
        devices << m;
    }
    std::stable_sort(devices.begin(), devices.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString() < b.toMap().value(QStringLiteral("name")).toString();
    });
    if (devices != m_devices) {
        m_devices = devices;
        Q_EMIT devicesChanged();
    }
}

void Nm::buildConnections()
{
    // Active connections by profile path: state and the interface they run on.
    QHash<QString, QVariantMap> activeByProfile;
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        if (it.value().contains(kIfActive))
            activeByProfile.insert(pathOf(it.value().value(kIfActive).value(QStringLiteral("Connection"))), it.value().value(kIfActive));
    }

    QVariantList connections;
    for (auto it = m_settingsCache.cbegin(); it != m_settingsCache.cend(); ++it) {
        const NmSettings &s = it.value();
        const QVariantMap c = s.value(QStringLiteral("connection"));
        const QString uuid = c.value(QStringLiteral("uuid")).toString();
        if (!Net::isUuid(uuid))
            continue;
        const QString type = c.value(QStringLiteral("type")).toString();
        const QVariantMap active = activeByProfile.value(it.key());
        QString device;
        const QStringList devicePaths = pathsOf(active.value(QStringLiteral("Devices")));
        if (!devicePaths.isEmpty())
            device = properties(devicePaths.constFirst(), kIfDevice).value(QStringLiteral("Interface")).toString();
        const QString state = activeStateName(active.value(QStringLiteral("State")).toUInt());
        const uint flags = properties(it.key(), kIfSettingsConnection).value(QStringLiteral("Flags")).toUInt();

        QVariantMap m;
        m.insert(QStringLiteral("name"), c.value(QStringLiteral("id")).toString());
        m.insert(QStringLiteral("uuid"), uuid);
        m.insert(QStringLiteral("type"), type);
        m.insert(QStringLiteral("typeLabel"), typeLabel(type));
        m.insert(QStringLiteral("device"), device);
        m.insert(QStringLiteral("active"), !active.isEmpty());
        m.insert(QStringLiteral("state"), state);
        m.insert(QStringLiteral("autoconnect"), c.value(QStringLiteral("autoconnect"), true).toBool());
        // Profiles NetworkManager generated for a device configured by
        // something else (flag 0x8, "external") vanish with that configuration.
        m.insert(QStringLiteral("readOnly"), c.value(QStringLiteral("read-only")).toBool() || (flags & 0x8));
        m.insert(QStringLiteral("hasDetails"), true);
        m.insert(QStringLiteral("interfaceName"), c.value(QStringLiteral("interface-name")).toString());
        m.insert(QStringLiteral("ssid"), QString::fromUtf8(s.value(QStringLiteral("802-11-wireless")).value(QStringLiteral("ssid")).toByteArray()));
        m.insert(QStringLiteral("keyMgmt"), s.value(QStringLiteral("802-11-wireless-security")).value(QStringLiteral("key-mgmt")).toString());
        m.insert(QStringLiteral("identity"), s.value(QStringLiteral("802-1x")).value(QStringLiteral("identity")).toString());
        const QString service = s.value(QStringLiteral("vpn")).value(QStringLiteral("service-type")).toString();
        const bool isVpn = type == QLatin1String("vpn") || type == QLatin1String("wireguard");
        m.insert(QStringLiteral("isVpn"), isVpn);
        m.insert(QStringLiteral("isWifi"), type == QLatin1String("802-11-wireless"));
        m.insert(QStringLiteral("vpnService"), service);
        m.insert(QStringLiteral("vpnLabel"), isVpn ? vpnLabel(type, service) : QString());
        m.insert(QStringLiteral("dnsOverTls"), dotName(QString::number(c.value(QStringLiteral("dns-over-tls"), -1).toInt())));
        for (const int family : {4, 6}) {
            const QVariantMap setting = s.value(QStringLiteral("ipv%1").arg(family));
            QVariantMap ip;
            ip.insert(QStringLiteral("method"), setting.value(QStringLiteral("method")).toString());
            ip.insert(QStringLiteral("addresses"), addressList(setting.value(QStringLiteral("address-data"))));
            ip.insert(QStringLiteral("gateway"), setting.value(QStringLiteral("gateway")).toString());
            ip.insert(QStringLiteral("dns"), settingDns(setting, family));
            ip.insert(QStringLiteral("search"), qdbus_cast<QStringList>(setting.value(QStringLiteral("dns-search"))));
            ip.insert(QStringLiteral("ignoreAutoDns"), setting.value(QStringLiteral("ignore-auto-dns")).toBool());
            m.insert(QStringLiteral("ipv%1").arg(family), ip);
        }
        connections << m;
    }
    // Active profiles first, then by name.
    std::stable_sort(connections.begin(), connections.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        const bool xa = x.value(QStringLiteral("active")).toBool(), ya = y.value(QStringLiteral("active")).toBool();
        if (xa != ya)
            return xa;
        return x.value(QStringLiteral("name")).toString().localeAwareCompare(y.value(QStringLiteral("name")).toString()) < 0;
    });
    if (connections != m_connections) {
        m_connections = connections;
        Q_EMIT connectionsChanged();
    }
}

void Nm::buildWifiNetworks()
{
    // Saved profiles by SSID, to mark known networks.
    QHash<QString, QString> saved;
    for (const QVariant &v : std::as_const(m_connections)) {
        const QVariantMap c = v.toMap();
        if (c.value(QStringLiteral("isWifi")).toBool() && !c.value(QStringLiteral("ssid")).toString().isEmpty())
            saved.insert(c.value(QStringLiteral("ssid")).toString(), c.value(QStringLiteral("uuid")).toString());
    }

    QVariantList networks;
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        if (!it.value().contains(kIfWireless))
            continue;
        const QVariantMap wireless = it.value().value(kIfWireless);
        const QString device = it.value().value(kIfDevice).value(QStringLiteral("Interface")).toString();
        const QString activeAp = pathOf(wireless.value(QStringLiteral("ActiveAccessPoint")));
        const QStringList accessPoints = pathsOf(wireless.value(QStringLiteral("AccessPoints")));
        for (const QString &apPath : accessPoints) {
            const QVariantMap ap = properties(apPath, kIfAccessPoint);
            if (ap.isEmpty())
                continue;
            const QString ssid = QString::fromUtf8(ap.value(QStringLiteral("Ssid")).toByteArray());
            const uint flags = ap.value(QStringLiteral("Flags")).toUInt();
            const uint wpa = ap.value(QStringLiteral("WpaFlags")).toUInt();
            const uint rsn = ap.value(QStringLiteral("RsnFlags")).toUInt();
            const uint frequency = ap.value(QStringLiteral("Frequency")).toUInt();
            const uint mode = ap.value(QStringLiteral("Mode")).toUInt();
            const QString security = apSecurity(flags, wpa, rsn);
            QVariantMap m;
            m.insert(QStringLiteral("inUse"), apPath == activeAp);
            m.insert(QStringLiteral("path"), apPath);
            m.insert(QStringLiteral("bssid"), ap.value(QStringLiteral("HwAddress")).toString());
            m.insert(QStringLiteral("ssid"), ssid);
            m.insert(QStringLiteral("hidden"), ssid.isEmpty());
            m.insert(QStringLiteral("mode"), mode == 1 ? QStringLiteral("Ad-Hoc") : mode == 3 ? QStringLiteral("AP")
                                             : mode == 4 ? QStringLiteral("Mesh") : QStringLiteral("Infra"));
            m.insert(QStringLiteral("channel"), wifiChannel(frequency));
            m.insert(QStringLiteral("frequency"), int(frequency));
            m.insert(QStringLiteral("rate"), tr("%1 Mbit/s").arg(ap.value(QStringLiteral("MaxBitrate")).toUInt() / 1000));
            m.insert(QStringLiteral("strength"), ap.value(QStringLiteral("Strength")).toInt());
            m.insert(QStringLiteral("security"), security);
            m.insert(QStringLiteral("secured"), !security.isEmpty());
            m.insert(QStringLiteral("enterprise"), security.contains(QLatin1String("802.1X")));
            m.insert(QStringLiteral("wep"), security == QLatin1String("WEP"));
            // Open to join without a password: opportunistic encryption only.
            m.insert(QStringLiteral("owe"), (rsn & (KeyMgmtOwe | KeyMgmtOweTm)) && !(rsn & (KeyMgmtPsk | KeyMgmtSae | KeyMgmt8021x)));
            m.insert(QStringLiteral("saeOnly"), (rsn & KeyMgmtSae) && !(rsn & KeyMgmtPsk) && !(wpa & KeyMgmtPsk));
            m.insert(QStringLiteral("device"), device);
            m.insert(QStringLiteral("savedUuid"), saved.value(ssid));
            networks << m;
        }
    }
    std::stable_sort(networks.begin(), networks.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        const bool xu = x.value(QStringLiteral("inUse")).toBool(), yu = y.value(QStringLiteral("inUse")).toBool();
        if (xu != yu)
            return xu;
        return x.value(QStringLiteral("strength")).toInt() > y.value(QStringLiteral("strength")).toInt();
    });
    if (networks != m_wifiNetworks) {
        m_wifiNetworks = networks;
        Q_EMIT wifiNetworksChanged();
    }
}

void Nm::loadVpnPlugins()
{
    // Every VPN plug-in registers itself with a key file naming its D-Bus service.
    QStringList plugins;
    for (const QString &dir : {QStringLiteral("/usr/lib/NetworkManager/VPN"), QStringLiteral("/etc/NetworkManager/VPN")}) {
        const QStringList files = QDir(dir).entryList({QStringLiteral("*.name")}, QDir::Files);
        for (const QString &file : files) {
            QFile f(dir + QLatin1Char('/') + file);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QStringList lines = QString::fromUtf8(f.read(16 * 1024)).split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                if (line.startsWith(QLatin1String("service="))) {
                    const QString name = line.mid(8).trimmed().section(QLatin1Char('.'), -1);
                    if (!name.isEmpty() && !plugins.contains(name))
                        plugins << name;
                }
            }
        }
    }
    plugins.sort();
    if (plugins != m_vpnPlugins) {
        m_vpnPlugins = plugins;
        Q_EMIT vpnPluginsChanged();
    }
}

QVariantList Nm::wifiDevices() const
{
    QVariantList out;
    for (const QVariant &v : m_devices) {
        if (v.toMap().value(QStringLiteral("type")) == QLatin1String("wifi"))
            out << v;
    }
    return out;
}

QVariantList Nm::savedWifi() const
{
    QVariantList out;
    for (const QVariant &v : m_connections) {
        if (v.toMap().value(QStringLiteral("isWifi")).toBool())
            out << v;
    }
    return out;
}

QVariantList Nm::vpnConnections() const
{
    QVariantList out;
    for (const QVariant &v : m_connections) {
        if (v.toMap().value(QStringLiteral("isVpn")).toBool())
            out << v;
    }
    return out;
}

int Nm::activeVpnCount() const
{
    int n = 0;
    for (const QVariant &v : m_connections) {
        const QVariantMap c = v.toMap();
        if (c.value(QStringLiteral("isVpn")).toBool() && c.value(QStringLiteral("state")) == QLatin1String("activated"))
            ++n;
    }
    return n;
}

QVariantMap Nm::deviceByName(const QString &name) const
{
    for (const QVariant &v : m_devices) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("name")) == name)
            return m;
    }
    return {};
}

QVariantMap Nm::connectionByUuid(const QString &uuid) const
{
    for (const QVariant &v : m_connections) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("uuid")) == uuid)
            return m;
    }
    return {};
}

// ---- operations ------------------------------------------------------------------

void Nm::beginBusy(const QString &target)
{
    m_busyTargets << target;
    Q_EMIT busyChanged();
}

void Nm::endBusy(const QString &target)
{
    m_busyTargets.removeOne(target);
    Q_EMIT busyChanged();
}

void Nm::failOperation(const QString &name, const QString &target, const QString &message)
{
    // Reported from the event loop, like every real result.
    QMetaObject::invokeMethod(this, [this, name, target, message] {
        Q_EMIT operationFinished(name, target, false, message);
    }, Qt::QueuedConnection);
}

void Nm::operation(const QString &name, const QString &target, const QStringList &arguments, int timeoutMs,
                   const QByteArray &stdinData, Done after)
{
    if (!ensureRunning(name, target))
        return;
    if (m_nmcli.isEmpty())
        m_nmcli = Net::findTool(QStringLiteral("nmcli"));
    if (m_nmcli.isEmpty()) {
        failOperation(name, target, tr("The nmcli tool is not installed (package network-manager)."));
        return;
    }
    beginBusy(target);
    nmcli(arguments, [this, name, target, after](const Nexus::CommandResult &r) {
        endBusy(target);
        if (after) {
            after(r);
        } else {
            const QString message = r.ok() ? Net::cleanMessage(r.outText()).section(QLatin1Char('\n'), -1) : errorText(r);
            Q_EMIT operationFinished(name, target, r.ok(), message);
        }
        refresh();
    }, timeoutMs, stdinData);
}

// ---- activations -----------------------------------------------------------------
//
// ActivateConnection() and AddAndActivateConnection() return as soon as the
// activation has started. Its outcome is the State of the active-connection
// object: StateChanged(state, reason) reports "activated" or "deactivated",
// and the device's own StateChanged carries the detailed reason of a failure.

bool Nm::ensureRunning(const QString &operation, const QString &target)
{
    if (running())
        return true;
    failOperation(operation, target, m_statusText.isEmpty() ? tr("NetworkManager is not running.") : m_statusText);
    return false;
}

void Nm::trackActivation(const QString &path, const Activation &activation)
{
    Activation a = activation;
    a.timeout = new QTimer(this);
    a.timeout->setSingleShot(true);
    connect(a.timeout, &QTimer::timeout, this, [this, path] { finishActivation(path, false, 6); });
    a.timeout->start(75000);
    m_activations.insert(path, a);
    // The state may already have changed before the signal subscription saw it.
    call(path, kIfProperties, QStringLiteral("GetAll"), {kIfActive}, [this, path](const QDBusMessage &reply) {
        const auto it = m_activations.find(path);
        if (it == m_activations.end())
            return;
        if (reply.type() == QDBusMessage::ErrorMessage) {
            finishActivation(path, false, 0);       // the object is already gone: it failed at once
            return;
        }
        const QVariantMap p = qdbus_cast<QVariantMap>(reply.arguments().value(0));
        const QStringList devices = pathsOf(p.value(QStringLiteral("Devices")));
        for (const QString &device : devices) {
            if (!it->devices.contains(device))
                it->devices << device;
        }
        const uint state = p.value(QStringLiteral("State")).toUInt();
        if (state == 2)
            finishActivation(path, true, 0);
        else if (state == 4)
            finishActivation(path, false, 0);
    });
}

void Nm::onActiveStateChanged(uint state, uint reason, const QDBusMessage &message)
{
    if (m_activations.contains(message.path())) {
        if (state == 2)
            finishActivation(message.path(), true, 0);
        else if (state == 4)
            finishActivation(message.path(), false, reason);
    }
    scheduleRefresh();
}

void Nm::onDeviceStateChanged(uint newState, uint, uint reason, const QDBusMessage &message)
{
    if (newState == 120)
        m_deviceFailure.insert(message.path(), reason);
    scheduleRefresh();
}

void Nm::finishActivation(const QString &path, bool ok, uint activeReason)
{
    if (!m_activations.contains(path))
        return;
    const Activation a = m_activations.take(path);
    if (a.timeout)
        a.timeout->deleteLater();
    endBusy(a.target);

    if (ok) {
        Q_EMIT operationFinished(a.operation, a.target, true, a.successText);
    } else {
        uint deviceReason = 0;
        for (const QString &device : a.devices) {
            if (m_deviceFailure.contains(device))
                deviceReason = m_deviceFailure.value(device);
        }
        // A profile created for this attempt that never worked is not kept.
        if (!a.createdProfile.isEmpty())
            call(a.createdProfile, kIfSettingsConnection, QStringLiteral("Delete"), {}, {});
        const bool noSecrets = activeReason == 9 || deviceReason == 7;
        if (noSecrets && a.askForSecret) {
            Q_EMIT secretsRequired(a.target, a.name);
        } else {
            QString reason = deviceReasonText(deviceReason);
            if (reason.isEmpty())
                reason = activeReasonText(activeReason);
            if (noSecrets && !a.noSecretsText.isEmpty())
                reason = a.noSecretsText;
            Q_EMIT operationFinished(a.operation, a.target, false,
                                     reason.isEmpty() ? tr("“%1” could not be activated; NetworkManager gave no reason.").arg(a.name)
                                                      : tr("“%1” could not be activated: %2.").arg(a.name, reason));
        }
    }
    refresh();
}

// Start tracking the activation a call answered with, or report the call's error.
void Nm::activationStarted(const QDBusMessage &reply, int activeArgument, Activation activation)
{
    if (reply.type() == QDBusMessage::ErrorMessage) {
        endBusy(activation.target);
        Q_EMIT operationFinished(activation.operation, activation.target, false, dbusError(reply));
        refresh();
        return;
    }
    if (activeArgument == 1)
        activation.createdProfile = activation.keepProfile ? QString() : pathOf(reply.arguments().value(0));
    trackActivation(pathOf(reply.arguments().value(activeArgument)), activation);
}

// The reply of a call that completes the operation by itself.
void Nm::simpleResult(const QString &operation, const QString &target, const QDBusMessage &reply, const QString &successText)
{
    endBusy(target);
    const bool ok = reply.type() != QDBusMessage::ErrorMessage;
    Q_EMIT operationFinished(operation, target, ok, ok ? successText : dbusError(reply));
    refresh();
}

void Nm::connectDevice(const QString &interface)
{
    const QString op = QStringLiteral("connect-device");
    if (!ensureRunning(op, interface))
        return;
    const QString device = devicePath(interface);
    if (!Net::isInterfaceName(interface) || device.isEmpty()) {
        failOperation(op, interface, tr("“%1” is not a NetworkManager device.").arg(interface));
        return;
    }
    Activation a;
    a.operation = op;
    a.target = interface;
    a.name = interface;
    a.devices = QStringList{device};
    a.successText = tr("%1 is connected.").arg(interface);
    // NetworkManager picks the profile itself, so the password cannot be
    // asked for here; say where it can be entered.
    a.noSecretsText = tr("its connection needs a password that is not stored. Activate the connection on the Wi-Fi, VPN "
                         "or IP configuration page to enter it");
    a.keepProfile = true;
    m_deviceFailure.remove(device);
    beginBusy(interface);
    const bool hasProfile = !pathsOf(properties(device, kIfDevice).value(QStringLiteral("AvailableConnections"))).isEmpty();
    if (hasProfile) {
        // "/" as the profile: the best profile available for the device.
        call(kNmPath, kNm, QStringLiteral("ActivateConnection"), {objectPath(QString()), objectPath(device), objectPath(QString())},
             [this, a](const QDBusMessage &reply) { activationStarted(reply, 0, a); }, kAuthTimeoutMs);
    } else {
        // No profile yet: NetworkManager completes an empty one for the
        // device (automatic addressing), as it does for a new wired link.
        call(kNmPath, kNm, QStringLiteral("AddAndActivateConnection"),
             {QVariant::fromValue(NmSettings()), objectPath(device), objectPath(QString())},
             [this, a](const QDBusMessage &reply) { activationStarted(reply, 1, a); }, kAuthTimeoutMs);
    }
}

void Nm::disconnectDevice(const QString &interface)
{
    const QString op = QStringLiteral("disconnect-device");
    if (!ensureRunning(op, interface))
        return;
    const QString device = devicePath(interface);
    if (!Net::isInterfaceName(interface) || device.isEmpty()) {
        failOperation(op, interface, tr("“%1” is not a NetworkManager device.").arg(interface));
        return;
    }
    beginBusy(interface);
    call(device, kIfDevice, QStringLiteral("Disconnect"), {}, [this, op, interface](const QDBusMessage &reply) {
        simpleResult(op, interface, reply, tr("%1 was disconnected.").arg(interface));
    }, kAuthTimeoutMs);
}

void Nm::activateConnection(const QString &uuid)
{
    const QString op = QStringLiteral("activate");
    if (!ensureRunning(op, uuid))
        return;
    const QVariantMap con = connectionByUuid(uuid);
    const QString profile = profilePath(uuid);
    if (!Net::isUuid(uuid) || con.isEmpty() || profile.isEmpty()) {
        failOperation(op, uuid, tr("Unknown connection profile."));
        return;
    }
    Activation a;
    a.operation = op;
    a.target = uuid;
    a.name = con.value(QStringLiteral("name")).toString();
    a.successText = tr("“%1” is now active.").arg(a.name);
    a.askForSecret = true;
    m_deviceFailure.clear();
    beginBusy(uuid);
    // "/" as the device: NetworkManager chooses the device the profile fits.
    call(kNmPath, kNm, QStringLiteral("ActivateConnection"), {objectPath(profile), objectPath(QString()), objectPath(QString())},
         [this, a](const QDBusMessage &reply) { activationStarted(reply, 0, a); }, kAuthTimeoutMs);
}

void Nm::activateWithSecret(const QString &uuid, const QString &secret)
{
    const QString op = QStringLiteral("activate");
    const QVariantMap con = connectionByUuid(uuid);
    if (!Net::isUuid(uuid) || con.isEmpty()) {
        failOperation(op, uuid, tr("Unknown connection profile."));
        return;
    }
    if (secret.isEmpty() || secret.size() > 1024 || hasControlCharacters(secret)) {
        failOperation(op, uuid, tr("The password is empty or contains control characters."));
        return;
    }
    // The secret travels on standard input, in nmcli's passwd-file syntax,
    // under the property names the profile type can ask for.
    const QString type = con.value(QStringLiteral("type")).toString();
    QStringList keys;
    if (type == QLatin1String("802-11-wireless"))
        keys << QStringLiteral("802-11-wireless-security.psk") << QStringLiteral("802-1x.password");
    else if (type == QLatin1String("vpn"))
        keys << QStringLiteral("vpn.secrets.password");
    else if (type == QLatin1String("wireguard"))
        keys << QStringLiteral("wireguard.private-key");
    else
        keys << QStringLiteral("802-1x.password");
    const QByteArray value = passwdFileValue(secret);
    QByteArray input;
    for (const QString &key : std::as_const(keys))
        input += key.toLatin1() + ':' + value + '\n';
    // 802.1X: NetworkManager asks for the identity together with the
    // password; confirm the one stored in the profile.
    const QString identity = con.value(QStringLiteral("identity")).toString();
    if (!identity.isEmpty() && !hasControlCharacters(identity))
        input += "802-1x.identity:" + passwdFileValue(identity) + '\n';

    const QString name = con.value(QStringLiteral("name")).toString();
    operation(op, uuid, {QStringLiteral("--wait"), QStringLiteral("60"), QStringLiteral("connection"), QStringLiteral("up"),
                         QStringLiteral("uuid"), uuid, QStringLiteral("passwd-file"), QStringLiteral("/dev/stdin")},
              75000, input, [this, op, uuid, name](const Nexus::CommandResult &r) {
        QString message = r.ok() ? tr("“%1” is now active.").arg(name) : errorText(r);
        if (!r.ok() && r.errText().contains(QLatin1String("Secrets were required")))
            message = tr("“%1” was not activated: the password was not accepted or more credentials are needed.").arg(name);
        Q_EMIT operationFinished(op, uuid, r.ok(), message);
    });
}

void Nm::deactivateConnection(const QString &uuid)
{
    const QString op = QStringLiteral("deactivate");
    if (!ensureRunning(op, uuid))
        return;
    const QVariantMap con = connectionByUuid(uuid);
    if (!Net::isUuid(uuid) || con.isEmpty()) {
        failOperation(op, uuid, tr("Unknown connection profile."));
        return;
    }
    const QString name = con.value(QStringLiteral("name")).toString();
    const QString active = activePath(uuid);
    if (active.isEmpty()) {
        failOperation(op, uuid, tr("“%1” is not active.").arg(name));
        return;
    }
    beginBusy(uuid);
    call(kNmPath, kNm, QStringLiteral("DeactivateConnection"), {objectPath(active)}, [this, op, uuid, name](const QDBusMessage &reply) {
        simpleResult(op, uuid, reply, tr("“%1” was disconnected.").arg(name));
    }, kAuthTimeoutMs);
}

void Nm::deleteConnection(const QString &uuid)
{
    const QString op = QStringLiteral("delete");
    if (!ensureRunning(op, uuid))
        return;
    const QVariantMap con = connectionByUuid(uuid);
    const QString profile = profilePath(uuid);
    if (!Net::isUuid(uuid) || con.isEmpty() || profile.isEmpty()) {
        failOperation(op, uuid, tr("Unknown connection profile."));
        return;
    }
    const QString name = con.value(QStringLiteral("name")).toString();
    beginBusy(uuid);
    call(profile, kIfSettingsConnection, QStringLiteral("Delete"), {}, [this, op, uuid, name](const QDBusMessage &reply) {
        simpleResult(op, uuid, reply, tr("“%1” was removed.").arg(name));
    }, kAuthTimeoutMs);
}

// Read the profile as NetworkManager holds it now, let 'mutate' change it and
// write it back. Update() replaces the whole profile; secrets are not part of
// what GetSettings returns and NetworkManager keeps the stored ones when an
// update carries none.
void Nm::updateProfile(const QString &profile, std::function<void(NmSettings &)> mutate, Reply done)
{
    call(profile, kIfSettingsConnection, QStringLiteral("GetSettings"), {}, [this, profile, mutate, done](const QDBusMessage &reply) {
        if (reply.type() == QDBusMessage::ErrorMessage) {
            done(reply);
            return;
        }
        NmSettings settings = qdbus_cast<NmSettings>(reply.arguments().value(0));
        mutate(settings);
        call(profile, kIfSettingsConnection, QStringLiteral("Update"), {QVariant::fromValue(settings)},
             [this, profile, done](const QDBusMessage &updated) {
            m_settingsCache.remove(profile);
            done(updated);
        }, kAuthTimeoutMs);
    });
}

void Nm::setAutoconnect(const QString &uuid, bool autoconnect)
{
    const QString op = QStringLiteral("autoconnect");
    if (!ensureRunning(op, uuid))
        return;
    const QString profile = profilePath(uuid);
    if (!Net::isUuid(uuid) || profile.isEmpty()) {
        failOperation(op, uuid, tr("Unknown connection profile."));
        return;
    }
    beginBusy(uuid);
    updateProfile(profile, [autoconnect](NmSettings &settings) {
        settings[QStringLiteral("connection")].insert(QStringLiteral("autoconnect"), autoconnect);
    }, [this, op, uuid, autoconnect](const QDBusMessage &reply) {
        simpleResult(op, uuid, reply, autoconnect ? tr("The connection will be activated automatically.")
                                                  : tr("The connection will only be activated manually."));
    });
}

void Nm::setWifiEnabled(bool enabled)
{
    const QString op = QStringLiteral("wifi-radio");
    if (!ensureRunning(op, op))
        return;
    beginBusy(op);
    call(kNmPath, kIfProperties, QStringLiteral("Set"),
         {kNm, QStringLiteral("WirelessEnabled"), QVariant::fromValue(QDBusVariant(enabled))},
         [this, op, enabled](const QDBusMessage &reply) {
        simpleResult(op, op, reply, enabled ? tr("Wi-Fi was switched on.") : tr("Wi-Fi was switched off."));
    }, kAuthTimeoutMs);
}

void Nm::refreshWifi(bool rescan)
{
    if (!running() || wifiDevices().isEmpty() || m_scanning)
        return;
    if (!rescan) {
        refresh();
        return;
    }
    m_scanning = true;
    Q_EMIT scanningChanged();
    // The scan is finished when the device's LastScan property changes (see
    // onPropertiesChanged); the timer only bounds the wait.
    m_scanTimer.start();
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        if (!it.value().contains(kIfWireless))
            continue;
        call(it.key(), kIfWireless, QStringLiteral("RequestScan"), {QVariant::fromValue(QVariantMap())},
             [this](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ErrorMessage)
                finishScan(dbusError(reply));
        });
    }
}

void Nm::finishScan(const QString &error)
{
    if (!m_scanning)
        return;
    m_scanning = false;
    m_scanTimer.stop();
    Q_EMIT scanningChanged();
    if (error.isEmpty())
        m_scanReportDue = true;         // reported once the new list has been read
    else
        Q_EMIT operationFinished(QStringLiteral("wifi-scan"), QStringLiteral("wifi-scan"), false, error);
    refresh();
}

QString Nm::wifiPasswordError(const QString &password) const
{
    if (hasControlCharacters(password))
        return tr("The password contains control characters.");
    static const QRegularExpression hexKey(QStringLiteral("^[0-9A-Fa-f]{64}$"));
    if (hexKey.match(password).hasMatch())
        return {};
    // A WPA passphrase is 8 to 63 bytes long.
    const int bytes = int(password.toUtf8().size());
    if (bytes < 8)
        return tr("A Wi-Fi password has at least 8 characters.");
    if (bytes > 63)
        return tr("A Wi-Fi password has at most 63 characters (or is a 64-digit hexadecimal key).");
    return {};
}

void Nm::connectWifi(const QString &ssid, const QString &interface, const QString &password, bool hidden)
{
    const QString op = QStringLiteral("wifi-connect");
    if (!ensureRunning(op, ssid))
        return;
    const QByteArray ssidBytes = ssid.toUtf8();
    if (ssid.isEmpty() || ssidBytes.size() > 32 || hasControlCharacters(ssid)) {
        failOperation(op, ssid, tr("The network name must be 1 to 32 bytes long and must not contain control characters."));
        return;
    }
    const QString device = devicePath(interface);
    if (!Net::isInterfaceName(interface) || device.isEmpty() || !m_objects.value(device).contains(kIfWireless)) {
        failOperation(op, ssid, tr("“%1” is not a Wi-Fi device.").arg(interface));
        return;
    }
    if (!password.isEmpty()) {
        const QString problem = wifiPasswordError(password);
        if (!problem.isEmpty()) {
            failOperation(op, ssid, problem);
            return;
        }
    }

    // The strongest access point announcing this name tells which key
    // management the network uses; a hidden network announces nothing.
    QVariantMap accessPoint;
    for (const QVariant &v : std::as_const(m_wifiNetworks)) {
        const QVariantMap n = v.toMap();
        if (n.value(QStringLiteral("ssid")).toString() == ssid && n.value(QStringLiteral("device")).toString() == interface) {
            accessPoint = n;
            break;
        }
    }

    NmSettings settings;
    QVariantMap connection;
    connection.insert(QStringLiteral("id"), ssid);
    connection.insert(QStringLiteral("type"), QStringLiteral("802-11-wireless"));
    connection.insert(QStringLiteral("uuid"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    settings.insert(QStringLiteral("connection"), connection);
    QVariantMap wireless;
    wireless.insert(QStringLiteral("ssid"), ssidBytes);
    wireless.insert(QStringLiteral("mode"), QStringLiteral("infrastructure"));
    if (hidden)
        wireless.insert(QStringLiteral("hidden"), true);
    settings.insert(QStringLiteral("802-11-wireless"), wireless);
    if (!password.isEmpty()) {
        // The passphrase is part of the profile handed to NetworkManager over
        // the system bus; NetworkManager stores it, this application does not.
        QVariantMap security;
        security.insert(QStringLiteral("key-mgmt"), accessPoint.value(QStringLiteral("saeOnly")).toBool() ? QStringLiteral("sae")
                                                                                                           : QStringLiteral("wpa-psk"));
        security.insert(QStringLiteral("psk"), password);
        settings.insert(QStringLiteral("802-11-wireless-security"), security);
    } else if (accessPoint.value(QStringLiteral("owe")).toBool()) {
        settings.insert(QStringLiteral("802-11-wireless-security"),
                        QVariantMap{{QStringLiteral("key-mgmt"), QStringLiteral("owe")}});
    }

    Activation a;
    a.operation = op;
    a.target = ssid;
    a.name = ssid;
    a.devices = QStringList{device};
    a.successText = tr("Connected to “%1”.").arg(ssid);
    a.noSecretsText = password.isEmpty() ? tr("the network requires a password") : tr("the password was not accepted");
    m_deviceFailure.remove(device);
    beginBusy(ssid);
    call(kNmPath, kNm, QStringLiteral("AddAndActivateConnection"),
         {QVariant::fromValue(settings), objectPath(device), objectPath(accessPoint.value(QStringLiteral("path")).toString())},
         [this, a](const QDBusMessage &reply) { activationStarted(reply, 1, a); }, kAuthTimeoutMs);
}

QString Nm::sniffVpnType(const QString &path) const
{
    QFile f(path);
    const QFileInfo info(path);
    if (!info.isFile() || !f.open(QIODevice::ReadOnly))
        return {};
    // Only the structure is inspected; the content (it holds keys) is not kept.
    const QString head = QString::fromUtf8(f.read(64 * 1024));
    if (head.contains(QLatin1String("[Interface]"), Qt::CaseInsensitive)
        && (head.contains(QLatin1String("PrivateKey"), Qt::CaseInsensitive) || head.contains(QLatin1String("[Peer]"), Qt::CaseInsensitive)))
        return QStringLiteral("wireguard");
    static const QRegularExpression openvpn(QStringLiteral("(^|\\n)\\s*(client|remote\\s+\\S+|dev\\s+(tun|tap)\\S*|<ca>|auth-user-pass)"));
    if (info.suffix().compare(QLatin1String("ovpn"), Qt::CaseInsensitive) == 0 || openvpn.match(head).hasMatch())
        return QStringLiteral("openvpn");
    return {};
}

bool Nm::vpnPluginInstalled(const QString &type) const
{
    // WireGuard is built into NetworkManager; everything else is a plug-in.
    return type == QLatin1String("wireguard") || m_vpnPlugins.contains(type);
}

void Nm::importVpn(const QString &type, const QString &path)
{
    const QString op = QStringLiteral("vpn-import");
    loadVpnPlugins();
    if (type != QLatin1String("wireguard") && type != QLatin1String("openvpn")) {
        failOperation(op, path, tr("Only WireGuard and OpenVPN configuration files can be imported."));
        return;
    }
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.isFile() || !info.isReadable()) {
        failOperation(op, path, tr("“%1” is not a readable file.").arg(path));
        return;
    }
    if (info.size() > 1024 * 1024) {
        failOperation(op, path, tr("“%1” is too large to be a VPN configuration.").arg(info.fileName()));
        return;
    }
    if (!vpnPluginInstalled(type)) {
        failOperation(op, path, tr("NetworkManager's OpenVPN plug-in is not installed (package network-manager-openvpn), "
                                   "so OpenVPN configurations cannot be imported."));
        return;
    }
    operation(op, path, {QStringLiteral("connection"), QStringLiteral("import"), QStringLiteral("type"), type,
                         QStringLiteral("file"), info.absoluteFilePath()},
              30000, {}, [this, op, path, type](const Nexus::CommandResult &r) {
        QString message;
        if (r.ok()) {
            // "Connection 'name' (uuid) successfully added."
            static const QRegularExpression added(QStringLiteral("Connection '(.*)' \\(([0-9a-f-]{36})\\)"));
            const auto match = added.match(r.outText());
            message = match.hasMatch() ? tr("Imported “%1”.").arg(match.captured(1)) : Net::cleanMessage(r.outText());
        } else {
            message = errorText(r);
            if (message.contains(QLatin1String("unknown VPN plugin")))
                message = tr("NetworkManager has no plug-in for %1 (package network-manager-%1).").arg(type);
        }
        Q_EMIT operationFinished(op, path, r.ok(), message);
    });
}

// ---- IP / DNS configuration ---------------------------------------------------------

QVariantMap Nm::planIpChange(const QString &uuid, const QVariantMap &config) const
{
    QStringList errors, warnings;
    QVariantList changes;
    QVariantMap normalized;
    const QVariantMap con = connectionByUuid(uuid);
    QVariantMap result;
    if (!Net::isUuid(uuid) || con.isEmpty() || !con.value(QStringLiteral("hasDetails")).toBool()) {
        result.insert(QStringLiteral("ok"), false);
        result.insert(QStringLiteral("errors"), QStringList{tr("The connection profile is not known to NetworkManager any more.")});
        return result;
    }

    auto change = [&changes](const QString &property, const QString &label, const QString &from,
                             const QString &to, const QString &) {
        changes << QVariantMap{{QStringLiteral("property"), property}, {QStringLiteral("label"), label},
                               {QStringLiteral("from"), from}, {QStringLiteral("to"), to}};
    };

    for (const int family : {4, 6}) {
        const QString key = QStringLiteral("ipv%1").arg(family);
        if (!config.contains(key))
            continue;
        const QString ipName = QStringLiteral("IPv%1").arg(family);
        const QVariantMap current = con.value(key).toMap();
        const QVariantMap wanted = config.value(key).toMap();

        const QString currentMethod = current.value(QStringLiteral("method")).toString();
        const QString method = wanted.contains(QStringLiteral("method"))
            ? wanted.value(QStringLiteral("method")).toString().trimmed() : currentMethod;
        const QStringList currentAddresses = current.value(QStringLiteral("addresses")).toStringList();
        QStringList addresses = wanted.contains(QStringLiteral("addresses"))
            ? userList(wanted.value(QStringLiteral("addresses"))) : currentAddresses;
        const QString currentGateway = current.value(QStringLiteral("gateway")).toString();
        QString gateway = wanted.contains(QStringLiteral("gateway"))
            ? wanted.value(QStringLiteral("gateway")).toString().trimmed() : currentGateway;
        const QStringList currentDns = current.value(QStringLiteral("dns")).toStringList();
        QStringList dns = wanted.contains(QStringLiteral("dns")) ? userList(wanted.value(QStringLiteral("dns"))) : currentDns;
        const QStringList currentSearch = current.value(QStringLiteral("search")).toStringList();
        QStringList search = wanted.contains(QStringLiteral("search")) ? userList(wanted.value(QStringLiteral("search"))) : currentSearch;
        const bool currentIgnore = current.value(QStringLiteral("ignoreAutoDns")).toBool();
        const bool ignore = wanted.contains(QStringLiteral("ignoreAutoDns"))
            ? wanted.value(QStringLiteral("ignoreAutoDns")).toBool() : currentIgnore;

        static const QStringList methods4 = {QStringLiteral("auto"), QStringLiteral("manual"), QStringLiteral("link-local"),
                                             QStringLiteral("shared"), QStringLiteral("disabled")};
        static const QStringList methods6 = {QStringLiteral("auto"), QStringLiteral("dhcp"), QStringLiteral("manual"),
                                             QStringLiteral("link-local"), QStringLiteral("shared"), QStringLiteral("ignore"),
                                             QStringLiteral("disabled")};
        if (!(family == 4 ? methods4 : methods6).contains(method)) {
            errors << tr("%1: “%2” is not a method NetworkManager knows.").arg(ipName, method);
            continue;
        }

        // NetworkManager refuses addresses, a gateway or DNS settings with
        // these methods, so they are cleared (and listed as changes).
        const bool noStatic = method == QLatin1String("disabled") || method == QLatin1String("ignore")
            || method == QLatin1String("link-local");
        if (noStatic) {
            addresses.clear();
            gateway.clear();
            dns.clear();
            search.clear();
        }

        QList<std::pair<QString, int>> parsed;
        for (const QString &entry : std::as_const(addresses)) {
            QString address, error;
            int prefix = 0;
            if (!Net::parseCidr(entry, family, &address, &prefix, &error)) {
                errors << QStringLiteral("%1: %2").arg(ipName, error);
                continue;
            }
            bool duplicate = false;
            for (const auto &p : std::as_const(parsed))
                duplicate = duplicate || p.first == address;
            if (duplicate)
                errors << tr("%1: the address %2 is listed twice.").arg(ipName, address);
            parsed.append({address, prefix});
        }
        QStringList normalizedAddresses;
        for (const auto &p : std::as_const(parsed))
            normalizedAddresses << QStringLiteral("%1/%2").arg(p.first).arg(p.second);

        if (method == QLatin1String("manual") && addresses.isEmpty())
            errors << tr("%1: the manual method needs at least one address with a prefix length.").arg(ipName);
        if (method == QLatin1String("shared") && addresses.size() > 1)
            errors << tr("%1: a shared connection has at most one address.").arg(ipName);

        if (!gateway.isEmpty()) {
            const QString canonical = Net::canonicalAddress(gateway, family);
            if (canonical.isEmpty()) {
                errors << tr("%1: the gateway “%2” is not a valid %1 address.").arg(ipName, gateway);
            } else {
                gateway = canonical;
                const QString reason = Net::unusableAddressReason(gateway, family);
                if (!reason.isEmpty())
                    errors << QStringLiteral("%1: %2").arg(ipName, reason);
                if (addresses.isEmpty()) {
                    errors << tr("%1: a gateway can only be set together with a static address.").arg(ipName);
                } else if (errors.isEmpty()) {
                    bool own = false, onLink = false;
                    for (const auto &p : std::as_const(parsed)) {
                        own = own || p.first == gateway;
                        onLink = onLink || Net::sameSubnet(p.first, gateway, p.second, family);
                    }
                    if (own)
                        errors << tr("%1: the gateway %2 is one of this computer's own addresses.").arg(ipName, gateway);
                    else if (!onLink && !gateway.startsWith(QLatin1String("fe80:"), Qt::CaseInsensitive))
                        warnings << tr("%1: the gateway %2 is outside every configured subnet; it is only reachable if "
                                       "the network routes to it on-link.").arg(ipName, gateway);
                }
            }
        }

        for (int i = 0; i < dns.size(); ++i) {
            const QString canonical = Net::canonicalAddress(dns.at(i), family);
            if (canonical.isEmpty()) {
                errors << tr("%1: the DNS server “%2” is not a valid %1 address.").arg(ipName, dns.at(i));
                continue;
            }
            dns[i] = canonical;
            const QString reason = Net::unusableAddressReason(canonical, family);
            if (!reason.isEmpty())
                errors << QStringLiteral("%1 DNS: %2").arg(ipName, reason);
            if (dns.indexOf(canonical) != i)
                errors << tr("%1: the DNS server %2 is listed more than once.").arg(ipName, canonical);
        }
        for (const QString &domain : std::as_const(search)) {
            // "~domain" makes it a routing-only domain; "~." routes everything.
            const QString bare = domain.startsWith(QLatin1Char('~')) ? domain.mid(1) : domain;
            if (!(bare.isEmpty() && domain == QLatin1String("~")) && bare != QLatin1String(".") && !Net::isDnsName(bare))
                errors << tr("%1: “%2” is not a valid search domain.").arg(ipName, domain);
        }

        const QString p = key + QLatin1Char('.');
        if (method != currentMethod)
            change(p + QStringLiteral("method"), tr("%1 method").arg(ipName), shown(currentMethod), method, method);
        if (normalizedAddresses != currentAddresses)
            change(p + QStringLiteral("addresses"), tr("%1 addresses").arg(ipName), shown(currentAddresses),
                   shown(normalizedAddresses), normalizedAddresses.join(QLatin1Char(',')));
        if (gateway != currentGateway)
            change(p + QStringLiteral("gateway"), tr("%1 gateway").arg(ipName), shown(currentGateway), shown(gateway), gateway);
        if (dns != currentDns)
            change(p + QStringLiteral("dns"), tr("%1 DNS servers").arg(ipName), shown(currentDns), shown(dns), dns.join(QLatin1Char(',')));
        if (search != currentSearch)
            change(p + QStringLiteral("dns-search"), tr("%1 search domains").arg(ipName), shown(currentSearch), shown(search),
                   search.join(QLatin1Char(',')));
        if (ignore != currentIgnore)
            change(p + QStringLiteral("ignore-auto-dns"), tr("%1: ignore DNS servers from DHCP / router").arg(ipName),
                   currentIgnore ? tr("yes") : tr("no"), ignore ? tr("yes") : tr("no"),
                   ignore ? QStringLiteral("yes") : QStringLiteral("no"));
        // What the profile's setting becomes (see applyIpChange()).
        normalized.insert(key, QVariantMap{{QStringLiteral("method"), method}, {QStringLiteral("addresses"), normalizedAddresses},
                                           {QStringLiteral("gateway"), gateway}, {QStringLiteral("dns"), dns},
                                           {QStringLiteral("search"), search}, {QStringLiteral("ignoreAutoDns"), ignore}});
    }

    if (config.contains(QStringLiteral("dnsOverTls"))) {
        const QString wanted = config.value(QStringLiteral("dnsOverTls")).toString();
        const QString current = con.value(QStringLiteral("dnsOverTls")).toString();
        static const QStringList values = {QStringLiteral("default"), QStringLiteral("no"), QStringLiteral("opportunistic"),
                                           QStringLiteral("yes")};
        if (!values.contains(wanted))
            errors << tr("“%1” is not a DNS-over-TLS mode.").arg(wanted);
        else if (wanted != current)
            change(QStringLiteral("connection.dns-over-tls"), tr("DNS over TLS"), current, wanted, dotNumber(wanted));
        if (values.contains(wanted))
            normalized.insert(QStringLiteral("dnsOverTls"), wanted);
    }

    result.insert(QStringLiteral("ok"), errors.isEmpty());
    result.insert(QStringLiteral("errors"), errors);
    result.insert(QStringLiteral("warnings"), warnings);
    result.insert(QStringLiteral("changes"), errors.isEmpty() ? changes : QVariantList());
    result.insert(QStringLiteral("normalized"), errors.isEmpty() ? normalized : QVariantMap());
    result.insert(QStringLiteral("active"), con.value(QStringLiteral("active")).toBool());
    result.insert(QStringLiteral("device"), con.value(QStringLiteral("device")).toString());
    result.insert(QStringLiteral("name"), con.value(QStringLiteral("name")).toString());
    return result;
}

void Nm::applyIpChange(const QString &uuid, const QVariantMap &config, bool reactivate)
{
    const QString op = QStringLiteral("ip-config");
    if (!ensureRunning(op, uuid))
        return;
    // Never trust a plan computed earlier: the profile may have changed since.
    const QVariantMap plan = planIpChange(uuid, config);
    if (!plan.value(QStringLiteral("ok")).toBool()) {
        failOperation(op, uuid, plan.value(QStringLiteral("errors")).toStringList().join(QLatin1Char(' ')));
        return;
    }
    const QString profile = profilePath(uuid);
    if (plan.value(QStringLiteral("changes")).toList().isEmpty() || profile.isEmpty()) {
        failOperation(op, uuid, tr("Nothing to change."));
        return;
    }
    const QString name = plan.value(QStringLiteral("name")).toString();
    const bool active = plan.value(QStringLiteral("active")).toBool();
    const QString device = devicePath(plan.value(QStringLiteral("device")).toString());
    const QVariantMap normalized = plan.value(QStringLiteral("normalized")).toMap();

    auto mutate = [normalized](NmSettings &settings) {
        for (const int family : {4, 6}) {
            const QString key = QStringLiteral("ipv%1").arg(family);
            if (!normalized.contains(key))
                continue;
            const QVariantMap wanted = normalized.value(key).toMap();
            QVariantMap &ip = settings[key];
            ip.insert(QStringLiteral("method"), wanted.value(QStringLiteral("method")).toString());

            // address-data (aa{sv}) supersedes the legacy binary "addresses".
            QList<QVariantMap> addressData;
            const QStringList addresses = wanted.value(QStringLiteral("addresses")).toStringList();
            for (const QString &entry : addresses) {
                addressData << QVariantMap{{QStringLiteral("address"), entry.section(QLatin1Char('/'), 0, 0)},
                                           {QStringLiteral("prefix"), entry.section(QLatin1Char('/'), 1, 1).toUInt()}};
            }
            ip.insert(QStringLiteral("address-data"), QVariant::fromValue(addressData));
            ip.remove(QStringLiteral("addresses"));

            const QString gateway = wanted.value(QStringLiteral("gateway")).toString();
            if (gateway.isEmpty())
                ip.remove(QStringLiteral("gateway"));
            else
                ip.insert(QStringLiteral("gateway"), gateway);

            // DNS servers in the binary form every NetworkManager version
            // reads: au (network byte order) for IPv4, aay for IPv6.
            const QStringList dns = wanted.value(QStringLiteral("dns")).toStringList();
            ip.remove(QStringLiteral("dns-data"));
            ip.remove(QStringLiteral("dns"));
            if (!dns.isEmpty() && family == 4) {
                QList<uint> servers;
                for (const QString &server : dns) {
                    in_addr a {};
                    if (inet_pton(AF_INET, server.toLatin1().constData(), &a) == 1)
                        servers << a.s_addr;
                }
                ip.insert(QStringLiteral("dns"), QVariant::fromValue(servers));
            } else if (!dns.isEmpty()) {
                QList<QByteArray> servers;
                for (const QString &server : dns) {
                    in6_addr a {};
                    if (inet_pton(AF_INET6, server.toLatin1().constData(), &a) == 1)
                        servers << QByteArray(reinterpret_cast<const char *>(&a), 16);
                }
                ip.insert(QStringLiteral("dns"), QVariant::fromValue(servers));
            }

            const QStringList search = wanted.value(QStringLiteral("search")).toStringList();
            if (search.isEmpty())
                ip.remove(QStringLiteral("dns-search"));
            else
                ip.insert(QStringLiteral("dns-search"), search);
            ip.insert(QStringLiteral("ignore-auto-dns"), wanted.value(QStringLiteral("ignoreAutoDns")).toBool());
        }
        if (normalized.contains(QStringLiteral("dnsOverTls")))
            settings[QStringLiteral("connection")].insert(QStringLiteral("dns-over-tls"),
                                                          dotNumber(normalized.value(QStringLiteral("dnsOverTls")).toString()).toInt());
    };

    beginBusy(uuid);
    updateProfile(profile, mutate, [this, op, uuid, name, active, reactivate, profile, device](const QDBusMessage &reply) {
        if (reply.type() == QDBusMessage::ErrorMessage || !active || !reactivate) {
            simpleResult(op, uuid, reply, active
                ? tr("“%1” was saved. The change applies the next time the connection is activated.").arg(name)
                : tr("“%1” was saved.").arg(name));
            return;
        }
        // Bring the profile up again so the running configuration matches it.
        Activation a;
        a.operation = op;
        a.target = uuid;
        a.name = name;
        a.successText = tr("“%1” was saved and re-activated.").arg(name);
        if (!device.isEmpty())
            a.devices = QStringList{device};
        m_deviceFailure.clear();
        call(kNmPath, kNm, QStringLiteral("ActivateConnection"), {objectPath(profile), objectPath(device), objectPath(QString())},
             [this, a](const QDBusMessage &started) { activationStarted(started, 0, a); }, kAuthTimeoutMs);
    });
}

// ---- dumps ---------------------------------------------------------------------------

QJsonObject Nm::dumpGeneral() const
{
    return QJsonObject{
        {QStringLiteral("status"), m_status},
        {QStringLiteral("running"), running()},
        {QStringLiteral("statusText"), m_statusText},
        {QStringLiteral("version"), m_version},
        {QStringLiteral("state"), m_state},
        {QStringLiteral("connectivity"), m_connectivity},
        {QStringLiteral("networkingEnabled"), m_networking},
        {QStringLiteral("wifiHardware"), m_wifiHardware},
        {QStringLiteral("wifiEnabled"), m_wifiEnabled},
    };
}

QJsonObject Nm::dumpWifi() const
{
    QJsonObject o = dumpGeneral();
    o.insert(QStringLiteral("devices"), QJsonArray::fromVariantList(wifiDevices()));
    o.insert(QStringLiteral("networks"), QJsonArray::fromVariantList(m_wifiNetworks));
    o.insert(QStringLiteral("savedNetworks"), QJsonArray::fromVariantList(savedWifi()));
    return o;
}

QJsonObject Nm::dumpConnections() const
{
    QJsonObject o = dumpGeneral();
    o.insert(QStringLiteral("devices"), QJsonArray::fromVariantList(m_devices));
    o.insert(QStringLiteral("connections"), QJsonArray::fromVariantList(m_connections));
    return o;
}

QJsonObject Nm::dumpVpn() const
{
    QJsonObject o = dumpGeneral();
    o.insert(QStringLiteral("plugins"), QJsonArray::fromStringList(m_vpnPlugins));
    o.insert(QStringLiteral("wireguardSupported"), true);
    o.insert(QStringLiteral("openvpnPluginInstalled"), m_vpnPlugins.contains(QStringLiteral("openvpn")));
    o.insert(QStringLiteral("connections"), QJsonArray::fromVariantList(vpnConnections()));
    o.insert(QStringLiteral("activeCount"), activeVpnCount());
    return o;
}
