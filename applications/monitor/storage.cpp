// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.h"

#include "sysfs.h"

#include <QFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <sys/stat.h>
#include <sys/statvfs.h>

using namespace Qt::StringLiterals;

namespace {

double round1(double v)
{
    return std::round(v * 10.0) / 10.0;
}

// mountinfo escapes space, tab, newline and backslash as \ooo.
QString unescapeMountField(const QByteArray &field)
{
    QByteArray out;
    out.reserve(field.size());
    for (qsizetype i = 0; i < field.size(); ++i) {
        const auto octal = [&field](qsizetype at) { return field.at(at) >= '0' && field.at(at) <= '7'; };
        if (field.at(i) == '\\' && i + 3 < field.size() && octal(i + 1) && octal(i + 2) && octal(i + 3)) {
            out.append(char((field.at(i + 1) - '0') * 64 + (field.at(i + 2) - '0') * 8 + (field.at(i + 3) - '0')));
            i += 3;
        } else {
            out.append(field.at(i));
        }
    }
    return QFile::decodeName(out);
}

// File systems that are views of kernel state or of other file systems, not
// storage of their own.
bool isVirtualFilesystem(const QString &type)
{
    static const QSet<QString> types = {
        u"proc"_s, u"sysfs"_s, u"devtmpfs"_s, u"devpts"_s, u"tmpfs"_s, u"ramfs"_s, u"rootfs"_s,
        u"cgroup"_s, u"cgroup2"_s, u"pstore"_s, u"bpf"_s, u"debugfs"_s, u"tracefs"_s,
        u"securityfs"_s, u"configfs"_s, u"fusectl"_s, u"mqueue"_s, u"hugetlbfs"_s, u"autofs"_s,
        u"binfmt_misc"_s, u"efivarfs"_s, u"rpc_pipefs"_s, u"nsfs"_s, u"selinuxfs"_s, u"nfsd"_s,
        u"squashfs"_s, u"overlay"_s, u"fuse.portal"_s, u"fuse.gvfsd-fuse"_s, u"fuse.snapfuse"_s,
    };
    return types.contains(type);
}

struct MountEntry
{
    QString root;           // path inside the file system that is mounted
    QString mountPoint;
    QString type;
    QString source;
};

QList<MountEntry> readMountInfo()
{
    // "36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue"
    QList<MountEntry> entries;
    const QList<QByteArray> lines = Sys::readFile(u"/proc/self/mountinfo"_s, 8 << 20).split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> fields = line.split(' ');
        const qsizetype separator = fields.indexOf(QByteArrayLiteral("-"));
        if (fields.size() < 7 || separator < 6 || separator + 2 >= fields.size())
            continue;
        MountEntry e;
        e.root = unescapeMountField(fields.at(3));
        e.mountPoint = unescapeMountField(fields.at(4));
        e.type = QString::fromLatin1(fields.at(separator + 1));
        e.source = unescapeMountField(fields.at(separator + 2));
        entries << e;
    }
    return entries;
}

bool statFilesystem(const QString &path, FilesystemInfo &info)
{
    struct statvfs vfs {};
    if (::statvfs(QFile::encodeName(path).constData(), &vfs) != 0 || vfs.f_blocks == 0)
        return false;
    const quint64 unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    info.total = qint64(quint64(vfs.f_blocks) * unit);
    info.used = qint64(quint64(vfs.f_blocks - vfs.f_bfree) * unit);
    info.available = qint64(quint64(vfs.f_bavail) * unit);
    info.readOnly = (vfs.f_flag & ST_RDONLY) != 0;
    return true;
}

} // namespace

// ---- file systems --------------------------------------------------------------

QVariantMap FilesystemInfo::toVariant() const
{
    return {
        {u"mountPoint"_s, mountPoint},
        {u"device"_s, device},
        {u"type"_s, type},
        {u"readOnly"_s, readOnly},
        {u"total"_s, total},
        {u"used"_s, used},
        {u"available"_s, available},
        {u"usedPercent"_s, round1(usedFraction() * 100.0)},
    };
}

FilesystemSample FilesystemSample::read()
{
    FilesystemSample sample;
    sample.valid = true;
    const QList<MountEntry> entries = readMountInfo();

    // Of several mounts on the same directory only the last one is visible.
    QHash<QString, qsizetype> visible;
    for (qsizetype i = 0; i < entries.size(); ++i)
        visible.insert(entries.at(i).mountPoint, i);

    struct Candidate
    {
        FilesystemInfo info;
        qsizetype rootLength = 0;
    };
    QList<Candidate> chosen;
    QHash<quint64, qsizetype> byDevice;     // st_dev → index in 'chosen'

    for (qsizetype i = 0; i < entries.size(); ++i) {
        const MountEntry &e = entries.at(i);
        if (visible.value(e.mountPoint) != i)
            continue;
        if (isVirtualFilesystem(e.type) && e.mountPoint != "/"_L1)
            continue;
        struct stat st {};
        if (::stat(QFile::encodeName(e.mountPoint).constData(), &st) != 0)
            continue;
        Candidate c;
        c.rootLength = e.root.size();
        c.info.mountPoint = e.mountPoint;
        c.info.device = e.source;
        c.info.type = e.type;
        if (!statFilesystem(e.mountPoint, c.info))
            continue;

        // The same file system mounted several times (bind mounts, btrfs
        // subvolumes share one pool but have their own st_dev) is listed once:
        // the mount closest to the root of the file system wins, then the
        // shortest mount point — the rule df(1) applies.
        const quint64 dev = quint64(st.st_dev);
        const auto existing = byDevice.constFind(dev);
        if (existing == byDevice.constEnd()) {
            byDevice.insert(dev, chosen.size());
            chosen << c;
        } else {
            Candidate &other = chosen[*existing];
            const bool better = c.rootLength != other.rootLength
                ? c.rootLength < other.rootLength
                : c.info.mountPoint.size() < other.info.mountPoint.size();
            if (better)
                other = c;
        }
    }
    for (const Candidate &c : std::as_const(chosen))
        sample.filesystems << c.info;

    // The root directory, whether or not it is a mount point of its own
    // (inside a chroot or container it usually is not).
    sample.root.mountPoint = u"/"_s;
    sample.rootValid = statFilesystem(u"/"_s, sample.root);
    if (sample.rootValid) {
        struct stat st {};
        if (::stat("/", &st) == 0) {
            const auto it = byDevice.constFind(quint64(st.st_dev));
            if (it != byDevice.constEnd()) {
                sample.root.device = chosen.at(*it).info.device;
                sample.root.type = chosen.at(*it).info.type;
            }
        }
    }
    return sample;
}

QVariantMap FilesystemSample::toVariant() const
{
    QVariantList list;
    for (const FilesystemInfo &fs : filesystems)
        list << fs.toVariant();
    return {
        {u"filesystems"_s, list},
        {u"root"_s, rootValid ? QVariant(root.toVariant()) : QVariant::fromValue(nullptr)},
    };
}

// ---- block devices -------------------------------------------------------------

QVariantMap DiskInfo::toVariant() const
{
    return {
        {u"name"_s, name},
        {u"model"_s, model},
        {u"kind"_s, kind},
        {u"size"_s, size},
        {u"readBytes"_s, qint64(readBytes)},
        {u"writeBytes"_s, qint64(writeBytes)},
        {u"readBytesPerSecond"_s, std::round(readRate)},
        {u"writeBytesPerSecond"_s, std::round(writeRate)},
        {u"busyPercent"_s, round1(busy * 100.0)},
    };
}

QVariantMap DiskSample::toVariant() const
{
    QVariantList list;
    for (const DiskInfo &disk : disks)
        list << disk.toVariant();
    return {
        {u"ratesValid"_s, ratesValid},
        {u"devices"_s, list},
        {u"readBytesPerSecond"_s, std::round(readRate)},
        {u"writeBytesPerSecond"_s, std::round(writeRate)},
    };
}

void DiskSource::resetRates()
{
    m_previousTime = 0;
    for (Device &device : m_devices)
        device.haveCounters = false;
}

void DiskSource::describe(const QString &name, Device &device) const
{
    // Names with a slash (cciss/c0d0) use '!' in sysfs.
    QString sysName = name;
    sysName.replace(u'/', u'!');
    const QString base = u"/sys/block/"_s + sysName;
    device.physical = Sys::exists(base + u"/device"_s);
    if (!device.physical)
        return;

    device.model = Sys::readText(base + u"/device/model"_s).simplified();
    if (device.model.isEmpty())
        device.model = Sys::readText(base + u"/device/name"_s).simplified();     // MMC/SD cards

    const QString driver = Sys::linkName(base + u"/device/driver"_s);
    const bool removable = Sys::readInteger(base + u"/removable"_s).value_or(0) == 1;
    const bool rotational = Sys::readInteger(base + u"/queue/rotational"_s).value_or(0) == 1;
    if (name.startsWith("nvme"_L1))
        device.kind = u"nvme"_s;
    else if (name.startsWith("mmcblk"_L1))
        device.kind = u"mmc"_s;
    else if (name.startsWith("sr"_L1))
        device.kind = u"optical"_s;
    else if (driver.startsWith("virtio"_L1) || name.startsWith("vd"_L1) || name.startsWith("xvd"_L1))
        device.kind = u"virtual"_s;
    else if (removable)
        device.kind = u"removable"_s;
    else
        device.kind = rotational ? u"hdd"_s : u"ssd"_s;
}

DiskSample DiskSource::sample()
{
    DiskSample s;
    const double now = Sys::monotonicSeconds();
    const double elapsed = m_previousTime > 0 ? now - m_previousTime : 0;
    s.ratesValid = elapsed > 0;
    ++m_generation;

    if (!Sys::readFile("/proc/diskstats", m_buffer))
        return s;

    // "major minor name reads merged sectors ms writes merged sectors ms in-flight io-ms weighted-ms ..."
    const char *p = m_buffer.constData();
    const char *const end = p + m_buffer.size();
    while (p < end) {
        const char *eol = static_cast<const char *>(std::memchr(p, '\n', size_t(end - p)));
        if (!eol)
            eol = end;
        const QList<QByteArray> fields = QByteArray::fromRawData(p, eol - p).simplified().split(' ');
        p = eol + 1;
        if (fields.size() < 14)
            continue;

        const QString name = QString::fromLatin1(fields.at(2));
        auto it = m_devices.find(name);
        if (it == m_devices.end()) {
            Device device;
            describe(name, device);
            it = m_devices.insert(name, device);
        }
        it->generation = m_generation;
        if (!it->physical)
            continue;

        QString sysName = name;
        sysName.replace(u'/', u'!');
        it->size = Sys::readInteger(u"/sys/block/%1/size"_s.arg(sysName)).value_or(0) * 512;

        Counters c;
        c.readSectors = fields.at(5).toULongLong();
        c.writeSectors = fields.at(9).toULongLong();
        c.ioMs = fields.at(12).toULongLong();

        DiskInfo info;
        info.name = name;
        info.model = it->model;
        info.kind = it->kind;
        info.size = it->size;
        info.readBytes = c.readSectors * 512;
        info.writeBytes = c.writeSectors * 512;
        if (it->haveCounters && elapsed > 0) {
            // A counter that went backwards means the device was replaced; show no rate for it.
            if (c.readSectors >= it->counters.readSectors)
                info.readRate = double(c.readSectors - it->counters.readSectors) * 512.0 / elapsed;
            if (c.writeSectors >= it->counters.writeSectors)
                info.writeRate = double(c.writeSectors - it->counters.writeSectors) * 512.0 / elapsed;
            if (c.ioMs >= it->counters.ioMs)
                info.busy = qBound(0.0, double(c.ioMs - it->counters.ioMs) / (elapsed * 1000.0), 1.0);
        }
        it->counters = c;
        it->haveCounters = true;

        if (info.size <= 0)
            continue;               // card reader or optical drive without a medium
        s.readRate += info.readRate;
        s.writeRate += info.writeRate;
        s.disks << info;
    }

    for (auto it = m_devices.begin(); it != m_devices.end();) {
        if (it->generation != m_generation)
            it = m_devices.erase(it);
        else
            ++it;
    }
    std::sort(s.disks.begin(), s.disks.end(), [](const DiskInfo &a, const DiskInfo &b) {
        return Sys::naturalLess(a.name, b.name);
    });
    m_previousTime = now;
    return s;
}
