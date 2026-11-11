// SPDX-License-Identifier: GPL-3.0-or-later
#include "networkbackend.h"

#include "dumpregistry.h"
#include "networkcommon.h"
#include "networkmanagerutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <memory>

using namespace NetHw;

SHADOW_CC_DUMP("network", [] {
    NetworkBackend backend;
    waitUntilReady(&backend, 12000);
    return QJsonValue(backend.toJson());
})

namespace {
const QString kProfileFields = QStringLiteral(
    "connection.uuid,connection.id,connection.type,connection.interface-name,"
    "ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns,"
    "ipv6.method,ipv6.addresses,ipv6.gateway,ipv6.dns,proxy.method,proxy.pac-url");

// "a, b  c" → [a, b, c]
QStringList splitList(const QString &text)
{
    static const QRegularExpression separator(QStringLiteral("[,\\s]+"));
    return text.split(separator, Qt::SkipEmptyParts);
}

QString text(const QVariantMap &map, const char *key)
{
    return map.value(QLatin1String(key)).toString().trimmed();
}

QString deviceStateText(int code, bool carrier)
{
    if (code == 100)
        return NetworkBackend::tr("Connected");
    if (code >= 40 && code <= 90)
        return NetworkBackend::tr("Connecting…");
    switch (code) {
    case 10: return NetworkBackend::tr("Not managed by NetworkManager");
    case 20: return carrier ? NetworkBackend::tr("Not ready") : NetworkBackend::tr("Cable unplugged");
    case 30: return NetworkBackend::tr("Disconnected");
    case 110: return NetworkBackend::tr("Disconnecting…");
    case 120: return NetworkBackend::tr("Connection failed");
    default: return NetworkBackend::tr("Unknown");
    }
}

// A host name (RFC 1123), an IPv4 address or an IPv6 address — no scheme, no
// user information, no port.
bool isHost(const QString &host)
{
    if (host.isEmpty() || host.size() > 253)
        return false;
    if (Nm::isIpv4Address(host) || Nm::isIpv6Address(host))
        return true;
    static const QRegularExpression name(QStringLiteral(
        "^(?=.*[A-Za-z])[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\\.?$"));
    return name.match(host).hasMatch();
}

bool isPort(const QVariant &value)
{
    bool ok = false;
    const int port = value.toString().trimmed().toInt(&ok);
    return ok && port >= 1 && port <= 65535;
}

// One entry of a no_proxy list: host or domain (".example.org", "*.example.org"),
// IP address, or network in CIDR notation; optionally with ":port".
bool isNoProxyEntry(const QString &entry)
{
    if (entry == QLatin1String("*") || Nm::isIpv4Address(entry) || Nm::isIpv6Address(entry)
        || Nm::isIpv4Cidr(entry) || Nm::isIpv6Cidr(entry))
        return true;
    static const QRegularExpression pattern(QStringLiteral("^(\\*)?\\.?[A-Za-z0-9]([A-Za-z0-9.-]{0,251}[A-Za-z0-9])?(:[0-9]{1,5})?$"));
    return pattern.match(entry).hasMatch();
}

QString proxyUrl(const QString &scheme, const QString &host, const QString &port)
{
    const QString bracketed = host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']') : host;
    return scheme + QStringLiteral("://") + bracketed + QLatin1Char(':') + port;
}

bool isPacUrl(const QString &value)
{
    static const QRegularExpression unsafe(QStringLiteral("[\\s\"'$\\\\`<>{}|^]"));
    if (value.isEmpty() || value.size() > 2000 || unsafe.match(value).hasMatch())
        return false;
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || !url.userInfo().isEmpty())
        return false;
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        return !url.host().isEmpty();
    return url.scheme() == QLatin1String("file") && url.path().startsWith(QLatin1Char('/'));
}
} // namespace

struct NetworkBackend::Fetch
{
    int pending = 0;
    bool failed = false;
    QString error;
    QString devices;
    QString connections;
    QString profiles;
};

NetworkBackend::NetworkBackend(QObject *parent)
    : QObject(parent)
{
    m_devices = new RowModel({
        QStringLiteral("name"), QStringLiteral("stateCode"), QStringLiteral("stateText"), QStringLiteral("connected"),
        QStringLiteral("managed"), QStringLiteral("carrier"), QStringLiteral("speed"), QStringLiteral("mac"),
        QStringLiteral("profile"), QStringLiteral("uuid"), QStringLiteral("ipv4"), QStringLiteral("gateway4"),
        QStringLiteral("ipv6"), QStringLiteral("gateway6"), QStringLiteral("dns"),
        QStringLiteral("method4"), QStringLiteral("addresses4"), QStringLiteral("profileGateway4"), QStringLiteral("dns4"),
        QStringLiteral("method6"), QStringLiteral("addresses6"), QStringLiteral("profileGateway6"), QStringLiteral("dns6"),
    }, QStringLiteral("name"), this);

    loadProxyFile();

    // Carrier changes arrive as NetworkManager signals; the timer is a
    // backstop while the page is open.
    m_poll.setInterval(20000);
    connect(&m_poll, &QTimer::timeout, this, &NetworkBackend::refresh);
    m_poll.start();

    m_watcher = new Nm::Watcher(this);
    connect(m_watcher, &Nm::Watcher::runningChanged, this, &NetworkBackend::refresh);
    connect(m_watcher, &Nm::Watcher::changed, this, &NetworkBackend::refresh);
}

QAbstractItemModel *NetworkBackend::devices() const { return m_devices; }
int NetworkBackend::deviceCount() const { return m_devices->count(); }

void NetworkBackend::setBusy(bool busy)
{
    if (busy == m_busy)
        return;
    m_busy = busy;
    Q_EMIT busyChanged();
}

// ---- reading -----------------------------------------------------------------------

void NetworkBackend::setUnavailable(const QString &state, const QString &message)
{
    m_devices->setRows({});
    m_activeProfiles.clear();
    m_proxyProfiles.clear();
    Q_EMIT devicesChanged();
    m_state = state;
    m_statusMessage = message;
    Q_EMIT stateChanged();
    finishRefresh();
}

void NetworkBackend::finishRefresh()
{
    m_refreshing = false;
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
    if (m_refreshAgain) {
        m_refreshAgain = false;
        QTimer::singleShot(0, this, &NetworkBackend::refresh);
    }
}

void NetworkBackend::refresh()
{
    if (m_refreshing) {
        m_refreshAgain = true;
        return;
    }
    if (!Nm::cliAvailable()) {
        setUnavailable(QStringLiteral("nm-missing"),
                       tr("nmcli is not installed. Network connections are managed by NetworkManager (package network-manager)."));
        return;
    }
    if (!m_watcher->known())
        return;
    if (!m_watcher->running()) {
        setUnavailable(QStringLiteral("nm-not-running"),
                       m_watcher->busConnected()
                           ? tr("NetworkManager is not running, so wired connections cannot be shown or changed. "
                                "It is started by NetworkManager.service.")
                           : tr("The system message bus is not available, so NetworkManager cannot be reached."));
        return;
    }

    m_refreshing = true;
    auto fetch = std::make_shared<Fetch>();
    const auto first = [this, fetch] {
        if (--fetch->pending > 0)
            return;
        if (fetch->failed) {
            publish(*fetch);
            return;
        }
        // Profiles worth a closer look: every Ethernet profile, and the active Wi-Fi ones (for the proxy).
        QStringList uuids;
        const QStringList lines = fetch->connections.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QStringList fields = Nm::splitTerse(line);
            if (fields.size() < 4 || !Nm::isUuid(fields.at(1)))
                continue;
            const bool active = fields.at(3) == QLatin1String("yes");
            if (fields.at(2) == QLatin1String("802-3-ethernet") || (active && fields.at(2) == QLatin1String("802-11-wireless")))
                uuids.append(fields.at(1));
        }
        if (uuids.isEmpty()) {
            publish(*fetch);
            return;
        }
        QStringList arguments = {QStringLiteral("-t"), QStringLiteral("-f"), kProfileFields,
                                 QStringLiteral("connection"), QStringLiteral("show")};
        arguments.append(uuids);
        Nm::run(this, arguments, [this, fetch](bool, const QString &out, const QString &) {
            fetch->profiles = out;
            publish(*fetch);
        });
    };

    fetch->pending = 2;
    Nm::run(this, {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("GENERAL,CAPABILITIES,WIRED-PROPERTIES,IP4,IP6"),
                   QStringLiteral("device"), QStringLiteral("show")},
            [fetch, first](bool ok, const QString &out, const QString &error) {
        fetch->devices = out;
        if (!ok) {
            fetch->failed = true;
            fetch->error = error;
        }
        first();
    });
    Nm::run(this, {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("NAME,UUID,TYPE,ACTIVE"),
                   QStringLiteral("connection"), QStringLiteral("show")},
            [fetch, first](bool, const QString &out, const QString &) {
        fetch->connections = out;
        first();
    });
}

void NetworkBackend::publish(const Fetch &fetch)
{
    if (fetch.failed) {
        setUnavailable(QStringLiteral("nm-not-running"), tr("NetworkManager does not answer (%1).").arg(fetch.error));
        return;
    }

    QHash<QString, Nm::Record> profiles;                // uuid → details
    QList<Nm::Record> ethernetProfiles;
    const QList<Nm::Record> profileRecords = Nm::parseRecords(fetch.profiles);
    for (const Nm::Record &record : profileRecords) {
        profiles.insert(record.value(QStringLiteral("connection.uuid")), record);
        if (record.value(QStringLiteral("connection.type")) == QLatin1String("802-3-ethernet"))
            ethernetProfiles.append(record);
    }

    // Active Ethernet and Wi-Fi profiles carry NetworkManager's proxy setting.
    m_activeProfiles.clear();
    m_proxyProfiles.clear();
    const QStringList connectionLines = fetch.connections.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : connectionLines) {
        const QStringList fields = Nm::splitTerse(line);
        if (fields.size() < 4 || fields.at(3) != QLatin1String("yes") || !profiles.contains(fields.at(1)))
            continue;
        const Nm::Record details = profiles.value(fields.at(1));
        ActiveProfile profile;
        profile.uuid = fields.at(1);
        profile.name = fields.at(0);
        profile.method = details.value(QStringLiteral("proxy.method"));
        profile.pacUrl = Nm::unset(details.value(QStringLiteral("proxy.pac-url")));
        m_activeProfiles.append(profile);
        if (profile.method == QLatin1String("auto"))
            m_proxyProfiles.append(profile.name);
    }

    QVariantList rows;
    const QList<Nm::Record> devices = Nm::parseRecords(fetch.devices);
    for (const Nm::Record &device : devices) {
        const QString name = device.value(QStringLiteral("GENERAL.DEVICE"));
        if (device.value(QStringLiteral("GENERAL.TYPE")) != QLatin1String("ethernet") || !Nm::isInterfaceName(name))
            continue;
        const int stateCode = Nm::leadingNumber(device.value(QStringLiteral("GENERAL.STATE")));
        const bool carrier = device.value(QStringLiteral("WIRED-PROPERTIES.CARRIER")) == QLatin1String("on");

        // The profile in use, or the one that would be used: bound to this
        // interface first, then one that is not bound to any.
        QString uuid = device.value(QStringLiteral("GENERAL.CON-UUID"));
        if (!Nm::isUuid(uuid) || !profiles.contains(uuid)) {
            uuid.clear();
            for (const Nm::Record &candidate : std::as_const(ethernetProfiles)) {
                const QString bound = Nm::unset(candidate.value(QStringLiteral("connection.interface-name")));
                if (bound == name) {
                    uuid = candidate.value(QStringLiteral("connection.uuid"));
                    break;
                }
                if (bound.isEmpty() && uuid.isEmpty())
                    uuid = candidate.value(QStringLiteral("connection.uuid"));
            }
        }
        const Nm::Record profile = profiles.value(uuid);
        const QString speed = device.value(QStringLiteral("CAPABILITIES.SPEED"));

        rows.append(QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("stateCode"), stateCode},
            {QStringLiteral("stateText"), deviceStateText(stateCode, carrier)},
            {QStringLiteral("connected"), stateCode == 100},
            {QStringLiteral("managed"), stateCode != 10},
            {QStringLiteral("carrier"), carrier},
            {QStringLiteral("speed"), speed == QLatin1String("unknown") ? QString() : speed},
            {QStringLiteral("mac"), device.value(QStringLiteral("GENERAL.HWADDR"))},
            {QStringLiteral("profile"), profile.value(QStringLiteral("connection.id"))},
            {QStringLiteral("uuid"), uuid},
            {QStringLiteral("ipv4"), Nm::indexedValues(device, QStringLiteral("IP4.ADDRESS")).join(QStringLiteral(", "))},
            {QStringLiteral("gateway4"), Nm::unset(device.value(QStringLiteral("IP4.GATEWAY")))},
            {QStringLiteral("ipv6"), Nm::indexedValues(device, QStringLiteral("IP6.ADDRESS")).join(QStringLiteral(", "))},
            {QStringLiteral("gateway6"), Nm::unset(device.value(QStringLiteral("IP6.GATEWAY")))},
            {QStringLiteral("dns"), (Nm::indexedValues(device, QStringLiteral("IP4.DNS"))
                                     + Nm::indexedValues(device, QStringLiteral("IP6.DNS"))).join(QStringLiteral(", "))},
            {QStringLiteral("method4"), profile.value(QStringLiteral("ipv4.method"))},
            {QStringLiteral("addresses4"), Nm::unset(profile.value(QStringLiteral("ipv4.addresses")))},
            {QStringLiteral("profileGateway4"), Nm::unset(profile.value(QStringLiteral("ipv4.gateway")))},
            {QStringLiteral("dns4"), Nm::unset(profile.value(QStringLiteral("ipv4.dns")))},
            {QStringLiteral("method6"), profile.value(QStringLiteral("ipv6.method"))},
            {QStringLiteral("addresses6"), Nm::unset(profile.value(QStringLiteral("ipv6.addresses")))},
            {QStringLiteral("profileGateway6"), Nm::unset(profile.value(QStringLiteral("ipv6.gateway")))},
            {QStringLiteral("dns6"), Nm::unset(profile.value(QStringLiteral("ipv6.dns")))},
        });
    }
    std::sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString() < b.toMap().value(QStringLiteral("name")).toString();
    });

    m_devices->setRows(rows);
    Q_EMIT devicesChanged();
    m_state = QStringLiteral("ok");
    m_statusMessage.clear();
    Q_EMIT stateChanged();
    finishRefresh();
}

// ---- wired interfaces -----------------------------------------------------------------

void NetworkBackend::setDeviceEnabled(const QString &device, bool enabled)
{
    if (m_devices->find(device).isEmpty() || !Nm::isInterfaceName(device)) {
        Q_EMIT failed(tr("The interface %1 is no longer there.").arg(device));
        return;
    }
    setBusy(true);
    Nm::run(this, {QStringLiteral("-w"), QStringLiteral("30"), QStringLiteral("device"),
                   enabled ? QStringLiteral("connect") : QStringLiteral("disconnect"), device},
            [this, device, enabled](bool ok, const QString &, const QString &error) {
        setBusy(false);
        if (!ok) {
            Q_EMIT failed(enabled ? tr("%1 could not be connected: %2").arg(device, error)
                                  : tr("%1 could not be disconnected: %2").arg(device, error));
        }
        refresh();
    }, 45000);
}

QString NetworkBackend::ipProblem(const QVariantMap &settings) const
{
    const QString method4 = text(settings, "method4");
    const QStringList addresses4 = splitList(text(settings, "addresses4"));
    const QString gateway4 = text(settings, "gateway4");
    const QStringList dns4 = splitList(text(settings, "dns4"));
    if (method4 != QLatin1String("auto") && method4 != QLatin1String("manual"))
        return tr("Choose automatic or manual IPv4 configuration.");
    if (method4 == QLatin1String("manual")) {
        if (addresses4.isEmpty())
            return tr("Manual IPv4 needs an address with its prefix length, for example 192.168.1.20/24.");
        for (const QString &address : addresses4) {
            if (!Nm::isIpv4Cidr(address))
                return tr("“%1” is not an IPv4 address with a prefix length (like 192.168.1.20/24).").arg(address);
            const int firstOctet = address.section(QLatin1Char('.'), 0, 0).toInt();
            if (firstOctet == 0 || firstOctet == 127 || firstOctet >= 224)
                return tr("“%1” cannot be used as the address of this computer.").arg(address);
        }
        if (!gateway4.isEmpty() && !Nm::isIpv4Address(gateway4))
            return tr("“%1” is not an IPv4 gateway address.").arg(gateway4);
    }
    for (const QString &server : dns4) {
        if (!Nm::isIpv4Address(server))
            return tr("“%1” is not an IPv4 DNS server address.").arg(server);
    }

    const QString method6 = text(settings, "method6");
    const QStringList addresses6 = splitList(text(settings, "addresses6"));
    const QString gateway6 = text(settings, "gateway6");
    const QStringList dns6 = splitList(text(settings, "dns6"));
    if (method6 != QLatin1String("auto") && method6 != QLatin1String("manual") && method6 != QLatin1String("disabled"))
        return tr("Choose automatic, manual or disabled IPv6 configuration.");
    if (method6 == QLatin1String("manual")) {
        if (addresses6.isEmpty())
            return tr("Manual IPv6 needs an address with its prefix length, for example 2001:db8::20/64.");
        for (const QString &address : addresses6) {
            if (!Nm::isIpv6Cidr(address))
                return tr("“%1” is not an IPv6 address with a prefix length (like 2001:db8::20/64).").arg(address);
        }
        if (!gateway6.isEmpty() && !Nm::isIpv6Address(gateway6))
            return tr("“%1” is not an IPv6 gateway address.").arg(gateway6);
    }
    if (method6 != QLatin1String("disabled")) {
        for (const QString &server : dns6) {
            if (!Nm::isIpv6Address(server))
                return tr("“%1” is not an IPv6 DNS server address.").arg(server);
        }
    }
    return QString();
}

void NetworkBackend::applyIp(const QString &device, const QVariantMap &settings)
{
    const QVariantMap row = m_devices->find(device);
    const QString uuid = row.value(QStringLiteral("uuid")).toString();
    if (row.isEmpty() || !Nm::isUuid(uuid)) {
        Q_EMIT failed(tr("%1 has no connection profile to change. Connect it once so that NetworkManager creates one.").arg(device));
        return;
    }
    const QString problem = ipProblem(settings);
    if (!problem.isEmpty()) {
        Q_EMIT failed(problem);
        return;
    }

    const QString method4 = text(settings, "method4");
    const QString method6 = text(settings, "method6");
    const QString dns4 = splitList(text(settings, "dns4")).join(QLatin1Char(','));
    const QString dns6 = method6 == QLatin1String("disabled") ? QString() : splitList(text(settings, "dns6")).join(QLatin1Char(','));

    // Ordered so that the profile is valid after every single assignment.
    QStringList arguments = {QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("uuid"), uuid};
    if (method4 == QLatin1String("manual")) {
        arguments << QStringLiteral("ipv4.addresses") << splitList(text(settings, "addresses4")).join(QLatin1Char(','))
                  << QStringLiteral("ipv4.method") << QStringLiteral("manual")
                  << QStringLiteral("ipv4.gateway") << text(settings, "gateway4");
    } else {
        arguments << QStringLiteral("ipv4.method") << QStringLiteral("auto")
                  << QStringLiteral("ipv4.gateway") << QString()
                  << QStringLiteral("ipv4.addresses") << QString();
    }
    // DNS servers given here replace the ones the network announces.
    arguments << QStringLiteral("ipv4.dns") << dns4
              << QStringLiteral("ipv4.ignore-auto-dns") << (dns4.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"));
    if (method6 == QLatin1String("manual")) {
        arguments << QStringLiteral("ipv6.addresses") << splitList(text(settings, "addresses6")).join(QLatin1Char(','))
                  << QStringLiteral("ipv6.method") << QStringLiteral("manual")
                  << QStringLiteral("ipv6.gateway") << text(settings, "gateway6");
    } else {
        arguments << QStringLiteral("ipv6.method") << method6
                  << QStringLiteral("ipv6.gateway") << QString()
                  << QStringLiteral("ipv6.addresses") << QString();
    }
    arguments << QStringLiteral("ipv6.dns") << dns6
              << QStringLiteral("ipv6.ignore-auto-dns") << (dns6.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"));

    const bool live = row.value(QStringLiteral("stateCode")).toInt() >= 40 && row.value(QStringLiteral("stateCode")).toInt() <= 100;
    setBusy(true);
    Nm::run(this, arguments, [this, device, uuid, live](bool ok, const QString &, const QString &error) {
        if (!ok) {
            setBusy(false);
            Q_EMIT failed(tr("NetworkManager did not accept the settings: %1").arg(error));
            refresh();
            return;
        }
        if (!live) {
            setBusy(false);
            Q_EMIT ipApplied(device);
            refresh();
            return;
        }
        // A live interface keeps its old configuration until the profile is activated again.
        Nm::run(this, {QStringLiteral("-w"), QStringLiteral("30"), QStringLiteral("connection"), QStringLiteral("up"),
                       QStringLiteral("uuid"), uuid, QStringLiteral("ifname"), device},
                [this, device](bool up, const QString &, const QString &upError) {
            setBusy(false);
            if (up)
                Q_EMIT ipApplied(device);
            else
                Q_EMIT failed(tr("The settings were saved, but %1 could not be brought up with them: %2").arg(device, upError));
            refresh();
        }, 45000);
    });
}

// ---- proxy --------------------------------------------------------------------------------

QString NetworkBackend::proxyFile() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/environment.d/90-shadow-proxy.conf");
}

void NetworkBackend::loadProxyFile()
{
    QVariantMap proxy{
        {QStringLiteral("mode"), QStringLiteral("none")},
        {QStringLiteral("httpHost"), QString()}, {QStringLiteral("httpPort"), QString()},
        {QStringLiteral("httpsHost"), QString()}, {QStringLiteral("httpsPort"), QString()},
        {QStringLiteral("socksHost"), QString()}, {QStringLiteral("socksPort"), QString()},
        {QStringLiteral("noProxy"), QString()}, {QStringLiteral("pacUrl"), QString()},
    };
    QString problem;
    QFile file(proxyFile());
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            problem = tr("%1 exists but cannot be read.").arg(proxyFile());
        } else {
            QMap<QString, QString> values;
            const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
            for (const QString &raw : lines) {
                const QString line = raw.trimmed();
                const qsizetype equals = line.indexOf(QLatin1Char('='));
                if (line.startsWith(QLatin1Char('#')) || equals <= 0)
                    continue;
                values.insert(line.left(equals), line.mid(equals + 1));
            }
            const auto take = [&values, &proxy](const QString &variable, const QString &hostKey, const QString &portKey) {
                const QUrl url(values.value(variable));
                if (!url.isValid() || url.host().isEmpty())
                    return false;
                proxy.insert(hostKey, url.host());
                proxy.insert(portKey, url.port() > 0 ? QString::number(url.port()) : QString());
                return true;
            };
            const bool http = take(QStringLiteral("http_proxy"), QStringLiteral("httpHost"), QStringLiteral("httpPort"));
            const bool https = take(QStringLiteral("https_proxy"), QStringLiteral("httpsHost"), QStringLiteral("httpsPort"));
            const bool socks = take(QStringLiteral("all_proxy"), QStringLiteral("socksHost"), QStringLiteral("socksPort"));
            proxy.insert(QStringLiteral("noProxy"), values.value(QStringLiteral("no_proxy")));
            if (values.contains(QStringLiteral("auto_proxy"))) {
                proxy.insert(QStringLiteral("mode"), QStringLiteral("auto"));
                proxy.insert(QStringLiteral("pacUrl"), values.value(QStringLiteral("auto_proxy")));
            } else if (http || https || socks) {
                proxy.insert(QStringLiteral("mode"), QStringLiteral("manual"));
            }
        }
    }
    if (proxy != m_proxy || problem != m_proxyFileProblem) {
        m_proxy = proxy;
        m_proxyFileProblem = problem;
        Q_EMIT proxyChanged();
    }
}

QString NetworkBackend::proxyProblem(const QVariantMap &proxy) const
{
    const QString mode = text(proxy, "mode");
    if (mode == QLatin1String("none"))
        return QString();
    if (mode == QLatin1String("auto")) {
        if (!isPacUrl(text(proxy, "pacUrl")))
            return tr("Enter the address of the configuration script, starting with http://, https:// or file:///.");
        return QString();
    }
    if (mode != QLatin1String("manual"))
        return tr("Choose no proxy, a manual proxy or automatic configuration.");

    struct Entry
    {
        const char *hostKey;
        const char *portKey;
        QString label;
    };
    const Entry entries[] = {
        {"httpHost", "httpPort", tr("HTTP proxy")},
        {"httpsHost", "httpsPort", tr("HTTPS proxy")},
        {"socksHost", "socksPort", tr("SOCKS proxy")},
    };
    bool any = false;
    for (const Entry &entry : entries) {
        const QString host = text(proxy, entry.hostKey);
        const QString port = text(proxy, entry.portKey);
        if (host.isEmpty() && port.isEmpty())
            continue;
        any = true;
        if (host.contains(QLatin1Char('@')) || host.contains(QLatin1String("://")) || host.contains(QLatin1Char('/')))
            return tr("%1: enter only the host name or address — no scheme, path, user name or password.").arg(entry.label);
        if (!isHost(host))
            return tr("%1: “%2” is not a host name or IP address.").arg(entry.label, host);
        if (!isPort(port))
            return tr("%1: the port has to be a number from 1 to 65535.").arg(entry.label);
    }
    if (!any)
        return tr("Enter at least one proxy server, or choose “None”.");
    const QStringList exceptions = splitList(text(proxy, "noProxy"));
    for (const QString &exception : exceptions) {
        if (!isNoProxyEntry(exception))
            return tr("“%1” is not a host, domain or network that can be excluded from the proxy.").arg(exception);
    }
    return QString();
}

bool NetworkBackend::writeProxyFile(const QVariantMap &proxy, QString *error)
{
    const QString path = proxyFile();
    const QString mode = text(proxy, "mode");
    if (mode == QLatin1String("none")) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            *error = tr("%1 could not be removed.").arg(path);
            return false;
        }
        return true;
    }

    QStringList lines = {
        QStringLiteral("# Proxy settings of the session, written by the SHADOW Control Center (Wired & Proxy)."),
        QStringLiteral("# Read by systemd --user at login (environment.d(5)). Changes made here are overwritten."),
    };
    const auto both = [&lines](const QString &variable, const QString &value) {
        lines << variable + QLatin1Char('=') + value << variable.toUpper() + QLatin1Char('=') + value;
    };
    if (mode == QLatin1String("auto")) {
        lines << QStringLiteral("auto_proxy=") + text(proxy, "pacUrl");
    } else {
        if (!text(proxy, "httpHost").isEmpty())
            both(QStringLiteral("http_proxy"), proxyUrl(QStringLiteral("http"), text(proxy, "httpHost"), text(proxy, "httpPort")));
        if (!text(proxy, "httpsHost").isEmpty())
            both(QStringLiteral("https_proxy"), proxyUrl(QStringLiteral("http"), text(proxy, "httpsHost"), text(proxy, "httpsPort")));
        if (!text(proxy, "socksHost").isEmpty())
            both(QStringLiteral("all_proxy"), proxyUrl(QStringLiteral("socks5"), text(proxy, "socksHost"), text(proxy, "socksPort")));
        const QString exceptions = splitList(text(proxy, "noProxy")).join(QLatin1Char(','));
        if (!exceptions.isEmpty())
            both(QStringLiteral("no_proxy"), exceptions);
    }

    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = tr("The directory for %1 could not be created.").arg(path);
        return false;
    }
    QSaveFile file(path);
    const QByteArray content = lines.join(QLatin1Char('\n')).toUtf8() + '\n';
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        *error = tr("%1 could not be written.").arg(path);
        return false;
    }
    return true;
}

void NetworkBackend::applyProxy(const QVariantMap &proxy)
{
    const QString problem = proxyProblem(proxy);
    if (!problem.isEmpty()) {
        Q_EMIT failed(problem);
        return;
    }
    QString error;
    if (!writeProxyFile(proxy, &error)) {
        Q_EMIT failed(error);
        return;
    }
    loadProxyFile();

    const QString mode = text(proxy, "mode");
    const QString saved = mode == QLatin1String("none") ? tr("The proxy settings were removed.")
                                                         : tr("The proxy settings were saved.");
    applyProxyToProfiles(proxy, saved + QLatin1Char(' ')
                                    + tr("Applications that are already running keep their old settings: log out and "
                                         "back in for the change to reach everything."));
}

void NetworkBackend::applyProxyToProfiles(const QVariantMap &proxy, const QString &note)
{
    // NetworkManager knows "none" and "auto" (a PAC URL) only.
    const bool automatic = text(proxy, "mode") == QLatin1String("auto");
    m_proxyMethod = automatic ? QStringLiteral("auto") : QStringLiteral("none");
    m_proxyPacUrl = automatic ? text(proxy, "pacUrl") : QString();
    m_proxyNote = note;
    m_proxyFailures.clear();
    m_proxyQueue.clear();
    if (available()) {
        for (const ActiveProfile &profile : std::as_const(m_activeProfiles)) {
            if (profile.method != m_proxyMethod || profile.pacUrl != m_proxyPacUrl)
                m_proxyQueue.append(profile);
        }
    }
    setBusy(true);
    nextProxyProfile();
}

void NetworkBackend::nextProxyProfile()
{
    if (m_proxyQueue.isEmpty()) {
        if (!m_proxyFailures.isEmpty()) {
            Q_EMIT failed(tr("NetworkManager did not take over the proxy setting for %1.")
                              .arg(m_proxyFailures.join(QStringLiteral("; "))));
        }
        setBusy(false);
        Q_EMIT proxyApplied(m_proxyNote);
        refresh();
        return;
    }
    const ActiveProfile profile = m_proxyQueue.takeFirst();
    Nm::run(this, {QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("uuid"), profile.uuid,
                   QStringLiteral("proxy.method"), m_proxyMethod, QStringLiteral("proxy.pac-url"), m_proxyPacUrl},
            [this, profile](bool ok, const QString &, const QString &error) {
        if (!ok)
            m_proxyFailures.append(profile.name + QStringLiteral(": ") + error);
        nextProxyProfile();
    });
}

QJsonObject NetworkBackend::toJson() const
{
    QJsonArray profiles;
    for (const ActiveProfile &profile : m_activeProfiles) {
        profiles.append(QJsonObject{
            {QStringLiteral("name"), profile.name},
            {QStringLiteral("uuid"), profile.uuid},
            {QStringLiteral("proxyMethod"), profile.method},
            {QStringLiteral("pacUrl"), profile.pacUrl},
        });
    }
    return {
        {QStringLiteral("state"), m_state},
        {QStringLiteral("available"), available()},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("devices"), QJsonArray::fromVariantList(m_devices->rows())},
        {QStringLiteral("proxy"), QJsonObject::fromVariantMap(m_proxy)},
        {QStringLiteral("proxyFile"), proxyFile()},
        {QStringLiteral("proxyFileProblem"), m_proxyFileProblem},
        {QStringLiteral("activeProfiles"), profiles},
    };
}
