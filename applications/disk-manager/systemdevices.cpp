// SPDX-License-Identifier: GPL-3.0-or-later
#include "systemdevices.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <sys/stat.h>
#include <sys/sysmacros.h>

using namespace Qt::StringLiterals;

namespace {
QString kernelNameOf(dev_t dev)
{
    if (major(dev) == 0)
        return {};      // anonymous device (overlay, btrfs sub-volume, tmpfs)
    const QString link = u"/sys/dev/block/%1:%2"_s.arg(major(dev)).arg(minor(dev));
    const QString real = QFileInfo(link).canonicalFilePath();
    return real.isEmpty() ? QString() : QFileInfo(real).fileName();
}

void collectDisks(const QString &name, QSet<QString> &out, int depth = 0)
{
    if (name.isEmpty() || depth > 8)
        return;
    const QString sys = QFileInfo(u"/sys/class/block/"_s + name).canonicalFilePath();
    if (sys.isEmpty())
        return;
    if (QFile::exists(sys + u"/partition"_s)) {
        collectDisks(QFileInfo(QFileInfo(sys).path()).fileName(), out, depth + 1);
        return;
    }
    const QStringList slaves = QDir(sys + u"/slaves"_s).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (slaves.isEmpty()) {
        out.insert(name);
        return;
    }
    for (const QString &slave : slaves)
        collectDisks(slave, out, depth + 1);
}

struct Use
{
    QString kernelName;
    QString what;       // e.g. "the root file system (/)"
};
} // namespace

QStringList SystemDevices::wholeDisks(const QString &kernelName)
{
    QSet<QString> disks;
    collectDisks(kernelName, disks);
    QStringList list(disks.begin(), disks.end());
    list.sort();
    return list;
}

bool SystemDevices::isLoopBacked(const QString &kernelName)
{
    const QStringList disks = wholeDisks(kernelName);
    if (disks.isEmpty())
        return false;
    for (const QString &d : disks) {
        if (!d.startsWith("loop"_L1))
            return false;
    }
    return true;
}

QHash<QString, QString> SystemDevices::protectedDisks()
{
    QList<Use> uses;
    const auto describe = [](const QString &mountPoint) -> QString {
        if (mountPoint == "/"_L1)
            return QCoreApplication::translate("SystemDevices", "the running system (/)");
        if (mountPoint == "/boot"_L1 || mountPoint == "/boot/efi"_L1 || mountPoint == "/efi"_L1)
            return QCoreApplication::translate("SystemDevices", "the boot files of the running system (%1)").arg(mountPoint);
        if (mountPoint == "/usr"_L1)
            return QCoreApplication::translate("SystemDevices", "the running system (/usr)");
        return QCoreApplication::translate("SystemDevices", "the live medium this session runs from (%1)").arg(mountPoint);
    };
    const QStringList systemMounts{u"/"_s, u"/boot"_s, u"/boot/efi"_s, u"/efi"_s, u"/usr"_s};
    const QStringList liveMounts{u"/run/live/medium"_s, u"/lib/live/mount/medium"_s, u"/run/initramfs/live"_s, u"/cdrom"_s};

    // 1. The devices behind the directories themselves.
    for (const QString &dir : systemMounts) {
        struct stat st{};
        if (::stat(QFile::encodeName(dir).constData(), &st) == 0) {
            const QString name = kernelNameOf(st.st_dev);
            if (!name.isEmpty())
                uses.append({name, describe(dir)});
        }
    }
    // 2. The mount table: covers anonymous devices (btrfs) through the mount
    //    source, and the live medium wherever it is mounted.
    QFile mountinfo(u"/proc/self/mountinfo"_s);
    if (mountinfo.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = mountinfo.readAll().split('\n');
        for (const QByteArray &line : lines) {
            const QList<QByteArray> f = line.split(' ');
            const int dash = f.indexOf("-");
            if (f.size() < 6 || dash < 0 || dash + 2 >= f.size())
                continue;
            QString mountPoint = QString::fromUtf8(f.at(4));
            mountPoint.replace("\\040"_L1, " "_L1);
            const bool live = liveMounts.contains(mountPoint) || mountPoint.startsWith("/run/live/"_L1);
            if (!systemMounts.contains(mountPoint) && !live)
                continue;
            const QList<QByteArray> mm = f.at(2).split(':');
            QString name;
            if (mm.size() == 2 && mm.at(0).toUInt() != 0)
                name = kernelNameOf(makedev(mm.at(0).toUInt(), mm.at(1).toUInt()));
            if (name.isEmpty()) {
                const QString source = QString::fromUtf8(f.at(dash + 2));
                if (source.startsWith("/dev/"_L1)) {
                    struct stat st{};
                    if (::stat(QFile::encodeName(source).constData(), &st) == 0 && S_ISBLK(st.st_mode))
                        name = kernelNameOf(st.st_rdev);
                }
            }
            if (!name.isEmpty())
                uses.append({name, describe(mountPoint)});
        }
    }
    // 3. Active swap areas.
    QFile swaps(u"/proc/swaps"_s);
    if (swaps.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = swaps.readAll().split('\n');
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray device = lines.at(i).simplified().split(' ').value(0);
            if (!device.startsWith("/dev/"))
                continue;
            struct stat st{};
            if (::stat(device.constData(), &st) == 0 && S_ISBLK(st.st_mode)) {
                const QString name = kernelNameOf(st.st_rdev);
                if (!name.isEmpty())
                    uses.append({name, QCoreApplication::translate("SystemDevices", "active swap space (%1)").arg(QString::fromUtf8(device))});
            }
        }
    }

    QHash<QString, QString> result;
    for (const Use &use : std::as_const(uses)) {
        const QStringList disks = wholeDisks(use.kernelName);
        for (const QString &disk : disks) {
            const QString reason = disk == use.kernelName
                ? QCoreApplication::translate("SystemDevices", "/dev/%1 holds %2").arg(disk, use.what)
                : QCoreApplication::translate("SystemDevices", "/dev/%1 holds %2 on /dev/%3").arg(disk, use.what, use.kernelName);
            if (!result.contains(disk))
                result.insert(disk, reason);
        }
    }
    return result;
}
