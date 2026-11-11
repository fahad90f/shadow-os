// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cpumemory.h"
#include "gpu.h"
#include "network.h"
#include "sensors.h"
#include "storage.h"

#include <QJsonObject>

// Everything read in one sampling pass, except file-system usage (statvfs can
// block, so it has its own, slower cadence — see FilesystemSample).
struct SystemSnapshot
{
    CpuInfo cpuInfo;
    CpuSample cpu;
    MemorySample memory;
    QList<GpuInfo> gpus;
    DiskSample disks;
    NetworkSample network;
    SensorSample sensors;
    PowerSample power;

    // The documents behind "--dump <topic>".
    QJsonObject cpuJson() const;
    QJsonObject memoryJson() const;
    QJsonObject gpuJson() const;
    QJsonObject disksJson(const FilesystemSample &filesystems) const;
    QJsonObject networkJson() const;
    QJsonObject sensorsJson() const;
    QJsonObject batteryJson() const;
};

// Owns the stateful sources. Not thread-safe: one thread at a time — the
// sampling thread of SystemMonitor, or main() in --dump mode.
class SystemSampler
{
public:
    SystemSnapshot sample();
    // Drop the baselines of all delta-based values (after sampling was paused).
    void resetRates();

private:
    bool m_haveInfo = false;
    CpuInfo m_info;
    CpuSource m_cpu;
    DiskSource m_disks;
    NetworkSource m_network;
    SensorSource m_sensors;
    GpuSource m_gpus;
};
