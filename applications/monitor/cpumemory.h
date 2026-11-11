// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantMap>

// Static description of the processor (/proc/cpuinfo; lscpu on machines
// whose cpuinfo has no "model name", i.e. most ARM boards).
struct CpuInfo
{
    QString model;
    int logicalCpus = 0;
    int physicalCores = 0;

    static CpuInfo read();
};

// One reading of the processor's activity.
struct CpuSample
{
    bool usageValid = false;        // utilisation needs two readings of /proc/stat
    double usage = 0;               // 0..1 over all online CPUs
    QList<int> coreIds;             // online logical CPUs, ascending
    QList<double> coreUsage;        // 0..1, parallel to coreIds
    double load1 = 0;
    double load5 = 0;
    double load15 = 0;
    int tasksRunnable = 0;          // scheduling entities runnable right now
    int tasksTotal = 0;             // processes and threads
    int processCount = 0;
    bool frequencyAvailable = false;
    double frequencyMhz = 0;        // mean over the online CPUs
    QList<double> coreFrequencyMhz; // parallel to coreIds when available
    QString frequencySource;        // "cpufreq" (scaling_cur_freq) or "cpuinfo" (cpu MHz)
    qint64 uptimeSeconds = 0;
    qint64 bootTime = 0;            // seconds since the epoch

    QVariantMap toVariant() const;
};

// Sources: /proc/stat (deltas between two readings), /proc/loadavg,
// /proc/uptime, /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq with
// /proc/cpuinfo "cpu MHz" as fallback.
class CpuSource
{
public:
    CpuSample sample();
    // Forget the previous reading, so the next sample only primes the deltas.
    void resetRates();

private:
    struct Times
    {
        quint64 total = 0;
        quint64 idle = 0;
    };

    void readFrequencies(CpuSample &sample);

    QByteArray m_buffer;
    bool m_havePrevious = false;
    Times m_previousTotal;
    QList<int> m_previousIds;
    QList<Times> m_previousCores;
    int m_cpufreqState = 0;         // 0 unknown, 1 present, -1 absent
    double m_cpuinfoReadAt = 0;
    QList<double> m_cpuinfoMhz;
};

// One reading of /proc/meminfo. "used" follows the MemAvailable semantics of
// free(1): used = MemTotal - MemAvailable.
struct MemorySample
{
    bool valid = false;
    qint64 total = 0;
    qint64 available = 0;
    qint64 used = 0;
    qint64 free = 0;
    qint64 buffers = 0;
    qint64 cached = 0;              // page cache + reclaimable slab, as free(1) reports it
    qint64 shared = 0;
    qint64 swapTotal = 0;
    qint64 swapFree = 0;
    qint64 swapUsed = 0;

    double usedFraction() const { return total > 0 ? double(used) / double(total) : 0; }
    double swapFraction() const { return swapTotal > 0 ? double(swapUsed) / double(swapTotal) : 0; }

    static MemorySample read();
    QVariantMap toVariant() const;
};
