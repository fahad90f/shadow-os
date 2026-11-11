// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifibackend.h"

#include "dumpregistry.h"
#include "networkcommon.h"
#include "networkmanagerutil.h"

#include <QDBusConnection>
#include <QJsonArray>
#include <QRegularExpression>

#include <memory>

using namespace NetHw;

NEXUS_CC_DUMP("wifi", [] {
    WifiBackend backend;
    waitUntilReady(&backend, 12000);
    return QJsonValue(backend.toJson());
})

namespace {
const QString kDeviceInterface = QStringLiteral("org.freedesktop.NetworkManager.Device");

QString securityKindOf(const QString &security)
{
    if (security.contains(QLatin1String("802.1X")))
        return QStringLiteral("enterprise");
    if (security.contains(QLatin1String("WPA3")) && !security.contains(QLatin1String("WPA2"))
        && !security.contains(QLatin1String("WPA1")))
        return QStringLiteral("sae");
    if (security.contains(QLatin1String("WPA")))
        return QStringLiteral("psk");
    if (security.contains(QLatin1String("WEP")))
        return QStringLiteral("wep");
    if (security.contains(QLatin1String("OWE")))
        return QStringLiteral("owe");
    return QStringLiteral("open");
}

QString bandOf(int megahertz)
{
    if (megahertz >= 5925)
        return QStringLiteral("6 GHz");
    if (megahertz >= 4900)
        return QStringLiteral("5 GHz");
    if (megahertz >= 2400 && megahertz < 2500)
        return QStringLiteral("2.4 GHz");
    return QString();
}

int barsOf(int strength)
{
    return strength >= 75 ? 4 : strength >= 50 ? 3 : strength >= 30 ? 2 : strength > 5 ? 1 : 0;
}

QString deviceStateText(int code)
{
    if (code == 100)
        return WifiBackend::tr("Connected");
    if (code >= 40 && code <= 90)
        return WifiBackend::tr("Connecting…");
    switch (code) {
    case 10: return WifiBackend::tr("Not managed by NetworkManager");
    case 20: return WifiBackend::tr("Not ready");
    case 30: return WifiBackend::tr("Not connected");
    case 110: return WifiBackend::tr("Disconnecting…");
    case 120: return WifiBackend::tr("Connection failed");
    default: return WifiBackend::tr("Unknown");
    }
}

const QStringList kMacPolicies = {QString(), QStringLiteral("permanent"), QStringLiteral("random"),
                                  QStringLiteral("stable"), QStringLiteral("preserve")};
} // namespace

struct WifiBackend::Fetch
{
    int pending = 0;
    bool failed = false;
    QString error;
    QString radio;
    QString devices;
    QString connections;
    QString accessPoints;
    QString profiles;
};

WifiBackend::WifiBackend(QObject *parent)
    : QObject(parent)
{
    m_networks = new RowModel({QStringLiteral("key"), QStringLiteral("ssid"), QStringLiteral("ssidHex"),
                               QStringLiteral("strength"), QStringLiteral("bars"), QStringLiteral("security"),
                               QStringLiteral("securityKind"), QStringLiteral("band"), QStringLiteral("inUse"),
                               QStringLiteral("saved"), QStringLiteral("savedUuid"), QStringLiteral("apPath"),
                               QStringLiteral("connectable"), QStringLiteral("note")},
                              QStringLiteral("key"), this);
    m_saved = new RowModel({QStringLiteral("uuid"), QStringLiteral("name"), QStringLiteral("ssid"),
                            QStringLiteral("autoconnect"), QStringLiteral("lastUsed"), QStringLiteral("active")},
                           QStringLiteral("uuid"), this);

    m_attemptTimeout.setSingleShot(true);
    m_attemptTimeout.setInterval(60000);
    connect(&m_attemptTimeout, &QTimer::timeout, this, [this] {
        endAttempt(false, tr("The network did not answer within a minute."));
    });

    // NetworkManager announces changes; the timer only covers what it does
    // not signal (rfkill switches).
    m_poll.setInterval(15000);
    connect(&m_poll, &QTimer::timeout, this, &WifiBackend::refresh);
    m_poll.start();

    m_watcher = new Nm::Watcher(this);
    connect(m_watcher, &Nm::Watcher::runningChanged, this, &WifiBackend::refresh);
    connect(m_watcher, &Nm::Watcher::changed, this, &WifiBackend::refresh);
}

WifiBackend::~WifiBackend() = default;

QAbstractItemModel *WifiBackend::networks() const { return m_networks; }
int WifiBackend::networkCount() const { return m_networks->count(); }
QAbstractItemModel *WifiBackend::saved() const { return m_saved; }
int WifiBackend::savedCount() const { return m_saved->count(); }

// ---- reading ---------------------------------------------------------------------

void WifiBackend::setUnavailable(const QString &state, const QString &message)
{
    m_networks->setRows({});
    m_saved->setRows({});
    Q_EMIT networksChanged();
    if (!m_details.isEmpty()) {
        m_details.clear();
        Q_EMIT detailsChanged();
    }
    m_device.clear();
    m_devicePath.clear();
    m_deviceCount = 0;
    m_state = state;
    m_statusMessage = message;
    Q_EMIT stateChanged();
    finishRefresh();
}

void WifiBackend::finishRefresh()
{
    m_refreshing = false;
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
    if (m_refreshAgain) {
        m_refreshAgain = false;
        QTimer::singleShot(0, this, &WifiBackend::refresh);
    }
}

void WifiBackend::refresh()
{
    if (m_refreshing) {
        m_refreshAgain = true;
        return;
    }
    const RfkillState rfkill = rfkillState(QStringLiteral("wlan"));
    m_rfkillSoft = rfkill.soft;
    m_rfkillHard = rfkill.hard;

    if (!Nm::cliAvailable()) {
        setUnavailable(QStringLiteral("nm-missing"),
                       tr("nmcli is not installed. Wi-Fi is managed by NetworkManager (package network-manager)."));
        return;
    }
    if (!m_watcher->known())
        return;                 // runningChanged() follows and calls refresh() again
    if (!m_watcher->running()) {
        setUnavailable(QStringLiteral("nm-not-running"),
                       m_watcher->busConnected()
                           ? tr("NetworkManager is not running, so Wi-Fi networks cannot be listed or joined. "
                                "It is started by NetworkManager.service.")
                           : tr("The system message bus is not available, so NetworkManager cannot be reached."));
        return;
    }

    m_refreshing = true;
    auto fetch = std::make_shared<Fetch>();
    const auto second = [this, fetch] {
        if (--fetch->pending > 0)
            return;
        publish(*fetch);
    };
    const auto first = [this, fetch, second] {
        if (--fetch->pending > 0)
            return;
        if (fetch->failed) {
            publish(*fetch);
            return;
        }
        // The Wi-Fi device and the saved Wi-Fi profiles decide what else to ask.
        QString device;
        const QList<Nm::Record> devices = Nm::parseRecords(fetch->devices);
        for (const Nm::Record &record : devices) {
            if (record.value(QStringLiteral("GENERAL.TYPE")) != QLatin1String("wifi"))
                continue;
            const QString name = record.value(QStringLiteral("GENERAL.DEVICE"));
            if (!Nm::isInterfaceName(name))
                continue;
            // Prefer the adapter that is connected.
            if (device.isEmpty() || Nm::leadingNumber(record.value(QStringLiteral("GENERAL.STATE"))) == 100)
                device = name;
        }
        QStringList uuids;
        const QStringList lines = fetch->connections.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QStringList fields = Nm::splitTerse(line);
            if (fields.size() >= 3 && fields.at(2) == QLatin1String("802-11-wireless") && Nm::isUuid(fields.at(1)))
                uuids.append(fields.at(1));
        }

        fetch->pending = 1;         // guard while the requests are started
        if (!device.isEmpty() && fetch->radio.endsWith(QLatin1String(":enabled"))) {
            ++fetch->pending;
            Nm::run(this, {QStringLiteral("-t"), QStringLiteral("-f"),
                           QStringLiteral("IN-USE,SSID,SSID-HEX,BSSID,CHAN,FREQ,RATE,SIGNAL,SECURITY,DBUS-PATH"),
                           QStringLiteral("device"), QStringLiteral("wifi"), QStringLiteral("list"),
                           QStringLiteral("ifname"), device, QStringLiteral("--rescan"), QStringLiteral("no")},
                    [fetch, second](bool, const QString &out, const QString &) {
                fetch->accessPoints = out;
                second();
            });
        }
        if (!uuids.isEmpty()) {
            ++fetch->pending;
            QStringList arguments = {QStringLiteral("-t"), QStringLiteral("-f"),
                                     QStringLiteral("connection.uuid,connection.id,connection.autoconnect,"
                                                    "802-11-wireless.ssid,802-11-wireless.cloned-mac-address"),
                                     QStringLiteral("connection"), QStringLiteral("show")};
            arguments.append(uuids);
            Nm::run(this, arguments, [fetch, second](bool, const QString &out, const QString &) {
                fetch->profiles = out;
                second();
            });
        }
        second();                   // releases the guard
    };

    fetch->pending = 3;
    Nm::run(this, {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("WIFI-HW,WIFI"), QStringLiteral("radio")},
            [fetch, first](bool ok, const QString &out, const QString &error) {
        fetch->radio = out.trimmed();
        if (!ok) {
            fetch->failed = true;
            fetch->error = error;
        }
        first();
    });
    Nm::run(this, {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("GENERAL,CAPABILITIES,IP4,IP6"),
                   QStringLiteral("device"), QStringLiteral("show")},
            [fetch, first](bool ok, const QString &out, const QString &error) {
        fetch->devices = out;
        if (!ok) {
            fetch->failed = true;
            fetch->error = error;
        }
        first();
    });
    Nm::run(this, {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("NAME,UUID,TYPE,AUTOCONNECT,TIMESTAMP,ACTIVE"),
                   QStringLiteral("connection"), QStringLiteral("show")},
            [fetch, first](bool, const QString &out, const QString &) {
        fetch->connections = out;
        first();
    });
}

void WifiBackend::publish(const Fetch &fetch)
{
    if (fetch.failed) {
        setUnavailable(QStringLiteral("nm-not-running"),
                       tr("NetworkManager does not answer (%1).").arg(fetch.error));
        return;
    }

    // ---- the Wi-Fi device --------------------------------------------------------
    Nm::Record device;
    int deviceCount = 0;
    const QList<Nm::Record> devices = Nm::parseRecords(fetch.devices);
    for (const Nm::Record &record : devices) {
        if (record.value(QStringLiteral("GENERAL.TYPE")) != QLatin1String("wifi")
            || !Nm::isInterfaceName(record.value(QStringLiteral("GENERAL.DEVICE"))))
            continue;
        ++deviceCount;
        if (device.isEmpty() || Nm::leadingNumber(record.value(QStringLiteral("GENERAL.STATE"))) == 100)
            device = record;
    }
    const QString hardware = fetch.radio.section(QLatin1Char(':'), 0, 0);
    const bool softwareOn = fetch.radio.section(QLatin1Char(':'), 1, 1) == QLatin1String("enabled");

    if (device.isEmpty()) {
        if (m_rfkillHard || hardware == QLatin1String("disabled")) {
            setUnavailable(QStringLiteral("rfkill-hard"),
                           tr("Wi-Fi is switched off by a hardware switch or by the firmware (rfkill hard block). "
                              "Use the wireless key or switch of this computer to turn it back on."));
        } else {
            setUnavailable(QStringLiteral("no-adapter"),
                           tr("NetworkManager is running, but it has no Wi-Fi adapter. A missing driver or firmware "
                              "package is the usual reason on hardware that does have one."));
        }
        return;
    }

    const QString deviceName = device.value(QStringLiteral("GENERAL.DEVICE"));
    const int deviceState = Nm::leadingNumber(device.value(QStringLiteral("GENERAL.STATE")));
    QString state, message;
    if (m_rfkillHard || hardware == QLatin1String("disabled")) {
        state = QStringLiteral("rfkill-hard");
        message = tr("Wi-Fi is switched off by a hardware switch or by the firmware (rfkill hard block). "
                     "Use the wireless key or switch of this computer to turn it back on.");
    } else if (deviceState == 10) {
        state = QStringLiteral("unmanaged");
        message = tr("The Wi-Fi adapter %1 is not managed by NetworkManager, so it cannot be controlled here.").arg(deviceName);
    } else if (!softwareOn) {
        state = QStringLiteral("radio-off");
        message = m_rfkillSoft ? tr("Wi-Fi is blocked in software (rfkill soft block). Switching it on removes the block.")
                               : tr("Wi-Fi is switched off.");
    } else {
        state = QStringLiteral("ok");
    }

    // ---- saved profiles ---------------------------------------------------------------
    QHash<QString, Nm::Record> profiles;            // uuid → details
    const QList<Nm::Record> profileRecords = Nm::parseRecords(fetch.profiles);
    for (const Nm::Record &record : profileRecords)
        profiles.insert(record.value(QStringLiteral("connection.uuid")), record);

    QVariantList savedRows;
    QHash<QString, QString> uuidBySsid;
    const QStringList connectionLines = fetch.connections.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : connectionLines) {
        const QStringList fields = Nm::splitTerse(line);
        if (fields.size() < 6 || fields.at(2) != QLatin1String("802-11-wireless") || !Nm::isUuid(fields.at(1)))
            continue;
        const QString uuid = fields.at(1);
        const Nm::Record details = profiles.value(uuid);
        QString ssid = details.value(QStringLiteral("802-11-wireless.ssid"));
        if (ssid.isEmpty())
            ssid = fields.at(0);
        // Of several profiles for one SSID the active one wins, then the first.
        const bool active = fields.at(5) == QLatin1String("yes");
        if (active || !uuidBySsid.contains(ssid))
            uuidBySsid.insert(ssid, uuid);
        savedRows.append(QVariantMap{
            {QStringLiteral("uuid"), uuid},
            {QStringLiteral("name"), fields.at(0)},
            {QStringLiteral("ssid"), ssid},
            {QStringLiteral("autoconnect"), fields.at(3) == QLatin1String("yes")},
            {QStringLiteral("lastUsed"), fields.at(4).toLongLong()},
            {QStringLiteral("active"), active},
        });
    }
    std::sort(savedRows.begin(), savedRows.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString().compare(b.toMap().value(QStringLiteral("name")).toString(),
                                                                         Qt::CaseInsensitive) < 0;
    });

    // ---- access points, grouped by network ----------------------------------------------
    struct Group
    {
        QVariantMap row;
        QStringList bands;
        int bestSignal = -1;
    };
    QMap<QString, Group> groups;
    QVariantMap activeAp;
    if (state == QLatin1String("ok")) {
        const QStringList apLines = fetch.accessPoints.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : apLines) {
            const QStringList fields = Nm::splitTerse(line);
            if (fields.size() < 10)
                continue;
            const bool inUse = fields.at(0).trimmed() == QLatin1String("*");
            const QString ssid = fields.at(1);
            const QString ssidHex = fields.at(2);
            if (ssid.isEmpty() || ssid == QLatin1String("--") || ssidHex.isEmpty())
                continue;           // hidden network: joined through "Hidden network…"
            const int frequency = Nm::leadingNumber(fields.at(5));
            const int strength = qBound(0, fields.at(7).toInt(), 100);
            const QString security = Nm::unset(fields.at(8)).trimmed();
            const QString kind = securityKindOf(security);
            const QString band = bandOf(frequency);
            const QString key = ssidHex + QLatin1Char(':') + kind;

            Group &group = groups[key];
            if (!band.isEmpty() && !group.bands.contains(band))
                group.bands.append(band);
            const bool wasInUse = group.row.value(QStringLiteral("inUse")).toBool();
            if (inUse || (!wasInUse && strength > group.bestSignal)) {
                group.bestSignal = strength;
                const QString savedUuid = uuidBySsid.value(ssid);
                QString note;
                if (kind == QLatin1String("enterprise") && savedUuid.isEmpty())
                    note = tr("Enterprise networks (802.1X) need a profile with certificates or credentials, which this page cannot create.");
                else if (kind == QLatin1String("wep") && savedUuid.isEmpty())
                    note = tr("WEP is obsolete and insecure; this page does not set up WEP networks.");
                group.row = {
                    {QStringLiteral("key"), key},
                    {QStringLiteral("ssid"), ssid},
                    {QStringLiteral("ssidHex"), ssidHex},
                    {QStringLiteral("strength"), strength},
                    {QStringLiteral("bars"), barsOf(strength)},
                    {QStringLiteral("security"), security.isEmpty() ? tr("Open") : security},
                    {QStringLiteral("securityKind"), kind},
                    {QStringLiteral("inUse"), inUse},
                    {QStringLiteral("saved"), !savedUuid.isEmpty()},
                    {QStringLiteral("savedUuid"), savedUuid},
                    {QStringLiteral("apPath"), fields.at(9)},
                    {QStringLiteral("connectable"), note.isEmpty()},
                    {QStringLiteral("note"), note},
                };
            }
            if (inUse) {
                activeAp = {
                    {QStringLiteral("ssid"), ssid},
                    {QStringLiteral("strength"), strength},
                    {QStringLiteral("security"), security.isEmpty() ? tr("Open") : security},
                    {QStringLiteral("band"), band},
                    {QStringLiteral("channel"), fields.at(4)},
                    {QStringLiteral("bssid"), fields.at(3)},
                    {QStringLiteral("rate"), fields.at(6)},
                };
            }
        }
    }
    QVariantList networkRows;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        it->bands.sort();
        it->row.insert(QStringLiteral("band"), it->bands.join(QStringLiteral(" / ")));
        networkRows.append(it->row);
    }
    // The connected network first, then known ones, then by signal.
    std::sort(networkRows.begin(), networkRows.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        const bool xUse = x.value(QStringLiteral("inUse")).toBool(), yUse = y.value(QStringLiteral("inUse")).toBool();
        if (xUse != yUse)
            return xUse;
        const bool xSaved = x.value(QStringLiteral("saved")).toBool(), ySaved = y.value(QStringLiteral("saved")).toBool();
        if (xSaved != ySaved)
            return xSaved;
        const int xSignal = x.value(QStringLiteral("strength")).toInt(), ySignal = y.value(QStringLiteral("strength")).toInt();
        if (xSignal != ySignal)
            return xSignal > ySignal;
        return x.value(QStringLiteral("ssid")).toString() < y.value(QStringLiteral("ssid")).toString();
    });

    // ---- details of the device's connection -----------------------------------------------
    const QString activeUuid = device.value(QStringLiteral("GENERAL.CON-UUID"));
    const QString speed = device.value(QStringLiteral("CAPABILITIES.SPEED"));
    QVariantMap details{
        {QStringLiteral("connected"), deviceState == 100},
        {QStringLiteral("stateCode"), deviceState},
        {QStringLiteral("stateText"), deviceStateText(deviceState)},
        {QStringLiteral("profile"), Nm::unset(device.value(QStringLiteral("GENERAL.CONNECTION")))},
        {QStringLiteral("uuid"), Nm::isUuid(activeUuid) ? activeUuid : QString()},
        {QStringLiteral("ssid"), activeAp.value(QStringLiteral("ssid")).toString()},
        {QStringLiteral("strength"), activeAp.value(QStringLiteral("strength"), 0).toInt()},
        {QStringLiteral("security"), activeAp.value(QStringLiteral("security")).toString()},
        {QStringLiteral("band"), activeAp.value(QStringLiteral("band")).toString()},
        {QStringLiteral("channel"), activeAp.value(QStringLiteral("channel")).toString()},
        {QStringLiteral("bssid"), activeAp.value(QStringLiteral("bssid")).toString()},
        {QStringLiteral("speed"), speed == QLatin1String("unknown") ? QString() : speed},
        {QStringLiteral("maxRate"), activeAp.value(QStringLiteral("rate")).toString()},
        {QStringLiteral("ipv4"), Nm::indexedValues(device, QStringLiteral("IP4.ADDRESS"))},
        {QStringLiteral("gateway4"), Nm::unset(device.value(QStringLiteral("IP4.GATEWAY")))},
        {QStringLiteral("dns"), Nm::indexedValues(device, QStringLiteral("IP4.DNS")) + Nm::indexedValues(device, QStringLiteral("IP6.DNS"))},
        {QStringLiteral("ipv6"), Nm::indexedValues(device, QStringLiteral("IP6.ADDRESS"))},
        {QStringLiteral("gateway6"), Nm::unset(device.value(QStringLiteral("IP6.GATEWAY")))},
        {QStringLiteral("mac"), device.value(QStringLiteral("GENERAL.HWADDR"))},
        {QStringLiteral("macPolicy"), Nm::unset(profiles.value(activeUuid).value(QStringLiteral("802-11-wireless.cloned-mac-address")))},
    };

    m_networks->setRows(networkRows);
    m_saved->setRows(savedRows);
    Q_EMIT networksChanged();
    if (details != m_details) {
        m_details = details;
        Q_EMIT detailsChanged();
    }
    m_device = deviceName;
    m_devicePath = device.value(QStringLiteral("GENERAL.DBUS-PATH"));
    m_deviceCount = deviceCount;
    m_state = state;
    m_statusMessage = message;
    Q_EMIT stateChanged();
    finishRefresh();
}

// ---- changes -----------------------------------------------------------------------------

void WifiBackend::runChange(const QStringList &arguments, const QString &failurePrefix, int timeoutMs)
{
    Nm::run(this, arguments, [this, failurePrefix](bool ok, const QString &, const QString &error) {
        if (!ok)
            Q_EMIT failed(failurePrefix + QStringLiteral(": ") + error);
        refresh();
    }, timeoutMs);
}

void WifiBackend::setRadioEnabled(bool enabled)
{
    if (!available()) {
        Q_EMIT failed(m_statusMessage);
        return;
    }
    runChange({QStringLiteral("radio"), QStringLiteral("wifi"), enabled ? QStringLiteral("on") : QStringLiteral("off")},
              enabled ? tr("Wi-Fi could not be switched on") : tr("Wi-Fi could not be switched off"));
}

void WifiBackend::scan()
{
    if (!radioEnabled() || m_device.isEmpty() || m_scanning)
        return;
    m_scanning = true;
    Q_EMIT busyChanged();
    // NetworkManager refuses a scan right after the previous one; the list is
    // simply read again in that case.
    Nm::run(this, {QStringLiteral("device"), QStringLiteral("wifi"), QStringLiteral("rescan"),
                   QStringLiteral("ifname"), m_device},
            [this](bool, const QString &, const QString &) {
        QTimer::singleShot(2500, this, &WifiBackend::refresh);
        QTimer::singleShot(6000, this, [this] {
            m_scanning = false;
            Q_EMIT busyChanged();
            refresh();
        });
    });
}

QString WifiBackend::passwordProblem(const QString &securityKind, const QString &password) const
{
    if (securityKind == QLatin1String("psk")) {
        static const QRegularExpression hex(QStringLiteral("^[0-9A-Fa-f]{64}$"));
        static const QRegularExpression printable(QStringLiteral("^[\\x20-\\x7e]{8,63}$"));
        if (hex.match(password).hasMatch() || printable.match(password).hasMatch())
            return QString();
        return tr("A WPA password has 8 to 63 characters (letters, digits and symbols), or is a 64-digit hexadecimal key.");
    }
    if (securityKind == QLatin1String("sae")) {
        if (password.isEmpty())
            return tr("Enter the password of the network.");
        if (password.toUtf8().size() > 127)
            return tr("The password is too long.");
    }
    return QString();
}

void WifiBackend::connectNetwork(const QString &key, const QString &password)
{
    if (m_connecting)
        return;
    const QVariantMap row = m_networks->find(key);
    if (row.isEmpty() || !radioEnabled()) {
        Q_EMIT failed(tr("The network is no longer in range."));
        return;
    }
    const QString ssid = row.value(QStringLiteral("ssid")).toString();
    const QString kind = row.value(QStringLiteral("securityKind")).toString();

    if (row.value(QStringLiteral("saved")).toBool()) {
        const QString uuid = row.value(QStringLiteral("savedUuid")).toString();
        m_connecting = true;
        m_connectingSsid = ssid;
        m_attemptConnectionPath.clear();
        Q_EMIT busyChanged();
        Nm::run(this, {QStringLiteral("-w"), QStringLiteral("45"), QStringLiteral("connection"), QStringLiteral("up"),
                       QStringLiteral("uuid"), uuid, QStringLiteral("ifname"), m_device},
                [this](bool ok, const QString &, const QString &error) {
            QString reason = error;
            if (error.contains(QLatin1String("Secrets were required")))
                reason = tr("the saved password was not accepted. Forget the network and join it again with the right password.");
            endAttempt(ok, reason);
        }, 60000);
        return;
    }

    if (!row.value(QStringLiteral("connectable")).toBool()) {
        Q_EMIT failed(row.value(QStringLiteral("note")).toString());
        return;
    }
    const bool needsPassword = kind == QLatin1String("psk") || kind == QLatin1String("sae");
    if (needsPassword) {
        const QString problem = passwordProblem(kind, password);
        if (!problem.isEmpty()) {
            Q_EMIT failed(problem);
            return;
        }
    }
    activateNew(ssid, QByteArray::fromHex(row.value(QStringLiteral("ssidHex")).toString().toLatin1()), kind,
                needsPassword ? password : QString(), row.value(QStringLiteral("apPath")).toString(), false);
}

void WifiBackend::connectHidden(const QString &ssid, const QString &securityKind, const QString &password)
{
    if (m_connecting)
        return;
    if (!radioEnabled() || m_devicePath.isEmpty()) {
        Q_EMIT failed(tr("Switch Wi-Fi on first."));
        return;
    }
    const QByteArray ssidBytes = ssid.toUtf8();
    if (ssidBytes.isEmpty() || ssidBytes.size() > 32) {
        Q_EMIT failed(tr("A network name has 1 to 32 bytes."));
        return;
    }
    if (securityKind != QLatin1String("open") && securityKind != QLatin1String("psk") && securityKind != QLatin1String("sae")) {
        Q_EMIT failed(tr("“%1” is not a kind of security this page can set up.").arg(securityKind));
        return;
    }
    const QString problem = passwordProblem(securityKind, password);
    if (!problem.isEmpty()) {
        Q_EMIT failed(problem);
        return;
    }
    activateNew(ssid, ssidBytes, securityKind, securityKind == QLatin1String("open") ? QString() : password,
                QStringLiteral("/"), true);
}

void WifiBackend::activateNew(const QString &ssidText, const QByteArray &ssid, const QString &securityKind,
                              const QString &password, const QString &apPath, bool hidden)
{
    if (m_devicePath.isEmpty() || ssid.isEmpty()) {
        Q_EMIT failed(tr("There is no Wi-Fi adapter to connect with."));
        return;
    }
    Nm::Settings settings;
    settings.insert(QStringLiteral("connection"), {
        {QStringLiteral("type"), QStringLiteral("802-11-wireless")},
        {QStringLiteral("id"), ssidText},
    });
    QVariantMap wireless{{QStringLiteral("ssid"), ssid}, {QStringLiteral("mode"), QStringLiteral("infrastructure")}};
    if (hidden)
        wireless.insert(QStringLiteral("hidden"), true);
    settings.insert(QStringLiteral("802-11-wireless"), wireless);
    if (securityKind == QLatin1String("psk")) {
        settings.insert(QStringLiteral("802-11-wireless-security"), {
            {QStringLiteral("key-mgmt"), QStringLiteral("wpa-psk")}, {QStringLiteral("psk"), password}});
    } else if (securityKind == QLatin1String("sae")) {
        settings.insert(QStringLiteral("802-11-wireless-security"), {
            {QStringLiteral("key-mgmt"), QStringLiteral("sae")}, {QStringLiteral("psk"), password}});
    } else if (securityKind == QLatin1String("owe")) {
        settings.insert(QStringLiteral("802-11-wireless-security"), {{QStringLiteral("key-mgmt"), QStringLiteral("owe")}});
    }

    m_connecting = true;
    m_connectingSsid = ssidText;
    m_attemptConnectionPath.clear();
    m_attemptSawActivity = false;
    Q_EMIT busyChanged();

    // Follow the device until the attempt is decided.
    m_attemptWatched = QDBusConnection::systemBus().connect(Nm::service(), m_devicePath, kDeviceInterface,
                                                            QStringLiteral("StateChanged"), this,
                                                            SLOT(onDeviceStateChanged(uint, uint, uint)));
    m_attemptTimeout.start();
    Nm::addAndActivate(settings, m_devicePath, apPath, this,
                       [this](bool ok, const QString &connectionPath, const QString &, const QString &error) {
        if (!m_connecting)
            return;
        if (!ok) {
            endAttempt(false, error);
            return;
        }
        m_attemptConnectionPath = connectionPath;
    });
}

void WifiBackend::onDeviceStateChanged(uint newState, uint oldState, uint reason)
{
    Q_UNUSED(oldState)
    if (!m_connecting)
        return;
    if (newState >= 40 && newState <= 90) {
        m_attemptSawActivity = true;
    } else if (newState == 100) {
        endAttempt(true, QString());
    } else if (newState == 120 || (newState == 30 && m_attemptSawActivity)) {
        endAttempt(false, Nm::stateReasonText(reason));
    }
}

void WifiBackend::endAttempt(bool success, const QString &error)
{
    if (!m_connecting)
        return;
    m_attemptTimeout.stop();
    if (m_attemptWatched) {
        QDBusConnection::systemBus().disconnect(Nm::service(), m_devicePath, kDeviceInterface, QStringLiteral("StateChanged"),
                                                this, SLOT(onDeviceStateChanged(uint, uint, uint)));
        m_attemptWatched = false;
    }
    const QString ssid = m_connectingSsid;
    const QString createdProfile = m_attemptConnectionPath;
    m_connecting = false;
    m_connectingSsid.clear();
    m_attemptConnectionPath.clear();
    Q_EMIT busyChanged();

    if (success) {
        Q_EMIT connected(ssid);
    } else {
        // A profile made for an attempt that failed would keep a wrong password around.
        if (!createdProfile.isEmpty())
            Nm::deleteConnection(createdProfile, this, [this](bool, const QString &) { refresh(); });
        Q_EMIT failed(tr("Could not connect to %1: %2").arg(ssid, error));
    }
    refresh();
}

void WifiBackend::disconnectDevice()
{
    if (m_device.isEmpty())
        return;
    runChange({QStringLiteral("device"), QStringLiteral("disconnect"), m_device}, tr("The connection could not be closed"));
}

void WifiBackend::forget(const QString &uuid)
{
    if (!Nm::isUuid(uuid))
        return;
    runChange({QStringLiteral("connection"), QStringLiteral("delete"), QStringLiteral("uuid"), uuid},
              tr("The network could not be forgotten"));
}

void WifiBackend::setAutoconnect(const QString &uuid, bool autoconnect)
{
    if (!Nm::isUuid(uuid))
        return;
    runChange({QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("uuid"), uuid,
               QStringLiteral("connection.autoconnect"), autoconnect ? QStringLiteral("yes") : QStringLiteral("no")},
              tr("The network could not be changed"));
}

void WifiBackend::setMacPolicy(const QString &uuid, const QString &policy)
{
    if (!Nm::isUuid(uuid) || !kMacPolicies.contains(policy)) {
        Q_EMIT failed(tr("“%1” is not a MAC address policy.").arg(policy));
        return;
    }
    runChange({QStringLiteral("connection"), QStringLiteral("modify"), QStringLiteral("uuid"), uuid,
               QStringLiteral("802-11-wireless.cloned-mac-address"), policy},
              tr("The MAC address setting could not be changed"));
}

QJsonObject WifiBackend::toJson() const
{
    // Access point object paths are run-time handles, not data.
    QJsonArray networks;
    const QVariantList rows = m_networks->rows();
    for (const QVariant &row : rows) {
        QVariantMap map = row.toMap();
        map.remove(QStringLiteral("apPath"));
        map.remove(QStringLiteral("key"));
        networks.append(QJsonObject::fromVariantMap(map));
    }
    return {
        {QStringLiteral("state"), m_state},
        {QStringLiteral("available"), available()},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("radioEnabled"), radioEnabled()},
        {QStringLiteral("rfkill"), QJsonObject{
            {QStringLiteral("hardBlocked"), m_rfkillHard},
            {QStringLiteral("softBlocked"), m_rfkillSoft},
        }},
        {QStringLiteral("device"), m_device},
        {QStringLiteral("deviceCount"), m_deviceCount},
        {QStringLiteral("networks"), networks},
        {QStringLiteral("saved"), QJsonArray::fromVariantList(m_saved->rows())},
        {QStringLiteral("connection"), QJsonObject::fromVariantMap(m_details)},
    };
}
