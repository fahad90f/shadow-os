// SPDX-License-Identifier: GPL-3.0-or-later
#include "systemsampler.h"

#include <QJsonArray>

#include <cmath>

using namespace Qt::StringLiterals;

SystemSnapshot SystemSampler::sample()
{
    if (!m_haveInfo) {
        m_info = CpuInfo::read();
        m_haveInfo = true;
    }
    SystemSnapshot s;
    s.cpuInfo = m_info;
    s.cpu = m_cpu.sample();
    s.memory = MemorySample::read();
    s.disks = m_disks.sample();
    s.network = m_network.sample();
    s.sensors = m_sensors.sample();
    s.power = PowerSample::read();
    s.gpus = m_gpus.sample();
    return s;
}

void SystemSampler::resetRates()
{
    m_cpu.resetRates();
    m_disks.resetRates();
    m_network.resetRates();
}

QJsonObject SystemSnapshot::cpuJson() const
{
    QVariantMap map = cpu.toVariant();
    map.insert(u"model"_s, cpuInfo.model);
    map.insert(u"logicalCpus"_s, cpuInfo.logicalCpus);
    map.insert(u"physicalCores"_s, cpuInfo.physicalCores);
    map.insert(u"temperatureCelsius"_s, sensors.cpuTemperatureAvailable
                                            ? QVariant(std::round(sensors.cpuTemperature * 10.0) / 10.0)
                                            : QVariant::fromValue(nullptr));
    map.insert(u"temperatureSource"_s, sensors.cpuTemperatureSource);
    return QJsonObject::fromVariantMap(map);
}

QJsonObject SystemSnapshot::memoryJson() const
{
    return QJsonObject::fromVariantMap(memory.toVariant());
}

QJsonObject SystemSnapshot::gpuJson() const
{
    QJsonArray list;
    for (const GpuInfo &gpu : gpus)
        list.append(QJsonObject::fromVariantMap(gpu.toVariant()));
    return {{u"gpus"_s, list}};
}

QJsonObject SystemSnapshot::disksJson(const FilesystemSample &filesystems) const
{
    QVariantMap map = filesystems.toVariant();
    map.insert(disks.toVariant());
    return QJsonObject::fromVariantMap(map);
}

QJsonObject SystemSnapshot::networkJson() const
{
    return QJsonObject::fromVariantMap(network.toVariant());
}

QJsonObject SystemSnapshot::sensorsJson() const
{
    return QJsonObject::fromVariantMap(sensors.toVariant());
}

QJsonObject SystemSnapshot::batteryJson() const
{
    return QJsonObject::fromVariantMap(power.toVariant());
}
