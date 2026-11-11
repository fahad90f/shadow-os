// SPDX-License-Identifier: GPL-3.0-or-later
#include "processreader.h"

#include "sysfs.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <pwd.h>
#include <time.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace {

constexpr quint64 kKernelThreadFlag = 0x00200000;   // PF_KTHREAD (include/linux/sched.h)

struct StatFields
{
    QByteArrayView comm;
    char state = '?';
    int ppid = 0;
    quint64 flags = 0;
    quint64 userTicks = 0;
    quint64 systemTicks = 0;
    int nice = 0;
    int threads = 0;
    quint64 startTicks = 0;
};

quint64 parseUnsigned(const char *p, const char *end)
{
    quint64 value = 0;
    while (p < end && *p >= '0' && *p <= '9')
        value = value * 10 + quint64(*p++ - '0');
    return value;
}

qint64 parseSigned(const char *p, const char *end)
{
    const bool negative = p < end && *p == '-';
    if (negative)
        ++p;
    const qint64 value = qint64(parseUnsigned(p, end));
    return negative ? -value : value;
}

// /proc/<pid>/stat: "pid (comm) state ppid pgrp session tty tpgid flags minflt
// cminflt majflt cmajflt utime stime cutime cstime priority nice num_threads
// itrealvalue starttime ...". comm may contain spaces and parentheses, so it
// ends at the last ')'.
bool parseStat(const char *buffer, qsizetype length, StatFields &out)
{
    const char *const end = buffer + length;
    const char *open = static_cast<const char *>(std::memchr(buffer, '(', size_t(length)));
    if (!open)
        return false;
    const char *close = end - 1;
    while (close > open && *close != ')')
        --close;
    if (close <= open)
        return false;
    out.comm = QByteArrayView(open + 1, close - open - 1);

    const char *p = close + 1;
    for (int field = 3; p < end; ++field) {
        while (p < end && *p == ' ')
            ++p;
        const char *token = p;
        while (p < end && *p != ' ' && *p != '\n')
            ++p;
        if (token == p)
            return false;
        switch (field) {
        case 3: out.state = *token; break;
        case 4: out.ppid = int(parseSigned(token, p)); break;
        case 9: out.flags = parseUnsigned(token, p); break;
        case 14: out.userTicks = parseUnsigned(token, p); break;
        case 15: out.systemTicks = parseUnsigned(token, p); break;
        case 19: out.nice = int(parseSigned(token, p)); break;
        case 20: out.threads = int(parseSigned(token, p)); break;
        case 22:
            out.startTicks = parseUnsigned(token, p);
            return true;
        default: break;
        }
    }
    return false;
}

// Seconds since boot on the clock process start times are measured with.
double boottimeSeconds()
{
    struct timespec ts {};
    if (::clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
        return 0;
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

} // namespace

QString ProcessReader::stateText(char state)
{
    switch (state) {
    case 'R': return tr("Running");
    case 'S': return tr("Sleeping");
    case 'D': return tr("Waiting for I/O");
    case 'I': return tr("Idle");
    case 'Z': return tr("Zombie");
    case 'T': return tr("Stopped");
    case 't': return tr("Traced");
    case 'X':
    case 'x': return tr("Dead");
    case 'P': return tr("Parked");
    case 'K': return tr("Wakekill");
    case 'W': return tr("Waking");
    default: return QString(QLatin1Char(state));
    }
}

QString ProcessRecord::stateText() const
{
    return ProcessReader::stateText(state);
}

QVariantMap ProcessRecord::toVariant() const
{
    return {
        {u"pid"_s, pid},
        {u"ppid"_s, ppid},
        {u"name"_s, name},
        {u"user"_s, user},
        {u"uid"_s, uid},
        {u"cpu"_s, std::round(cpu * 10.0) / 10.0},
        {u"cpuTime"_s, std::round(cpuTime * 100.0) / 100.0},
        {u"memory"_s, memory},
        {u"state"_s, stateText()},
        {u"stateCode"_s, QString(QLatin1Char(state))},
        {u"threads"_s, threads},
        {u"nice"_s, nice},
        {u"command"_s, command},
        {u"kernelThread"_s, kernelThread},
    };
}

ProcessReader::ProcessReader()
{
    const long ticks = ::sysconf(_SC_CLK_TCK);
    if (ticks > 0)
        m_ticksPerSecond = double(ticks);
}

ProcessReader::Identity ProcessReader::identify(int pid)
{
    Identity identity;
    char path[40];
    char buffer[1024];
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    const qsizetype length = Sys::readOnce(path, buffer, sizeof buffer);
    StatFields st;
    if (length <= 0 || !parseStat(buffer, length, st))
        return identity;
    identity.exists = true;
    identity.startTicks = st.startTicks;
    identity.kernelThread = (st.flags & kKernelThreadFlag) != 0;
    return identity;
}

QString ProcessReader::userName(quint32 uid)
{
    const auto it = m_users.constFind(uid);
    if (it != m_users.constEnd())
        return *it;

    QString name;
    std::vector<char> storage(16384);
    struct passwd pwd {};
    struct passwd *result = nullptr;
    if (::getpwuid_r(uid, &pwd, storage.data(), storage.size(), &result) == 0 && result && result->pw_name)
        name = QString::fromLocal8Bit(result->pw_name);
    if (name.isEmpty())
        name = QString::number(uid);
    m_users.insert(uid, name);
    return name;
}

// /proc/<pid>/status: the effective user id — the second number of
// "Uid:\t<real>\t<effective>\t<saved>\t<fs>" — and the resident set size
// "VmRSS:\t  1234 kB". (The owner of /proc/<pid> would be cheaper to get, but
// reads root for every process that is not dumpable; the rss field of
// /proc/<pid>/stat is cheaper too, but only approximate — the kernel batches
// its per-CPU counters — whereas this is the figure ps(1) prints.)
// Kernel threads and zombies have no VmRSS line: they hold no memory.
bool ProcessReader::readStatus(int pid, quint32 &uid, qint64 &residentBytes)
{
    char path[40];
    char buffer[4096];
    std::snprintf(path, sizeof path, "/proc/%d/status", pid);
    if (Sys::readOnce(path, buffer, sizeof buffer) <= 0)
        return false;

    const auto skipBlanks = [](const char *p) {
        while (*p == ' ' || *p == '\t')
            ++p;
        return p;
    };
    if (const char *line = std::strstr(buffer, "\nUid:")) {
        const char *p = skipBlanks(line + 5);
        while (*p >= '0' && *p <= '9')
            ++p;
        p = skipBlanks(p);
        if (*p >= '0' && *p <= '9') {
            quint32 value = 0;
            while (*p >= '0' && *p <= '9')
                value = value * 10 + quint32(*p++ - '0');
            uid = value;
        }
    }
    residentBytes = 0;
    if (const char *line = std::strstr(buffer, "\nVmRSS:")) {
        const char *p = skipBlanks(line + 7);
        qint64 kib = 0;
        while (*p >= '0' && *p <= '9')
            kib = kib * 10 + (*p++ - '0');
        residentBytes = kib * 1024;
    }
    return true;
}

// Command line and display name of a process.
void ProcessReader::describe(int pid, Entry &e)
{
    char path[40];
    char buffer[4096];

    // Arguments are separated by NUL bytes.
    std::snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    qsizetype length = Sys::readOnce(path, buffer, sizeof buffer);
    QByteArray argv0;
    e.command.clear();
    if (length > 0) {
        argv0 = QByteArray(buffer);
        const bool truncated = length == qsizetype(sizeof buffer) - 1;
        while (length > 0 && static_cast<unsigned char>(buffer[length - 1]) <= 0x20)
            --length;
        for (qsizetype i = 0; i < length; ++i) {
            if (static_cast<unsigned char>(buffer[i]) < 0x20)
                buffer[i] = ' ';
        }
        e.command = QString::fromUtf8(buffer, length);
        if (truncated)
            e.command += QChar(0x2026);
    }

    e.name = QString::fromUtf8(e.comm);
    if (e.kernelThread) {
        e.command = QLatin1Char('[') + e.name + QLatin1Char(']');
    } else if (e.comm.size() >= 15 && !argv0.isEmpty()) {
        // The kernel keeps only 15 bytes of the name; argv[0] usually has the rest.
        const QByteArray base = argv0.mid(argv0.lastIndexOf('/') + 1);
        if (base.startsWith(e.comm) && base.size() <= 64)
            e.name = QString::fromUtf8(base);
    }
}

QList<ProcessRecord> ProcessReader::scan()
{
    QList<ProcessRecord> list;
    DIR *dir = ::opendir("/proc");
    if (!dir)
        return list;

    const double now = Sys::monotonicSeconds();
    const double elapsed = m_previousScan > 0 ? now - m_previousScan : 0;
    const double sinceBoot = boottimeSeconds();
    ++m_generation;
    list.reserve(m_entries.size() + 16);

    char path[40];
    char buffer[1024];
    while (const dirent *entry = ::readdir(dir)) {
        const char *digits = entry->d_name;
        if (*digits < '1' || *digits > '9')
            continue;
        int pid = 0;
        while (*digits >= '0' && *digits <= '9')
            pid = pid * 10 + (*digits++ - '0');
        if (*digits != '\0')
            continue;

        std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
        const qsizetype length = Sys::readOnce(path, buffer, sizeof buffer);
        StatFields st;
        if (length <= 0 || !parseStat(buffer, length, st))
            continue;               // exited while the directory was being read

        Entry &e = m_entries[pid];
        const bool isNew = e.generation == 0 || e.startTicks != st.startTicks;
        const quint64 ticks = st.userTicks + st.systemTicks;

        ProcessRecord r;
        if (isNew) {
            e = Entry();
            e.startTicks = st.startTicks;
            // A process born after the previous scan used all of its CPU time
            // within this interval; an older one that is merely new to the
            // reader has no baseline yet.
            const double age = sinceBoot - double(st.startTicks) / m_ticksPerSecond;
            if (elapsed > 0 && sinceBoot > 0 && age <= elapsed + 0.05)
                r.cpu = double(ticks) / m_ticksPerSecond / elapsed * 100.0;
        } else if (elapsed > 0 && ticks >= e.cpuTicks) {
            r.cpu = double(ticks - e.cpuTicks) / m_ticksPerSecond / elapsed * 100.0;
        }
        e.cpuTicks = ticks;

        qint64 resident = 0;
        const quint32 previousUid = e.uid;
        if (!readStatus(pid, e.uid, resident) && isNew)
            continue;               // exited between the two reads
        if (isNew || e.uid != previousUid)
            e.user = userName(e.uid);

        // exec() and prctl(PR_SET_NAME) change the name; the command line can
        // be rewritten at any time, so it is refreshed every 8th scan as well.
        if (isNew || e.comm != st.comm || ((m_generation + pid) & 7) == 0) {
            e.comm = st.comm.toByteArray();
            e.kernelThread = (st.flags & kKernelThreadFlag) != 0;
            describe(pid, e);
        }
        e.generation = m_generation;

        r.pid = pid;
        r.ppid = st.ppid;
        r.name = e.name;
        r.user = e.user;
        r.uid = e.uid;
        r.cpuTime = double(ticks) / m_ticksPerSecond;
        r.memory = resident;
        r.state = st.state;
        r.threads = st.threads;
        r.nice = st.nice;
        r.startTicks = st.startTicks;
        r.command = e.command;
        r.kernelThread = e.kernelThread;
        list << r;
    }
    ::closedir(dir);

    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->generation != m_generation)
            it = m_entries.erase(it);
        else
            ++it;
    }
    m_previousScan = now;
    return list;
}
