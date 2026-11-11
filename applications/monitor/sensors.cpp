// SPDX-License-Identifier: GPL-3.0-or-later
#include "sensors.h"

#include "sysfs.h"

#include <algorithm>
#include <cmath>
#include <optional>

using namespace Qt::StringLiterals;

namespace {

QVariant nullValue()
{
    return QVariant::fromValue(nullptr);
}

QVariant optional(bool available, double value)
{
    return available ? QVariant(value) : nullValue();
}

double round1(double v)
{
    return std::round(v * 10.0) / 10.0;
}

double round2(double v)
{
    return std::round(v * 100.0) / 100.0;
}

// A limit the driver left unset reads 0 or an absurd number; only plausible
// limits are used to grade a reading.
bool plausibleLimit(double celsius)
{
    return celsius > 0 && celsius < 250;
}

// "<prefix><number>_input" → number, otherwise -1.
int channelNumber(const QString &entry, QLatin1StringView prefix)
{
    if (!entry.startsWith(prefix) || !entry.endsWith("_input"_L1))
        return -1;
    bool ok = false;
    const int n = QStringView(entry).mid(prefix.size(), entry.size() - prefix.size() - 6).toInt(&ok);
    return ok ? n : -1;
}

} // namespace

// ---- temperatures and fans ------------------------------------------------------

QString TemperatureReading::level() const
{
    if (hasCritical && celsius >= critical)
        return u"critical"_s;
    if (hasHigh && celsius >= high)
        return u"high"_s;
    return u"normal"_s;
}

QVariantMap TemperatureReading::toVariant() const
{
    return {
        {u"chip"_s, chip},
        {u"device"_s, device},
        {u"label"_s, label},
        {u"celsius"_s, round1(celsius)},
        {u"high"_s, optional(hasHigh, round1(high))},
        {u"critical"_s, optional(hasCritical, round1(critical))},
        {u"level"_s, level()},
    };
}

QVariantMap FanReading::toVariant() const
{
    return {
        {u"chip"_s, chip},
        {u"device"_s, device},
        {u"label"_s, label},
        {u"rpm"_s, rpm},
    };
}

QVariantMap SensorSample::toVariant() const
{
    QVariantList temps, fanList;
    for (const TemperatureReading &t : temperatures)
        temps << t.toVariant();
    for (const FanReading &f : fans)
        fanList << f.toVariant();
    return {
        {u"temperatures"_s, temps},
        {u"fans"_s, fanList},
        {u"cpuTemperatureCelsius"_s, optional(cpuTemperatureAvailable, round1(cpuTemperature))},
        {u"cpuTemperatureSource"_s, cpuTemperatureSource},
    };
}

void SensorSource::discover(const QStringList &hwmon, const QStringList &zones)
{
    m_chips.clear();
    m_zones.clear();

    for (const QString &entry : hwmon) {
        const QString path = u"/sys/class/hwmon/"_s + entry;
        // The thermal core registers a hwmon device for each zone type; the
        // zones themselves are read below.
        if (Sys::canonicalPath(path).contains("/thermal_zone"_L1))
            continue;

        Chip chip;
        chip.name = Sys::readText(path + u"/name"_s);
        if (chip.name.isEmpty())
            chip.name = entry;
        chip.device = Sys::linkName(path + u"/device"_s);
        chip.runtimeStatusPath = path + u"/device/power/runtime_status"_s;

        const QStringList attributes = Sys::listDirectory(path);
        for (const QString &attribute : attributes) {
            if (const int n = channelNumber(attribute, "temp"_L1); n >= 0) {
                Channel c;
                c.inputPath = path + u'/' + attribute;
                c.label = Sys::readText(path + u"/temp%1_label"_s.arg(n));
                if (c.label.isEmpty())
                    c.label = u"temp%1"_s.arg(n);
                if (const auto max = Sys::readInteger(path + u"/temp%1_max"_s.arg(n)); max && plausibleLimit(double(*max) / 1000.0)) {
                    c.hasHigh = true;
                    c.high = double(*max) / 1000.0;
                }
                if (const auto crit = Sys::readInteger(path + u"/temp%1_crit"_s.arg(n)); crit && plausibleLimit(double(*crit) / 1000.0)) {
                    c.hasCritical = true;
                    c.critical = double(*crit) / 1000.0;
                }
                chip.temperatures << c;
            } else if (const int f = channelNumber(attribute, "fan"_L1); f >= 0) {
                Channel c;
                c.inputPath = path + u'/' + attribute;
                c.label = Sys::readText(path + u"/fan%1_label"_s.arg(f));
                if (c.label.isEmpty())
                    c.label = u"fan%1"_s.arg(f);
                chip.fans << c;
            }
        }
        if (!chip.temperatures.isEmpty() || !chip.fans.isEmpty())
            m_chips << chip;
    }

    for (const QString &entry : zones) {
        if (!entry.startsWith("thermal_zone"_L1))
            continue;
        const QString path = u"/sys/class/thermal/"_s + entry;
        Zone zone;
        zone.tempPath = path + u"/temp"_s;
        zone.type = Sys::readText(path + u"/type"_s);
        if (zone.type.isEmpty())
            zone.type = entry;
        zone.device = entry;
        for (int i = 0; i < 16; ++i) {
            const QString type = Sys::readText(path + u"/trip_point_%1_type"_s.arg(i));
            if (type.isEmpty())
                break;
            if (type != "critical"_L1)
                continue;
            if (const auto t = Sys::readInteger(path + u"/trip_point_%1_temp"_s.arg(i)); t && plausibleLimit(double(*t) / 1000.0)) {
                zone.hasCritical = true;
                zone.critical = double(*t) / 1000.0;
            }
            break;
        }
        m_zones << zone;
    }
}

SensorSample SensorSource::sample()
{
    // Sensors come and go (USB devices, drivers loaded later): rediscover
    // whenever the set of devices changes.
    const QStringList hwmon = Sys::listDirectory(u"/sys/class/hwmon"_s);
    const QStringList zones = Sys::listDirectory(u"/sys/class/thermal"_s);
    if (!m_discovered || hwmon != m_hwmonEntries || zones != m_zoneEntries) {
        discover(hwmon, zones);
        m_hwmonEntries = hwmon;
        m_zoneEntries = zones;
        m_discovered = true;
    }

    SensorSample s;
    for (const Chip &chip : std::as_const(m_chips)) {
        // Reading a sensor resumes a runtime-suspended device (and polling
        // would keep it awake), so a sleeping device is left alone.
        if (Sys::readText(chip.runtimeStatusPath) == "suspended"_L1)
            continue;
        for (const Channel &c : chip.temperatures) {
            // Reading fails while a sensor is not ready or unplugged; such a
            // sensor is simply not listed for this sample.
            const auto value = Sys::readInteger(c.inputPath);
            if (!value)
                continue;
            TemperatureReading t;
            t.chip = chip.name;
            t.device = chip.device;
            t.label = c.label;
            t.celsius = double(*value) / 1000.0;
            t.hasHigh = c.hasHigh;
            t.high = c.high;
            t.hasCritical = c.hasCritical;
            t.critical = c.critical;
            s.temperatures << t;
        }
        for (const Channel &c : chip.fans) {
            const auto value = Sys::readInteger(c.inputPath);
            if (!value)
                continue;
            FanReading f;
            f.chip = chip.name;
            f.device = chip.device;
            f.label = c.label;
            f.rpm = int(*value);
            s.fans << f;
        }
    }
    for (const Zone &zone : std::as_const(m_zones)) {
        const auto value = Sys::readInteger(zone.tempPath);
        if (!value)
            continue;
        TemperatureReading t;
        t.chip = u"thermal zone"_s;
        t.device = zone.device;
        t.label = zone.type;
        t.celsius = double(*value) / 1000.0;
        t.hasCritical = zone.hasCritical;
        t.critical = zone.critical;
        s.temperatures << t;
    }

    selectCpuTemperature(s);
    return s;
}

// Which reading is "the" processor temperature depends on the platform:
//   Intel   coretemp   "Package id N" (else the hottest core)
//   AMD     k10temp / zenpower   "Tdie" (else "Tctl", which may include an offset)
//   others  a thermal zone or chip named after the CPU or SoC
// With several candidates of the same rank (two sockets) the hottest is shown.
void SensorSource::selectCpuTemperature(SensorSample &s)
{
    int bestRank = 0;
    for (const TemperatureReading &t : std::as_const(s.temperatures)) {
        const QString chip = t.chip.toLower();
        const QString label = t.label.toLower();
        int rank = 0;
        if (chip == "coretemp"_L1) {
            rank = label.startsWith("package id"_L1) ? 100 : 80;
        } else if (chip == "k10temp"_L1 || chip == "zenpower"_L1) {
            rank = label == "tdie"_L1 ? 95 : label == "tctl"_L1 ? 90 : label.startsWith("temp"_L1) ? 85 : 0;
        } else if (chip == "thermal zone"_L1) {
            rank = label == "x86_pkg_temp"_L1 ? 75 : label.contains("cpu"_L1) ? 70 : label.startsWith("soc"_L1) ? 50 : 0;
        } else if (chip.contains("cpu"_L1) || chip == "k8temp"_L1) {
            rank = 70;
        }
        if (rank == 0)
            continue;
        if (rank > bestRank || (rank == bestRank && t.celsius > s.cpuTemperature)) {
            bestRank = rank;
            s.cpuTemperatureAvailable = true;
            s.cpuTemperature = t.celsius;
            s.cpuTemperatureSource = t.chip + u" · "_s + t.label;
        }
    }
}

// ---- batteries and external power ---------------------------------------------

QVariantMap BatteryInfo::toVariant() const
{
    return {
        {u"name"_s, name},
        {u"model"_s, model},
        {u"manufacturer"_s, manufacturer},
        {u"technology"_s, technology},
        {u"peripheral"_s, peripheral},
        {u"status"_s, status},
        {u"capacityLevel"_s, capacityLevel},
        {u"health"_s, health},
        {u"percentage"_s, optional(percentageAvailable, round1(percentage))},
        {u"rateWatts"_s, optional(rateAvailable, round2(rateWatts))},
        {u"energyNowWh"_s, optional(energyAvailable, round2(energyNowWh))},
        {u"energyFullWh"_s, optional(energyAvailable, round2(energyFullWh))},
        {u"energyDesignWh"_s, optional(designAvailable, round2(energyDesignWh))},
        {u"capacityHealthPercent"_s, optional(capacityHealthAvailable, round1(capacityHealthPercent))},
        {u"cycles"_s, cyclesAvailable ? QVariant(cycles) : nullValue()},
        {u"voltage"_s, optional(voltageAvailable, round2(voltage))},
        {u"temperatureCelsius"_s, optional(temperatureAvailable, round1(temperature))},
        {u"secondsRemaining"_s, timeAvailable ? QVariant(secondsRemaining) : nullValue()},
        {u"timeEstimated"_s, timeEstimated},
    };
}

QVariantMap PowerSample::toVariant() const
{
    QVariantList list;
    for (const BatteryInfo &battery : batteries)
        list << battery.toVariant();
    return {
        {u"batteries"_s, list},
        {u"externalPowerKnown"_s, externalPowerKnown},
        {u"externalPowerOnline"_s, externalPowerKnown ? QVariant(externalPowerOnline) : nullValue()},
    };
}

PowerSample PowerSample::read()
{
    PowerSample sample;
    const QStringList supplies = Sys::listDirectory(u"/sys/class/power_supply"_s);
    for (const QString &supply : supplies) {
        const QString base = u"/sys/class/power_supply/"_s + supply + u'/';
        const QString type = Sys::readText(base + u"type"_s);

        if (type == "Mains"_L1 || type == "USB"_L1) {
            if (const auto online = Sys::readInteger(base + u"online"_s)) {
                sample.externalPowerKnown = true;
                sample.externalPowerOnline = sample.externalPowerOnline || *online > 0;
            }
            continue;
        }
        if (type != "Battery"_L1)
            continue;
        if (Sys::readInteger(base + u"present"_s).value_or(1) == 0)
            continue;               // empty battery bay

        // Micro-units (µWh, µAh, µV, µW, µA) as plain units; empty when the attribute is missing.
        const auto micro = [&base](const QString &attribute) -> std::optional<double> {
            const auto value = Sys::readInteger(base + attribute);
            if (!value)
                return std::nullopt;
            return double(*value) / 1e6;
        };

        BatteryInfo b;
        b.name = supply;
        b.model = Sys::readText(base + u"model_name"_s);
        b.manufacturer = Sys::readText(base + u"manufacturer"_s);
        b.technology = Sys::readText(base + u"technology"_s);
        if (b.technology == "Unknown"_L1)
            b.technology.clear();
        b.peripheral = Sys::readText(base + u"scope"_s) == "Device"_L1;
        b.status = Sys::readText(base + u"status"_s);
        b.capacityLevel = Sys::readText(base + u"capacity_level"_s);
        if (b.capacityLevel == "Unknown"_L1)
            b.capacityLevel.clear();
        b.health = Sys::readText(base + u"health"_s);
        if (b.health == "Unknown"_L1)
            b.health.clear();

        const auto voltageNow = micro(u"voltage_now"_s);
        const auto voltageDesign = micro(u"voltage_min_design"_s);
        if (voltageNow && *voltageNow > 0) {
            b.voltageAvailable = true;
            b.voltage = *voltageNow;
        }

        // Capacity is reported either as energy (µWh) or as charge (µAh).
        // Charge is converted with the design voltage where known, so the
        // figures do not wander with the momentary voltage.
        auto now = micro(u"energy_now"_s);
        auto full = micro(u"energy_full"_s);
        auto design = micro(u"energy_full_design"_s);
        bool energyUnits = full.has_value() || now.has_value();
        if (!energyUnits) {
            now = micro(u"charge_now"_s);
            full = micro(u"charge_full"_s);
            design = micro(u"charge_full_design"_s);
        }
        const double volts = voltageDesign && *voltageDesign > 0 ? *voltageDesign : b.voltageAvailable ? b.voltage : 0;
        const double toWh = energyUnits ? 1.0 : volts;

        if (full && *full > 0 && design && *design > 0) {
            b.capacityHealthAvailable = true;
            b.capacityHealthPercent = *full / *design * 100.0;
        }
        if (now && full && *full > 0 && toWh > 0) {
            b.energyAvailable = true;
            b.energyNowWh = *now * toWh;
            b.energyFullWh = *full * toWh;
            if (design && *design > 0) {
                b.designAvailable = true;
                b.energyDesignWh = *design * toWh;
            }
        }

        if (const auto capacity = Sys::readInteger(base + u"capacity"_s)) {
            b.percentageAvailable = true;
            b.percentage = qBound(0.0, double(*capacity), 100.0);
        } else if (now && full && *full > 0) {
            b.percentageAvailable = true;
            b.percentage = qBound(0.0, *now / *full * 100.0, 100.0);
        }

        // Some drivers report a negative current or power while discharging.
        if (const auto power = micro(u"power_now"_s)) {
            b.rateAvailable = true;
            b.rateWatts = std::fabs(*power);
        } else if (const auto current = micro(u"current_now"_s); current && b.voltageAvailable) {
            b.rateAvailable = true;
            b.rateWatts = std::fabs(*current) * b.voltage;
        }

        if (const auto cycles = Sys::readInteger(base + u"cycle_count"_s); cycles && *cycles > 0) {
            b.cyclesAvailable = true;
            b.cycles = int(*cycles);
        }
        if (const auto temp = Sys::readInteger(base + u"temp"_s)) {
            b.temperatureAvailable = true;
            b.temperature = double(*temp) / 10.0;       // tenths of a degree Celsius
        }

        const bool charging = b.status == "Charging"_L1;
        const bool discharging = b.status == "Discharging"_L1;
        const auto reported = Sys::readInteger(base + (charging ? u"time_to_full_now"_s : u"time_to_empty_now"_s));
        if ((charging || discharging) && reported && *reported > 0) {
            b.timeAvailable = true;
            b.secondsRemaining = *reported;
        } else if ((charging || discharging) && b.energyAvailable && b.rateAvailable && b.rateWatts > 0.01) {
            const double wh = charging ? b.energyFullWh - b.energyNowWh : b.energyNowWh;
            if (wh > 0) {
                b.timeAvailable = true;
                b.timeEstimated = true;
                b.secondsRemaining = qint64(wh / b.rateWatts * 3600.0);
            }
        }
        sample.batteries << b;
    }

    // The machine's own batteries before those of peripherals.
    std::stable_sort(sample.batteries.begin(), sample.batteries.end(), [](const BatteryInfo &a, const BatteryInfo &b) {
        return !a.peripheral && b.peripheral;
    });
    return sample;
}
