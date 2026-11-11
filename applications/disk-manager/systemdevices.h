// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

// Which whole disks the running system depends on.
//
// A disk is "in use by the system" when it (or one of its partitions, or a
// device-mapper/RAID device stacked on them) carries the file system mounted
// at /, /boot, /boot/efi, /efi or /usr, an active swap area, or the live
// medium of a live session. Destructive actions on such disks are refused.
//
// Sources: stat(2) of those directories, /proc/self/mountinfo, /proc/swaps
// and the device topology in /sys/class/block (partition → disk, holders).
class SystemDevices
{
public:
    // Kernel names of whole disks ("sda", "nvme0n1", "loop3") → why they are protected.
    static QHash<QString, QString> protectedDisks();

    // Whole disks below a block device, following partitions and
    // device-mapper/RAID members: "sda2" → {"sda"}, "dm-0" → {"nvme0n1"}.
    static QStringList wholeDisks(const QString &kernelName);

    // True if every disk below the device is a loop device.
    static bool isLoopBacked(const QString &kernelName);
};
