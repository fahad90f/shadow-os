// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "jobthread.h"
#include "systemsampler.h"

#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>

#include <memory>

// Live system figures for the UI.
//
// While 'active', a sample is taken every 'interval' milliseconds on a
// worker thread (SystemSampler) and published here; delta-based values
// (CPU utilisation, disk and network rates) appear with the second reading,
// which follows the first after half a second. Rolling histories of
// 'historySize' samples feed the charts. File-system usage is refreshed
// every few seconds on a thread of its own, because statvfs() may block.
//
// Nothing is sampled while 'active' is false — the window binds it to its
// visibility — and the first sample after a pause only re-primes the deltas.
class SystemMonitor : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(int interval READ interval WRITE setInterval NOTIFY intervalChanged)
    Q_PROPERTY(int historySize READ historySize CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY updated)
    Q_PROPERTY(bool ratesReady READ ratesReady NOTIFY updated)

    // ---- processor
    Q_PROPERTY(QString cpuModel READ cpuModel NOTIFY updated)
    Q_PROPERTY(int cpuCores READ cpuCores NOTIFY updated)
    Q_PROPERTY(int cpuThreads READ cpuThreads NOTIFY updated)
    Q_PROPERTY(qreal cpuUsage READ cpuUsage NOTIFY updated)
    Q_PROPERTY(QList<int> coreIds READ coreIds NOTIFY updated)
    Q_PROPERTY(QList<qreal> coreUsage READ coreUsage NOTIFY updated)
    Q_PROPERTY(QList<qreal> loadAverage READ loadAverage NOTIFY updated)
    Q_PROPERTY(int tasksRunnable READ tasksRunnable NOTIFY updated)
    Q_PROPERTY(int tasksTotal READ tasksTotal NOTIFY updated)
    Q_PROPERTY(int processCount READ processCount NOTIFY updated)
    Q_PROPERTY(bool cpuFrequencyAvailable READ cpuFrequencyAvailable NOTIFY updated)
    Q_PROPERTY(qreal cpuFrequencyMhz READ cpuFrequencyMhz NOTIFY updated)
    Q_PROPERTY(bool cpuTemperatureAvailable READ cpuTemperatureAvailable NOTIFY updated)
    Q_PROPERTY(qreal cpuTemperature READ cpuTemperature NOTIFY updated)
    Q_PROPERTY(QString cpuTemperatureSource READ cpuTemperatureSource NOTIFY updated)
    Q_PROPERTY(qint64 uptimeSeconds READ uptimeSeconds NOTIFY updated)
    Q_PROPERTY(QList<qreal> cpuHistory READ cpuHistory NOTIFY updated)
    Q_PROPERTY(QList<qreal> temperatureHistory READ temperatureHistory NOTIFY updated)

    // ---- memory (bytes)
    Q_PROPERTY(qint64 memoryTotal READ memoryTotal NOTIFY updated)
    Q_PROPERTY(qint64 memoryUsed READ memoryUsed NOTIFY updated)
    Q_PROPERTY(qint64 memoryAvailable READ memoryAvailable NOTIFY updated)
    Q_PROPERTY(qint64 memoryFree READ memoryFree NOTIFY updated)
    Q_PROPERTY(qint64 memoryBuffers READ memoryBuffers NOTIFY updated)
    Q_PROPERTY(qint64 memoryCached READ memoryCached NOTIFY updated)
    Q_PROPERTY(qint64 memoryShared READ memoryShared NOTIFY updated)
    Q_PROPERTY(qreal memoryFraction READ memoryFraction NOTIFY updated)
    Q_PROPERTY(qint64 swapTotal READ swapTotal NOTIFY updated)
    Q_PROPERTY(qint64 swapUsed READ swapUsed NOTIFY updated)
    Q_PROPERTY(qreal swapFraction READ swapFraction NOTIFY updated)
    Q_PROPERTY(QList<qreal> memoryHistory READ memoryHistory NOTIFY updated)
    Q_PROPERTY(QList<qreal> swapHistory READ swapHistory NOTIFY updated)

    // ---- graphics: list of maps (GpuInfo::toVariant) and the device the overview shows
    Q_PROPERTY(QVariantList gpus READ gpus NOTIFY updated)
    Q_PROPERTY(bool gpuUsageAvailable READ gpuUsageAvailable NOTIFY updated)
    Q_PROPERTY(qreal gpuUsage READ gpuUsage NOTIFY updated)
    Q_PROPERTY(QString gpuName READ gpuName NOTIFY updated)
    Q_PROPERTY(QString gpuStatus READ gpuStatus NOTIFY updated)
    Q_PROPERTY(QList<qreal> gpuHistory READ gpuHistory NOTIFY updated)

    // ---- storage
    Q_PROPERTY(QVariantList filesystems READ filesystems NOTIFY filesystemsChanged)
    Q_PROPERTY(bool filesystemsReady READ filesystemsReady NOTIFY filesystemsChanged)
    Q_PROPERTY(bool rootAvailable READ rootAvailable NOTIFY filesystemsChanged)
    Q_PROPERTY(qreal rootFraction READ rootFraction NOTIFY filesystemsChanged)
    Q_PROPERTY(qint64 rootUsed READ rootUsed NOTIFY filesystemsChanged)
    Q_PROPERTY(qint64 rootTotal READ rootTotal NOTIFY filesystemsChanged)
    Q_PROPERTY(qint64 rootFree READ rootFree NOTIFY filesystemsChanged)
    Q_PROPERTY(QVariantList disks READ disks NOTIFY updated)
    Q_PROPERTY(qreal diskReadRate READ diskReadRate NOTIFY updated)
    Q_PROPERTY(qreal diskWriteRate READ diskWriteRate NOTIFY updated)
    Q_PROPERTY(QList<qreal> diskReadHistory READ diskReadHistory NOTIFY updated)
    Q_PROPERTY(QList<qreal> diskWriteHistory READ diskWriteHistory NOTIFY updated)

    // ---- network (each interface map also carries rxHistory / txHistory)
    Q_PROPERTY(QVariantList interfaces READ interfaces NOTIFY updated)
    Q_PROPERTY(qreal networkRxRate READ networkRxRate NOTIFY updated)
    Q_PROPERTY(qreal networkTxRate READ networkTxRate NOTIFY updated)
    Q_PROPERTY(QList<qreal> networkRxHistory READ networkRxHistory NOTIFY updated)
    Q_PROPERTY(QList<qreal> networkTxHistory READ networkTxHistory NOTIFY updated)

    // ---- sensors and power
    Q_PROPERTY(QVariantList temperatures READ temperatures NOTIFY updated)
    Q_PROPERTY(QVariantList fans READ fans NOTIFY updated)
    Q_PROPERTY(QVariantList batteries READ batteries NOTIFY updated)
    Q_PROPERTY(bool externalPowerKnown READ externalPowerKnown NOTIFY updated)
    Q_PROPERTY(bool externalPowerOnline READ externalPowerOnline NOTIFY updated)

    Q_PROPERTY(bool systemInformationAvailable READ systemInformationAvailable CONSTANT)

public:
    explicit SystemMonitor(QObject *parent = nullptr);
    ~SystemMonitor() override;

    bool active() const { return m_active; }
    void setActive(bool active);
    int interval() const { return m_interval; }
    void setInterval(int milliseconds);
    int historySize() const { return HistorySize; }
    bool ready() const { return m_ready; }
    bool ratesReady() const { return m_ratesReady; }

    QString cpuModel() const { return m_snapshot.cpuInfo.model; }
    int cpuCores() const { return m_snapshot.cpuInfo.physicalCores; }
    int cpuThreads() const { return m_snapshot.cpuInfo.logicalCpus; }
    qreal cpuUsage() const { return m_snapshot.cpu.usage; }
    QList<int> coreIds() const { return m_snapshot.cpu.coreIds; }
    QList<qreal> coreUsage() const { return m_snapshot.cpu.coreUsage; }
    QList<qreal> loadAverage() const { return {m_snapshot.cpu.load1, m_snapshot.cpu.load5, m_snapshot.cpu.load15}; }
    int tasksRunnable() const { return m_snapshot.cpu.tasksRunnable; }
    int tasksTotal() const { return m_snapshot.cpu.tasksTotal; }
    int processCount() const { return m_snapshot.cpu.processCount; }
    bool cpuFrequencyAvailable() const { return m_snapshot.cpu.frequencyAvailable; }
    qreal cpuFrequencyMhz() const { return m_snapshot.cpu.frequencyMhz; }
    bool cpuTemperatureAvailable() const { return m_snapshot.sensors.cpuTemperatureAvailable; }
    qreal cpuTemperature() const { return m_snapshot.sensors.cpuTemperature; }
    QString cpuTemperatureSource() const { return m_snapshot.sensors.cpuTemperatureSource; }
    qint64 uptimeSeconds() const { return m_snapshot.cpu.uptimeSeconds; }
    QList<qreal> cpuHistory() const { return m_cpuHistory; }
    QList<qreal> temperatureHistory() const { return m_temperatureHistory; }

    qint64 memoryTotal() const { return m_snapshot.memory.total; }
    qint64 memoryUsed() const { return m_snapshot.memory.used; }
    qint64 memoryAvailable() const { return m_snapshot.memory.available; }
    qint64 memoryFree() const { return m_snapshot.memory.free; }
    qint64 memoryBuffers() const { return m_snapshot.memory.buffers; }
    qint64 memoryCached() const { return m_snapshot.memory.cached; }
    qint64 memoryShared() const { return m_snapshot.memory.shared; }
    qreal memoryFraction() const { return m_snapshot.memory.usedFraction(); }
    qint64 swapTotal() const { return m_snapshot.memory.swapTotal; }
    qint64 swapUsed() const { return m_snapshot.memory.swapUsed; }
    qreal swapFraction() const { return m_snapshot.memory.swapFraction(); }
    QList<qreal> memoryHistory() const { return m_memoryHistory; }
    QList<qreal> swapHistory() const { return m_swapHistory; }

    QVariantList gpus() const { return m_gpuList; }
    bool gpuUsageAvailable() const { return m_gpuUsageAvailable; }
    qreal gpuUsage() const { return m_gpuUsage; }
    QString gpuName() const { return m_gpuName; }
    QString gpuStatus() const { return m_gpuStatus; }
    QList<qreal> gpuHistory() const { return m_gpuHistory; }

    QVariantList filesystems() const { return m_filesystemList; }
    bool filesystemsReady() const { return m_filesystems.valid; }
    bool rootAvailable() const { return m_filesystems.rootValid; }
    qreal rootFraction() const { return m_filesystems.root.usedFraction(); }
    qint64 rootUsed() const { return m_filesystems.root.used; }
    qint64 rootTotal() const { return m_filesystems.root.total; }
    qint64 rootFree() const { return m_filesystems.root.available; }
    QVariantList disks() const { return m_diskList; }
    qreal diskReadRate() const { return m_snapshot.disks.readRate; }
    qreal diskWriteRate() const { return m_snapshot.disks.writeRate; }
    QList<qreal> diskReadHistory() const { return m_diskReadHistory; }
    QList<qreal> diskWriteHistory() const { return m_diskWriteHistory; }

    QVariantList interfaces() const { return m_interfaceList; }
    qreal networkRxRate() const { return m_snapshot.network.rxRate; }
    qreal networkTxRate() const { return m_snapshot.network.txRate; }
    QList<qreal> networkRxHistory() const { return m_networkRxHistory; }
    QList<qreal> networkTxHistory() const { return m_networkTxHistory; }

    QVariantList temperatures() const { return m_temperatureList; }
    QVariantList fans() const { return m_fanList; }
    QVariantList batteries() const { return m_batteryList; }
    bool externalPowerKnown() const { return m_snapshot.power.externalPowerKnown; }
    bool externalPowerOnline() const { return m_snapshot.power.externalPowerOnline; }

    bool systemInformationAvailable() const { return !m_systemInformation.isEmpty(); }
    // Start SHADOW System Information; false if it could not be started.
    Q_INVOKABLE bool openSystemInformation() const;

Q_SIGNALS:
    void activeChanged();
    void intervalChanged();
    void updated();
    void filesystemsChanged();

private:
    static constexpr int HistorySize = 120;
    struct InterfaceHistory
    {
        QList<qreal> rx;
        QList<qreal> tx;
    };

    void requestSample();
    void requestFilesystems();
    void apply(const SystemSnapshot &snapshot);
    void clearHistories();
    static void push(QList<qreal> &history, qreal value);

    bool m_active = false;
    int m_interval = 1000;
    bool m_ready = false;
    bool m_ratesReady = false;
    bool m_resetRates = false;
    QTimer m_timer;
    QTimer m_warmUp;

    SystemSnapshot m_snapshot;
    FilesystemSample m_filesystems;
    double m_filesystemsReadAt = 0;

    QList<qreal> m_cpuHistory, m_temperatureHistory, m_memoryHistory, m_swapHistory, m_gpuHistory;
    QList<qreal> m_diskReadHistory, m_diskWriteHistory, m_networkRxHistory, m_networkTxHistory;
    QHash<QString, InterfaceHistory> m_interfaceHistory;

    QVariantList m_gpuList, m_filesystemList, m_diskList, m_interfaceList;
    QVariantList m_temperatureList, m_fanList, m_batteryList;
    bool m_gpuUsageAvailable = false;
    qreal m_gpuUsage = 0;
    QString m_gpuName, m_gpuStatus;
    QString m_systemInformation;

    // Declared last: the threads stop before anything above is destroyed.
    std::shared_ptr<SystemSampler> m_sampler;
    JobThread m_worker;
    JobThread m_filesystemWorker;
};
