// SPDX-License-Identifier: GPL-3.0-or-later
#include "vpnbackend.h"

#include "dumpregistry.h"
#include "networkcommon.h"
#include "networkmanagerutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <memory>

using namespace NetHw;

NEXUS_CC_DUMP("vpn", [] {
    VpnBackend backend;
    waitUntilReady(&backend, 12000);
    return QJsonValue(backend.toJson());
})

namespace {
const QString kOpenvpnService = QStringLiteral("org.freedesktop.NetworkManager.openvpn");

struct KnownService
{
    const char *suffix;
    const char *label;
    const char *package;
};
// NetworkManager VPN services and the Debian packages that provide them.
const KnownService kServices[] = {
    {"openvpn", "OpenVPN", "network-manager-openvpn"},
    {"vpnc", "Cisco VPN (vpnc)", "network-manager-vpnc"},
    {"openconnect", "OpenConnect", "network-manager-openconnect"},
    {"pptp", "PPTP", "network-manager-pptp"},
    {"l2tp", "L2TP/IPsec", "network-manager-l2tp"},
    {"strongswan", "IPsec (strongSwan)", "network-manager-strongswan"},
    {"libreswan", "IPsec (Libreswan)", "network-manager-libreswan"},
    {"fortisslvpn", "Fortinet SSL VPN", "network-manager-fortisslvpn"},
    {"sstp", "SSTP", "network-manager-sstp"},
    {"iodine", "iodine", "network-manager-iodine"},
    {"ssh", "SSH", "network-manager-ssh"},
};

const KnownService *knownService(const QString &service)
{
    const QString suffix = service.section(QLatin1Char('.'), -1);
    for (const KnownService &known : kServices) {
        if (suffix == QLatin1String(known.suffix))
            return &known;
    }
    return nullptr;
}

QString serviceLabel(const QString &service)
{
    if (const KnownService *known = knownService(service))
        return QString::fromLatin1(known->label);
    const QString suffix = service.section(QLatin1Char('.'), -1);
    return suffix.isEmpty() ? VpnBackend::tr("VPN") : suffix;
}

QString missingPluginText(const QString &service)
{
    if (const KnownService *known = knownService(service)) {
        return VpnBackend::tr("NetworkManager's %1 plug-in is not installed (package %2).")
            .arg(QString::fromLatin1(known->label), QString::fromLatin1(known->package));
    }
    return VpnBackend::tr("The NetworkManager VPN plug-in for the service %1 is not installed.").arg(service);
}
} // namespace

struct VpnBackend::Fetch
{
    bool failed = false;
    QString error;
    QString connections;
    QString profiles;
};

VpnBackend::VpnBackend(QObject *parent)
    : QObject(parent)
{
    m_profiles = new RowModel({QStringLiteral("uuid"), QStringLiteral("name"), QStringLiteral("kind"),
                               QStringLiteral("typeLabel"), QStringLiteral("stateId"), QStringLiteral("stateText"),
                               QStringLiteral("active"), QStringLiteral("autoconnect"), QStringLiteral("autoconnectNote"),
                               QStringLiteral("problem"), QStringLiteral("busy")},
                              QStringLiteral("uuid"), this);
    loadPlugins();

    m_poll.setInterval(20000);
    connect(&m_poll, &QTimer::timeout, this, &VpnBackend::refresh);
    m_poll.start();

    m_watcher = new Nm::Watcher(this);
    connect(m_watcher, &Nm::Watcher::runningChanged, this, &VpnBackend::refresh);
    connect(m_watcher, &Nm::Watcher::changed, this, &VpnBackend::refresh);
}

QAbstractItemModel *VpnBackend::profiles() const { return m_profiles; }
int VpnBackend::profileCount() const { return m_profiles->count(); }

bool VpnBackend::openvpnInstalled() const
{
    for (const QVariant &plugin : m_plugins) {
        if (plugin.toMap().value(QStringLiteral("service")).toString() == kOpenvpnService)
            return true;
    }
    return false;
}

void VpnBackend::loadPlugins()
{
    // NetworkManager finds its VPN plug-ins through these descriptor files.
    QVariantList plugins;
    QSet<QString> seen;
    for (const QString &path : {QStringLiteral("/usr/lib/NetworkManager/VPN"), QStringLiteral("/etc/NetworkManager/VPN")}) {
        const QDir dir(path);
        const QStringList names = dir.entryList({QStringLiteral("*.name")}, QDir::Files, QDir::Name);
        for (const QString &name : names) {
            QFile file(dir.absoluteFilePath(name));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QString section, pluginName, service;
            const QStringList lines = QString::fromUtf8(file.read(16384)).split(QLatin1Char('\n'));
            for (const QString &raw : lines) {
                const QString line = raw.trimmed();
                if (line.startsWith(QLatin1Char('['))) {
                    section = line;
                } else if (section == QLatin1String("[VPN Connection]")) {
                    if (line.startsWith(QLatin1String("name=")))
                        pluginName = line.mid(5).trimmed();
                    else if (line.startsWith(QLatin1String("service=")))
                        service = line.mid(8).trimmed();
                }
            }
            if (service.isEmpty() || seen.contains(service))
                continue;
            seen.insert(service);
            plugins.append(QVariantMap{
                {QStringLiteral("name"), pluginName},
                {QStringLiteral("service"), service},
                {QStringLiteral("label"), serviceLabel(service)},
            });
        }
    }
    if (plugins != m_plugins) {
        m_plugins = plugins;
        Q_EMIT pluginsChanged();
    }
}

// ---- reading ------------------------------------------------------------------------

void VpnBackend::setUnavailable(const QString &state, const QString &message)
{
    m_profiles->setRows({});
    m_bases.clear();
    Q_EMIT profilesChanged();
    m_state = state;
    m_statusMessage = message;
    Q_EMIT stateChanged();
    finishRefresh();
}

void VpnBackend::finishRefresh()
{
    m_refreshing = false;
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
    if (m_refreshAgain) {
        m_refreshAgain = false;
        QTimer::singleShot(0, this, &VpnBackend::refresh);
    }
}

void VpnBackend::refresh()
{
    if (m_refreshing) {
        m_refreshAgain = true;
        return;
    }
    loadPlugins();
    if (!Nm::cliAvailable()) {
        setUnavailable(QStringLiteral("nm-missing"),
                       tr("nmcli is not installed. VPN connections are managed by NetworkManager (package network-manager)."));
        return;
    }
    if (!m_watcher->known())
        return;
    if (!m_watcher->running()) {
        setUnavailable(QStringLiteral("nm-not-running"),
                       m_watcher->busConnected()
                           ? tr("NetworkManager is not running, so VPN profiles cannot be listed or connected. "
                                "It is started by NetworkManager.service.")
                           : tr("The system message bus is not available, so NetworkManager cannot be reached."));
        return;
    }

    m_refreshing = true;
    auto fetch = std::make_shared<Fetch>();
    Nm::run(this, {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("NAME,UUID,TYPE,AUTOCONNECT,ACTIVE,STATE"),
                   QStringLiteral("connection"), QStringLiteral("show")},
            [this, fetch](bool ok, const QString &out, const QString &error) {
        fetch->connections = out;
        if (!ok) {
            fetch->failed = true;
            fetch->error = error;
            publish(*fetch);
            return;
        }
        // Details are only needed when there is a VPN at all.
        QStringList uuids;
        bool anyVpn = false;
        const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QStringList fields = Nm::splitTerse(line);
            if (fields.size() < 3 || !Nm::isUuid(fields.at(1)))
                continue;
            uuids.append(fields.at(1));
            anyVpn = anyVpn || fields.at(2) == QLatin1String("vpn") || fields.at(2) == QLatin1String("wireguard");
        }
        if (!anyVpn) {
            publish(*fetch);
            return;
        }
        QStringList arguments = {QStringLiteral("-t"), QStringLiteral("-f"),
                                 QStringLiteral("connection.uuid,connection.secondaries,vpn.service-type"),
                                 QStringLiteral("connection"), QStringLiteral("show")};
        arguments.append(uuids);
        Nm::run(this, arguments, [this, fetch](bool, const QString &details, const QString &) {
            fetch->profiles = details;
            publish(*fetch);
        });
    });
}

void VpnBackend::publish(const Fetch &fetch)
{
    if (fetch.failed) {
        setUnavailable(QStringLiteral("nm-not-running"), tr("NetworkManager does not answer (%1).").arg(fetch.error));
        return;
    }

    QHash<QString, Nm::Record> details;
    const QList<Nm::Record> records = Nm::parseRecords(fetch.profiles);
    for (const Nm::Record &record : records)
        details.insert(record.value(QStringLiteral("connection.uuid")), record);

    QSet<QString> installed;
    for (const QVariant &plugin : std::as_const(m_plugins))
        installed.insert(plugin.toMap().value(QStringLiteral("service")).toString());

    struct Entry
    {
        QString name, uuid, type, state;
        bool autoconnect = false, active = false;
    };
    QList<Entry> entries;
    m_bases.clear();
    const QStringList lines = fetch.connections.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = Nm::splitTerse(line);
        if (fields.size() < 6 || !Nm::isUuid(fields.at(1)))
            continue;
        Entry entry;
        entry.name = fields.at(0);
        entry.uuid = fields.at(1);
        entry.type = fields.at(2);
        entry.autoconnect = fields.at(3) == QLatin1String("yes");
        entry.active = fields.at(4) == QLatin1String("yes");
        entry.state = fields.at(5);
        entries.append(entry);
        // Networks a plug-in VPN can be started with.
        if (entry.type == QLatin1String("802-3-ethernet") || entry.type == QLatin1String("802-11-wireless")) {
            Base base;
            base.uuid = entry.uuid;
            base.name = entry.name;
            base.active = entry.active;
            base.secondaries = details.value(entry.uuid).value(QStringLiteral("connection.secondaries"))
                                   .split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (QString &secondary : base.secondaries)
                secondary = secondary.trimmed();
            m_bases.append(base);
        }
    }

    QVariantList rows;
    for (const Entry &entry : std::as_const(entries)) {
        const bool wireguard = entry.type == QLatin1String("wireguard");
        if (!wireguard && entry.type != QLatin1String("vpn"))
            continue;
        const QString service = details.value(entry.uuid).value(QStringLiteral("vpn.service-type"));

        QString stateId = QStringLiteral("inactive"), stateText = tr("Not connected");
        if (entry.state == QLatin1String("activated")) {
            stateId = QStringLiteral("active");
            stateText = tr("Connected");
        } else if (entry.state == QLatin1String("activating")) {
            stateId = QStringLiteral("activating");
            stateText = tr("Connecting…");
        } else if (entry.state == QLatin1String("deactivating")) {
            stateId = QStringLiteral("deactivating");
            stateText = tr("Disconnecting…");
        }

        bool autoconnect = entry.autoconnect;
        QString note;
        if (!wireguard) {
            QStringList via;
            for (const Base &base : std::as_const(m_bases)) {
                if (base.secondaries.contains(entry.uuid))
                    via.append(base.name);
            }
            autoconnect = !via.isEmpty();
            if (autoconnect)
                note = tr("Starts together with: %1").arg(via.join(QStringLiteral(", ")));
        }

        rows.append(QVariantMap{
            {QStringLiteral("uuid"), entry.uuid},
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("kind"), wireguard ? QStringLiteral("wireguard") : QStringLiteral("vpn")},
            {QStringLiteral("typeLabel"), wireguard ? QStringLiteral("WireGuard") : serviceLabel(service)},
            {QStringLiteral("stateId"), stateId},
            {QStringLiteral("stateText"), stateText},
            {QStringLiteral("active"), stateId != QLatin1String("inactive")},
            {QStringLiteral("autoconnect"), autoconnect},
            {QStringLiteral("autoconnectNote"), note},
            {QStringLiteral("problem"), wireguard || installed.contains(service) ? QString() : missingPluginText(service)},
            {QStringLiteral("busy"), m_busy.contains(entry.uuid)},
        });
    }
    std::sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString().compare(b.toMap().value(QStringLiteral("name")).toString(),
                                                                         Qt::CaseInsensitive) < 0;
    });

    m_profiles->setRows(rows);
    Q_EMIT profilesChanged();
    m_state = QStringLiteral("ok");
    m_statusMessage.clear();
    Q_EMIT stateChanged();
    finishRefresh();
}

// ---- changes ---------------------------------------------------------------------------

void VpnBackend::setBusy(const QString &uuid, bool busy)
{
    if (busy)
        m_busy.insert(uuid);
    else
        m_busy.remove(uuid);
    QVariantList rows = m_profiles->rows();
    for (QVariant &row : rows) {
        QVariantMap map = row.toMap();
        map.insert(QStringLiteral("busy"), m_busy.contains(map.value(QStringLiteral("uuid")).toString()));
        row = map;
    }
    m_profiles->setRows(rows);
}

QString VpnBackend::friendlyError(const QString &error) const
{
    static const QRegularExpression notInstalled(QStringLiteral("VPN service '([^']+)' was not installed"));
    static const QRegularExpression unknownPlugin(QStringLiteral("unknown VPN plugin \"([^\"]+)\""));
    QRegularExpressionMatch match = notInstalled.match(error);
    if (!match.hasMatch())
        match = unknownPlugin.match(error);
    if (match.hasMatch())
        return missingPluginText(match.captured(1));
    if (error.contains(QLatin1String("ecrets were required")) || error.contains(QLatin1String("no valid VPN secrets")))
        return tr("the VPN needs a password or key that is not stored in the profile, and nothing in this session answered NetworkManager's request for it.");
    return error;
}

void VpnBackend::runSteps(const QString &uuid, QList<QStringList> steps, const QString &failurePrefix)
{
    if (!m_steps.isEmpty() || !m_stepsUuid.isEmpty()) {
        Q_EMIT failed(tr("Another change is still in progress."));
        return;
    }
    m_steps = std::move(steps);
    m_stepsUuid = uuid;
    m_stepsPrefix = failurePrefix;
    setBusy(uuid, true);
    nextStep();
}

void VpnBackend::nextStep()
{
    if (m_steps.isEmpty()) {
        const QString uuid = m_stepsUuid;
        m_stepsUuid.clear();
        setBusy(uuid, false);
        refresh();
        return;
    }
    const QStringList arguments = m_steps.takeFirst();
    Nm::run(this, arguments, [this](bool ok, const QString &, const QString &error) {
        if (!ok) {
            Q_EMIT failed(m_stepsPrefix + QStringLiteral(": ") + friendlyError(error));
            m_steps.clear();            // do not carry on after a failure
        }
        nextStep();
    }, 90000);
}

void VpnBackend::connectProfile(const QString &uuid)
{
    const QVariantMap row = m_profiles->find(uuid);
    if (row.isEmpty() || !Nm::isUuid(uuid))
        return;
    const QString problem = row.value(QStringLiteral("problem")).toString();
    if (!problem.isEmpty()) {
        Q_EMIT failed(problem);
        return;
    }
    runSteps(uuid, {{QStringLiteral("-w"), QStringLiteral("60"), QStringLiteral("connection"), QStringLiteral("up"),
                     QStringLiteral("uuid"), uuid}},
             tr("%1 could not be connected").arg(row.value(QStringLiteral("name")).toString()));
}

void VpnBackend::disconnectProfile(const QString &uuid)
{
    const QVariantMap row = m_profiles->find(uuid);
    if (row.isEmpty() || !Nm::isUuid(uuid))
        return;
    runSteps(uuid, {{QStringLiteral("-w"), QStringLiteral("30"), QStringLiteral("connection"), QStringLiteral("down"),
                     QStringLiteral("uuid"), uuid}},
             tr("%1 could not be disconnected").arg(row.value(QStringLiteral("name")).toString()));
}

void VpnBackend::removeProfile(const QString &uuid)
{
    const QVariantMap row = m_profiles->find(uuid);
    if (row.isEmpty() || !Nm::isUuid(uuid))
        return;
    QList<QStringList> steps;
    // A network that still lists a deleted VPN as secondary would fail to connect.
    for (const Base &base : std::as_const(m_bases)) {
        if (base.secondaries.contains(uuid)) {
            steps.append({QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("uuid"), base.uuid,
                          QStringLiteral("-connection.secondaries"), uuid});
        }
    }
    steps.append({QStringLiteral("connection"), QStringLiteral("delete"), QStringLiteral("uuid"), uuid});
    runSteps(uuid, steps, tr("%1 could not be deleted").arg(row.value(QStringLiteral("name")).toString()));
}

void VpnBackend::setAutoconnect(const QString &uuid, bool autoconnect)
{
    const QVariantMap row = m_profiles->find(uuid);
    if (row.isEmpty() || !Nm::isUuid(uuid))
        return;
    const QString name = row.value(QStringLiteral("name")).toString();
    QList<QStringList> steps;
    if (row.value(QStringLiteral("kind")).toString() == QLatin1String("wireguard")) {
        steps.append({QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("uuid"), uuid,
                      QStringLiteral("connection.autoconnect"), autoconnect ? QStringLiteral("yes") : QStringLiteral("no")});
    } else if (autoconnect) {
        for (const Base &base : std::as_const(m_bases)) {
            if (base.active && !base.secondaries.contains(uuid)) {
                steps.append({QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("uuid"), base.uuid,
                              QStringLiteral("+connection.secondaries"), uuid});
            }
        }
        if (steps.isEmpty()) {
            bool listed = false;
            for (const Base &base : std::as_const(m_bases))
                listed = listed || base.secondaries.contains(uuid);
            if (!listed) {
                Q_EMIT failed(tr("Connect to a network first: NetworkManager starts a VPN of this kind together with a "
                                 "network connection, and none is active right now."));
                refresh();
            }
            return;
        }
    } else {
        for (const Base &base : std::as_const(m_bases)) {
            if (base.secondaries.contains(uuid)) {
                steps.append({QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("uuid"), base.uuid,
                              QStringLiteral("-connection.secondaries"), uuid});
            }
        }
        if (steps.isEmpty())
            return;
    }
    runSteps(uuid, steps, tr("The automatic connection of %1 could not be changed").arg(name));
}

// ---- import -------------------------------------------------------------------------------

void VpnBackend::importFile(const QUrl &file)
{
    if (m_importing)
        return;
    if (!available()) {
        Q_EMIT failed(m_statusMessage);
        return;
    }
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString(QUrl::PreferLocalFile);
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) {
        Q_EMIT failed(tr("%1 is not a readable file.").arg(path));
        return;
    }
    if (info.size() > 1024 * 1024) {
        Q_EMIT failed(tr("%1 is too large to be a VPN profile.").arg(info.fileName()));
        return;
    }
    const QString suffix = info.suffix().toLower();
    const bool wireguard = suffix == QLatin1String("conf");
    if (!wireguard && suffix != QLatin1String("ovpn")) {
        Q_EMIT failed(tr("Choose a WireGuard profile (.conf) or an OpenVPN profile (.ovpn)."));
        return;
    }
    if (!wireguard && !openvpnInstalled()) {
        Q_EMIT failed(tr("OpenVPN profiles cannot be imported: %1").arg(missingPluginText(kOpenvpnService)));
        return;
    }

    QString importPath = info.absoluteFilePath();
    QString wantedName;
    auto scratch = std::make_shared<std::unique_ptr<QTemporaryDir>>();
    if (wireguard && !Nm::isInterfaceName(info.completeBaseName())) {
        // NetworkManager names the WireGuard interface after the file. Import
        // a private copy with a valid name and give the profile its real name afterwards.
        QString interfaceName = info.completeBaseName().toLower();
        interfaceName.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]+")), QStringLiteral("-"));
        interfaceName = interfaceName.left(15);
        while (interfaceName.startsWith(QLatin1Char('-')))
            interfaceName.remove(0, 1);
        while (interfaceName.endsWith(QLatin1Char('-')))
            interfaceName.chop(1);
        if (interfaceName.isEmpty())
            interfaceName = QStringLiteral("wg0");
        *scratch = std::make_unique<QTemporaryDir>();           // mode 0700, removed when released
        const QString copy = (*scratch)->filePath(interfaceName + QStringLiteral(".conf"));
        if (!(*scratch)->isValid() || !QFile::copy(importPath, copy)
            || !QFile::setPermissions(copy, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            Q_EMIT failed(tr("%1 could not be prepared for the import.").arg(info.fileName()));
            return;
        }
        importPath = copy;
        wantedName = info.completeBaseName();
    }

    m_importing = true;
    Q_EMIT busyChanged();
    Nm::run(this, {QStringLiteral("connection"), QStringLiteral("import"), QStringLiteral("type"),
                   wireguard ? QStringLiteral("wireguard") : QStringLiteral("openvpn"), QStringLiteral("file"), importPath},
            [this, scratch, wantedName, wireguard](bool ok, const QString &out, const QString &error) {
        scratch->reset();               // the private copy holds a key: delete it now
        finishImport(ok, out, error, wantedName, wireguard);
    });
}

void VpnBackend::finishImport(bool ok, const QString &out, const QString &error, const QString &wantedName, bool wireguard)
{
    const auto done = [this](const QString &name) {
        m_importing = false;
        Q_EMIT busyChanged();
        if (!name.isEmpty())
            Q_EMIT imported(name);
        refresh();
    };
    if (!ok) {
        Q_EMIT failed(tr("The profile could not be imported: %1").arg(friendlyError(error)));
        done(QString());
        return;
    }
    // "Connection 'name' (uuid) successfully added."
    static const QRegularExpression added(QStringLiteral("Connection '(.*)' \\(([0-9a-fA-F-]{36})\\) successfully added"));
    const QRegularExpressionMatch match = added.match(out);
    const QString uuid = match.captured(2);
    const QString name = wantedName.isEmpty() ? match.captured(1) : wantedName;
    if (!match.hasMatch() || !Nm::isUuid(uuid) || (!wireguard && wantedName.isEmpty())) {
        done(name.isEmpty() ? tr("the profile") : name);
        return;
    }
    // WireGuard profiles would otherwise connect by themselves right away;
    // and a profile imported from a renamed copy gets its real name back.
    QStringList arguments = {QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("uuid"), uuid};
    if (wireguard)
        arguments << QStringLiteral("connection.autoconnect") << QStringLiteral("no");
    if (!wantedName.isEmpty())
        arguments << QStringLiteral("connection.id") << wantedName;
    Nm::run(this, arguments, [done, name](bool, const QString &, const QString &) { done(name); });
}

QJsonObject VpnBackend::toJson() const
{
    return {
        {QStringLiteral("state"), m_state},
        {QStringLiteral("available"), available()},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("profiles"), QJsonArray::fromVariantList(m_profiles->rows())},
        {QStringLiteral("plugins"), QJsonArray::fromVariantList(m_plugins)},
        {QStringLiteral("openvpnInstalled"), openvpnInstalled()},
    };
}
