// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantMap>

// One process, as read from /proc/<pid>/.
struct ProcessRecord
{
    int pid = 0;
    int ppid = 0;
    QString name;                   // executable name; taken from argv[0] where the kernel cut it to 15 characters
    QString user;                   // effective user (the numeric id if it has no passwd entry)
    quint32 uid = 0;
    double cpu = 0;                 // percent of one CPU since the previous scan (100 = one core busy), like top(1)
    double cpuTime = 0;             // seconds of CPU time (user + system) since the process started
    qint64 memory = 0;              // resident set size in bytes
    char state = '?';               // state letter of /proc/<pid>/stat
    int threads = 0;
    int nice = 0;
    quint64 startTicks = 0;         // start time in clock ticks since boot; with the PID it identifies the process
    QString command;                // command line; "[name]" for kernel threads, as ps(1) prints them
    bool kernelThread = false;
    int depth = 0;                  // nesting level in the tree view (set by ProcessModel)

    bool operator==(const ProcessRecord &other) const = default;

    QString stateText() const;
    QVariantMap toVariant() const;
};

// Reads the process table. One scan costs two small reads per process
// (/proc/<pid>/stat and status); the command line, which rarely changes, is
// re-read only for new processes, after an exec, and every few scans.
// The CPU share is computed from the CPU time consumed between two scans.
class ProcessReader
{
    Q_DECLARE_TR_FUNCTIONS(ProcessReader)

public:
    ProcessReader();

    QList<ProcessRecord> scan();

    struct Identity
    {
        bool exists = false;
        quint64 startTicks = 0;
        bool kernelThread = false;
    };
    // Current identity of 'pid', to make sure a signal reaches the process
    // the user chose and not a newer one that got the same number.
    static Identity identify(int pid);

    static QString stateText(char state);

private:
    struct Entry
    {
        quint64 startTicks = 0;
        quint64 cpuTicks = 0;
        QByteArray comm;
        QString name;
        QString user;
        quint32 uid = 0;
        QString command;
        bool kernelThread = false;
        int generation = 0;
    };

    static bool readStatus(int pid, quint32 &uid, qint64 &residentBytes);
    void describe(int pid, Entry &entry);
    QString userName(quint32 uid);

    QHash<int, Entry> m_entries;
    QHash<quint32, QString> m_users;
    double m_previousScan = 0;
    int m_generation = 0;
    double m_ticksPerSecond = 100;
};
