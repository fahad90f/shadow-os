// SPDX-License-Identifier: GPL-3.0-or-later
#include "statuspower.h"

#include "statusdbus.h"

#include <QJsonArray>

namespace StatusModule {

namespace {

const QString kUPower = QStringLiteral("org.freedesktop.UPower");
const QString kUPowerPath = QStringLiteral("/org/freedesktop/UPower");
const QString kDisplayDevicePath = QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice");
const QString kUPowerDevice = QStringLiteral("org.freedesktop.UPower.Device");

// org.freedesktop.UPower.Device: Type
constexpr int kTypeBattery = 2;
constexpr int kTypeUps = 3;

QString batteryStateName(int state)
{
    switch (state) {    // org.freedesktop.UPower.Device: State
    case 1: return QStringLiteral("charging");
    case 2: return QStringLiteral("discharging");
    case 3: return QStringLiteral("empty");
    case 4: return QStringLiteral("full");
    case 5: return QStringLiteral("pending-charge");
    case 6: return QStringLiteral("pending-discharge");
    default: return QStringLiteral("unknown");
    }
}

QString warningLevelName(int level)
{
    switch (level) {    // org.freedesktop.UPower.Device: WarningLevel
    case 3: return QStringLiteral("low");
    case 4: return QStringLiteral("critical");
    case 5: return QStringLiteral("action");
    default: return QStringLiteral("none");
    }
}

struct Endpoint {
    QString service;
    QString path;
    QString interface;
};

const Endpoint &profilesEndpoint(int index)
{
    static const Endpoint endpoints[2] = {
        {QStringLiteral("org.freedesktop.UPower.PowerProfiles"), QStringLiteral("/org/freedesktop/UPower/PowerProfiles"),
         QStringLiteral("org.freedesktop.UPower.PowerProfiles")},
        {QStringLiteral("net.hadess.PowerProfiles"), QStringLiteral("/net/hadess/PowerProfiles"),
         QStringLiteral("net.hadess.PowerProfiles")},
    };
    return endpoints[index];
}

QString busProblem(const QDBusConnection &bus, const QString &serviceProblem)
{
    return bus.isConnected() ? serviceProblem : BatteryBackend::tr("The system bus is not available.");
}

} // namespace

// ---- battery ------------------------------------------------------------------------

BatteryBackend::BatteryBackend(QObject *parent)
    : Backend(parent), m_bus(QDBusConnection::systemBus())
{
    m_daemon = new DBusProperties(m_bus, kUPower, kUPowerPath, kUPower, this);
    m_display = new DBusProperties(m_bus, kUPower, kDisplayDevicePath, kUPowerDevice, this);
    // UPower is a bus-activated service: the first question may start it. A
    // UPower that is stopped later is not started again from here.
    m_daemon->setAutoStart(true);
    m_display->setAutoStart(true);
    connect(m_daemon, &DBusProperties::changed, this, &BatteryBackend::recompute);
    connect(m_display, &DBusProperties::changed, this, &BatteryBackend::recompute);
    dbusWatchService(m_bus, kUPower, this, [this] {
        m_daemon->load();
        m_display->load();
    }, [this] {
        m_daemon->clear();
        m_display->clear();
    });
    m_daemon->load();
    m_display->load();
}

void BatteryBackend::recompute()
{
    if (!m_daemon->isLoaded() || !m_display->isLoaded())
        return;
    m_daemon->setAutoStart(false);
    m_display->setAutoStart(false);

    Snapshot s;
    const bool up = m_daemon->isValid() && m_display->isValid();
    if (up) {
        const int type = int(m_display->integer(QStringLiteral("Type")));
        s.present = m_display->boolean(QStringLiteral("IsPresent")) && (type == kTypeBattery || type == kTypeUps);
        s.onBattery = m_daemon->boolean(QStringLiteral("OnBattery"));
        if (s.present) {
            s.percentage = qBound(0, qRound(m_display->real(QStringLiteral("Percentage"))), 100);
            s.state = batteryStateName(int(m_display->integer(QStringLiteral("State"))));
            s.secondsToEmpty = m_display->integer(QStringLiteral("TimeToEmpty"));
            s.secondsToFull = m_display->integer(QStringLiteral("TimeToFull"));
            s.warningLevel = warningLevelName(int(m_display->integer(QStringLiteral("WarningLevel"))));
        }
    }
    if (s != m_now) {
        m_now = s;
        Q_EMIT changed();
    }
    if (up)
        setAvailable(true);
    else
        setAvailable(false, busProblem(m_bus, tr("UPower is not running.")));
}

QJsonObject BatteryBackend::details() const
{
    QJsonObject json;
    if (!isAvailable())
        return json;
    json.insert(QStringLiteral("daemonVersion"), m_daemon->string(QStringLiteral("DaemonVersion")));
    json.insert(QStringLiteral("onBattery"), m_now.onBattery);
    json.insert(QStringLiteral("present"), m_now.present);
    json.insert(QStringLiteral("displayDeviceType"), int(m_display->integer(QStringLiteral("Type"))));
    if (m_now.present) {
        json.insert(QStringLiteral("percentage"), m_now.percentage);
        json.insert(QStringLiteral("state"), m_now.state);
        json.insert(QStringLiteral("secondsToEmpty"), m_now.secondsToEmpty);
        json.insert(QStringLiteral("secondsToFull"), m_now.secondsToFull);
        json.insert(QStringLiteral("warningLevel"), m_now.warningLevel);
    }
    return json;
}

// ---- power profiles -------------------------------------------------------------------

PowerProfilesBackend::PowerProfilesBackend(QObject *parent)
    : Backend(parent), m_bus(QDBusConnection::systemBus())
{
    for (int i = 0; i < 2; ++i) {
        const Endpoint &endpoint = profilesEndpoint(i);
        m_endpoints[i] = new DBusProperties(m_bus, endpoint.service, endpoint.path, endpoint.interface, this);
        // Bus-activated like UPower; see BatteryBackend.
        m_endpoints[i]->setAutoStart(true);
        connect(m_endpoints[i], &DBusProperties::changed, this, [this, i] { endpointChanged(i); });
        dbusWatchService(m_bus, endpoint.service, this, [this] { probe(0); }, [this, i] { m_endpoints[i]->clear(); });
    }
    probe(0);
}

void PowerProfilesBackend::probe(int endpoint)
{
    m_endpoint = endpoint;
    m_endpoints[endpoint]->load();
}

void PowerProfilesBackend::endpointChanged(int endpoint)
{
    const DBusProperties *properties = m_endpoints[endpoint];
    if (endpoint != m_endpoint || !properties->isLoaded())
        return;

    QStringList profiles;
    QString active, degraded;
    if (!properties->isValid()) {
        if (endpoint == 0) {
            probe(1);       // an older daemon that only has the previous name
            return;
        }
    } else {
        QStringList offered;
        const QList<QVariantMap> list = dbusMapList(properties->value(QStringLiteral("Profiles")));
        for (const QVariantMap &profile : list)
            offered << profile.value(QStringLiteral("Profile")).toString();
        for (const QString &id : {QStringLiteral("power-saver"), QStringLiteral("balanced"), QStringLiteral("performance")}) {
            if (offered.removeAll(id) > 0)
                profiles << id;
        }
        profiles << offered;    // anything a future daemon adds
        profiles.removeAll(QString());
        active = properties->string(QStringLiteral("ActiveProfile"));
        degraded = properties->string(QStringLiteral("PerformanceDegraded"));
    }
    for (DBusProperties *each : m_endpoints)
        each->setAutoStart(false);

    if (profiles != m_profiles || active != m_active || degraded != m_degraded) {
        m_profiles = profiles;
        m_active = active;
        m_degraded = degraded;
        Q_EMIT changed();
    }
    if (!properties->isValid())
        setAvailable(false, busProblem(m_bus, tr("No power profile service (power-profiles-daemon) is running.")));
    else if (profiles.isEmpty())
        setAvailable(false, tr("The power profile service offers no profiles on this machine."));
    else
        setAvailable(true);
}

void PowerProfilesBackend::setActiveProfile(const QString &profile)
{
    if (!isAvailable() || !m_profiles.contains(profile))
        return;
    if (profile == m_active) {
        Q_EMIT changed();
        return;
    }
    m_endpoints[m_endpoint]->set(QStringLiteral("ActiveProfile"), profile, [this](const QString &error) {
        if (error.isEmpty())
            return;     // the daemon announces the new profile itself
        Q_EMIT failed(tr("The power profile could not be changed: %1").arg(error));
        Q_EMIT changed();
    });
}

void PowerProfilesBackend::setActiveIndex(int index)
{
    if (index >= 0 && index < m_profiles.size())
        setActiveProfile(m_profiles.at(index));
}

QJsonObject PowerProfilesBackend::details() const
{
    QJsonObject json;
    if (!isAvailable())
        return json;
    const DBusProperties *properties = m_endpoints[m_endpoint];
    json.insert(QStringLiteral("service"), properties->service());
    json.insert(QStringLiteral("version"), properties->string(QStringLiteral("Version")));
    json.insert(QStringLiteral("profiles"), QJsonArray::fromStringList(m_profiles));
    json.insert(QStringLiteral("activeProfile"), m_active);
    json.insert(QStringLiteral("performanceDegraded"), m_degraded);
    return json;
}

} // namespace StatusModule
