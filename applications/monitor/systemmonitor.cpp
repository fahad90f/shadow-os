// SPDX-License-Identifier: GPL-3.0-or-later
#include "systemmonitor.h"

#include "sysfs.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace {

const int kIntervals[] = {1000, 2000, 5000};
const int kWarmUpMs = 500;
const double kFilesystemPeriod = 5.0;       // seconds between statvfs passes

bool validInterval(int milliseconds)
{
    for (const int value : kIntervals) {
        if (value == milliseconds)
            return true;
    }
    return false;
}

QVariantList toList(const QList<qreal> &values)
{
    QVariantList list;
    list.reserve(values.size());
    for (const qreal value : values)
        list << value;
    return list;
}

} // namespace

SystemMonitor::SystemMonitor(QObject *parent)
    : QObject(parent)
    , m_sampler(std::make_shared<SystemSampler>())
    , m_worker(u"nexus-monitor-sampler"_s)
    , m_filesystemWorker(u"nexus-monitor-statvfs"_s)
{
    const int stored = QSettings().value(u"sampling/intervalMs"_s, 1000).toInt();
    if (validInterval(stored))
        m_interval = stored;

    m_timer.setInterval(m_interval);
    connect(&m_timer, &QTimer::timeout, this, &SystemMonitor::requestSample);
    m_warmUp.setSingleShot(true);
    m_warmUp.setInterval(kWarmUpMs);
    connect(&m_warmUp, &QTimer::timeout, this, [this] {
        requestSample();
        if (m_active)
            m_timer.start();        // the regular cadence continues from here
    });

    // Next to this binary first, so an uninstalled build tree works too.
    const QString local = QCoreApplication::applicationDirPath() + u"/nexus-sysinfo"_s;
    m_systemInformation = QFileInfo(local).isExecutable() ? local
                                                          : QStandardPaths::findExecutable(u"nexus-sysinfo"_s);
}

SystemMonitor::~SystemMonitor() = default;

void SystemMonitor::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (m_active) {
        // Whatever was read before the pause is too old to compute rates from.
        m_resetRates = true;
        requestSample();
        m_timer.start();
    } else {
        m_timer.stop();
        m_warmUp.stop();
    }
    Q_EMIT activeChanged();
}

void SystemMonitor::setInterval(int milliseconds)
{
    if (!validInterval(milliseconds) || milliseconds == m_interval)
        return;
    m_interval = milliseconds;
    m_timer.setInterval(m_interval);
    QSettings().setValue(u"sampling/intervalMs"_s, m_interval);
    // Samples taken at another spacing would distort the time axis.
    clearHistories();
    Q_EMIT intervalChanged();
    Q_EMIT updated();
}

void SystemMonitor::requestSample()
{
    if (!m_active)
        return;
    const bool reset = m_resetRates;
    const std::shared_ptr<SystemSampler> sampler = m_sampler;
    const bool started = m_worker.run(
        [sampler, reset] {
            if (reset)
                sampler->resetRates();
            return sampler->sample();
        },
        [this](const SystemSnapshot &snapshot) { apply(snapshot); });
    if (started)
        m_resetRates = false;
    requestFilesystems();
}

void SystemMonitor::requestFilesystems()
{
    const double now = Sys::monotonicSeconds();
    if (m_filesystems.valid && now - m_filesystemsReadAt < kFilesystemPeriod)
        return;
    const bool started = m_filesystemWorker.run([] { return FilesystemSample::read(); },
                                                [this](const FilesystemSample &sample) {
        m_filesystems = sample;
        m_filesystemList.clear();
        for (const FilesystemInfo &fs : std::as_const(m_filesystems.filesystems))
            m_filesystemList << fs.toVariant();
        Q_EMIT filesystemsChanged();
    });
    if (started)
        m_filesystemsReadAt = now;
}

void SystemMonitor::push(QList<qreal> &history, qreal value)
{
    history.append(value);
    if (history.size() > HistorySize)
        history.remove(0, history.size() - HistorySize);
}

void SystemMonitor::clearHistories()
{
    m_cpuHistory.clear();
    m_temperatureHistory.clear();
    m_memoryHistory.clear();
    m_swapHistory.clear();
    m_gpuHistory.clear();
    m_diskReadHistory.clear();
    m_diskWriteHistory.clear();
    m_networkRxHistory.clear();
    m_networkTxHistory.clear();
    m_interfaceHistory.clear();
}

void SystemMonitor::apply(const SystemSnapshot &snapshot)
{
    m_snapshot = snapshot;
    m_ready = true;
    m_ratesReady = snapshot.cpu.usageValid;

    if (snapshot.cpu.usageValid)
        push(m_cpuHistory, snapshot.cpu.usage);
    if (snapshot.memory.valid) {
        push(m_memoryHistory, snapshot.memory.usedFraction());
        push(m_swapHistory, snapshot.memory.swapFraction());
    }
    if (snapshot.sensors.cpuTemperatureAvailable)
        push(m_temperatureHistory, snapshot.sensors.cpuTemperature);
    else
        m_temperatureHistory.clear();
    if (snapshot.disks.ratesValid) {
        push(m_diskReadHistory, snapshot.disks.readRate);
        push(m_diskWriteHistory, snapshot.disks.writeRate);
    }
    if (snapshot.network.ratesValid) {
        push(m_networkRxHistory, snapshot.network.rxRate);
        push(m_networkTxHistory, snapshot.network.txRate);
    }

    // The overview shows one graphics device: the first that reports its
    // utilisation, otherwise the first one found.
    m_gpuList.clear();
    const GpuInfo *primary = nullptr;
    for (const GpuInfo &gpu : std::as_const(m_snapshot.gpus)) {
        m_gpuList << gpu.toVariant();
        if (!primary || (!primary->utilizationAvailable && gpu.utilizationAvailable))
            primary = &gpu;
    }
    m_gpuUsageAvailable = primary && primary->utilizationAvailable;
    m_gpuUsage = m_gpuUsageAvailable ? primary->utilization / 100.0 : 0;
    m_gpuName = primary ? primary->name : QString();
    m_gpuStatus = !primary ? tr("No graphics device detected") : primary->note;
    if (m_gpuUsageAvailable)
        push(m_gpuHistory, m_gpuUsage);
    else
        m_gpuHistory.clear();

    m_diskList.clear();
    for (const DiskInfo &disk : std::as_const(m_snapshot.disks.disks))
        m_diskList << disk.toVariant();

    m_interfaceList.clear();
    QHash<QString, InterfaceHistory> histories;
    for (const InterfaceInfo &iface : std::as_const(m_snapshot.network.interfaces)) {
        InterfaceHistory history = m_interfaceHistory.value(iface.name);
        if (snapshot.network.ratesValid) {
            push(history.rx, iface.rxRate);
            push(history.tx, iface.txRate);
        }
        QVariantMap map = iface.toVariant();
        map.insert(u"rxHistory"_s, toList(history.rx));
        map.insert(u"txHistory"_s, toList(history.tx));
        m_interfaceList << map;
        histories.insert(iface.name, history);
    }
    m_interfaceHistory = histories;     // interfaces that disappeared lose their history

    m_temperatureList.clear();
    for (const TemperatureReading &t : std::as_const(m_snapshot.sensors.temperatures))
        m_temperatureList << t.toVariant();
    m_fanList.clear();
    for (const FanReading &f : std::as_const(m_snapshot.sensors.fans))
        m_fanList << f.toVariant();
    m_batteryList.clear();
    for (const BatteryInfo &b : std::as_const(m_snapshot.power.batteries))
        m_batteryList << b.toVariant();

    Q_EMIT updated();

    // The first reading after a start or a pause has no deltas; take the
    // second one soon instead of leaving the gauges empty for a whole interval.
    if (!snapshot.cpu.usageValid && m_active)
        m_warmUp.start();
}

bool SystemMonitor::openSystemInformation() const
{
    return !m_systemInformation.isEmpty() && QProcess::startDetached(m_systemInformation, {});
}
