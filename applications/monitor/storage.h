// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantMap>

// Usage of one mounted file system (statvfs(3)), with the numbers df(1) prints.
struct FilesystemInfo
{
    QString mountPoint;
    QString device;                 // mount source, e.g. /dev/nvme0n1p2
    QString type;                   // ext4, btrfs, vfat, nfs4, ...
    bool readOnly = false;
    qint64 total = 0;               // bytes
    qint64 used = 0;
    qint64 available = 0;           // to unprivileged users

    // Share of the usable space that is taken, as in df's "Use%".
    double usedFraction() const { return used + available > 0 ? double(used) / double(used + available) : 0; }
    QVariantMap toVariant() const;
};

struct FilesystemSample
{
    bool valid = false;
    QList<FilesystemInfo> filesystems;
    FilesystemInfo root;            // statvfs("/"), also when "/" is not a mount point of its own
    bool rootValid = false;

    // Mounted file systems that hold data: /proc/self/mountinfo without the
    // kernel's virtual ones, one entry per device. statvfs can block for as
    // long as a network server takes to answer, so call this off the UI thread.
    static FilesystemSample read();
    QVariantMap toVariant() const;
};

// Activity of one physical block device (/proc/diskstats, /sys/block).
struct DiskInfo
{
    QString name;                   // sda, nvme0n1, mmcblk0, vda
    QString model;                  // as reported by the device; may be empty
    QString kind;                   // "nvme", "ssd", "hdd", "removable", "mmc", "virtual", "optical" or ""
    qint64 size = 0;                // bytes
    quint64 readBytes = 0;          // since boot
    quint64 writeBytes = 0;
    double readRate = 0;            // bytes per second since the previous sample
    double writeRate = 0;
    double busy = 0;                // 0..1: share of the interval with I/O in flight

    QVariantMap toVariant() const;
};

struct DiskSample
{
    bool ratesValid = false;        // rates need two readings
    QList<DiskInfo> disks;
    double readRate = 0;            // sum over the physical devices
    double writeRate = 0;

    QVariantMap toVariant() const;
};

// Sources: /proc/diskstats (sector counters, 512-byte units) and
// /sys/block/<name>/{size,removable,queue/rotational,device/model}.
// Only whole devices backed by hardware are listed; partitions, loop, RAM,
// zram, device-mapper and MD devices would count the same I/O twice.
class DiskSource
{
public:
    DiskSample sample();
    void resetRates();

private:
    struct Counters
    {
        quint64 readSectors = 0;
        quint64 writeSectors = 0;
        quint64 ioMs = 0;
    };
    struct Device
    {
        bool physical = false;
        QString model;
        QString kind;
        qint64 size = 0;
        bool haveCounters = false;
        Counters counters;
        int generation = 0;
    };

    void describe(const QString &name, Device &device) const;

    QByteArray m_buffer;
    QHash<QString, Device> m_devices;
    double m_previousTime = 0;
    int m_generation = 0;
};
