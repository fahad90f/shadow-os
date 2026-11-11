// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// One graphics device and whatever its driver reports about it. A value the
// driver does not expose stays "not available" — nothing is estimated.
struct GpuInfo
{
    QString card;                   // DRM node name ("card0"); empty for a device only nvidia-smi knows
    QString name;                   // product name from the PCI ID database, else a description by driver
    QString driver;                 // amdgpu, i915, xe, nouveau, nvidia, virtio_gpu, ...
    QString pciSlot;                // "0000:01:00.0"; empty for platform devices
    QString source;                 // where the figures come from: "sysfs", "nvidia-smi" or ""
    bool suspended = false;         // runtime-suspended: not polled, so it can stay powered down
    QString note;                   // why utilisation is not available, for the UI

    bool utilizationAvailable = false;
    double utilization = 0;         // percent
    bool temperatureAvailable = false;
    double temperature = 0;         // degrees Celsius
    bool memoryAvailable = false;
    qint64 memoryUsed = 0;          // bytes of video memory
    qint64 memoryTotal = 0;
    bool powerAvailable = false;
    double powerWatts = 0;
    bool fanAvailable = false;
    int fanRpm = 0;

    QVariantMap toVariant() const;
};

// Sources:
//   /sys/class/drm/cardN/device/          driver, PCI identity, runtime PM state
//     gpu_busy_percent                    utilisation (amdgpu)
//     mem_info_vram_used / _total         video memory (amdgpu)
//     hwmon/hwmonN/temp1_input, power1_*, fan1_input   temperature, power, fan
//   nvidia-smi --query-gpu=... --format=csv   proprietary NVIDIA driver, only
//                                              if the tool is installed
//   /usr/share/misc/pci.ids               product names
class GpuSource
{
    Q_DECLARE_TR_FUNCTIONS(GpuSource)

public:
    QList<GpuInfo> sample();

private:
    struct Card
    {
        QString card;
        QString devicePath;         // /sys/class/drm/cardN/device
        QString name;
        QString driver;
        QString pciSlot;
        QString hwmonPath;          // first hwmon directory of the device; may be empty
    };

    void discover(const QStringList &entries);
    void queryNvidia(QList<GpuInfo> &gpus);

    bool m_discovered = false;
    QStringList m_entries;
    QList<Card> m_cards;
    int m_nvidiaSmi = 0;            // 0 unknown, 1 installed, -1 not installed
};
