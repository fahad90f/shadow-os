// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// ---- temperatures and fans ------------------------------------------------------

struct TemperatureReading
{
    QString chip;                   // hwmon chip name ("coretemp", "k10temp", "nvme") or "thermal zone"
    QString device;                 // what the chip belongs to ("coretemp.0", "nvme0", "thermal_zone2")
    QString label;                  // "Package id 0", "Composite", "temp1", zone type, ...
    double celsius = 0;
    bool hasHigh = false;           // tempN_max
    double high = 0;
    bool hasCritical = false;       // tempN_crit or the zone's critical trip point
    double critical = 0;

    // "critical", "high" or "normal" relative to the sensor's own limits.
    QString level() const;
    QVariantMap toVariant() const;
};

struct FanReading
{
    QString chip;
    QString device;
    QString label;
    int rpm = 0;

    QVariantMap toVariant() const;
};

struct SensorSample
{
    QList<TemperatureReading> temperatures;
    QList<FanReading> fans;
    bool cpuTemperatureAvailable = false;
    double cpuTemperature = 0;
    QString cpuTemperatureSource;   // "coretemp · Package id 0"

    QVariantMap toVariant() const;
};

// Sources: /sys/class/hwmon/hwmonN/{name,tempN_input,tempN_label,tempN_max,
// tempN_crit,fanN_input,fanN_label} and /sys/class/thermal/thermal_zoneN/
// {type,temp,trip_point_N_*}. hwmon devices that merely mirror a thermal zone
// are skipped, so a sensor is not listed twice. The sensors of a device that
// is runtime-suspended (a laptop's discrete GPU, typically) are not read:
// reading them would power the device up again.
class SensorSource
{
public:
    SensorSample sample();

private:
    struct Channel
    {
        QString inputPath;
        QString label;
        bool hasHigh = false;
        double high = 0;
        bool hasCritical = false;
        double critical = 0;
    };
    struct Chip
    {
        QString name;
        QString device;
        QString runtimeStatusPath;  // <device>/power/runtime_status
        QList<Channel> temperatures;
        QList<Channel> fans;
    };
    struct Zone
    {
        QString tempPath;
        QString type;
        QString device;
        bool hasCritical = false;
        double critical = 0;
    };

    void discover(const QStringList &hwmon, const QStringList &zones);
    static void selectCpuTemperature(SensorSample &sample);

    bool m_discovered = false;
    QStringList m_hwmonEntries;
    QStringList m_zoneEntries;
    QList<Chip> m_chips;
    QList<Zone> m_zones;
};

// ---- batteries and external power ---------------------------------------------

struct BatteryInfo
{
    QString name;                   // BAT0, hidpp_battery_0, ...
    QString model;
    QString manufacturer;
    QString technology;             // Li-ion, Li-poly, ...
    bool peripheral = false;        // scope "Device": battery of a mouse, keyboard, headset
    QString status;                 // kernel wording: Charging, Discharging, Full, Not charging, Unknown
    QString capacityLevel;          // Low, Normal, High, Full — for devices without a percentage
    QString health;                 // "health" attribute where the driver has one

    bool percentageAvailable = false;
    double percentage = 0;          // 0..100
    bool rateAvailable = false;
    double rateWatts = 0;           // charge or discharge power
    bool energyAvailable = false;
    double energyNowWh = 0;
    double energyFullWh = 0;
    bool designAvailable = false;
    double energyDesignWh = 0;
    bool capacityHealthAvailable = false;
    double capacityHealthPercent = 0;   // full capacity relative to the design capacity
    bool cyclesAvailable = false;
    int cycles = 0;
    bool voltageAvailable = false;
    double voltage = 0;
    bool temperatureAvailable = false;
    double temperature = 0;
    bool timeAvailable = false;
    qint64 secondsRemaining = 0;    // to empty while discharging, to full while charging
    bool timeEstimated = false;     // computed from energy and rate rather than reported by the device

    QVariantMap toVariant() const;
};

struct PowerSample
{
    QList<BatteryInfo> batteries;
    bool externalPowerKnown = false;    // a mains/USB supply is registered
    bool externalPowerOnline = false;

    // /sys/class/power_supply/*/ (ABI: Documentation/ABI/testing/sysfs-class-power).
    static PowerSample read();
    QVariantMap toVariant() const;
};
