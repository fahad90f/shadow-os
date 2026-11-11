// SPDX-License-Identifier: GPL-3.0-or-later
#include "powerbackend.h"

#include "dumpregistry.h"
#include "networkcommon.h"

#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <memory>

using namespace NetHw;

SHADOW_CC_DUMP("power", [] {
    PowerBackend backend;
    waitUntilReady(&backend);
    return QJsonValue(backend.toJson());
})

namespace {
const QString kUPower = QStringLiteral("org.freedesktop.UPower");
const QString kUPowerPath = QStringLiteral("/org/freedesktop/UPower");
const QString kUPowerDevice = QStringLiteral("org.freedesktop.UPower.Device");
const QString kDisplayDevice = QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice");
const QString kLogind = QStringLiteral("org.freedesktop.login1");
const QString kLogindPath = QStringLiteral("/org/freedesktop/login1");
const QString kLogindManager = QStringLiteral("org.freedesktop.login1.Manager");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");

struct ProfileService
{
    QString service, path, interface;
};
const QList<ProfileService> &profileServices()
{
    static const QList<ProfileService> list = {
        {QStringLiteral("org.freedesktop.UPower.PowerProfiles"), QStringLiteral("/org/freedesktop/UPower/PowerProfiles"),
         QStringLiteral("org.freedesktop.UPower.PowerProfiles")},
        {QStringLiteral("net.hadess.PowerProfiles"), QStringLiteral("/net/hadess/PowerProfiles"),
         QStringLiteral("net.hadess.PowerProfiles")},
    };
    return list;
}

// Defaults of ~/.config/shadow/power.conf — keep in sync with the header.
const QMap<QString, int> &idleDefaults()
{
    static const QMap<QString, int> defaults = {
        {QStringLiteral("IdleScreenOffMinutes"), 10},
        {QStringLiteral("IdleLockMinutes"), 15},
        {QStringLiteral("IdleSuspendMinutes"), 0},
        {QStringLiteral("IdleScreenOffMinutesOnBattery"), 5},
        {QStringLiteral("IdleLockMinutesOnBattery"), 10},
        {QStringLiteral("IdleSuspendMinutesOnBattery"), 20},
    };
    return defaults;
}

QSettings powerSettings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("shadow"), QStringLiteral("power"));
}

QString readSysfs(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLatin1(file.read(256)).trimmed();
}

// org.freedesktop.UPower.Device State
QString batteryStateId(uint state)
{
    switch (state) {
    case 1: return QStringLiteral("charging");
    case 2: return QStringLiteral("discharging");
    case 3: return QStringLiteral("empty");
    case 4: return QStringLiteral("fully-charged");
    case 5: return QStringLiteral("pending-charge");
    case 6: return QStringLiteral("pending-discharge");
    default: return QStringLiteral("unknown");
    }
}

QString batteryStateText(uint state)
{
    switch (state) {
    case 1: return PowerBackend::tr("Charging");
    case 2: return PowerBackend::tr("Discharging");
    case 3: return PowerBackend::tr("Empty");
    case 4: return PowerBackend::tr("Fully charged");
    case 5: return PowerBackend::tr("Plugged in, not charging");
    case 6: return PowerBackend::tr("On battery, idle");
    default: return PowerBackend::tr("Unknown state");
    }
}

// org.freedesktop.UPower.Device Type
QString deviceKind(uint type)
{
    switch (type) {
    case 1: return PowerBackend::tr("Power adapter");
    case 2: return PowerBackend::tr("Battery");
    case 3: return PowerBackend::tr("UPS");
    case 4: return PowerBackend::tr("Monitor");
    case 5: return PowerBackend::tr("Mouse");
    case 6: return PowerBackend::tr("Keyboard");
    case 7: return PowerBackend::tr("PDA");
    case 8: return PowerBackend::tr("Phone");
    case 9: return PowerBackend::tr("Media player");
    case 10: return PowerBackend::tr("Tablet");
    case 11: return PowerBackend::tr("Computer");
    case 12: return PowerBackend::tr("Game controller");
    case 13: return PowerBackend::tr("Pen");
    case 14: return PowerBackend::tr("Touchpad");
    case 15: return PowerBackend::tr("Modem");
    case 16: return PowerBackend::tr("Network device");
    case 17: return PowerBackend::tr("Headset");
    case 18: return PowerBackend::tr("Speakers");
    case 19: return PowerBackend::tr("Headphones");
    case 20: return PowerBackend::tr("Video device");
    case 21: return PowerBackend::tr("Audio device");
    case 22: return PowerBackend::tr("Remote control");
    case 23: return PowerBackend::tr("Printer");
    case 24: return PowerBackend::tr("Scanner");
    case 25: return PowerBackend::tr("Camera");
    case 26: return PowerBackend::tr("Wearable");
    case 27: return PowerBackend::tr("Toy");
    case 28: return PowerBackend::tr("Bluetooth device");
    default: return PowerBackend::tr("Device");
    }
}

QString deviceIcon(uint type)
{
    switch (type) {
    case 1: return QStringLiteral("plug");
    case 3: return QStringLiteral("plug");
    case 4: case 20: return QStringLiteral("monitor");
    case 5: return QStringLiteral("mouse");
    case 6: return QStringLiteral("keyboard");
    case 11: return QStringLiteral("laptop");
    case 14: return QStringLiteral("touchpad");
    case 16: return QStringLiteral("network");
    case 17: case 18: case 19: case 21: return QStringLiteral("music");
    case 23: return QStringLiteral("printer");
    case 25: return QStringLiteral("camera");
    case 28: return QStringLiteral("bluetooth");
    default: return QStringLiteral("battery");
    }
}

// org.freedesktop.UPower.Device Technology
QString technologyText(uint technology)
{
    switch (technology) {
    case 1: return PowerBackend::tr("Lithium-ion");
    case 2: return PowerBackend::tr("Lithium-polymer");
    case 3: return PowerBackend::tr("Lithium iron phosphate");
    case 4: return PowerBackend::tr("Lead-acid");
    case 5: return PowerBackend::tr("Nickel-cadmium");
    case 6: return PowerBackend::tr("Nickel-metal hydride");
    default: return QString();
    }
}

// org.freedesktop.UPower.Device BatteryLevel (coarse reporting)
QString batteryLevelText(uint level)
{
    switch (level) {
    case 3: return PowerBackend::tr("Low");
    case 4: return PowerBackend::tr("Critical");
    case 6: return PowerBackend::tr("Normal");
    case 7: return PowerBackend::tr("High");
    case 8: return PowerBackend::tr("Full");
    default: return QString();
    }
}

QString deviceName(const QVariantMap &p)
{
    const QString vendor = p.value(QStringLiteral("Vendor")).toString().trimmed();
    const QString model = p.value(QStringLiteral("Model")).toString().trimmed();
    if (!model.isEmpty() && !vendor.isEmpty() && !model.startsWith(vendor, Qt::CaseInsensitive))
        return vendor + QLatin1Char(' ') + model;
    if (!model.isEmpty())
        return model;
    if (!vendor.isEmpty())
        return vendor;
    return QString();
}
} // namespace

PowerBackend::PowerBackend(QObject *parent)
    : QObject(parent)
{
    m_batteries = new RowModel({QStringLiteral("path"), QStringLiteral("name"), QStringLiteral("nativePath"),
                                QStringLiteral("percentage"), QStringLiteral("stateId"), QStringLiteral("stateText"),
                                QStringLiteral("energy"), QStringLiteral("energyFull"), QStringLiteral("energyFullDesign"),
                                QStringLiteral("energyRate"), QStringLiteral("voltage"), QStringLiteral("capacity"),
                                QStringLiteral("chargeCycles"), QStringLiteral("technology"),
                                QStringLiteral("timeToEmpty"), QStringLiteral("timeToFull"), QStringLiteral("serial")},
                               QStringLiteral("path"), this);
    m_peripherals = new RowModel({QStringLiteral("path"), QStringLiteral("name"), QStringLiteral("kind"),
                                  QStringLiteral("deviceIcon"), QStringLiteral("percentage"), QStringLiteral("hasPercentage"),
                                  QStringLiteral("levelText"), QStringLiteral("stateId"), QStringLiteral("stateText")},
                                 QStringLiteral("path"), this);

    for (QTimer *timer : {&m_upowerTimer, &m_profilesTimer}) {
        timer->setSingleShot(true);
        timer->setInterval(400);
    }
    connect(&m_upowerTimer, &QTimer::timeout, this, &PowerBackend::reloadUPower);
    connect(&m_profilesTimer, &QTimer::timeout, this, [this] { reloadProfiles(); });

    // Panel brightness has no change notification in sysfs; poll modestly.
    // The page (and with it this object) only exists while it is shown.
    m_backlightTimer.setInterval(3000);
    connect(&m_backlightTimer, &QTimer::timeout, this, &PowerBackend::readBacklight);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        bus.connect(kUPower, QString(), kProperties, QStringLiteral("PropertiesChanged"), this, SLOT(scheduleUPowerReload()));
        bus.connect(kUPower, kUPowerPath, kUPower, QStringLiteral("DeviceAdded"), this, SLOT(scheduleUPowerReload()));
        bus.connect(kUPower, kUPowerPath, kUPower, QStringLiteral("DeviceRemoved"), this, SLOT(scheduleUPowerReload()));
        for (const ProfileService &candidate : profileServices()) {
            bus.connect(candidate.service, candidate.path, kProperties, QStringLiteral("PropertiesChanged"),
                        this, SLOT(scheduleProfilesReload()));
        }
    }

    loadSettings();
    readBacklight();
    if (brightnessAvailable())
        m_backlightTimer.start();

    m_pendingSteps = 3;
    reloadUPower();
    reloadProfiles();
    reloadLogind();
}

QAbstractItemModel *PowerBackend::batteries() const { return m_batteries; }
int PowerBackend::batteryCount() const { return m_batteries->count(); }
QAbstractItemModel *PowerBackend::peripherals() const { return m_peripherals; }
int PowerBackend::peripheralCount() const { return m_peripherals->count(); }

void PowerBackend::stepDone()
{
    if (m_pendingSteps > 0 && --m_pendingSteps == 0 && !m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
}

void PowerBackend::refresh()
{
    loadSettings();
    readBacklight();
    reloadUPower();
    reloadProfiles();
    reloadLogind();
}

void PowerBackend::scheduleUPowerReload()
{
    if (!m_upowerTimer.isActive())
        m_upowerTimer.start();
}

void PowerBackend::scheduleProfilesReload()
{
    if (!m_profilesTimer.isActive())
        m_profilesTimer.start();
}

// ---- UPower ------------------------------------------------------------------

void PowerBackend::clearUPower(const QString &state, const QString &message)
{
    m_batteries->setRows({});
    m_peripherals->setRows({});
    m_battery.clear();
    m_linePower.clear();
    m_hasBattery = false;
    m_onBattery = false;
    m_lidPresent = false;
    m_daemonVersion.clear();
    Q_EMIT batteryChanged();
    if (state != m_state || message != m_statusMessage) {
        m_state = state;
        m_statusMessage = message;
        Q_EMIT stateChanged();
    }
}

void PowerBackend::reloadUPower()
{
    const quint64 generation = ++m_upowerGeneration;
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        QTimer::singleShot(0, this, [this, generation] {
            if (generation != m_upowerGeneration)
                return;
            clearUPower(QStringLiteral("no-system-bus"),
                        tr("The system message bus is not available, so UPower cannot be asked about batteries."));
            stepDone();
        });
        return;
    }

    dbusGetAll(bus, kUPower, kUPowerPath, kUPower, this,
               [this, bus, generation](bool ok, const QVariantMap &daemon, const QString &error) {
        if (generation != m_upowerGeneration)
            return;
        if (!ok) {
            clearUPower(QStringLiteral("upower-unavailable"),
                        tr("UPower does not answer (%1). Battery information is provided by the upower service.").arg(error));
            stepDone();
            return;
        }
        dbusCall(bus, dbusMethod(kUPower, kUPowerPath, kUPower, QStringLiteral("EnumerateDevices")), this,
                 [this, bus, daemon, generation](const QDBusMessage &reply) {
            if (generation != m_upowerGeneration)
                return;
            QStringList paths;
            if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
                paths = dbusPlain(reply.arguments().constFirst()).toStringList();
            paths.append(kDisplayDevice);

            auto pending = std::make_shared<int>(int(paths.size()));
            auto devices = std::make_shared<QMap<QString, QVariantMap>>();
            for (const QString &path : std::as_const(paths)) {
                dbusGetAll(bus, kUPower, path, kUPowerDevice, this,
                           [this, path, pending, devices, daemon, generation](bool got, const QVariantMap &properties, const QString &) {
                    if (got)
                        devices->insert(path, properties);
                    if (--*pending > 0 || generation != m_upowerGeneration)
                        return;
                    publishUPower(daemon, *devices);
                    stepDone();
                });
            }
        });
    });
}

void PowerBackend::publishUPower(const QVariantMap &daemon, const QMap<QString, QVariantMap> &devices)
{
    m_onBattery = daemon.value(QStringLiteral("OnBattery")).toBool();
    m_lidPresent = daemon.value(QStringLiteral("LidIsPresent")).toBool();
    m_daemonVersion = daemon.value(QStringLiteral("DaemonVersion")).toString();

    QVariantList batteries, peripherals;
    m_linePower.clear();
    for (auto it = devices.constBegin(); it != devices.constEnd(); ++it) {
        if (it.key() == kDisplayDevice)
            continue;
        const QVariantMap &p = it.value();
        const uint type = p.value(QStringLiteral("Type")).toUInt();
        const uint state = p.value(QStringLiteral("State")).toUInt();
        const QString nativePath = p.value(QStringLiteral("NativePath")).toString();
        if (type == 1) {
            m_linePower.append(QVariantMap{
                {QStringLiteral("nativePath"), nativePath},
                {QStringLiteral("online"), p.value(QStringLiteral("Online")).toBool()},
            });
            continue;
        }
        if (!p.value(QStringLiteral("IsPresent")).toBool())
            continue;
        QString name = deviceName(p);
        if (p.value(QStringLiteral("PowerSupply")).toBool() && type == 2) {
            if (name.isEmpty())
                name = nativePath.isEmpty() ? tr("Battery") : nativePath;
            batteries.append(QVariantMap{
                {QStringLiteral("path"), it.key()},
                {QStringLiteral("name"), name},
                {QStringLiteral("nativePath"), nativePath},
                {QStringLiteral("percentage"), p.value(QStringLiteral("Percentage")).toDouble()},
                {QStringLiteral("stateId"), batteryStateId(state)},
                {QStringLiteral("stateText"), batteryStateText(state)},
                {QStringLiteral("energy"), p.value(QStringLiteral("Energy")).toDouble()},
                {QStringLiteral("energyFull"), p.value(QStringLiteral("EnergyFull")).toDouble()},
                {QStringLiteral("energyFullDesign"), p.value(QStringLiteral("EnergyFullDesign")).toDouble()},
                {QStringLiteral("energyRate"), p.value(QStringLiteral("EnergyRate")).toDouble()},
                {QStringLiteral("voltage"), p.value(QStringLiteral("Voltage")).toDouble()},
                {QStringLiteral("capacity"), p.value(QStringLiteral("Capacity")).toDouble()},
                {QStringLiteral("chargeCycles"), p.value(QStringLiteral("ChargeCycles"), -1).toInt()},
                {QStringLiteral("technology"), technologyText(p.value(QStringLiteral("Technology")).toUInt())},
                {QStringLiteral("timeToEmpty"), p.value(QStringLiteral("TimeToEmpty")).toLongLong()},
                {QStringLiteral("timeToFull"), p.value(QStringLiteral("TimeToFull")).toLongLong()},
                {QStringLiteral("serial"), p.value(QStringLiteral("Serial")).toString()},
            });
        } else {
            // Batteries of peripherals: mice, keyboards, headsets, phones, UPS units …
            const uint level = p.value(QStringLiteral("BatteryLevel")).toUInt();
            const bool coarse = level > 1;      // 0 = unknown, 1 = none (percentage is valid)
            if (name.isEmpty())
                name = deviceKind(type);
            peripherals.append(QVariantMap{
                {QStringLiteral("path"), it.key()},
                {QStringLiteral("name"), name},
                {QStringLiteral("kind"), deviceKind(type)},
                {QStringLiteral("deviceIcon"), deviceIcon(type)},
                {QStringLiteral("percentage"), p.value(QStringLiteral("Percentage")).toDouble()},
                {QStringLiteral("hasPercentage"), !coarse},
                {QStringLiteral("levelText"), coarse ? batteryLevelText(level) : QString()},
                {QStringLiteral("stateId"), batteryStateId(state)},
                {QStringLiteral("stateText"), batteryStateText(state)},
            });
        }
    }
    m_batteries->setRows(batteries);
    m_peripherals->setRows(peripherals);

    QVariantMap battery;
    const QVariantMap display = devices.value(kDisplayDevice);
    const uint displayType = display.value(QStringLiteral("Type")).toUInt();
    m_hasBattery = display.value(QStringLiteral("IsPresent")).toBool() && (displayType == 2 || displayType == 3);
    if (m_hasBattery) {
        const uint state = display.value(QStringLiteral("State")).toUInt();
        battery = {
            {QStringLiteral("percentage"), display.value(QStringLiteral("Percentage")).toDouble()},
            {QStringLiteral("stateId"), batteryStateId(state)},
            {QStringLiteral("stateText"), batteryStateText(state)},
            {QStringLiteral("timeToEmpty"), display.value(QStringLiteral("TimeToEmpty")).toLongLong()},
            {QStringLiteral("timeToFull"), display.value(QStringLiteral("TimeToFull")).toLongLong()},
            {QStringLiteral("energyRate"), display.value(QStringLiteral("EnergyRate")).toDouble()},
            {QStringLiteral("energy"), display.value(QStringLiteral("Energy")).toDouble()},
            {QStringLiteral("energyFull"), display.value(QStringLiteral("EnergyFull")).toDouble()},
            {QStringLiteral("warningLevel"), display.value(QStringLiteral("WarningLevel")).toUInt()},
            {QStringLiteral("isUps"), displayType == 3},
            {QStringLiteral("icon"), state == 1 || state == 4 || state == 5 ? QStringLiteral("battery-charging")
                                                                            : QStringLiteral("battery")},
        };
    }
    m_battery = battery;
    Q_EMIT batteryChanged();

    if (m_state != QLatin1String("ok") || !m_statusMessage.isEmpty()) {
        m_state = QStringLiteral("ok");
        m_statusMessage.clear();
        Q_EMIT stateChanged();
    }
}

// ---- power profiles --------------------------------------------------------------

void PowerBackend::reloadProfiles(int candidate)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    const auto unavailable = [this](const QString &message) {
        m_profilesAvailable = false;
        m_profilesStatus = message;
        m_profilesService.clear();
        m_profiles.clear();
        m_activeProfile.clear();
        m_performanceDegraded.clear();
        m_profileHolds.clear();
        m_profilesVersion.clear();
        Q_EMIT profilesChanged();
    };
    if (!bus.isConnected()) {
        QTimer::singleShot(0, this, [this, unavailable] {
            unavailable(tr("The system message bus is not available."));
            stepDone();
        });
        return;
    }
    if (candidate >= profileServices().size()) {
        unavailable(tr("power-profiles-daemon is not installed or not running, so power profiles cannot be selected."));
        stepDone();
        return;
    }

    const ProfileService service = profileServices().at(candidate);
    dbusGetAll(bus, service.service, service.path, service.interface, this,
               [this, service, candidate](bool ok, const QVariantMap &properties, const QString &) {
        if (!ok) {
            reloadProfiles(candidate + 1);
            return;
        }
        QVariantList profiles;
        const QVariantList raw = dbusPlain(properties.value(QStringLiteral("Profiles"))).toList();
        for (const QVariant &entry : raw) {
            const QVariantMap map = entry.toMap();
            const QString id = map.value(QStringLiteral("Profile")).toString();
            if (id.isEmpty())
                continue;
            QString label = id, description;
            if (id == QLatin1String("power-saver")) {
                label = tr("Power saver");
                description = tr("Longest battery life, reduced performance");
            } else if (id == QLatin1String("balanced")) {
                label = tr("Balanced");
                description = tr("Performance when needed, efficient otherwise");
            } else if (id == QLatin1String("performance")) {
                label = tr("Performance");
                description = tr("Highest performance, more heat and energy use");
            }
            QStringList drivers;
            for (const QString &key : {QStringLiteral("Driver"), QStringLiteral("CpuDriver"), QStringLiteral("PlatformDriver")}) {
                const QString driver = map.value(key).toString();
                if (!driver.isEmpty() && !drivers.contains(driver))
                    drivers.append(driver);
            }
            profiles.append(QVariantMap{
                {QStringLiteral("id"), id},
                {QStringLiteral("label"), label},
                {QStringLiteral("description"), description},
                {QStringLiteral("driver"), drivers.join(QStringLiteral(", "))},
            });
        }
        QVariantList holds;
        const QVariantList rawHolds = dbusPlain(properties.value(QStringLiteral("ActiveProfileHolds"))).toList();
        for (const QVariant &entry : rawHolds) {
            const QVariantMap map = entry.toMap();
            holds.append(QVariantMap{
                {QStringLiteral("application"), map.value(QStringLiteral("ApplicationId")).toString()},
                {QStringLiteral("profile"), map.value(QStringLiteral("Profile")).toString()},
                {QStringLiteral("reason"), map.value(QStringLiteral("Reason")).toString()},
            });
        }
        // PerformanceInhibited is the pre-0.9 name of the same information.
        QString degraded = properties.value(QStringLiteral("PerformanceDegraded")).toString();
        if (degraded.isEmpty())
            degraded = properties.value(QStringLiteral("PerformanceInhibited")).toString();

        m_profilesAvailable = !profiles.isEmpty();
        m_profilesStatus = profiles.isEmpty() ? tr("power-profiles-daemon reports no profiles for this hardware.") : QString();
        m_profilesService = service.service;
        m_profilesPath = service.path;
        m_profilesInterface = service.interface;
        m_profiles = profiles;
        m_activeProfile = properties.value(QStringLiteral("ActiveProfile")).toString();
        m_performanceDegraded = degraded;
        m_profileHolds = holds;
        m_profilesVersion = properties.value(QStringLiteral("Version")).toString();
        Q_EMIT profilesChanged();
        stepDone();
    });
}

QString PowerBackend::performanceDegradedText() const
{
    if (m_performanceDegraded.isEmpty())
        return QString();
    if (m_performanceDegraded == QLatin1String("lap-detected"))
        return tr("Performance is reduced because the computer is resting on a lap.");
    if (m_performanceDegraded == QLatin1String("high-operating-temperature"))
        return tr("Performance is reduced because the computer is running hot.");
    return tr("Performance is reduced (%1).").arg(m_performanceDegraded);
}

void PowerBackend::setActiveProfile(const QString &profile)
{
    bool known = false;
    for (const QVariant &entry : std::as_const(m_profiles))
        known = known || entry.toMap().value(QStringLiteral("id")).toString() == profile;
    if (!m_profilesAvailable || !known) {
        Q_EMIT failed(tr("The power profile “%1” is not available.").arg(profile));
        return;
    }
    dbusSet(QDBusConnection::systemBus(), m_profilesService, m_profilesPath, m_profilesInterface,
            QStringLiteral("ActiveProfile"), profile, this, [this](bool ok, const QString &error) {
        if (!ok)
            Q_EMIT failed(tr("The power profile could not be changed: %1").arg(error));
        reloadProfiles();
    });
}

// ---- brightness --------------------------------------------------------------------

void PowerBackend::readBacklight()
{
    // Like systemd-backlight: prefer firmware, then platform, then raw drivers.
    const QDir dir(QStringLiteral("/sys/class/backlight"));
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QString name, type;
    int rank = 99;
    for (const QString &entry : entries) {
        const QString entryType = readSysfs(dir.absoluteFilePath(entry) + QStringLiteral("/type"));
        const int entryRank = entryType == QLatin1String("firmware") ? 0
                            : entryType == QLatin1String("platform") ? 1
                            : entryType == QLatin1String("raw") ? 2 : 3;
        if (entryRank < rank) {
            rank = entryRank;
            name = entry;
            type = entryType;
        }
    }
    int max = 0, value = 0;
    if (!name.isEmpty()) {
        const QString base = dir.absoluteFilePath(name);
        max = readSysfs(base + QStringLiteral("/max_brightness")).toInt();
        value = readSysfs(base + QStringLiteral("/brightness")).toInt();
    }
    if (name != m_backlightName || max != m_backlightMax || value != m_backlightValue) {
        m_backlightName = name;
        m_backlightType = type;
        m_backlightMax = max;
        m_backlightValue = value;
        Q_EMIT brightnessChanged();
    }
}

int PowerBackend::brightnessPercent() const
{
    if (m_backlightMax <= 0)
        return 0;
    return qBound(0, qRound(m_backlightValue * 100.0 / m_backlightMax), 100);
}

void PowerBackend::setBrightness(int percent)
{
    if (!brightnessAvailable())
        return;
    percent = qBound(1, percent, 100);          // never switch the panel fully off
    const uint raw = uint(qMax(1, qRound(m_backlightMax * percent / 100.0)));
    const QDBusMessage call = dbusMethod(kLogind, QStringLiteral("/org/freedesktop/login1/session/auto"),
                                         QStringLiteral("org.freedesktop.login1.Session"), QStringLiteral("SetBrightness"),
                                         {QStringLiteral("backlight"), m_backlightName, QVariant::fromValue(raw)});
    dbusCall(QDBusConnection::systemBus(), call, this, [this](const QDBusMessage &reply) {
        if (reply.type() != QDBusMessage::ReplyMessage)
            Q_EMIT failed(tr("The brightness could not be changed: %1").arg(dbusErrorText(reply)));
        readBacklight();
    });
}

// ---- logind --------------------------------------------------------------------------

bool PowerBackend::suspendUsable() const
{
    return m_canSuspend == QLatin1String("yes") || m_canSuspend == QLatin1String("challenge");
}

QString PowerBackend::capabilityText(const QString &answer) const
{
    if (answer == QLatin1String("yes"))
        return tr("Available");
    if (answer == QLatin1String("challenge"))
        return tr("Available after authentication");
    if (answer == QLatin1String("no"))
        return tr("Not permitted");
    if (answer == QLatin1String("na"))
        return tr("Not supported by this hardware or configuration");
    return tr("Unknown — systemd-logind is not reachable");
}

QString PowerBackend::actionText(const QString &action) const
{
    if (action == QLatin1String("ignore"))
        return tr("Do nothing");
    if (action == QLatin1String("poweroff"))
        return tr("Power off");
    if (action == QLatin1String("reboot"))
        return tr("Restart");
    if (action == QLatin1String("halt"))
        return tr("Halt");
    if (action == QLatin1String("kexec"))
        return tr("Restart (kexec)");
    if (action == QLatin1String("suspend"))
        return tr("Suspend");
    if (action == QLatin1String("hibernate"))
        return tr("Hibernate");
    if (action == QLatin1String("hybrid-sleep"))
        return tr("Hybrid sleep");
    if (action == QLatin1String("suspend-then-hibernate"))
        return tr("Suspend, then hibernate");
    if (action == QLatin1String("sleep"))
        return tr("Sleep");
    if (action == QLatin1String("lock"))
        return tr("Lock the screen");
    return action;
}

void PowerBackend::reloadLogind()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    const auto fallback = [this] {
        m_logindAvailable = false;
        m_canSuspend = QStringLiteral("unknown");
        m_canHibernate = QStringLiteral("unknown");
        readLidActionsFromFiles();
        Q_EMIT logindChanged();
        stepDone();
    };
    if (!bus.isConnected()) {
        QTimer::singleShot(0, this, fallback);
        return;
    }
    dbusGetAll(bus, kLogind, kLogindPath, kLogindManager, this,
               [this, bus, fallback](bool ok, const QVariantMap &properties, const QString &) {
        if (!ok) {
            fallback();
            return;
        }
        m_logindAvailable = true;
        m_lidAction = properties.value(QStringLiteral("HandleLidSwitch")).toString();
        m_lidActionExternalPower = properties.value(QStringLiteral("HandleLidSwitchExternalPower")).toString();
        m_lidActionDocked = properties.value(QStringLiteral("HandleLidSwitchDocked")).toString();
        m_lidActionSource = QStringLiteral("logind");
        dbusCall(bus, dbusMethod(kLogind, kLogindPath, kLogindManager, QStringLiteral("CanSuspend")), this,
                 [this, bus](const QDBusMessage &suspend) {
            m_canSuspend = suspend.type() == QDBusMessage::ReplyMessage && !suspend.arguments().isEmpty()
                               ? suspend.arguments().constFirst().toString() : QStringLiteral("unknown");
            dbusCall(bus, dbusMethod(kLogind, kLogindPath, kLogindManager, QStringLiteral("CanHibernate")), this,
                     [this](const QDBusMessage &hibernate) {
                m_canHibernate = hibernate.type() == QDBusMessage::ReplyMessage && !hibernate.arguments().isEmpty()
                                     ? hibernate.arguments().constFirst().toString() : QStringLiteral("unknown");
                Q_EMIT logindChanged();
                stepDone();
            });
        });
    });
}

void PowerBackend::readLidActionsFromFiles()
{
    // logind.conf(5): the main file, then drop-ins in lexical order of their
    // file name; /etc overrides /run overrides /usr/lib for the same name.
    QStringList files;
    for (const QString &main : {QStringLiteral("/usr/lib/systemd/logind.conf"), QStringLiteral("/etc/systemd/logind.conf")}) {
        if (QFile::exists(main))
            files.append(main);
    }
    QMap<QString, QString> dropIns;         // file name → path (highest priority directory wins)
    for (const QString &dirPath : {QStringLiteral("/usr/lib/systemd/logind.conf.d"), QStringLiteral("/run/systemd/logind.conf.d"),
                                   QStringLiteral("/etc/systemd/logind.conf.d")}) {
        const QDir dir(dirPath);
        const QStringList names = dir.entryList({QStringLiteral("*.conf")}, QDir::Files, QDir::Name);
        for (const QString &name : names)
            dropIns.insert(name, dir.absoluteFilePath(name));
    }
    files.append(dropIns.values());

    QMap<QString, QString> values;
    static const QRegularExpression assignment(QStringLiteral("^\\s*(HandleLidSwitch(?:ExternalPower|Docked)?)\\s*=\\s*(\\S*)"));
    for (const QString &path : std::as_const(files)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QString section;
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')) || line.isEmpty())
                continue;
            if (line.startsWith(QLatin1Char('['))) {
                section = line;
                continue;
            }
            if (section != QLatin1String("[Login]"))
                continue;
            const QRegularExpressionMatch match = assignment.match(line);
            if (match.hasMatch())
                values.insert(match.captured(1), match.captured(2));
        }
    }

    // systemd's built-in defaults (logind.conf(5)).
    const bool configured = !values.isEmpty();
    m_lidAction = values.value(QStringLiteral("HandleLidSwitch"), QStringLiteral("suspend"));
    m_lidActionExternalPower = values.value(QStringLiteral("HandleLidSwitchExternalPower"), m_lidAction);
    m_lidActionDocked = values.value(QStringLiteral("HandleLidSwitchDocked"), QStringLiteral("ignore"));
    m_lidActionSource = configured ? QStringLiteral("logind.conf") : QStringLiteral("default");
}

// ---- ~/.config/shadow/power.conf -----------------------------------------------------

QString PowerBackend::settingsFile() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/shadow/power.conf");
}

void PowerBackend::loadSettings()
{
    QSettings settings = powerSettings();
    settings.sync();
    settings.beginGroup(QStringLiteral("Power"));
    QMap<QString, int> idle;
    const QMap<QString, int> &defaults = idleDefaults();
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        bool ok = false;
        int minutes = settings.value(it.key(), it.value()).toInt(&ok);
        if (!ok || minutes < 0 || minutes > 1440)
            minutes = it.value();
        idle.insert(it.key(), minutes);
    }
    if (idle != m_idle) {
        m_idle = idle;
        Q_EMIT settingsChanged();
    }
}

bool PowerBackend::setIdleMinutes(const QString &key, int minutes)
{
    if (!idleDefaults().contains(key)) {
        Q_EMIT failed(tr("“%1” is not a power setting.").arg(key));
        return false;
    }
    if (minutes < 0 || minutes > 1440) {
        Q_EMIT failed(tr("The time must be between 0 (never) and 1440 minutes."));
        return false;
    }
    if (m_idle.value(key) == minutes)
        return true;
    QSettings settings = powerSettings();
    settings.beginGroup(QStringLiteral("Power"));
    settings.setValue(key, minutes);
    settings.endGroup();
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        Q_EMIT failed(tr("%1 could not be written.").arg(settingsFile()));
        return false;
    }
    m_idle.insert(key, minutes);
    Q_EMIT settingsChanged();
    return true;
}

QJsonObject PowerBackend::toJson() const
{
    QJsonObject idle;
    for (auto it = m_idle.constBegin(); it != m_idle.constEnd(); ++it)
        idle.insert(it.key(), it.value());

    return {
        {QStringLiteral("state"), m_state},
        {QStringLiteral("available"), available()},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("upowerVersion"), m_daemonVersion},
        {QStringLiteral("onBattery"), m_onBattery},
        {QStringLiteral("hasBattery"), m_hasBattery},
        {QStringLiteral("lidPresent"), m_lidPresent},
        {QStringLiteral("battery"), QJsonObject::fromVariantMap(m_battery)},
        {QStringLiteral("batteries"), QJsonArray::fromVariantList(m_batteries->rows())},
        {QStringLiteral("peripherals"), QJsonArray::fromVariantList(m_peripherals->rows())},
        {QStringLiteral("linePower"), QJsonArray::fromVariantList(m_linePower)},
        {QStringLiteral("profiles"), QJsonObject{
            {QStringLiteral("available"), m_profilesAvailable},
            {QStringLiteral("status"), m_profilesStatus},
            {QStringLiteral("service"), m_profilesService},
            {QStringLiteral("version"), m_profilesVersion},
            {QStringLiteral("active"), m_activeProfile},
            {QStringLiteral("list"), QJsonArray::fromVariantList(m_profiles)},
            {QStringLiteral("performanceDegraded"), m_performanceDegraded},
            {QStringLiteral("holds"), QJsonArray::fromVariantList(m_profileHolds)},
        }},
        {QStringLiteral("brightness"), QJsonObject{
            {QStringLiteral("available"), brightnessAvailable()},
            {QStringLiteral("device"), m_backlightName},
            {QStringLiteral("type"), m_backlightType},
            {QStringLiteral("value"), m_backlightValue},
            {QStringLiteral("max"), m_backlightMax},
            {QStringLiteral("percent"), brightnessPercent()},
        }},
        {QStringLiteral("logind"), QJsonObject{
            {QStringLiteral("available"), m_logindAvailable},
            {QStringLiteral("canSuspend"), m_canSuspend},
            {QStringLiteral("canHibernate"), m_canHibernate},
            {QStringLiteral("lidAction"), m_lidAction},
            {QStringLiteral("lidActionExternalPower"), m_lidActionExternalPower},
            {QStringLiteral("lidActionDocked"), m_lidActionDocked},
            {QStringLiteral("lidActionSource"), m_lidActionSource},
        }},
        {QStringLiteral("settingsFile"), settingsFile()},
        {QStringLiteral("settings"), idle},
    };
}
