// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

using UDisksInterfaces = QMap<QString, QVariantMap>;                    // interface → properties
using UDisksObjects = QMap<QDBusObjectPath, UDisksInterfaces>;          // object path → interfaces
Q_DECLARE_METATYPE(UDisksInterfaces)
Q_DECLARE_METATYPE(UDisksObjects)

class ImageWorker;

// Drives, partitions and storage jobs as reported by UDisks2
// (org.freedesktop.UDisks2 on the system bus), and the actions on them.
//
// The object tree is fetched once with ObjectManager.GetManagedObjects and
// then kept current from the InterfacesAdded / InterfacesRemoved /
// PropertiesChanged signals. Every action is an asynchronous D-Bus call;
// authorisation prompts come from UDisks through polkit. The application
// itself needs no privilege and runs no tool as root.
class UDisksClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY availabilityChanged)
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY availabilityChanged)
    Q_PROPERTY(QString version READ version NOTIFY availabilityChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    // File-system types UDisks can create right now (its mkfs tool is installed).
    Q_PROPERTY(QStringList formatTypes READ formatTypes NOTIFY capabilitiesChanged)
    // Per type: {format, formatTool, resize, resizeMode, resizeTool, check, checkTool, repair, repairTool}
    Q_PROPERTY(QVariantMap capabilities READ capabilities NOTIFY capabilitiesChanged)
    Q_PROPERTY(QVariantMap imageJob READ imageJob NOTIFY imageJobChanged)
    Q_PROPERTY(int pendingOperations READ pendingOperations NOTIFY pendingChanged)
public:
    explicit UDisksClient(QObject *parent = nullptr);
    ~UDisksClient() override;

    bool loaded() const { return m_loaded; }
    bool available() const { return m_available; }
    QString unavailableReason() const { return m_reason; }
    QString version() const { return m_version; }
    QVariantList devices() const { return m_devices; }
    QVariantList jobs() const { return m_jobs; }
    QStringList formatTypes() const;
    QVariantMap capabilities() const { return m_capabilities; }
    QVariantMap imageJob() const;
    int pendingOperations() const { return m_pending; }
    bool capabilitiesLoaded() const { return m_capabilityCalls == 0 && m_capabilitiesRequested; }

    QJsonObject protectedDisksJson() const;

    // (Re)connect to UDisks and fetch everything.
    Q_INVOKABLE void reload();

    // Why a destructive action (format, delete partition, new partition
    // table, restore image) on this block device is refused; empty = allowed.
    Q_INVOKABLE QString destructiveRefusal(const QString &blockPath) const;

    // ---- actions; each ends with operationFinished() -------------------------
    Q_INVOKABLE void mount(const QString &blockPath);
    Q_INVOKABLE void unmount(const QString &blockPath);
    Q_INVOKABLE void eject(const QString &drivePath);
    Q_INVOKABLE void powerOff(const QString &drivePath);
    // The passphrase goes to UDisks in the D-Bus call only; it is never stored or logged.
    Q_INVOKABLE void unlock(const QString &blockPath, const QString &passphrase);
    Q_INVOKABLE void lock(const QString &blockPath);
    Q_INVOKABLE void setLabel(const QString &blockPath, const QString &label);
    Q_INVOKABLE void check(const QString &blockPath);
    Q_INVOKABLE void repair(const QString &blockPath);
    // New size of the partition in bytes; the file system on it follows.
    Q_INVOKABLE void resize(const QString &blockPath, double newSize);
    // size 0 = all of the free space that starts at offset.
    Q_INVOKABLE void createPartition(const QString &tablePath, double offset, double size, const QString &fsType, const QString &label);
    // Destructive (see destructiveRefusal()).
    Q_INVOKABLE void format(const QString &blockPath, const QString &fsType, const QString &label);
    Q_INVOKABLE void deletePartition(const QString &blockPath);
    Q_INVOKABLE void createPartitionTable(const QString &blockPath, const QString &type);     // "gpt" | "dos"
    Q_INVOKABLE void restoreImage(const QString &blockPath, const QString &file);
    // Not destructive: copies the device into a file.
    Q_INVOKABLE void createImage(const QString &blockPath, const QString &file);
    Q_INVOKABLE void cancelImageJob();
    Q_INVOKABLE void cancelJob(const QString &jobPath);
    // Ask the drive for fresh SMART data.
    Q_INVOKABLE void refreshSmart(const QString &drivePath);

    // Helpers for the dialogs.
    Q_INVOKABLE QVariantMap imageFileInfo(const QString &file) const;      // {exists, size, readable, directoryWritable, path}
    Q_INVOKABLE QString defaultImagePath(const QString &deviceName) const;

Q_SIGNALS:
    void availabilityChanged();
    void devicesChanged();
    void jobsChanged();
    void capabilitiesChanged();
    void imageJobChanged();
    void pendingChanged();
    // operation: human-readable ("Format /dev/sdb1"); message: detail or error text.
    void operationFinished(const QString &operation, bool ok, const QString &message);

private Q_SLOTS:
    void interfacesAdded(const QDBusObjectPath &path, const UDisksInterfaces &interfaces);
    void interfacesRemoved(const QDBusObjectPath &path, const QStringList &interfaces);
    void propertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated, const QDBusMessage &message);

private:
    using Reply = std::function<void(const QDBusMessage &)>;
    void setUnavailable(const QString &reason);
    void fetchCapabilities();
    void fetchSmartAttributes();
    void rebuild();
    void scheduleRebuild();
    QVariantMap volumeFor(const QString &blockPath, const QHash<QString, QString> &protectedDisks) const;
    QVariantMap smartFor(const QString &drivePath) const;
    QVariantMap property(const QString &path, const char *interface) const;
    bool hasInterface(const QString &path, const char *interface) const;
    QString deviceOf(const QString &blockPath) const;
    QStringList mountPointsBelow(const QString &blockPath) const;
    // Refuse (returns false and reports) when a modifying action is not allowed.
    bool guard(const QString &operation, const QString &blockPath, bool destructive);
    void call(const QString &operation, const QString &path, const QString &interface, const QString &method,
              const QVariantList &arguments, Reply onSuccess = {});
    void finish(const QString &operation, bool ok, const QString &message);
    void startImage(bool restore, const QString &blockPath, const QString &file);

    UDisksObjects m_objects;
    QDBusServiceWatcher m_watcher;
    QTimer m_rebuildTimer;
    QTimer m_imageTimer;
    bool m_loaded = false;
    bool m_available = false;
    bool m_capabilitiesRequested = false;
    int m_capabilityCalls = 0;
    QString m_reason;
    QString m_version;
    QVariantList m_devices;
    QVariantList m_jobs;
    QVariantMap m_capabilities;
    QHash<QString, QVariantList> m_smartAttributes;     // drive path → attribute rows
    QHash<QString, qulonglong> m_smartFetched;          // drive path → SmartUpdated of the cached rows
    int m_pending = 0;
    ImageWorker *m_image = nullptr;
    QVariantMap m_imageInfo;
};
