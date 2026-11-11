// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpumemory.h"

#include "sysfs.h"

#include <command.h>

#include <QSet>

#include <cmath>
#include <cstring>

#include <dirent.h>

using namespace Qt::StringLiterals;

namespace {

QVariant nullValue()
{
    return QVariant::fromValue(nullptr);
}

double round1(double v)
{
    return std::round(v * 10.0) / 10.0;
}

// Processes are the only entries of /proc whose names start with a digit.
int countProcesses()
{
    DIR *dir = ::opendir("/proc");
    if (!dir)
        return 0;
    int count = 0;
    while (const dirent *entry = ::readdir(dir)) {
        if (entry->d_name[0] >= '1' && entry->d_name[0] <= '9')
            ++count;
    }
    ::closedir(dir);
    return count;
}

} // namespace

// ---- CpuInfo -----------------------------------------------------------------

CpuInfo CpuInfo::read()
{
    CpuInfo info;
    QSet<QByteArray> cores;
    QByteArray physical, implementer, part;

    const QList<QByteArray> lines = Sys::readFile(u"/proc/cpuinfo"_s, 16 << 20).split('\n');
    for (const QByteArray &line : lines) {
        const qsizetype colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QByteArray key = line.left(colon).trimmed();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (key == "processor") {
            ++info.logicalCpus;
        } else if (key == "model name") {
            if (info.model.isEmpty())
                info.model = QString::fromUtf8(value).simplified();
        } else if (key == "physical id") {
            physical = value;
        } else if (key == "core id") {
            cores.insert(physical + ':' + value);
        } else if (key == "CPU implementer") {
            if (implementer.isEmpty())
                implementer = value;
        } else if (key == "CPU part") {
            if (part.isEmpty())
                part = value;
        }
    }
    info.physicalCores = cores.isEmpty() ? info.logicalCpus : int(cores.size());

    if (info.model.isEmpty()) {
        // AArch64 has no "model name"; lscpu resolves implementer/part to a name.
        const Shadow::CommandResult r = Shadow::Command::runSync(u"lscpu"_s, {}, 3000);
        if (r.ok()) {
            const QStringList out = r.outText().split(u'\n');
            for (const QString &line : out) {
                if (line.startsWith("Model name:"_L1)) {
                    info.model = line.section(u':', 1).simplified();
                    break;
                }
            }
        }
        if (info.model.isEmpty() && !implementer.isEmpty())
            info.model = u"ARM implementer %1 part %2"_s.arg(QString::fromLatin1(implementer), QString::fromLatin1(part));
    }
    return info;
}

// ---- CpuSource ---------------------------------------------------------------

void CpuSource::resetRates()
{
    m_havePrevious = false;
}

CpuSample CpuSource::sample()
{
    CpuSample s;

    // /proc/stat: "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
    // (guest time is already contained in user/nice). iowait counts as idle.
    Times total;
    QList<Times> cores;
    bool haveTotal = false;
    if (Sys::readFile("/proc/stat", m_buffer)) {
        const char *p = m_buffer.constData();
        const char *const end = p + m_buffer.size();
        while (p < end) {
            const char *eol = static_cast<const char *>(std::memchr(p, '\n', size_t(end - p)));
            if (!eol)
                eol = end;
            if (eol - p > 4 && std::memcmp(p, "cpu", 3) == 0) {
                const char *q = p + 3;
                int id = -1;
                if (*q >= '0' && *q <= '9') {
                    id = 0;
                    while (q < eol && *q >= '0' && *q <= '9')
                        id = id * 10 + (*q++ - '0');
                }
                quint64 field[8] = {};
                for (int n = 0; n < 8; ++n) {
                    while (q < eol && *q == ' ')
                        ++q;
                    if (q >= eol || *q < '0' || *q > '9')
                        break;
                    quint64 value = 0;
                    while (q < eol && *q >= '0' && *q <= '9')
                        value = value * 10 + quint64(*q++ - '0');
                    field[n] = value;
                }
                Times t;
                t.idle = field[3] + field[4];
                for (const quint64 value : field)
                    t.total += value;
                if (id < 0) {
                    total = t;
                    haveTotal = true;
                } else {
                    s.coreIds << id;
                    cores << t;
                }
            } else if (eol - p > 6 && std::memcmp(p, "btime ", 6) == 0) {
                s.bootTime = QByteArray::fromRawData(p + 6, eol - p - 6).trimmed().toLongLong();
            }
            p = eol + 1;
        }
    }

    auto fraction = [](const Times &now, const Times &before) {
        if (now.total <= before.total)
            return 0.0;
        const double span = double(now.total - before.total);
        const double idle = now.idle >= before.idle ? double(now.idle - before.idle) : 0.0;
        return qBound(0.0, 1.0 - idle / span, 1.0);
    };

    s.coreUsage.fill(0.0, s.coreIds.size());
    if (haveTotal) {
        // CPUs can go on- and offline; a changed set restarts the deltas.
        if (m_havePrevious && m_previousIds == s.coreIds) {
            s.usageValid = true;
            s.usage = fraction(total, m_previousTotal);
            for (qsizetype i = 0; i < cores.size(); ++i)
                s.coreUsage[i] = fraction(cores.at(i), m_previousCores.at(i));
        }
        m_previousTotal = total;
        m_previousIds = s.coreIds;
        m_previousCores = cores;
        m_havePrevious = true;
    }

    // /proc/loadavg: "0.71 1.95 1.88 2/431 12345"
    const QList<QByteArray> load = Sys::readFile(u"/proc/loadavg"_s, 256).simplified().split(' ');
    if (load.size() >= 4) {
        s.load1 = load.at(0).toDouble();
        s.load5 = load.at(1).toDouble();
        s.load15 = load.at(2).toDouble();
        const QList<QByteArray> tasks = load.at(3).split('/');
        if (tasks.size() == 2) {
            s.tasksRunnable = tasks.at(0).toInt();
            s.tasksTotal = tasks.at(1).toInt();
        }
    }

    const QList<QByteArray> uptime = Sys::readFile(u"/proc/uptime"_s, 128).simplified().split(' ');
    if (!uptime.isEmpty())
        s.uptimeSeconds = qint64(uptime.at(0).toDouble());

    s.processCount = countProcesses();
    readFrequencies(s);
    return s;
}

void CpuSource::readFrequencies(CpuSample &s)
{
    if (s.coreIds.isEmpty())
        return;

    if (m_cpufreqState >= 0) {
        QList<double> mhz;
        mhz.reserve(s.coreIds.size());
        double sum = 0;
        int count = 0;
        for (const int id : std::as_const(s.coreIds)) {
            const auto khz = Sys::readInteger(u"/sys/devices/system/cpu/cpu%1/cpufreq/scaling_cur_freq"_s.arg(id));
            const double value = khz && *khz > 0 ? double(*khz) / 1000.0 : 0.0;
            mhz << value;
            if (value > 0) {
                sum += value;
                ++count;
            }
        }
        if (count > 0) {
            m_cpufreqState = 1;
            s.frequencyAvailable = true;
            s.frequencyMhz = sum / count;
            s.coreFrequencyMhz = mhz;
            s.frequencySource = u"cpufreq"_s;
            return;
        }
        if (m_cpufreqState == 1)
            return;                 // driver present but nothing readable right now
        m_cpufreqState = -1;        // no cpufreq driver (typical for virtual machines)
    }

    // Fallback: "cpu MHz" of /proc/cpuinfo (x86). Producing that file is not
    // free on large machines, so it is refreshed every few seconds only.
    const double now = Sys::monotonicSeconds();
    if (m_cpuinfoReadAt == 0 || now - m_cpuinfoReadAt >= 5.0) {
        m_cpuinfoReadAt = now;
        m_cpuinfoMhz.clear();
        const QList<QByteArray> lines = Sys::readFile(u"/proc/cpuinfo"_s, 16 << 20).split('\n');
        for (const QByteArray &line : lines) {
            if (!line.startsWith("cpu MHz"))
                continue;
            const double value = line.mid(line.indexOf(':') + 1).trimmed().toDouble();
            if (value > 0)
                m_cpuinfoMhz << value;
        }
    }
    if (m_cpuinfoMhz.isEmpty())
        return;
    double sum = 0;
    for (const double value : std::as_const(m_cpuinfoMhz))
        sum += value;
    s.frequencyAvailable = true;
    s.frequencyMhz = sum / double(m_cpuinfoMhz.size());
    if (m_cpuinfoMhz.size() == s.coreIds.size())
        s.coreFrequencyMhz = m_cpuinfoMhz;
    s.frequencySource = u"cpuinfo"_s;
}

QVariantMap CpuSample::toVariant() const
{
    QVariantList cores;
    for (qsizetype i = 0; i < coreIds.size(); ++i) {
        QVariantMap core{
            {u"id"_s, coreIds.at(i)},
            {u"usagePercent"_s, usageValid ? QVariant(round1(coreUsage.at(i) * 100.0)) : nullValue()},
        };
        if (i < coreFrequencyMhz.size())
            core.insert(u"frequencyMhz"_s, round1(coreFrequencyMhz.at(i)));
        cores << core;
    }
    return {
        {u"usageValid"_s, usageValid},
        {u"usagePercent"_s, usageValid ? QVariant(round1(usage * 100.0)) : nullValue()},
        {u"cores"_s, cores},
        {u"loadAverage"_s, QVariantList{load1, load5, load15}},
        {u"tasksRunnable"_s, tasksRunnable},
        {u"tasksTotal"_s, tasksTotal},
        {u"processCount"_s, processCount},
        {u"frequencyMhz"_s, frequencyAvailable ? QVariant(round1(frequencyMhz)) : nullValue()},
        {u"frequencySource"_s, frequencySource},
        {u"uptimeSeconds"_s, uptimeSeconds},
        {u"bootTime"_s, bootTime},
    };
}

// ---- MemorySample ------------------------------------------------------------

MemorySample MemorySample::read()
{
    MemorySample m;
    QByteArray data;
    if (!Sys::readFile("/proc/meminfo", data))
        return m;

    qint64 memFree = 0, memAvailable = -1, buffers = 0, cached = 0, reclaimable = 0;
    const char *p = data.constData();
    const char *const end = p + data.size();
    while (p < end) {
        const char *eol = static_cast<const char *>(std::memchr(p, '\n', size_t(end - p)));
        if (!eol)
            eol = end;
        const char *colon = static_cast<const char *>(std::memchr(p, ':', size_t(eol - p)));
        if (colon) {
            const QByteArrayView key(p, colon - p);
            const char *q = colon + 1;
            while (q < eol && *q == ' ')
                ++q;
            qint64 kb = 0;
            while (q < eol && *q >= '0' && *q <= '9')
                kb = kb * 10 + (*q++ - '0');
            const qint64 bytes = kb * 1024;
            if (key == "MemTotal") m.total = bytes;
            else if (key == "MemFree") memFree = bytes;
            else if (key == "MemAvailable") memAvailable = bytes;
            else if (key == "Buffers") buffers = bytes;
            else if (key == "Cached") cached = bytes;
            else if (key == "SReclaimable") reclaimable = bytes;
            else if (key == "Shmem") m.shared = bytes;
            else if (key == "SwapTotal") m.swapTotal = bytes;
            else if (key == "SwapFree") m.swapFree = bytes;
        }
        p = eol + 1;
    }
    if (m.total <= 0)
        return m;

    m.valid = true;
    m.free = memFree;
    m.buffers = buffers;
    m.cached = cached + reclaimable;
    // Kernels older than 3.14 have no MemAvailable; approximate it the way free(1) used to.
    m.available = memAvailable >= 0 ? memAvailable : memFree + buffers + m.cached;
    m.available = qMin(m.available, m.total);
    m.used = m.total - m.available;
    m.swapUsed = qMax<qint64>(0, m.swapTotal - m.swapFree);
    return m;
}

QVariantMap MemorySample::toVariant() const
{
    return {
        {u"valid"_s, valid},
        {u"total"_s, total},
        {u"used"_s, used},
        {u"available"_s, available},
        {u"free"_s, free},
        {u"buffers"_s, buffers},
        {u"cached"_s, cached},
        {u"shared"_s, shared},
        {u"usedPercent"_s, round1(usedFraction() * 100.0)},
        {u"swapTotal"_s, swapTotal},
        {u"swapUsed"_s, swapUsed},
        {u"swapFree"_s, swapFree},
        {u"swapUsedPercent"_s, round1(swapFraction() * 100.0)},
    };
}
