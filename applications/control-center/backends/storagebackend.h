// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <QMap>

#include <atomic>
#include <functional>
#include <memory>

class QThread;
class QSocketNotifier;

// Storage page.
//
// Disks and partitions   UDisks2 on the system bus (org.freedesktop.UDisks2,
//                        ObjectManager.GetManagedObjects; interfaces Block,
//                        Partition, Filesystem, Drive, Drive.Ata,
//                        NVMe.Controller). When UDisks2 cannot be reached:
//                        "lsblk -J -b" — a read-only view without mount
//                        actions and without SMART data from the service.
// File-system usage      statvfs(3) of every mounted file system (QStorageInfo);
//                        refreshed when /proc/self/mounts changes and
//                        periodically.
// SMART health           UDisks2 (Drive.Ata, NVMe.Controller); if it has none,
//                        "smartctl --json -H -A <device>" is tried, which
//                        normally works only with administrator rights —
//                        "not reported" is shown when nothing can be read.
// Removable media        Filesystem.Mount/Unmount, Drive.Eject, Drive.PowerOff
//                        of UDisks2 (polkit decides; nothing runs as root here).
// Home folder by kind    a walk of $HOME in a worker thread (fts(3), physical,
//                        not crossing file systems, hard links counted once,
//                        allocated size = st_blocks × 512). It can be
//                        cancelled, reports progress, and nothing is cached:
//                        every figure on screen comes from the walk that
//                        produced it.
class StorageBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY disksChanged)
    Q_PROPERTY(QString source READ source NOTIFY disksChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY disksChanged)
    Q_PROPERTY(QVariantList disks READ disks NOTIFY disksChanged)
    Q_PROPERTY(QVariantList volumes READ volumes NOTIFY volumesChanged)
    Q_PROPERTY(QString busyDevice READ busyDevice NOTIFY busyChanged)
    Q_PROPERTY(bool diskManagerAvailable READ diskManagerAvailable CONSTANT)
    Q_PROPERTY(QString homePath READ homePath CONSTANT)
    Q_PROPERTY(QString homeState READ homeState NOTIFY homeChanged)
    Q_PROPERTY(QVariantList homeCategories READ homeCategories NOTIFY homeChanged)
    Q_PROPERTY(double homeBytes READ homeBytes NOTIFY homeChanged)
    Q_PROPERTY(double homeFiles READ homeFiles NOTIFY homeChanged)
    Q_PROPERTY(int homeUnreadable READ homeUnreadable NOTIFY homeChanged)
    Q_PROPERTY(double homeVolumeBytes READ homeVolumeBytes NOTIFY volumesChanged)

public:
    explicit StorageBackend(QObject *parent = nullptr);
    ~StorageBackend() override;

    bool ready() const { return m_ready; }
    // Enumerated, and no smartctl query is still running (for --dump).
    bool settled() const { return m_ready && m_smartctlRunning == 0; }
    // "udisks", "lsblk" or "none"
    QString source() const { return m_source; }
    QString statusMessage() const { return m_statusMessage; }
    // Each disk: id, device, name, size, removable, kind, bus, drivePath,
    // canEject, canPowerOff, canRemoveSafely, smart {state: ok|failing|unknown,
    // text, detail}, partitions [{device, objectPath, name, size, fsType,
    // label, kind, depth, mountPoints, mounted, canMount, canUnmount,
    // usageKnown, used, total, readOnly}].
    QVariantList disks() const { return m_disks; }
    // Each mounted file system: mountPoint, device, fsType, name, total, used,
    // free, fraction, readOnly.
    QVariantList volumes() const { return m_volumes; }
    QString busyDevice() const { return m_busyDevice; }
    bool diskManagerAvailable() const;

    QString homePath() const;
    // "idle" (not started), "scanning", "done", "cancelled"
    QString homeState() const { return m_homeState; }
    // Each: id, title, bytes, files — only categories that hold something.
    QVariantList homeCategories() const { return m_homeCategories; }
    double homeBytes() const { return m_homeBytes; }
    double homeFiles() const { return m_homeFiles; }
    int homeUnreadable() const { return m_homeUnreadable; }
    // Size of the file system the home folder is on (0 if unknown).
    double homeVolumeBytes() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void mount(const QString &objectPath);
    Q_INVOKABLE void unmount(const QString &objectPath);
    // Unmount everything on the drive, then eject and power it off as far as
    // the drive supports.
    Q_INVOKABLE void removeSafely(const QString &diskId);
    Q_INVOKABLE void startHomeScan();
    Q_INVOKABLE void cancelHomeScan();
    Q_INVOKABLE bool openDiskManager();

    QJsonObject toJson() const;
    QJsonObject homeToJson() const;

Q_SIGNALS:
    void disksChanged();
    void volumesChanged();
    void busyChanged();
    void homeChanged();
    void failed(const QString &message);
    void done(const QString &message);

private Q_SLOTS:
    void scheduleRefresh();

private:
    struct ScanShared;

    void loadVolumes();
    void loadUDisks();
    void loadLsblk(const QString &reason);
    void finishDisks(const QVariantList &disks, const QString &source, const QString &message);
    void querySmartctl();
    void setBusy(const QString &device);
    void udisksCall(const QString &path, const QString &interface, const QString &method, const QString &device,
                    std::function<void(bool ok, const QString &error)> done);
    void removeStep(const QString &diskId, int step);
    void homeProgress(quint64 generation, const QVariantList &categories, double bytes, double files, int unreadable,
                      bool finished, bool cancelled);
    void stopScanThread();

    bool m_ready = false;
    QString m_source = QStringLiteral("none");
    QString m_statusMessage;
    QVariantList m_disks, m_volumes;
    QString m_busyDevice;
    QTimer m_refreshTimer, m_usageTimer;
    QSocketNotifier *m_mountNotifier = nullptr;
    int m_mountsFd = -1;
    bool m_smartUpdateRequested = false;
    int m_smartctlRunning = 0;
    QMap<QString, QVariantMap> m_smartctl;      // device → smart map read with smartctl

    QString m_homeState = QStringLiteral("idle");
    QVariantList m_homeCategories;
    double m_homeBytes = 0, m_homeFiles = 0;
    int m_homeUnreadable = 0;
    quint64 m_scanGeneration = 0;
    QThread *m_scanThread = nullptr;
    std::shared_ptr<ScanShared> m_scan;
};
