// SPDX-License-Identifier: GPL-3.0-or-later
#include "netstate.h"

#include "netutil.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <arpa/inet.h>
#include <csignal>
#include <memory>
#include <sys/prctl.h>
#include <sys/socket.h>

namespace {

const char *kResolveService = "org.freedesktop.resolve1";
const char *kResolvePath = "/org/freedesktop/resolve1";
const char *kPropertiesInterface = "org.freedesktop.DBus.Properties";

QString sysRead(const QString &interface, const char *file)
{
    QFile f(QStringLiteral("/sys/class/net/%1/%2").arg(interface, QString::fromLatin1(file)));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    // Attributes such as "speed" exist for every interface but fail with
    // EINVAL when the driver cannot answer; that reads as an empty string.
    return QString::fromLatin1(f.read(256)).trimmed();
}

QStringList toStringList(const QJsonValue &v)
{
    QStringList out;
    const QJsonArray a = v.toArray();
    for (const QJsonValue &e : a)
        out << e.toString();
    return out;
}

// Human-readable kind of an interface and a coarse category for its icon.
void classify(QVariantMap &m, bool hasDevice)
{
    const QString linkType = m.value(QStringLiteral("linkType")).toString();
    const QString kind = m.value(QStringLiteral("kind")).toString();
    QString label, category = QStringLiteral("other");

    if (linkType == QLatin1String("loopback")) {
        label = QObject::tr("Loopback");
        category = QStringLiteral("loopback");
    } else if (m.value(QStringLiteral("wireless")).toBool()) {
        label = QObject::tr("Wi-Fi");
        category = QStringLiteral("wifi");
    } else if (!kind.isEmpty()) {
        static const QHash<QString, QString> names = {
            {QStringLiteral("bridge"), QStringLiteral("Bridge")},
            {QStringLiteral("bond"), QStringLiteral("Bond")},
            {QStringLiteral("team"), QStringLiteral("Team")},
            {QStringLiteral("vlan"), QStringLiteral("VLAN")},
            {QStringLiteral("veth"), QStringLiteral("Virtual Ethernet pair (veth)")},
            {QStringLiteral("wireguard"), QStringLiteral("WireGuard tunnel")},
            {QStringLiteral("vxlan"), QStringLiteral("VXLAN")},
            {QStringLiteral("geneve"), QStringLiteral("Geneve")},
            {QStringLiteral("dummy"), QStringLiteral("Dummy")},
            {QStringLiteral("ifb"), QStringLiteral("Intermediate functional block (ifb)")},
            {QStringLiteral("macvlan"), QStringLiteral("MACVLAN")},
            {QStringLiteral("macvtap"), QStringLiteral("MACVTAP")},
            {QStringLiteral("ipvlan"), QStringLiteral("IPVLAN")},
            {QStringLiteral("vrf"), QStringLiteral("VRF")},
        };
        if (kind == QLatin1String("tun")) {
            label = m.value(QStringLiteral("tunType")).toString() == QLatin1String("tap")
                ? QObject::tr("TAP device") : QObject::tr("TUN device");
        } else if (names.contains(kind)) {
            label = names.value(kind);
        } else {
            label = QObject::tr("Virtual (%1)").arg(kind);
        }
        static const QSet<QString> tunnels = {
            QStringLiteral("wireguard"), QStringLiteral("tun"), QStringLiteral("gre"), QStringLiteral("gretap"),
            QStringLiteral("ip6gre"), QStringLiteral("ipip"), QStringLiteral("sit"), QStringLiteral("vti"),
            QStringLiteral("vti6"), QStringLiteral("ip6tnl"), QStringLiteral("xfrm"), QStringLiteral("ppp"),
        };
        category = tunnels.contains(kind) ? QStringLiteral("tunnel") : QStringLiteral("virtual");
    } else if (linkType == QLatin1String("ether")) {
        label = hasDevice ? QObject::tr("Ethernet") : QObject::tr("Ethernet (virtual)");
        category = hasDevice ? QStringLiteral("ethernet") : QStringLiteral("virtual");
    } else if (linkType == QLatin1String("none")) {
        label = QObject::tr("Point-to-point");
        category = QStringLiteral("tunnel");
    } else if (linkType == QLatin1String("ppp")) {
        label = QStringLiteral("PPP");
        category = QStringLiteral("tunnel");
    } else {
        label = linkType.isEmpty() ? QObject::tr("Unknown") : linkType;
    }
    m.insert(QStringLiteral("typeLabel"), label);
    m.insert(QStringLiteral("category"), category);
}

QString ipFromBytes(int family, const QByteArray &bytes)
{
    char buffer[INET6_ADDRSTRLEN] = {};
    if (family == AF_INET && bytes.size() == 4 && inet_ntop(AF_INET, bytes.constData(), buffer, sizeof buffer))
        return QString::fromLatin1(buffer);
    if (family == AF_INET6 && bytes.size() == 16 && inet_ntop(AF_INET6, bytes.constData(), buffer, sizeof buffer))
        return QString::fromLatin1(buffer);
    return {};
}

// a(iiay) (manager) or a(iay) (link): [ifindex,] family, address bytes.
QVariantList resolvedServers(const QVariant &v, bool hasIfindex)
{
    QVariantList out;
    if (!v.canConvert<QDBusArgument>())
        return out;
    const QDBusArgument arg = v.value<QDBusArgument>();
    if (arg.currentType() != QDBusArgument::ArrayType)
        return out;
    arg.beginArray();
    while (!arg.atEnd()) {
        int ifindex = 0, family = 0;
        QByteArray bytes;
        arg.beginStructure();
        if (hasIfindex)
            arg >> ifindex;
        arg >> family >> bytes;
        arg.endStructure();
        const QString address = ipFromBytes(family, bytes);
        if (!address.isEmpty())
            out << QVariantMap{{QStringLiteral("ifindex"), ifindex}, {QStringLiteral("address"), address}};
    }
    arg.endArray();
    return out;
}

// (iiay) (manager) or (iay) (link).
QString resolvedServer(const QVariant &v, bool hasIfindex)
{
    if (!v.canConvert<QDBusArgument>())
        return {};
    const QDBusArgument arg = v.value<QDBusArgument>();
    if (arg.currentType() != QDBusArgument::StructureType)
        return {};
    int ifindex = 0, family = 0;
    QByteArray bytes;
    arg.beginStructure();
    if (hasIfindex)
        arg >> ifindex;
    arg >> family >> bytes;
    arg.endStructure();
    return ipFromBytes(family, bytes);
}

// a(isb) (manager) or a(sb) (link): [ifindex,] domain, route-only.
QVariantList resolvedDomains(const QVariant &v, bool hasIfindex)
{
    QVariantList out;
    if (!v.canConvert<QDBusArgument>())
        return out;
    const QDBusArgument arg = v.value<QDBusArgument>();
    if (arg.currentType() != QDBusArgument::ArrayType)
        return out;
    arg.beginArray();
    while (!arg.atEnd()) {
        int ifindex = 0;
        QString domain;
        bool routeOnly = false;
        arg.beginStructure();
        if (hasIfindex)
            arg >> ifindex;
        arg >> domain >> routeOnly;
        arg.endStructure();
        out << QVariantMap{{QStringLiteral("ifindex"), ifindex}, {QStringLiteral("domain"), domain},
                           {QStringLiteral("routeOnly"), routeOnly}};
    }
    arg.endArray();
    return out;
}

QVariantMap readResolvConf()
{
    const QString path = QStringLiteral("/etc/resolv.conf");
    QVariantMap out;
    out.insert(QStringLiteral("path"), path);
    const QFileInfo info(path);
    out.insert(QStringLiteral("exists"), info.exists());
    const QString target = info.isSymLink() ? info.canonicalFilePath() : QString();
    out.insert(QStringLiteral("linkTarget"), target);

    QStringList nameservers, search, options;
    QString comments;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(f.read(64 * 1024)).split(QLatin1Char('\n'));
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty())
                continue;
            if (line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'))) {
                comments += line + QLatin1Char('\n');
                continue;
            }
            const QStringList words = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (words.size() < 2)
                continue;
            const QString &keyword = words.at(0);
            if (keyword == QLatin1String("nameserver")) {
                nameservers << words.at(1);
            } else if (keyword == QLatin1String("search") || keyword == QLatin1String("domain")) {
                // The last "search"/"domain" line wins, as in the C library.
                search = words.mid(1);
            } else if (keyword == QLatin1String("options")) {
                options << words.mid(1);
            }
        }
    }
    out.insert(QStringLiteral("nameservers"), nameservers);
    out.insert(QStringLiteral("search"), search);
    out.insert(QStringLiteral("options"), options);

    QString managedBy;
    if (!info.exists()) {
        managedBy = QObject::tr("missing");
    } else if (target.endsWith(QLatin1String("/systemd/resolve/stub-resolv.conf"))) {
        managedBy = QObject::tr("systemd-resolved (local stub resolver)");
    } else if (target.contains(QLatin1String("/systemd/resolve/"))) {
        managedBy = QObject::tr("systemd-resolved (upstream servers)");
    } else if (target.contains(QLatin1String("/NetworkManager/")) || comments.contains(QLatin1String("NetworkManager"))) {
        managedBy = QStringLiteral("NetworkManager");
    } else if (comments.contains(QLatin1String("systemd-resolved"))) {
        managedBy = QStringLiteral("systemd-resolved");
    } else if (target.contains(QLatin1String("/resolvconf/")) || comments.contains(QLatin1String("resolvconf"))) {
        managedBy = QStringLiteral("resolvconf");
    } else if (comments.contains(QLatin1String("dhclient")) || comments.contains(QLatin1String("dhcpcd"))) {
        managedBy = QObject::tr("DHCP client");
    } else {
        managedBy = QObject::tr("static file (not generated by a service)");
    }
    out.insert(QStringLiteral("managedBy"), managedBy);
    return out;
}

} // namespace

NetState::NetState(QObject *parent)
    : QObject(parent)
{
    m_sampleTimer.setInterval(1000);
    connect(&m_sampleTimer, &QTimer::timeout, this, &NetState::sampleCounters);
    m_slowTimer.setInterval(10000);
    connect(&m_slowTimer, &QTimer::timeout, this, &NetState::refresh);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &NetState::refresh);
}

NetState::~NetState()
{
    stopMonitor();
}

void NetState::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active) {
        refresh();
        m_sampleClock.invalidate();
        sampleCounters();
        m_sampleTimer.start();
        m_slowTimer.start();
        startMonitor();
    } else {
        m_sampleTimer.stop();
        m_slowTimer.stop();
        m_debounce.stop();
        stopMonitor();
    }
    Q_EMIT activeChanged();
}

void NetState::startMonitor()
{
    if (m_monitor)
        return;
    const QString ip = Net::findTool(QStringLiteral("ip"));
    if (ip.isEmpty())
        return;
    // `ip monitor` prints one line per netlink event. Only the fact that
    // something changed is used; the state itself is re-read as JSON.
    m_monitor = new QProcess(this);
    m_monitor->setProcessEnvironment(Shadow::Command::parseEnvironment());
    m_monitor->setProgram(ip);
    m_monitor->setArguments({QStringLiteral("monitor"), QStringLiteral("link"), QStringLiteral("address"),
                             QStringLiteral("route")});
    m_monitor->setStandardInputFile(QProcess::nullDevice());
    m_monitor->setStandardErrorFile(QProcess::nullDevice());
    m_monitor->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
    connect(m_monitor, &QProcess::readyReadStandardOutput, this, [this] {
        m_monitor->readAllStandardOutput();
        if (!m_debounce.isActive())
            m_debounce.start();
    });
    m_monitor->start();
}

void NetState::stopMonitor()
{
    if (!m_monitor)
        return;
    m_monitor->disconnect(this);
    if (m_monitor->state() != QProcess::NotRunning) {
        m_monitor->kill();
        m_monitor->waitForFinished(1000);
    }
    delete m_monitor;
    m_monitor = nullptr;
}

void NetState::refresh()
{
    if (m_refreshing) {
        m_refreshQueued = true;
        return;
    }
    m_refreshing = true;
    m_pending = 4;

    const QString ip = Net::findTool(QStringLiteral("ip"));
    const QString program = ip.isEmpty() ? QStringLiteral("ip") : ip;
    const QString j = QStringLiteral("-j"), details = QStringLiteral("-details");
    Shadow::Command::run(this, program, {j, QStringLiteral("-s"), details, QStringLiteral("address"), QStringLiteral("show")},
                        [this](const Shadow::CommandResult &r) { m_addrResult = r; stepDone(); }, 10000);
    Shadow::Command::run(this, program, {j, QStringLiteral("-4"), details, QStringLiteral("route"), QStringLiteral("show"),
                                        QStringLiteral("table"), QStringLiteral("all")},
                        [this](const Shadow::CommandResult &r) { m_route4Result = r; stepDone(); }, 10000);
    Shadow::Command::run(this, program, {j, QStringLiteral("-6"), details, QStringLiteral("route"), QStringLiteral("show"),
                                        QStringLiteral("table"), QStringLiteral("all")},
                        [this](const Shadow::CommandResult &r) { m_route6Result = r; stepDone(); }, 10000);
    Shadow::Command::run(this, program, {j, QStringLiteral("neigh"), QStringLiteral("show")},
                        [this](const Shadow::CommandResult &r) { m_neighResult = r; stepDone(); }, 10000);
}

void NetState::stepDone()
{
    if (--m_pending > 0)
        return;
    // Always finish from the event loop so refreshed() is never emitted from
    // inside refresh(), whatever the process layer does.
    QMetaObject::invokeMethod(this, [this] { finishRefresh(); }, Qt::QueuedConnection);
}

void NetState::finishRefresh()
{
    QString error;
    if (m_addrResult.exitCode == -1) {
        error = tr("The “ip” tool could not be started. Install the iproute2 package.");
    } else if (!m_addrResult.ok()) {
        error = tr("“ip address show” failed: %1").arg(Net::cleanMessage(m_addrResult.errText()));
    }
    if (error != m_error) {
        m_error = error;
        Q_EMIT errorChanged();
    }

    parseInterfaces(m_addrResult.ok() ? m_addrResult.out : QByteArray());
    // An address family the kernel does not provide (IPv6 disabled at boot)
    // makes `ip -6 route` fail; that simply means there are no such routes.
    parseRoutes(m_route4Result.ok() ? m_route4Result.out : QByteArray(),
                m_route6Result.ok() ? m_route6Result.out : QByteArray());
    parseNeighbours(m_neighResult.ok() ? m_neighResult.out : QByteArray());
    computeSummary();
    readRfkill();
    refreshDns();
}

void NetState::parseInterfaces(const QByteArray &json)
{
    QVariantList compact, full;
    const QJsonArray array = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &value : array) {
        const QJsonObject o = value.toObject();
        const QString name = o.value(QLatin1String("ifname")).toString();
        if (name.isEmpty())
            continue;
        const QStringList flags = toStringList(o.value(QLatin1String("flags")));
        const QJsonObject linkinfo = o.value(QLatin1String("linkinfo")).toObject();

        QVariantMap m;
        m.insert(QStringLiteral("name"), name);
        m.insert(QStringLiteral("ifindex"), o.value(QLatin1String("ifindex")).toInt());
        m.insert(QStringLiteral("flags"), flags);
        m.insert(QStringLiteral("linkType"), o.value(QLatin1String("link_type")).toString());
        m.insert(QStringLiteral("kind"), linkinfo.value(QLatin1String("info_kind")).toString());
        m.insert(QStringLiteral("tunType"),
                 linkinfo.value(QLatin1String("info_data")).toObject().value(QLatin1String("type")).toString());
        m.insert(QStringLiteral("master"), o.value(QLatin1String("master")).toString());
        m.insert(QStringLiteral("peer"), o.value(QLatin1String("link")).toString());
        m.insert(QStringLiteral("operstate"), o.value(QLatin1String("operstate")).toString());
        m.insert(QStringLiteral("mtu"), o.value(QLatin1String("mtu")).toInt());
        const QString mac = o.value(QLatin1String("address")).toString();
        m.insert(QStringLiteral("mac"), mac == QLatin1String("00:00:00:00:00:00") ? QString() : mac);
        m.insert(QStringLiteral("bus"), o.value(QLatin1String("parentbus")).toString());
        m.insert(QStringLiteral("busDevice"), o.value(QLatin1String("parentdev")).toString());

        const bool adminUp = flags.contains(QLatin1String("UP"));
        const bool lowerUp = flags.contains(QLatin1String("LOWER_UP"));
        const QString operstate = m.value(QStringLiteral("operstate")).toString();
        m.insert(QStringLiteral("adminUp"), adminUp);
        m.insert(QStringLiteral("carrier"), lowerUp);
        // "status", not "state": QML delegates get the roles as properties and
        // every Item already has a "state".
        m.insert(QStringLiteral("status"), !adminUp ? QStringLiteral("down")
                 : lowerUp ? QStringLiteral("up")
                 : operstate == QLatin1String("DORMANT") ? QStringLiteral("dormant") : QStringLiteral("no-carrier"));

        // sysfs: what the driver reports about the physical link.
        bool ok = false;
        const int speed = sysRead(name, "speed").toInt(&ok);
        m.insert(QStringLiteral("speedMbps"), ok && speed > 0 && speed < 1000000 ? speed : -1);
        const QString duplex = sysRead(name, "duplex");
        m.insert(QStringLiteral("duplex"), duplex == QLatin1String("full") || duplex == QLatin1String("half") ? duplex : QString());
        const QString sysPath = QStringLiteral("/sys/class/net/") + name;
        const bool hasDevice = QFileInfo::exists(sysPath + QStringLiteral("/device"));
        QString driver = QFileInfo(sysPath + QStringLiteral("/device/driver")).canonicalFilePath().section(QLatin1Char('/'), -1);
        if (driver.isEmpty())
            driver = m.value(QStringLiteral("kind")).toString();
        m.insert(QStringLiteral("driver"), driver);
        m.insert(QStringLiteral("physical"), hasDevice);
        m.insert(QStringLiteral("wireless"), QFileInfo::exists(sysPath + QStringLiteral("/wireless"))
                                                 || QFileInfo::exists(sysPath + QStringLiteral("/phy80211")));
        classify(m, hasDevice);

        QVariantList addresses, addressesFull;
        const QJsonArray addrInfo = o.value(QLatin1String("addr_info")).toArray();
        for (const QJsonValue &av : addrInfo) {
            const QJsonObject a = av.toObject();
            const QString family = a.value(QLatin1String("family")).toString();
            if (family != QLatin1String("inet") && family != QLatin1String("inet6"))
                continue;
            const QString local = a.value(QLatin1String("local")).toString();
            const int prefix = a.value(QLatin1String("prefixlen")).toInt();
            QVariantMap am;
            am.insert(QStringLiteral("interface"), name);
            am.insert(QStringLiteral("family"), family == QLatin1String("inet") ? QStringLiteral("IPv4") : QStringLiteral("IPv6"));
            am.insert(QStringLiteral("address"), local);
            am.insert(QStringLiteral("prefixLength"), prefix);
            am.insert(QStringLiteral("cidr"), QStringLiteral("%1/%2").arg(local).arg(prefix));
            am.insert(QStringLiteral("scope"), a.value(QLatin1String("scope")).toString());
            am.insert(QStringLiteral("label"), a.value(QLatin1String("label")).toString());
            am.insert(QStringLiteral("broadcast"), a.value(QLatin1String("broadcast")).toString());
            // On point-to-point links "address" is the peer.
            am.insert(QStringLiteral("peer"), a.value(QLatin1String("address")).toString());
            QStringList addrFlags;
            for (const char *flag : {"dynamic", "temporary", "deprecated", "tentative", "dadfailed", "secondary",
                                     "mngtmpaddr", "noprefixroute", "permanent"}) {
                if (a.value(QLatin1String(flag)).toBool())
                    addrFlags << QString::fromLatin1(flag);
            }
            am.insert(QStringLiteral("flags"), addrFlags);
            am.insert(QStringLiteral("dynamic"), addrFlags.contains(QLatin1String("dynamic")));
            addresses << am;

            // 0xffffffff is the kernel's "forever".
            const qint64 valid = qint64(a.value(QLatin1String("valid_life_time")).toDouble(-1));
            const qint64 preferred = qint64(a.value(QLatin1String("preferred_life_time")).toDouble(-1));
            am.insert(QStringLiteral("validLifetime"), valid >= 4294967295LL ? -1 : valid);
            am.insert(QStringLiteral("preferredLifetime"), preferred >= 4294967295LL ? -1 : preferred);
            addressesFull << am;
        }
        QVariantMap fullMap = m;
        m.insert(QStringLiteral("addresses"), addresses);
        fullMap.insert(QStringLiteral("addresses"), addressesFull);

        const QJsonObject stats = o.value(QLatin1String("stats64")).toObject();
        const QJsonObject rx = stats.value(QLatin1String("rx")).toObject();
        const QJsonObject tx = stats.value(QLatin1String("tx")).toObject();
        auto number = [](const QJsonObject &obj, const char *key) {
            return qint64(obj.value(QLatin1String(key)).toDouble());
        };
        fullMap.insert(QStringLiteral("rxBytes"), number(rx, "bytes"));
        fullMap.insert(QStringLiteral("rxPackets"), number(rx, "packets"));
        fullMap.insert(QStringLiteral("rxErrors"), number(rx, "errors"));
        fullMap.insert(QStringLiteral("rxDropped"), number(rx, "dropped"));
        fullMap.insert(QStringLiteral("txBytes"), number(tx, "bytes"));
        fullMap.insert(QStringLiteral("txPackets"), number(tx, "packets"));
        fullMap.insert(QStringLiteral("txErrors"), number(tx, "errors"));
        fullMap.insert(QStringLiteral("txDropped"), number(tx, "dropped"));

        compact << m;
        full << fullMap;
    }
    m_interfacesFull = full;
    if (compact != m_interfaces) {
        m_interfaces = compact;
        Q_EMIT interfacesChanged();
    }
}

void NetState::parseRoutes(const QByteArray &json4, const QByteArray &json6)
{
    QVariantList routes;
    int order = 0;
    auto parse = [&routes, &order](const QByteArray &json, const QString &family) {
        const QJsonArray array = QJsonDocument::fromJson(json).array();
        for (const QJsonValue &value : array) {
            const QJsonObject o = value.toObject();
            QVariantMap m;
            m.insert(QStringLiteral("order"), order++);
            m.insert(QStringLiteral("family"), family);
            m.insert(QStringLiteral("destination"), o.value(QLatin1String("dst")).toString());

            QString gateway = o.value(QLatin1String("gateway")).toString();
            QString device = o.value(QLatin1String("dev")).toString();
            if (gateway.isEmpty() && o.contains(QLatin1String("via")))      // next hop of the other family
                gateway = o.value(QLatin1String("via")).toObject().value(QLatin1String("host")).toString();
            const QJsonArray nexthops = o.value(QLatin1String("nexthops")).toArray();
            if (!nexthops.isEmpty()) {                                       // multipath route
                QStringList gateways, devices;
                for (const QJsonValue &nh : nexthops) {
                    const QJsonObject n = nh.toObject();
                    const QString g = n.value(QLatin1String("gateway")).toString();
                    const QString d = n.value(QLatin1String("dev")).toString();
                    if (!g.isEmpty())
                        gateways << g;
                    if (!d.isEmpty() && !devices.contains(d))
                        devices << d;
                }
                gateway = gateways.join(QStringLiteral(", "));
                device = devices.join(QStringLiteral(", "));
            }
            m.insert(QStringLiteral("gateway"), gateway);
            m.insert(QStringLiteral("device"), device);
            m.insert(QStringLiteral("metric"), o.value(QLatin1String("metric")).toInt());
            // The protocol and table are names from /etc/iproute2 or plain numbers.
            const QJsonValue protocol = o.value(QLatin1String("protocol"));
            m.insert(QStringLiteral("protocol"), protocol.isDouble() ? QString::number(protocol.toInt()) : protocol.toString());
            const QJsonValue scope = o.value(QLatin1String("scope"));
            m.insert(QStringLiteral("scope"), scope.isUndefined() ? QStringLiteral("global")
                     : scope.isDouble() ? QString::number(scope.toInt()) : scope.toString());
            const QJsonValue table = o.value(QLatin1String("table"));
            m.insert(QStringLiteral("table"), table.isUndefined() ? QStringLiteral("main")
                     : table.isDouble() ? QString::number(table.toInt()) : table.toString());
            const QString type = o.value(QLatin1String("type")).toString();
            m.insert(QStringLiteral("type"), type.isEmpty() ? QStringLiteral("unicast") : type);
            m.insert(QStringLiteral("source"), o.value(QLatin1String("prefsrc")).toString());
            m.insert(QStringLiteral("flags"), toStringList(o.value(QLatin1String("flags"))).join(QLatin1Char(' ')));
            routes << m;
        }
    };
    parse(json4, QStringLiteral("IPv4"));
    parse(json6, QStringLiteral("IPv6"));

    // The default route in use: lowest metric, the main table first.
    auto pickDefault = [&routes](const QString &family) {
        QVariantMap best;
        bool bestMain = false;
        for (const QVariant &v : std::as_const(routes)) {
            const QVariantMap r = v.toMap();
            if (r.value(QStringLiteral("family")) != family || r.value(QStringLiteral("destination")) != QLatin1String("default")
                || r.value(QStringLiteral("type")) != QLatin1String("unicast"))
                continue;
            const bool isMain = r.value(QStringLiteral("table")) == QLatin1String("main");
            if (best.isEmpty() || (isMain && !bestMain)
                || (isMain == bestMain && r.value(QStringLiteral("metric")).toInt() < best.value(QStringLiteral("metric")).toInt())) {
                best = r;
                bestMain = isMain;
            }
        }
        return best;
    };
    const QVariantMap default4 = pickDefault(QStringLiteral("IPv4"));
    const QVariantMap default6 = pickDefault(QStringLiteral("IPv6"));
    if (routes != m_routes || default4 != m_default4 || default6 != m_default6) {
        m_routes = routes;
        m_default4 = default4;
        m_default6 = default6;
        Q_EMIT routesChanged();
    }
}

void NetState::parseNeighbours(const QByteArray &json)
{
    QVariantList neighbours;
    const QJsonArray array = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &value : array) {
        const QJsonObject o = value.toObject();
        const QString address = o.value(QLatin1String("dst")).toString();
        QVariantMap m;
        m.insert(QStringLiteral("address"), address);
        m.insert(QStringLiteral("family"), address.contains(QLatin1Char(':')) ? QStringLiteral("IPv6") : QStringLiteral("IPv4"));
        m.insert(QStringLiteral("device"), o.value(QLatin1String("dev")).toString());
        m.insert(QStringLiteral("mac"), o.value(QLatin1String("lladdr")).toString());
        m.insert(QStringLiteral("state"), toStringList(o.value(QLatin1String("state"))).join(QLatin1Char(' ')));
        m.insert(QStringLiteral("router"), o.contains(QLatin1String("router")));
        neighbours << m;
    }
    if (neighbours != m_neighbours) {
        m_neighbours = neighbours;
        Q_EMIT neighboursChanged();
    }
}

void NetState::computeSummary()
{
    int total = 0, up = 0;
    bool anyGlobalAddress = false;
    QString firstUsable;
    for (const QVariant &v : std::as_const(m_interfaces)) {
        const QVariantMap i = v.toMap();
        if (i.value(QStringLiteral("category")) == QLatin1String("loopback"))
            continue;
        ++total;
        if (i.value(QStringLiteral("status")) != QLatin1String("up"))
            continue;
        ++up;
        const QVariantList addresses = i.value(QStringLiteral("addresses")).toList();
        for (const QVariant &av : addresses) {
            if (av.toMap().value(QStringLiteral("scope")) == QLatin1String("global")) {
                anyGlobalAddress = true;
                if (firstUsable.isEmpty())
                    firstUsable = i.value(QStringLiteral("name")).toString();
            }
        }
    }

    QString primary = m_default4.value(QStringLiteral("device")).toString();
    if (primary.isEmpty())
        primary = m_default6.value(QStringLiteral("device")).toString();
    if (primary.isEmpty())
        primary = firstUsable;
    // A multipath default route names several devices; use the first.
    primary = primary.section(QLatin1Char(','), 0, 0).trimmed();

    QString primary4, primary6;
    const QVariantList addresses = interfaceByName(primary).value(QStringLiteral("addresses")).toList();
    const QString preferred4 = m_default4.value(QStringLiteral("source")).toString();
    for (const QVariant &av : addresses) {
        const QVariantMap a = av.toMap();
        if (a.value(QStringLiteral("scope")) != QLatin1String("global"))
            continue;
        const QStringList flags = a.value(QStringLiteral("flags")).toStringList();
        if (a.value(QStringLiteral("family")) == QLatin1String("IPv4")) {
            if (primary4.isEmpty() || a.value(QStringLiteral("address")) == preferred4)
                primary4 = a.value(QStringLiteral("cidr")).toString();
        } else if (primary6.isEmpty() && !flags.contains(QLatin1String("deprecated"))
                   && !flags.contains(QLatin1String("tentative"))) {
            primary6 = a.value(QStringLiteral("cidr")).toString();
        }
    }

    const QString linkState = total == 0 ? QStringLiteral("no-adapter")
        : up == 0 ? QStringLiteral("no-link")
        : !anyGlobalAddress ? QStringLiteral("no-address")
        : (!m_default4.isEmpty() || !m_default6.isEmpty()) ? QStringLiteral("connected") : QStringLiteral("local");

    if (total != m_total || up != m_up || primary != m_primaryInterface || primary4 != m_primary4
        || primary6 != m_primary6 || linkState != m_linkState) {
        m_total = total;
        m_up = up;
        m_primaryInterface = primary;
        m_primary4 = primary4;
        m_primary6 = primary6;
        m_linkState = linkState;
        Q_EMIT summaryChanged();
    }
}

void NetState::readRfkill()
{
    // Stable sysfs ABI: Documentation/ABI/stable/sysfs-class-rfkill.
    QVariantList switches;
    const QDir dir(QStringLiteral("/sys/class/rfkill"));
    const QStringList entries = dir.entryList({QStringLiteral("rfkill*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        auto read = [&dir, &entry](const char *file) {
            QFile f(dir.filePath(entry + QLatin1Char('/') + QString::fromLatin1(file)));
            return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.read(128)).trimmed() : QString();
        };
        QVariantMap m;
        m.insert(QStringLiteral("id"), entry);
        m.insert(QStringLiteral("name"), read("name"));
        m.insert(QStringLiteral("type"), read("type"));
        m.insert(QStringLiteral("softBlocked"), read("soft") == QLatin1String("1"));
        m.insert(QStringLiteral("hardBlocked"), read("hard") == QLatin1String("1"));
        switches << m;
    }
    if (switches != m_rfkill) {
        m_rfkill = switches;
        Q_EMIT rfkillChanged();
    }
}

QVariantList NetState::wirelessInterfaces() const
{
    QVariantList out;
    for (const QVariant &v : m_interfaces) {
        if (v.toMap().value(QStringLiteral("wireless")).toBool())
            out << v;
    }
    return out;
}

QVariantMap NetState::interfaceByName(const QString &name) const
{
    for (const QVariant &v : m_interfaces) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("name")) == name)
            return m;
    }
    return {};
}

// ---- DNS ---------------------------------------------------------------------

void NetState::refreshDns()
{
    queryResolved();
}

void NetState::queryResolved()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        finishDns({});
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(kResolveService), QString::fromLatin1(kResolvePath),
                                                       QString::fromLatin1(kPropertiesInterface), QStringLiteral("GetAll"));
    call << QStringLiteral("org.freedesktop.resolve1.Manager");
    call.setAutoStartService(false);     // only talk to a resolver that is already running
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError()) {
            finishDns({});
            return;
        }
        const QVariantMap p = reply.value();
        QVariantMap resolved;
        resolved.insert(QStringLiteral("running"), true);
        resolved.insert(QStringLiteral("dnsOverTls"), p.value(QStringLiteral("DNSOverTLS")).toString());
        resolved.insert(QStringLiteral("dnssec"), p.value(QStringLiteral("DNSSEC")).toString());
        resolved.insert(QStringLiteral("resolvConfMode"), p.value(QStringLiteral("ResolvConfMode")).toString());
        resolved.insert(QStringLiteral("llmnr"), p.value(QStringLiteral("LLMNR")).toString());
        resolved.insert(QStringLiteral("multicastDns"), p.value(QStringLiteral("MulticastDNS")).toString());
        resolved.insert(QStringLiteral("stubListener"), p.value(QStringLiteral("DNSStubListener")).toString());
        resolved.insert(QStringLiteral("currentServer"), resolvedServer(p.value(QStringLiteral("CurrentDNSServer")), true));
        resolved.insert(QStringLiteral("servers"), resolvedServers(p.value(QStringLiteral("DNS")), true));
        resolved.insert(QStringLiteral("fallbackServers"), resolvedServers(p.value(QStringLiteral("FallbackDNS")), true));
        resolved.insert(QStringLiteral("domains"), resolvedDomains(p.value(QStringLiteral("Domains")), true));
        queryResolvedLinks(resolved);
    });
}

void NetState::queryResolvedLinks(QVariantMap resolved)
{
    // Per-link settings (DNS-over-TLS, current server) live on link objects.
    QList<int> indexes;
    const QVariantList servers = resolved.value(QStringLiteral("servers")).toList();
    const QVariantList domains = resolved.value(QStringLiteral("domains")).toList();
    for (const QVariantList &list : {servers, domains}) {
        for (const QVariant &v : list) {
            const int ifindex = v.toMap().value(QStringLiteral("ifindex")).toInt();
            if (ifindex > 0 && !indexes.contains(ifindex))
                indexes << ifindex;
        }
    }
    if (indexes.isEmpty()) {
        finishDns(resolved);
        return;
    }

    struct Pending
    {
        QVariantMap resolved;
        QVariantMap links;      // ifindex (as string) → properties
        int outstanding = 0;
    };
    auto pending = std::make_shared<Pending>();
    pending->resolved = resolved;
    pending->outstanding = int(indexes.size());
    auto done = [this, pending] {
        if (--pending->outstanding > 0)
            return;
        pending->resolved.insert(QStringLiteral("linkProperties"), pending->links);
        finishDns(pending->resolved);
    };

    QDBusConnection bus = QDBusConnection::systemBus();
    for (const int ifindex : std::as_const(indexes)) {
        QDBusMessage getLink = QDBusMessage::createMethodCall(QString::fromLatin1(kResolveService), QString::fromLatin1(kResolvePath),
                                                              QStringLiteral("org.freedesktop.resolve1.Manager"),
                                                              QStringLiteral("GetLink"));
        getLink << ifindex;
        getLink.setAutoStartService(false);
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(getLink, 3000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, pending, done, ifindex](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            const QDBusPendingReply<QDBusObjectPath> reply = *w;
            if (reply.isError()) {
                done();
                return;
            }
            QDBusMessage getAll = QDBusMessage::createMethodCall(QString::fromLatin1(kResolveService), reply.value().path(),
                                                                 QString::fromLatin1(kPropertiesInterface), QStringLiteral("GetAll"));
            getAll << QStringLiteral("org.freedesktop.resolve1.Link");
            getAll.setAutoStartService(false);
            auto *inner = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(getAll, 3000), this);
            connect(inner, &QDBusPendingCallWatcher::finished, this, [pending, done, ifindex](QDBusPendingCallWatcher *iw) {
                iw->deleteLater();
                const QDBusPendingReply<QVariantMap> props = *iw;
                if (!props.isError()) {
                    const QVariantMap p = props.value();
                    QVariantMap link;
                    link.insert(QStringLiteral("dnsOverTls"), p.value(QStringLiteral("DNSOverTLS")).toString());
                    link.insert(QStringLiteral("dnssec"), p.value(QStringLiteral("DNSSEC")).toString());
                    link.insert(QStringLiteral("defaultRoute"), p.value(QStringLiteral("DefaultRoute")).toBool());
                    link.insert(QStringLiteral("currentServer"), resolvedServer(p.value(QStringLiteral("CurrentDNSServer")), false));
                    pending->links.insert(QString::number(ifindex), link);
                }
                done();
            });
        });
    }
}

void NetState::finishDns(const QVariantMap &resolvedIn)
{
    QVariantMap dns;
    const QVariantMap resolvConf = readResolvConf();
    dns.insert(QStringLiteral("resolvConf"), resolvConf);

    QStringList servers, search;
    if (!resolvedIn.isEmpty()) {
        QVariantMap resolved = resolvedIn;
        const QVariantMap linkProperties = resolved.take(QStringLiteral("linkProperties")).toMap();
        auto nameOf = [this](int ifindex) {
            for (const QVariant &v : std::as_const(m_interfaces)) {
                const QVariantMap i = v.toMap();
                if (i.value(QStringLiteral("ifindex")).toInt() == ifindex)
                    return i.value(QStringLiteral("name")).toString();
            }
            return QString::number(ifindex);
        };

        // Regroup the flat (ifindex, …) lists per link; index 0 is global.
        QMap<int, QVariantMap> links;
        const QVariantList all = resolved.take(QStringLiteral("servers")).toList();
        for (const QVariant &v : all) {
            const QVariantMap s = v.toMap();
            const int ifindex = s.value(QStringLiteral("ifindex")).toInt();
            QVariantMap &link = links[ifindex];
            QStringList list = link.value(QStringLiteral("servers")).toStringList();
            list << s.value(QStringLiteral("address")).toString();
            link.insert(QStringLiteral("servers"), list);
            if (!servers.contains(s.value(QStringLiteral("address")).toString()))
                servers << s.value(QStringLiteral("address")).toString();
        }
        const QVariantList domains = resolved.take(QStringLiteral("domains")).toList();
        for (const QVariant &v : domains) {
            const QVariantMap d = v.toMap();
            const int ifindex = d.value(QStringLiteral("ifindex")).toInt();
            const QString domain = d.value(QStringLiteral("domain")).toString();
            const bool routeOnly = d.value(QStringLiteral("routeOnly")).toBool();
            QVariantMap &link = links[ifindex];
            QStringList list = link.value(routeOnly ? QStringLiteral("routingDomains") : QStringLiteral("searchDomains")).toStringList();
            list << domain;
            link.insert(routeOnly ? QStringLiteral("routingDomains") : QStringLiteral("searchDomains"), list);
            if (!routeOnly && !search.contains(domain))
                search << domain;
        }

        QVariantList linkList;
        for (auto it = links.constBegin(); it != links.constEnd(); ++it) {
            QVariantMap link = it.value();
            link.insert(QStringLiteral("ifindex"), it.key());
            link.insert(QStringLiteral("name"), it.key() == 0 ? QString() : nameOf(it.key()));
            link.insert(QStringLiteral("global"), it.key() == 0);
            if (!link.contains(QStringLiteral("servers")))
                link.insert(QStringLiteral("servers"), QStringList());
            if (!link.contains(QStringLiteral("searchDomains")))
                link.insert(QStringLiteral("searchDomains"), QStringList());
            if (!link.contains(QStringLiteral("routingDomains")))
                link.insert(QStringLiteral("routingDomains"), QStringList());
            const QVariantMap props = linkProperties.value(QString::number(it.key())).toMap();
            link.insert(QStringLiteral("dnsOverTls"), it.key() == 0 ? resolved.value(QStringLiteral("dnsOverTls"))
                                                                     : props.value(QStringLiteral("dnsOverTls")));
            link.insert(QStringLiteral("dnssec"), it.key() == 0 ? resolved.value(QStringLiteral("dnssec"))
                                                                 : props.value(QStringLiteral("dnssec")));
            link.insert(QStringLiteral("currentServer"), it.key() == 0 ? QString() : props.value(QStringLiteral("currentServer")).toString());
            link.insert(QStringLiteral("defaultRoute"), props.value(QStringLiteral("defaultRoute")).toBool());
            linkList << link;
        }
        resolved.insert(QStringLiteral("links"), linkList);

        QStringList fallback;
        const QVariantList fallbackServers = resolved.take(QStringLiteral("fallbackServers")).toList();
        for (const QVariant &v : fallbackServers)
            fallback << v.toMap().value(QStringLiteral("address")).toString();
        resolved.insert(QStringLiteral("fallbackServers"), fallback);
        const bool usingFallback = servers.isEmpty() && !fallback.isEmpty();
        resolved.insert(QStringLiteral("usingFallback"), usingFallback);
        if (usingFallback)
            servers = fallback;

        dns.insert(QStringLiteral("source"), QStringLiteral("systemd-resolved"));
        dns.insert(QStringLiteral("resolved"), resolved);
    } else {
        servers = resolvConf.value(QStringLiteral("nameservers")).toStringList();
        search = resolvConf.value(QStringLiteral("search")).toStringList();
        dns.insert(QStringLiteral("source"), QStringLiteral("resolv.conf"));
    }
    dns.insert(QStringLiteral("servers"), servers);
    dns.insert(QStringLiteral("search"), search);

    if (dns != m_dns) {
        m_dns = dns;
        Q_EMIT dnsChanged();
    }

    m_refreshing = false;
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
    Q_EMIT refreshed();
    if (m_refreshQueued) {
        m_refreshQueued = false;
        refresh();
    }
}

// ---- live counters -------------------------------------------------------------

bool NetState::readCounters(const QString &interface, Counters *out)
{
    const QString base = QStringLiteral("/sys/class/net/%1/statistics/").arg(interface);
    auto read = [&base](const char *name, qint64 *value) {
        QFile f(base + QString::fromLatin1(name));
        if (!f.open(QIODevice::ReadOnly))
            return false;
        *value = f.read(64).trimmed().toLongLong();
        return true;
    };
    return read("rx_bytes", &out->rxBytes) && read("tx_bytes", &out->txBytes)
        && read("rx_packets", &out->rxPackets) && read("tx_packets", &out->txPackets)
        && read("rx_errors", &out->rxErrors) && read("tx_errors", &out->txErrors)
        && read("rx_dropped", &out->rxDropped) && read("tx_dropped", &out->txDropped);
}

void NetState::sampleCounters()
{
    const double seconds = m_sampleClock.isValid() ? m_sampleClock.restart() / 1000.0 : 0.0;
    if (!m_sampleClock.isValid())
        m_sampleClock.start();

    const QStringList names = QDir(QStringLiteral("/sys/class/net")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QSet<QString> seen;
    bool membershipChanged = false;
    for (const QString &name : names) {
        Counters now;
        if (!readCounters(name, &now))
            continue;
        seen.insert(name);
        if (!m_live.contains(name))
            membershipChanged = true;
        Live &live = m_live[name];
        if (live.sampled && seconds > 0.05) {
            // A counter that went backwards was reset (driver reload); skip that interval.
            live.rxRate = now.rxBytes >= live.last.rxBytes ? double(now.rxBytes - live.last.rxBytes) / seconds : 0;
            live.txRate = now.txBytes >= live.last.txBytes ? double(now.txBytes - live.last.txBytes) / seconds : 0;
            live.rxHistory.append(live.rxRate);
            live.txHistory.append(live.txRate);
            while (live.rxHistory.size() > HistoryCapacity)
                live.rxHistory.removeFirst();
            while (live.txHistory.size() > HistoryCapacity)
                live.txHistory.removeFirst();
        }
        live.sampled = true;
        live.last = now;

        QVariantMap m;
        m.insert(QStringLiteral("available"), true);
        m.insert(QStringLiteral("rxRate"), live.rxRate);
        m.insert(QStringLiteral("txRate"), live.txRate);
        m.insert(QStringLiteral("rxBytes"), now.rxBytes);
        m.insert(QStringLiteral("txBytes"), now.txBytes);
        m.insert(QStringLiteral("rxPackets"), now.rxPackets);
        m.insert(QStringLiteral("txPackets"), now.txPackets);
        m.insert(QStringLiteral("rxErrors"), now.rxErrors);
        m.insert(QStringLiteral("txErrors"), now.txErrors);
        m.insert(QStringLiteral("rxDropped"), now.rxDropped);
        m.insert(QStringLiteral("txDropped"), now.txDropped);
        m.insert(QStringLiteral("rxHistory"), live.rxHistory);
        m.insert(QStringLiteral("txHistory"), live.txHistory);
        live.cached = m;
    }
    for (auto it = m_live.begin(); it != m_live.end();) {
        if (!seen.contains(it.key())) {
            it = m_live.erase(it);
            membershipChanged = true;
        } else {
            ++it;
        }
    }
    // An interface appeared or vanished between two structural refreshes.
    if (membershipChanged && m_ready && !m_debounce.isActive())
        m_debounce.start();

    QVariantMap stats;
    for (auto it = m_live.constBegin(); it != m_live.constEnd(); ++it)
        stats.insert(it.key(), it->cached);
    m_liveStats = stats;
    ++m_statsTick;
    Q_EMIT statsTickChanged();
}

// ---- dumps ---------------------------------------------------------------------

QJsonArray NetState::dumpInterfaces() const
{
    return QJsonArray::fromVariantList(m_interfacesFull);
}

QJsonArray NetState::dumpAddresses() const
{
    QJsonArray out;
    for (const QVariant &v : m_interfacesFull) {
        const QVariantList addresses = v.toMap().value(QStringLiteral("addresses")).toList();
        for (const QVariant &a : addresses)
            out.append(QJsonObject::fromVariantMap(a.toMap()));
    }
    return out;
}

QJsonArray NetState::dumpRoutes() const
{
    return QJsonArray::fromVariantList(m_routes);
}

QJsonObject NetState::dumpDns() const
{
    return QJsonObject::fromVariantMap(m_dns);
}

QJsonObject NetState::dumpSummary() const
{
    return QJsonObject{
        {QStringLiteral("error"), m_error},
        {QStringLiteral("linkState"), m_linkState},
        {QStringLiteral("interfacesUp"), m_up},
        {QStringLiteral("interfacesTotal"), m_total},
        {QStringLiteral("primaryInterface"), m_primaryInterface},
        {QStringLiteral("primaryAddress4"), m_primary4},
        {QStringLiteral("primaryAddress6"), m_primary6},
        {QStringLiteral("defaultRoute4"), QJsonObject::fromVariantMap(m_default4)},
        {QStringLiteral("defaultRoute6"), QJsonObject::fromVariantMap(m_default6)},
        {QStringLiteral("dnsSource"), m_dns.value(QStringLiteral("source")).toString()},
        {QStringLiteral("dnsServers"), QJsonArray::fromStringList(m_dns.value(QStringLiteral("servers")).toStringList())},
        {QStringLiteral("dnsSearch"), QJsonArray::fromStringList(m_dns.value(QStringLiteral("search")).toStringList())},
    };
}
