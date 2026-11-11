// SPDX-License-Identifier: GPL-3.0-or-later
#include "udisksclient.h"

#include "imageworker.h"
#include "systemdevices.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QStandardPaths>

#include <algorithm>
#include <sys/statvfs.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace {
const QString kService = u"org.freedesktop.UDisks2"_s;
const QString kRoot = u"/org/freedesktop/UDisks2"_s;
const QString kManagerPath = u"/org/freedesktop/UDisks2/Manager"_s;
const char *kBlock = "org.freedesktop.UDisks2.Block";
const char *kPartition = "org.freedesktop.UDisks2.Partition";
const char *kPartitionTable = "org.freedesktop.UDisks2.PartitionTable";
const char *kFilesystem = "org.freedesktop.UDisks2.Filesystem";
const char *kEncrypted = "org.freedesktop.UDisks2.Encrypted";
const char *kSwapspace = "org.freedesktop.UDisks2.Swapspace";
const char *kLoop = "org.freedesktop.UDisks2.Loop";
const char *kDrive = "org.freedesktop.UDisks2.Drive";
const char *kDriveAta = "org.freedesktop.UDisks2.Drive.Ata";
const char *kNvme = "org.freedesktop.UDisks2.NVMe.Controller";
const char *kJob = "org.freedesktop.UDisks2.Job";
const char *kManager = "org.freedesktop.UDisks2.Manager";

// Storage operations can take hours; the D-Bus reply must not time out before.
constexpr int kLongTimeout = 24 * 60 * 60 * 1000;
constexpr qint64 kMiB = 1024 * 1024;

// libblockdev resize capability flags (Manager.CanResize).
constexpr qulonglong kOfflineShrink = 1 << 1;
constexpr qulonglong kOfflineGrow = 1 << 2;

QString bytesToString(const QVariant &v)
{
    QByteArray b = v.toByteArray();
    while (b.endsWith('\0'))
        b.chop(1);
    return QString::fromLocal8Bit(b);
}

QStringList byteArrayList(const QVariant &v)
{
    QList<QByteArray> list;
    if (v.userType() == qMetaTypeId<QDBusArgument>())
        list = qdbus_cast<QList<QByteArray>>(v.value<QDBusArgument>());
    else
        list = v.value<QList<QByteArray>>();
    QStringList out;
    for (QByteArray b : std::as_const(list)) {
        while (b.endsWith('\0'))
            b.chop(1);
        out << QString::fromLocal8Bit(b);
    }
    return out;
}

QString objectPath(const QVariant &v)
{
    if (v.userType() == qMetaTypeId<QDBusObjectPath>())
        return v.value<QDBusObjectPath>().path();
    return v.toString();
}

QStringList objectPaths(const QVariant &v)
{
    QList<QDBusObjectPath> list;
    if (v.userType() == qMetaTypeId<QDBusArgument>())
        list = qdbus_cast<QList<QDBusObjectPath>>(v.value<QDBusArgument>());
    else
        list = v.value<QList<QDBusObjectPath>>();
    QStringList out;
    for (const QDBusObjectPath &p : std::as_const(list))
        out << p.path();
    return out;
}

// "GDBus.Error:org.freedesktop.UDisks2.Error.Failed: Error …" → "Error …"
QString cleanError(const QDBusError &error)
{
    QString text = error.message();
    if (text.startsWith("GDBus.Error:"_L1)) {
        const qsizetype colon = text.indexOf(": "_L1);
        if (colon > 0)
            text = text.mid(colon + 2);
    }
    if (error.type() == QDBusError::AccessDenied || error.name().endsWith(".NotAuthorized"_L1)
        || error.name().endsWith(".NotAuthorizedDismissed"_L1) || error.name().endsWith(".NotAuthorizedCanObtain"_L1))
        return UDisksClient::tr("Not authorised: %1").arg(text);
    return text.trimmed();
}

QString busOf(const QString &kernelName, const QString &connectionBus)
{
    if (kernelName.startsWith("loop"_L1))
        return UDisksClient::tr("Loop device (file)");
    if (kernelName.startsWith("dm-"_L1))
        return UDisksClient::tr("Device mapper");
    if (kernelName.startsWith("md"_L1))
        return UDisksClient::tr("Software RAID");
    if (kernelName.startsWith("zram"_L1))
        return UDisksClient::tr("Compressed RAM");
    const QString sys = QFileInfo(u"/sys/class/block/"_s + kernelName).canonicalFilePath();
    if (connectionBus == "usb"_L1 || sys.contains("/usb"_L1))
        return u"USB"_s;
    if (connectionBus == "sdio"_L1 || kernelName.startsWith("mmcblk"_L1))
        return u"SD/MMC"_s;
    if (connectionBus == "ieee1394"_L1)
        return u"FireWire"_s;
    if (kernelName.startsWith("nvme"_L1))
        return u"NVMe"_s;
    if (sys.contains("/virtio"_L1))
        return u"VirtIO"_s;
    if (sys.contains("/ata"_L1))
        return u"SATA"_s;
    if (kernelName.startsWith("sr"_L1))
        return UDisksClient::tr("Optical");
    if (kernelName.startsWith("sd"_L1))
        return u"SCSI/SAS"_s;
    return {};
}

QString jobLabel(const QString &operation)
{
    static const QHash<QString, QString> labels{
        {u"filesystem-mount"_s, UDisksClient::tr("Mounting")},
        {u"filesystem-unmount"_s, UDisksClient::tr("Unmounting")},
        {u"filesystem-modify"_s, UDisksClient::tr("Changing the file system")},
        {u"filesystem-resize"_s, UDisksClient::tr("Resizing the file system")},
        {u"filesystem-check"_s, UDisksClient::tr("Checking the file system")},
        {u"filesystem-repair"_s, UDisksClient::tr("Repairing the file system")},
        {u"format-erase"_s, UDisksClient::tr("Erasing")},
        {u"format-mkfs"_s, UDisksClient::tr("Creating the file system")},
        {u"partition-create"_s, UDisksClient::tr("Creating the partition")},
        {u"partition-delete"_s, UDisksClient::tr("Deleting the partition")},
        {u"partition-modify"_s, UDisksClient::tr("Changing the partition")},
        {u"partitiontable-create"_s, UDisksClient::tr("Creating the partition table")},
        {u"encrypted-unlock"_s, UDisksClient::tr("Unlocking")},
        {u"encrypted-lock"_s, UDisksClient::tr("Locking")},
        {u"encrypted-modify"_s, UDisksClient::tr("Changing the encrypted volume")},
        {u"encrypted-resize"_s, UDisksClient::tr("Resizing the encrypted volume")},
        {u"drive-eject"_s, UDisksClient::tr("Ejecting")},
        {u"drive-power-off"_s, UDisksClient::tr("Powering off")},
        {u"ata-smart-update"_s, UDisksClient::tr("Reading SMART data")},
        {u"ata-smart-selftest"_s, UDisksClient::tr("SMART self-test")},
        {u"ata-secure-erase"_s, UDisksClient::tr("Secure erase")},
        {u"swapspace-start"_s, UDisksClient::tr("Activating swap")},
        {u"swapspace-stop"_s, UDisksClient::tr("Deactivating swap")},
        {u"loop-setup"_s, UDisksClient::tr("Setting up a loop device")},
        {u"cleanup"_s, UDisksClient::tr("Cleaning up")},
    };
    return labels.value(operation, operation);
}
} // namespace

UDisksClient::UDisksClient(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<UDisksInterfaces>();
    qDBusRegisterMetaType<UDisksObjects>();

    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(80);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &UDisksClient::rebuild);
    m_imageTimer.setInterval(250);
    connect(&m_imageTimer, &QTimer::timeout, this, &UDisksClient::imageJobChanged);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        m_watcher.setConnection(bus);
        m_watcher.addWatchedService(kService);
        m_watcher.setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
        connect(&m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this](const QString &, const QString &, const QString &newOwner) {
            if (newOwner.isEmpty())
                setUnavailable(tr("The storage service (udisksd) stopped."));
            else
                reload();
        });
        bus.connect(kService, kRoot, u"org.freedesktop.DBus.ObjectManager"_s, u"InterfacesAdded"_s,
                    this, SLOT(interfacesAdded(QDBusObjectPath,UDisksInterfaces)));
        bus.connect(kService, kRoot, u"org.freedesktop.DBus.ObjectManager"_s, u"InterfacesRemoved"_s,
                    this, SLOT(interfacesRemoved(QDBusObjectPath,QStringList)));
        bus.connect(kService, QString(), u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s,
                    this, SLOT(propertiesChanged(QString,QVariantMap,QStringList,QDBusMessage)));
    }
    reload();
}

UDisksClient::~UDisksClient()
{
    delete m_image;
}

void UDisksClient::setUnavailable(const QString &reason)
{
    m_loaded = true;
    m_available = false;
    m_reason = reason;
    m_objects.clear();
    m_devices.clear();
    m_jobs.clear();
    Q_EMIT availabilityChanged();
    Q_EMIT devicesChanged();
    Q_EMIT jobsChanged();
}

void UDisksClient::reload()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        setUnavailable(tr("The system message bus (D-Bus) is not reachable, so the storage service cannot be contacted."));
        return;
    }
    QDBusMessage get = QDBusMessage::createMethodCall(kService, kRoot, u"org.freedesktop.DBus.ObjectManager"_s, u"GetManagedObjects"_s);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(get, 30000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<UDisksObjects> reply = *w;
        if (reply.isError()) {
            const QDBusError e = reply.error();
            if (e.type() == QDBusError::ServiceUnknown || e.type() == QDBusError::NoReply || e.name().endsWith(".Spawn.ChildExited"_L1)
                || e.name().contains(".Spawn."_L1))
                setUnavailable(tr("The storage service UDisks2 (package udisks2) is not installed or could not be started: %1").arg(e.message()));
            else
                setUnavailable(tr("The storage service UDisks2 did not answer: %1").arg(e.message()));
            return;
        }
        m_objects = reply.value();
        m_loaded = true;
        m_available = true;
        m_reason.clear();
        m_version = property(kManagerPath, kManager).value(u"Version"_s).toString();
        Q_EMIT availabilityChanged();
        rebuild();
        fetchCapabilities();
    });
}

// ---- object tree ---------------------------------------------------------------

void UDisksClient::interfacesAdded(const QDBusObjectPath &path, const UDisksInterfaces &interfaces)
{
    if (!m_available)
        return;
    UDisksInterfaces &object = m_objects[path];
    for (auto it = interfaces.constBegin(); it != interfaces.constEnd(); ++it)
        object.insert(it.key(), it.value());
    // A job can be over within milliseconds: show it at once.
    if (interfaces.contains(QString::fromLatin1(kJob)))
        rebuild();
    else
        scheduleRebuild();
}

// Coalesces bursts of change signals without ever postponing the update:
// the model is rebuilt at most 80 ms after the first change.
void UDisksClient::scheduleRebuild()
{
    if (!m_rebuildTimer.isActive())
        m_rebuildTimer.start();
}

void UDisksClient::interfacesRemoved(const QDBusObjectPath &path, const QStringList &interfaces)
{
    if (!m_available)
        return;
    auto it = m_objects.find(path);
    if (it == m_objects.end())
        return;
    for (const QString &i : interfaces)
        it->remove(i);
    if (it->isEmpty())
        m_objects.erase(it);
    scheduleRebuild();
}

void UDisksClient::propertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated, const QDBusMessage &message)
{
    Q_UNUSED(invalidated)
    if (!m_available)
        return;
    auto it = m_objects.find(QDBusObjectPath(message.path()));
    if (it == m_objects.end())
        return;
    QVariantMap &properties = (*it)[interface];
    for (auto c = changed.constBegin(); c != changed.constEnd(); ++c)
        properties.insert(c.key(), c.value());
    scheduleRebuild();
}

QVariantMap UDisksClient::property(const QString &path, const char *interface) const
{
    const auto it = m_objects.constFind(QDBusObjectPath(path));
    if (it == m_objects.constEnd())
        return {};
    return it->value(QString::fromLatin1(interface));
}

bool UDisksClient::hasInterface(const QString &path, const char *interface) const
{
    const auto it = m_objects.constFind(QDBusObjectPath(path));
    return it != m_objects.constEnd() && it->contains(QString::fromLatin1(interface));
}

QString UDisksClient::deviceOf(const QString &blockPath) const
{
    return bytesToString(property(blockPath, kBlock).value(u"Device"_s));
}

// Mount points of the device, its partitions and unlocked volumes on them.
QStringList UDisksClient::mountPointsBelow(const QString &blockPath) const
{
    QStringList points;
    for (auto it = m_objects.constBegin(); it != m_objects.constEnd(); ++it) {
        const QString path = it.key().path();
        const QVariantMap block = it->value(QString::fromLatin1(kBlock));
        if (block.isEmpty())
            continue;
        bool below = path == blockPath;
        if (!below && it->contains(QString::fromLatin1(kPartition)))
            below = objectPath(it->value(QString::fromLatin1(kPartition)).value(u"Table"_s)) == blockPath;
        if (!below) {
            const QString backing = objectPath(block.value(u"CryptoBackingDevice"_s));
            if (backing.size() > 1) {
                below = backing == blockPath
                    || objectPath(property(backing, kPartition).value(u"Table"_s)) == blockPath;
            }
        }
        if (!below)
            continue;
        points += byteArrayList(it->value(QString::fromLatin1(kFilesystem)).value(u"MountPoints"_s));
        if (it->value(QString::fromLatin1(kSwapspace)).value(u"Active"_s).toBool())
            points << tr("active swap on %1").arg(bytesToString(block.value(u"Device"_s)));
    }
    return points;
}

QVariantMap UDisksClient::volumeFor(const QString &blockPath, const QHash<QString, QString> &protectedDisks) const
{
    const QVariantMap block = property(blockPath, kBlock);
    const QVariantMap part = property(blockPath, kPartition);
    const QString device = bytesToString(block.value(u"Device"_s));
    const QString kernelName = QFileInfo(device).fileName();

    QVariantMap v;
    v[u"path"_s] = blockPath;
    v[u"device"_s] = device;
    v[u"free"_s] = false;
    v[u"size"_s] = block.value(u"Size"_s).toULongLong();
    v[u"readOnly"_s] = block.value(u"ReadOnly"_s).toBool();
    v[u"isPartition"_s] = !part.isEmpty();
    if (!part.isEmpty()) {
        v[u"number"_s] = part.value(u"Number"_s).toUInt();
        v[u"offset"_s] = part.value(u"Offset"_s).toULongLong();
        v[u"partitionType"_s] = part.value(u"Type"_s).toString();
        v[u"partitionName"_s] = part.value(u"Name"_s).toString();
        v[u"partitionUuid"_s] = part.value(u"UUID"_s).toString();
        v[u"isContainer"_s] = part.value(u"IsContainer"_s).toBool();
        v[u"isContained"_s] = part.value(u"IsContained"_s).toBool();
        v[u"tablePath"_s] = objectPath(part.value(u"Table"_s));
    }

    // Content: directly on the block device, or inside an unlocked LUKS container.
    const bool encrypted = hasInterface(blockPath, kEncrypted) || block.value(u"IdUsage"_s).toString() == "crypto"_L1;
    QString contentPath = blockPath;
    QString cleartextPath;
    if (encrypted) {
        for (auto it = m_objects.constBegin(); it != m_objects.constEnd(); ++it) {
            if (objectPath(it->value(QString::fromLatin1(kBlock)).value(u"CryptoBackingDevice"_s)) == blockPath) {
                cleartextPath = it.key().path();
                break;
            }
        }
        if (!cleartextPath.isEmpty())
            contentPath = cleartextPath;
    }
    const QVariantMap content = property(contentPath, kBlock);
    v[u"encrypted"_s] = encrypted;
    v[u"unlocked"_s] = !cleartextPath.isEmpty();
    v[u"canUnlock"_s] = hasInterface(blockPath, kEncrypted);
    v[u"cleartextDevice"_s] = cleartextPath.isEmpty() ? QString() : bytesToString(content.value(u"Device"_s));
    v[u"contentPath"_s] = contentPath;
    v[u"containerType"_s] = encrypted ? block.value(u"IdType"_s).toString() : QString();
    const bool locked = encrypted && cleartextPath.isEmpty();
    v[u"fsType"_s] = locked ? block.value(u"IdType"_s).toString() : content.value(u"IdType"_s).toString();
    v[u"usage"_s] = locked ? u"crypto"_s : content.value(u"IdUsage"_s).toString();
    v[u"fsVersion"_s] = locked ? block.value(u"IdVersion"_s).toString() : content.value(u"IdVersion"_s).toString();
    v[u"label"_s] = locked ? block.value(u"IdLabel"_s).toString() : content.value(u"IdLabel"_s).toString();
    v[u"uuid"_s] = locked ? block.value(u"IdUUID"_s).toString() : content.value(u"IdUUID"_s).toString();
    v[u"hasFilesystem"_s] = hasInterface(contentPath, kFilesystem);
    const QStringList mountPoints = byteArrayList(property(contentPath, kFilesystem).value(u"MountPoints"_s));
    v[u"mountPoints"_s] = mountPoints;
    v[u"mounted"_s] = !mountPoints.isEmpty();
    v[u"isSwap"_s] = hasInterface(contentPath, kSwapspace);
    v[u"swapActive"_s] = property(contentPath, kSwapspace).value(u"Active"_s).toBool();
    v[u"hasTable"_s] = hasInterface(blockPath, kPartitionTable);

    // Usage is only known while the file system is mounted (statvfs).
    v[u"usageKnown"_s] = false;
    if (!mountPoints.isEmpty()) {
        struct statvfs st{};
        if (::statvfs(QFile::encodeName(mountPoints.constFirst()).constData(), &st) == 0 && st.f_blocks > 0) {
            const qulonglong total = qulonglong(st.f_blocks) * st.f_frsize;
            const qulonglong avail = qulonglong(st.f_bavail) * st.f_frsize;
            const qulonglong freeBytes = qulonglong(st.f_bfree) * st.f_frsize;
            v[u"usageKnown"_s] = true;
            v[u"fsTotal"_s] = total;
            v[u"fsUsed"_s] = total - freeBytes;
            v[u"fsAvailable"_s] = avail;
        }
    }

    QString reason;
    const QStringList disks = SystemDevices::wholeDisks(kernelName);
    for (const QString &disk : disks) {
        if (protectedDisks.contains(disk)) {
            reason = protectedDisks.value(disk);
            break;
        }
    }
    v[u"protectedReason"_s] = reason;
    return v;
}

QVariantMap UDisksClient::smartFor(const QString &drivePath) const
{
    QVariantMap s;
    const QVariantMap ata = property(drivePath, kDriveAta);
    const QVariantMap nvme = property(drivePath, kNvme);
    if (!ata.isEmpty()) {
        s[u"kind"_s] = u"ata"_s;
        s[u"supported"_s] = ata.value(u"SmartSupported"_s).toBool();
        s[u"enabled"_s] = ata.value(u"SmartEnabled"_s).toBool();
        const qulonglong updated = ata.value(u"SmartUpdated"_s).toULongLong();
        s[u"updated"_s] = updated;
        s[u"hasData"_s] = updated > 0;
        s[u"failing"_s] = ata.value(u"SmartFailing"_s).toBool();
        const double kelvin = ata.value(u"SmartTemperature"_s).toDouble();
        s[u"temperatureKnown"_s] = kelvin > 0;
        s[u"temperatureCelsius"_s] = kelvin > 0 ? kelvin - 273.15 : 0.0;
        s[u"powerOnSeconds"_s] = ata.value(u"SmartPowerOnSeconds"_s).toULongLong();
        s[u"selfTest"_s] = ata.value(u"SmartSelftestStatus"_s).toString();
        s[u"badSectors"_s] = ata.value(u"SmartNumBadSectors"_s).toLongLong();
        s[u"attributesFailing"_s] = ata.value(u"SmartNumAttributesFailing"_s).toInt();
        s[u"attributesFailedInThePast"_s] = ata.value(u"SmartNumAttributesFailedInThePast"_s).toInt();
    } else if (!nvme.isEmpty()) {
        s[u"kind"_s] = u"nvme"_s;
        s[u"supported"_s] = true;
        s[u"enabled"_s] = true;
        const qulonglong updated = nvme.value(u"SmartUpdated"_s).toULongLong();
        s[u"updated"_s] = updated;
        s[u"hasData"_s] = updated > 0;
        const QStringList warnings = nvme.value(u"SmartCriticalWarning"_s).toStringList();
        s[u"failing"_s] = !warnings.isEmpty();
        s[u"criticalWarnings"_s] = warnings;
        const int kelvin = nvme.value(u"SmartTemperature"_s).toInt();
        s[u"temperatureKnown"_s] = kelvin > 0;
        s[u"temperatureCelsius"_s] = kelvin > 0 ? kelvin - 273.15 : 0.0;
        s[u"powerOnSeconds"_s] = nvme.value(u"SmartPowerOnHours"_s).toULongLong() * 3600ULL;
        s[u"selfTest"_s] = nvme.value(u"SmartSelftestStatus"_s).toString();
    } else {
        s[u"kind"_s] = QString();
        s[u"supported"_s] = false;
        s[u"enabled"_s] = false;
        s[u"hasData"_s] = false;
    }
    s[u"attributes"_s] = m_smartAttributes.value(drivePath);
    return s;
}

void UDisksClient::rebuild()
{
    if (!m_available)
        return;
    const QHash<QString, QString> protectedDisks = SystemDevices::protectedDisks();

    // Top-level block devices: whole disks, loop devices, RAID arrays —
    // not partitions and not the clear-text side of an encrypted volume.
    QVariantList devices;
    for (auto it = m_objects.constBegin(); it != m_objects.constEnd(); ++it) {
        const QVariantMap block = it->value(QString::fromLatin1(kBlock));
        if (block.isEmpty() || it->contains(QString::fromLatin1(kPartition)))
            continue;
        if (objectPath(block.value(u"CryptoBackingDevice"_s)).size() > 1)
            continue;
        const QString blockPath = it.key().path();
        const QString device = bytesToString(block.value(u"Device"_s));
        const QString kernelName = QFileInfo(device).fileName();
        const QString drivePath = objectPath(block.value(u"Drive"_s));
        const QVariantMap drive = drivePath.size() > 1 ? property(drivePath, kDrive) : QVariantMap();
        const qulonglong size = block.value(u"Size"_s).toULongLong();
        // Unconfigured loop devices, empty zram devices and the like are not disks.
        if (size == 0 && drive.isEmpty())
            continue;
        if (block.value(u"HintIgnore"_s).toBool())
            continue;

        QVariantMap d;
        d[u"path"_s] = blockPath;
        d[u"drivePath"_s] = drive.isEmpty() ? QString() : drivePath;
        d[u"device"_s] = device;
        d[u"kernelName"_s] = kernelName;
        d[u"size"_s] = size;
        d[u"readOnly"_s] = block.value(u"ReadOnly"_s).toBool();
        const QVariantMap loop = it->value(QString::fromLatin1(kLoop));
        const bool isLoop = !loop.isEmpty();
        d[u"isLoop"_s] = isLoop;
        d[u"backingFile"_s] = isLoop ? bytesToString(loop.value(u"BackingFile"_s)) : QString();
        d[u"vendor"_s] = drive.value(u"Vendor"_s).toString().trimmed();
        d[u"model"_s] = drive.value(u"Model"_s).toString().trimmed();
        d[u"serial"_s] = drive.value(u"Serial"_s).toString();
        d[u"revision"_s] = drive.value(u"Revision"_s).toString();
        d[u"wwn"_s] = drive.value(u"WWN"_s).toString();
        d[u"removable"_s] = drive.value(u"Removable"_s).toBool() || drive.value(u"MediaRemovable"_s).toBool();
        d[u"ejectable"_s] = drive.value(u"Ejectable"_s).toBool();
        d[u"canPowerOff"_s] = drive.value(u"CanPowerOff"_s).toBool();
        d[u"mediaAvailable"_s] = drive.isEmpty() ? true : drive.value(u"MediaAvailable"_s).toBool();
        d[u"rotationRate"_s] = drive.value(u"RotationRate"_s).toInt();     // 0 = solid state, -1 = unknown rotating
        d[u"hasDrive"_s] = !drive.isEmpty();
        d[u"bus"_s] = busOf(kernelName, drive.value(u"ConnectionBus"_s).toString());
        QString name = (d.value(u"vendor"_s).toString() + u' ' + d.value(u"model"_s).toString()).trimmed();
        if (name.isEmpty())
            name = isLoop ? tr("Loop device") : d.value(u"bus"_s).toString().isEmpty() ? tr("Block device") : tr("%1 disk").arg(d.value(u"bus"_s).toString());
        d[u"name"_s] = name;
        d[u"smart"_s] = smartFor(drivePath);
        d[u"protectedReason"_s] = protectedDisks.value(kernelName);
        const QString tableType = it->value(QString::fromLatin1(kPartitionTable)).value(u"Type"_s).toString();
        d[u"hasTable"_s] = it->contains(QString::fromLatin1(kPartitionTable));
        d[u"tableType"_s] = tableType;
        d[u"mountPoints"_s] = mountPointsBelow(blockPath);

        // Partitions in disk order, with the unallocated space between them.
        QVariantList volumes;
        if (it->contains(QString::fromLatin1(kPartitionTable))) {
            QList<QVariantMap> parts;
            for (auto p = m_objects.constBegin(); p != m_objects.constEnd(); ++p) {
                const QVariantMap part = p->value(QString::fromLatin1(kPartition));
                if (!part.isEmpty() && objectPath(part.value(u"Table"_s)) == blockPath)
                    parts.append(volumeFor(p.key().path(), protectedDisks));
            }
            std::sort(parts.begin(), parts.end(), [](const QVariantMap &a, const QVariantMap &b) {
                return a.value(u"offset"_s).toULongLong() < b.value(u"offset"_s).toULongLong();
            });
            // GPT keeps the first and last MiB for its headers; MBR the first.
            const qulonglong firstUsable = kMiB;
            const qulonglong lastUsable = size > 2 * qulonglong(kMiB) ? (tableType == "gpt"_L1 ? size - qulonglong(kMiB) : size) : size;
            qulonglong cursor = firstUsable;
            const auto addFree = [&](qulonglong from, qulonglong to) {
                if (to <= from || to - from < qulonglong(kMiB))
                    return;
                volumes.append(QVariantMap{
                    {u"free"_s, true}, {u"path"_s, QString()}, {u"device"_s, QString()},
                    {u"offset"_s, from}, {u"size"_s, to - from}, {u"tablePath"_s, blockPath},
                    {u"protectedReason"_s, protectedDisks.value(kernelName)},
                });
            };
            for (const QVariantMap &p : std::as_const(parts)) {
                if (p.value(u"isContainer"_s).toBool()) {
                    volumes.append(p);      // extended partition: listed, its logical partitions follow
                    continue;
                }
                const qulonglong offset = p.value(u"offset"_s).toULongLong();
                addFree(cursor, offset);
                volumes.append(p);
                cursor = qMax(cursor, offset + p.value(u"size"_s).toULongLong());
            }
            addFree(cursor, lastUsable);
        } else if (size > 0) {
            // No partition table: the device as a whole is the volume.
            volumes.append(volumeFor(blockPath, protectedDisks));
        }
        d[u"volumes"_s] = volumes;
        devices.append(d);
    }
    std::sort(devices.begin(), devices.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();
        const int gx = x.value(u"hasDrive"_s).toBool() ? (x.value(u"removable"_s).toBool() ? 1 : 0) : 2;
        const int gy = y.value(u"hasDrive"_s).toBool() ? (y.value(u"removable"_s).toBool() ? 1 : 0) : 2;
        if (gx != gy)
            return gx < gy;
        return x.value(u"device"_s).toString() < y.value(u"device"_s).toString();
    });

    QVariantList jobs;
    for (auto it = m_objects.constBegin(); it != m_objects.constEnd(); ++it) {
        const QVariantMap job = it->value(QString::fromLatin1(kJob));
        if (job.isEmpty())
            continue;
        QStringList targets;
        const QStringList objects = objectPaths(job.value(u"Objects"_s));
        for (const QString &o : objects) {
            const QString dev = deviceOf(o);
            targets << (dev.isEmpty() ? QFileInfo(o).fileName() : dev);
        }
        const qulonglong expectedEnd = job.value(u"ExpectedEndTime"_s).toULongLong();   // µs since the epoch
        const qulonglong now = qulonglong(QDateTime::currentMSecsSinceEpoch()) * 1000ULL;
        jobs.append(QVariantMap{
            {u"path"_s, it.key().path()},
            {u"operation"_s, job.value(u"Operation"_s).toString()},
            {u"label"_s, jobLabel(job.value(u"Operation"_s).toString())},
            {u"targets"_s, targets},
            {u"progressValid"_s, job.value(u"ProgressValid"_s).toBool()},
            {u"progress"_s, job.value(u"Progress"_s).toDouble()},
            {u"cancelable"_s, job.value(u"Cancelable"_s).toBool()},
            {u"bytes"_s, job.value(u"Bytes"_s).toULongLong()},
            {u"rate"_s, job.value(u"Rate"_s).toULongLong()},
            {u"secondsLeft"_s, expectedEnd > now ? double(expectedEnd - now) / 1e6 : 0.0},
        });
    }

    const bool devicesDiffer = devices != m_devices;
    const bool jobsDiffer = jobs != m_jobs;
    m_devices = devices;
    m_jobs = jobs;
    if (devicesDiffer)
        Q_EMIT devicesChanged();
    if (jobsDiffer)
        Q_EMIT jobsChanged();
    fetchSmartAttributes();
}

// ---- capabilities ----------------------------------------------------------------

QStringList UDisksClient::formatTypes() const
{
    // In UDisks' own order, which lists the common Linux types first.
    QStringList types;
    const QStringList supported = property(kManagerPath, kManager).value(u"SupportedFilesystems"_s).toStringList();
    for (const QString &t : supported) {
        if (m_capabilities.value(t).toMap().value(u"format"_s).toBool())
            types << t;
    }
    return types;
}

void UDisksClient::fetchCapabilities()
{
    const QStringList supported = property(kManagerPath, kManager).value(u"SupportedFilesystems"_s).toStringList();
    m_capabilities.clear();
    m_capabilitiesRequested = true;
    QDBusConnection bus = QDBusConnection::systemBus();
    struct Query { const char *method; const char *key; };
    static const Query queries[] = {{"CanFormat", "format"}, {"CanResize", "resize"}, {"CanCheck", "check"}, {"CanRepair", "repair"}};
    for (const QString &type : supported) {
        for (const Query &q : queries) {
            QDBusMessage msg = QDBusMessage::createMethodCall(kService, kManagerPath, QString::fromLatin1(kManager), QString::fromLatin1(q.method));
            msg << type;
            ++m_capabilityCalls;
            auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg, 30000), this);
            const QString key = QString::fromLatin1(q.key);
            connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, type, key](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                const QDBusMessage reply = w->reply();
                QVariantMap entry = m_capabilities.value(type).toMap();
                bool available = false;
                qulonglong mode = 0;
                QString tool;
                if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
                    // (bs) or, for CanResize, (bts)
                    const QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
                    arg.beginStructure();
                    arg >> available;
                    if (key == "resize"_L1)
                        arg >> mode;
                    arg >> tool;
                    arg.endStructure();
                }
                entry[key] = available;
                entry[key + u"Tool"_s] = tool;      // the missing program when not available
                if (key == "resize"_L1) {
                    entry[u"resizeMode"_s] = mode;
                    entry[u"offlineShrink"_s] = available && (mode & kOfflineShrink) != 0;
                    entry[u"offlineGrow"_s] = available && (mode & kOfflineGrow) != 0;
                }
                m_capabilities[type] = entry;
                if (--m_capabilityCalls == 0)
                    Q_EMIT capabilitiesChanged();
            });
        }
    }
    if (m_capabilityCalls == 0)
        Q_EMIT capabilitiesChanged();
}

// ---- SMART -----------------------------------------------------------------------

void UDisksClient::fetchSmartAttributes()
{
    for (auto it = m_objects.constBegin(); it != m_objects.constEnd(); ++it) {
        const QString path = it.key().path();
        const QVariantMap ata = it->value(QString::fromLatin1(kDriveAta));
        const QVariantMap nvme = it->value(QString::fromLatin1(kNvme));
        const bool isAta = !ata.isEmpty() && ata.value(u"SmartSupported"_s).toBool() && ata.value(u"SmartEnabled"_s).toBool();
        const bool isNvme = !nvme.isEmpty();
        if (!isAta && !isNvme)
            continue;
        const qulonglong updated = (isAta ? ata : nvme).value(u"SmartUpdated"_s).toULongLong();
        if (updated == 0 || m_smartFetched.value(path) == updated)
            continue;
        m_smartFetched[path] = updated;
        QDBusMessage msg = QDBusMessage::createMethodCall(kService, path, QString::fromLatin1(isAta ? kDriveAta : kNvme), u"SmartGetAttributes"_s);
        msg << QVariantMap();
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, 30000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, path, isAta](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            const QDBusMessage reply = w->reply();
            if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
                return;
            QVariantList rows;
            if (isAta) {
                // a(ysqiiixia{sv}): id, name, flags, value, worst, threshold, pretty, pretty unit, expansion
                const QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
                arg.beginArray();
                while (!arg.atEnd()) {
                    uchar id = 0;
                    QString name;
                    ushort flags = 0;
                    int value = 0, worst = 0, threshold = 0, unit = 0;
                    qlonglong pretty = 0;
                    QVariantMap expansion;
                    arg.beginStructure();
                    arg >> id >> name >> flags >> value >> worst >> threshold >> pretty >> unit >> expansion;
                    arg.endStructure();
                    QString prettyText;
                    switch (unit) {     // 2 = milliseconds, 3 = sectors, 4 = millikelvin, otherwise dimensionless
                    case 2: prettyText = tr("%1 h").arg(QString::number(double(pretty) / 3600000.0, 'f', 1)); break;
                    case 3: prettyText = tr("%1 sectors").arg(pretty); break;
                    case 4: prettyText = tr("%1 °C").arg(QString::number(double(pretty) / 1000.0 - 273.15, 'f', 0)); break;
                    case 1: prettyText = QString::number(pretty); break;
                    default: break;
                    }
                    rows.append(QVariantMap{
                        {u"id"_s, int(id)}, {u"name"_s, name}, {u"value"_s, value}, {u"worst"_s, worst},
                        {u"threshold"_s, threshold}, {u"raw"_s, prettyText},
                        {u"preFail"_s, (flags & 0x0001) != 0},
                        {u"failing"_s, threshold > 0 && value > 0 && value <= threshold},
                    });
                }
                arg.endArray();
            } else {
                const QVariantMap attributes = qdbus_cast<QVariantMap>(reply.arguments().constFirst());
                for (auto a = attributes.constBegin(); a != attributes.constEnd(); ++a) {
                    const QVariant v = a.value();
                    QString text;
                    if (v.userType() == qMetaTypeId<QDBusArgument>())
                        continue;       // arrays (temperature sensors) are not listed
                    text = v.toString();
                    rows.append(QVariantMap{{u"name"_s, a.key()}, {u"raw"_s, text}});
                }
            }
            m_smartAttributes[path] = rows;
            scheduleRebuild();
        });
    }
}

void UDisksClient::refreshSmart(const QString &drivePath)
{
    const bool ata = hasInterface(drivePath, kDriveAta);
    if (!ata && !hasInterface(drivePath, kNvme))
        return;
    call(tr("Read SMART data"), drivePath, QString::fromLatin1(ata ? kDriveAta : kNvme), u"SmartUpdate"_s, {QVariantMap()});
}

// ---- actions ---------------------------------------------------------------------

void UDisksClient::finish(const QString &operation, bool ok, const QString &message)
{
    Q_EMIT operationFinished(operation, ok, message);
}

void UDisksClient::call(const QString &operation, const QString &path, const QString &interface, const QString &method,
                        const QVariantList &arguments, Reply onSuccess)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, path, interface, method);
    msg.setArguments(arguments);
    ++m_pending;
    Q_EMIT pendingChanged();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, kLongTimeout), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, operation, onSuccess](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        --m_pending;
        Q_EMIT pendingChanged();
        const QDBusMessage reply = w->reply();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            finish(operation, false, cleanError(w->error()));
            return;
        }
        if (onSuccess)
            onSuccess(reply);
        else
            finish(operation, true, {});
    });
}

QString UDisksClient::destructiveRefusal(const QString &blockPath) const
{
    const QString device = deviceOf(blockPath);
    if (device.isEmpty())
        return tr("The device no longer exists.");
    const QString kernelName = QFileInfo(device).fileName();
    const QHash<QString, QString> protectedDisks = SystemDevices::protectedDisks();
    const QStringList disks = SystemDevices::wholeDisks(kernelName);
    for (const QString &disk : disks) {
        if (protectedDisks.contains(disk))
            return tr("Refused: %1. Destructive actions on this disk are not possible from the running system.").arg(protectedDisks.value(disk));
    }
    return {};
}

bool UDisksClient::guard(const QString &operation, const QString &blockPath, bool destructive)
{
    if (!m_available) {
        finish(operation, false, tr("The storage service is not available."));
        return false;
    }
    const QString device = deviceOf(blockPath);
    if (device.isEmpty()) {
        finish(operation, false, tr("The device no longer exists."));
        return false;
    }
    // Test rigs set this so that a mistake in a test can never reach a real disk.
    if (qEnvironmentVariableIntValue("NEXUS_DISKS_TEST_LOOP_ONLY") == 1 && !SystemDevices::isLoopBacked(QFileInfo(device).fileName())) {
        finish(operation, false, u"NEXUS_DISKS_TEST_LOOP_ONLY is set: %1 is not a loop device, nothing was changed."_s.arg(device));
        return false;
    }
    if (destructive) {
        const QString refusal = destructiveRefusal(blockPath);
        if (!refusal.isEmpty()) {
            finish(operation, false, refusal);
            return false;
        }
        const QStringList mounted = mountPointsBelow(blockPath);
        if (!mounted.isEmpty()) {
            finish(operation, false, tr("%1 is in use (%2). Unmount it first.").arg(device, mounted.join(u", "_s)));
            return false;
        }
    }
    return true;
}

void UDisksClient::mount(const QString &blockPath)
{
    const QString op = tr("Mount %1").arg(deviceOf(blockPath));
    call(op, blockPath, QString::fromLatin1(kFilesystem), u"Mount"_s, {QVariantMap()}, [this, op](const QDBusMessage &reply) {
        finish(op, true, tr("Mounted at %1").arg(reply.arguments().value(0).toString()));
    });
}

void UDisksClient::unmount(const QString &blockPath)
{
    call(tr("Unmount %1").arg(deviceOf(blockPath)), blockPath, QString::fromLatin1(kFilesystem), u"Unmount"_s, {QVariantMap()});
}

void UDisksClient::eject(const QString &drivePath)
{
    call(tr("Eject"), drivePath, QString::fromLatin1(kDrive), u"Eject"_s, {QVariantMap()});
}

void UDisksClient::powerOff(const QString &drivePath)
{
    call(tr("Power off"), drivePath, QString::fromLatin1(kDrive), u"PowerOff"_s, {QVariantMap()});
}

void UDisksClient::unlock(const QString &blockPath, const QString &passphrase)
{
    const QString op = tr("Unlock %1").arg(deviceOf(blockPath));
    call(op, blockPath, QString::fromLatin1(kEncrypted), u"Unlock"_s, {passphrase, QVariantMap()}, [this, op](const QDBusMessage &reply) {
        const QString cleartext = objectPath(reply.arguments().value(0));
        finish(op, true, tr("Unlocked as %1").arg(QFileInfo(cleartext).fileName()));
    });
}

void UDisksClient::lock(const QString &blockPath)
{
    call(tr("Lock %1").arg(deviceOf(blockPath)), blockPath, QString::fromLatin1(kEncrypted), u"Lock"_s, {QVariantMap()});
}

void UDisksClient::setLabel(const QString &blockPath, const QString &label)
{
    const QString op = tr("Change the label of %1").arg(deviceOf(blockPath));
    if (!guard(op, blockPath, false))
        return;
    call(op, blockPath, QString::fromLatin1(kFilesystem), u"SetLabel"_s, {label, QVariantMap()});
}

void UDisksClient::check(const QString &blockPath)
{
    const QString op = tr("Check %1").arg(deviceOf(blockPath));
    call(op, blockPath, QString::fromLatin1(kFilesystem), u"Check"_s, {QVariantMap()}, [this, op](const QDBusMessage &reply) {
        const bool clean = reply.arguments().value(0).toBool();
        finish(op, clean, clean ? tr("The file system is consistent.") : tr("The check found errors. Repair the file system."));
    });
}

void UDisksClient::repair(const QString &blockPath)
{
    const QString op = tr("Repair %1").arg(deviceOf(blockPath));
    if (!guard(op, blockPath, false))
        return;
    call(op, blockPath, QString::fromLatin1(kFilesystem), u"Repair"_s, {QVariantMap()}, [this, op](const QDBusMessage &reply) {
        const bool repaired = reply.arguments().value(0).toBool();
        finish(op, repaired, repaired ? tr("The file system was repaired.") : tr("The file system could not be repaired completely."));
    });
}

void UDisksClient::resize(const QString &blockPath, double newSize)
{
    const QString op = tr("Resize %1").arg(deviceOf(blockPath));
    if (!guard(op, blockPath, false))
        return;
    const qulonglong target = qulonglong(newSize);
    const qulonglong current = property(blockPath, kBlock).value(u"Size"_s).toULongLong();
    if (!hasInterface(blockPath, kPartition) || target == 0 || target == current) {
        finish(op, false, tr("Nothing to do: the size is unchanged or the device is not a partition."));
        return;
    }
    const bool hasFs = hasInterface(blockPath, kFilesystem);
    if (hasFs && !byteArrayList(property(blockPath, kFilesystem).value(u"MountPoints"_s)).isEmpty()) {
        finish(op, false, tr("Unmount the file system before resizing it."));
        return;
    }
    const QString partitionIface = QString::fromLatin1(kPartition);
    const QString fsIface = QString::fromLatin1(kFilesystem);
    if (!hasFs) {
        call(op, blockPath, partitionIface, u"Resize"_s, {target, QVariantMap()});
    } else if (target > current) {
        // Grow: partition first, then let the file system fill it (size 0).
        call(op, blockPath, partitionIface, u"Resize"_s, {target, QVariantMap()}, [this, op, blockPath, fsIface](const QDBusMessage &) {
            call(op, blockPath, fsIface, u"Resize"_s, {qulonglong(0), QVariantMap()});
        });
    } else {
        // Shrink: file system first, then the partition around it.
        call(op, blockPath, fsIface, u"Resize"_s, {target, QVariantMap()}, [this, op, blockPath, partitionIface, target](const QDBusMessage &) {
            call(op, blockPath, partitionIface, u"Resize"_s, {target, QVariantMap()});
        });
    }
}

void UDisksClient::createPartition(const QString &tablePath, double offset, double size, const QString &fsType, const QString &label)
{
    const QString op = tr("Create a partition on %1").arg(deviceOf(tablePath));
    if (!guard(op, tablePath, false))
        return;
    if (!fsType.isEmpty() && !formatTypes().contains(fsType)) {
        finish(op, false, tr("The file system type “%1” is not available on this system.").arg(fsType));
        return;
    }
    const auto done = [this, op](const QDBusMessage &reply) {
        const QString created = objectPath(reply.arguments().value(0));
        finish(op, true, tr("Created %1").arg(QFileInfo(created).fileName()));
    };
    if (fsType.isEmpty()) {
        call(op, tablePath, QString::fromLatin1(kPartitionTable), u"CreatePartition"_s,
             {qulonglong(offset), qulonglong(size), QString(), QString(), QVariantMap()}, done);
        return;
    }
    QVariantMap formatOptions;
    if (!label.isEmpty())
        formatOptions[u"label"_s] = label;
    call(op, tablePath, QString::fromLatin1(kPartitionTable), u"CreatePartitionAndFormat"_s,
         {qulonglong(offset), qulonglong(size), QString(), QString(), QVariantMap(), fsType, formatOptions}, done);
}

void UDisksClient::format(const QString &blockPath, const QString &fsType, const QString &label)
{
    const QString op = tr("Format %1").arg(deviceOf(blockPath));
    if (!guard(op, blockPath, true))
        return;
    if (!formatTypes().contains(fsType)) {
        finish(op, false, tr("The file system type “%1” is not available on this system.").arg(fsType));
        return;
    }
    QVariantMap options;
    if (!label.isEmpty())
        options[u"label"_s] = label;
    options[u"update-partition-type"_s] = true;
    call(op, blockPath, QString::fromLatin1(kBlock), u"Format"_s, {fsType, options});
}

void UDisksClient::deletePartition(const QString &blockPath)
{
    const QString op = tr("Delete %1").arg(deviceOf(blockPath));
    if (!guard(op, blockPath, true))
        return;
    if (!hasInterface(blockPath, kPartition)) {
        finish(op, false, tr("Not a partition."));
        return;
    }
    call(op, blockPath, QString::fromLatin1(kPartition), u"Delete"_s, {QVariantMap()});
}

void UDisksClient::createPartitionTable(const QString &blockPath, const QString &type)
{
    const QString op = tr("New partition table on %1").arg(deviceOf(blockPath));
    if (!guard(op, blockPath, true))
        return;
    if (type != "gpt"_L1 && type != "dos"_L1) {
        finish(op, false, tr("Unknown partition table type “%1”.").arg(type));
        return;
    }
    if (hasInterface(blockPath, kPartition)) {
        finish(op, false, tr("A partition table can only be created on a whole disk."));
        return;
    }
    call(op, blockPath, QString::fromLatin1(kBlock), u"Format"_s, {type, QVariantMap()});
}

// ---- disk images -------------------------------------------------------------------

QVariantMap UDisksClient::imageFileInfo(const QString &file) const
{
    QString path = file.trimmed();
    if (path.startsWith("~/"_L1))
        path = QDir::homePath() + path.mid(1);
    const QFileInfo info(path);
    const QFileInfo dir(info.absolutePath());
    return QVariantMap{
        {u"path"_s, info.absoluteFilePath()},
        {u"exists"_s, info.exists() && info.isFile()},
        {u"isDirectory"_s, info.isDir()},
        {u"readable"_s, info.isReadable()},
        {u"size"_s, info.exists() ? qulonglong(info.size()) : 0ULL},
        {u"directoryExists"_s, dir.isDir()},
        {u"directoryWritable"_s, dir.isDir() && dir.isWritable()},
    };
}

QString UDisksClient::defaultImagePath(const QString &deviceName) const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QDir::homePath();
    return dir + u'/' + deviceName + u'-' + QDate::currentDate().toString(Qt::ISODate) + u".img"_s;
}

QVariantMap UDisksClient::imageJob() const
{
    if (!m_image)
        return QVariantMap{{u"active"_s, false}};
    QVariantMap j = m_imageInfo;
    const qint64 done = m_image->done();
    const qint64 total = m_image->total();
    const double seconds = m_image->clock.elapsed() / 1000.0;
    const double rate = seconds > 0.5 ? done / seconds : 0.0;
    j[u"active"_s] = true;
    j[u"done"_s] = double(done);
    j[u"total"_s] = double(total);
    j[u"progress"_s] = total > 0 ? double(done) / double(total) : 0.0;
    j[u"rate"_s] = rate;
    j[u"secondsLeft"_s] = rate > 0 ? double(total - done) / rate : 0.0;
    j[u"cancelling"_s] = m_image->cancelled();
    return j;
}

void UDisksClient::createImage(const QString &blockPath, const QString &file)
{
    startImage(false, blockPath, file);
}

void UDisksClient::restoreImage(const QString &blockPath, const QString &file)
{
    startImage(true, blockPath, file);
}

void UDisksClient::startImage(bool restore, const QString &blockPath, const QString &file)
{
    const QString device = deviceOf(blockPath);
    const QString op = restore ? tr("Restore image to %1").arg(device) : tr("Create image of %1").arg(device);
    if (m_image) {
        finish(op, false, tr("Another disk image operation is still running."));
        return;
    }
    if (restore ? !guard(op, blockPath, true) : !m_available) {
        if (!restore)
            finish(op, false, tr("The storage service is not available."));
        return;
    }
    const QVariantMap info = imageFileInfo(file);
    const QString path = info.value(u"path"_s).toString();
    const qulonglong deviceSize = property(blockPath, kBlock).value(u"Size"_s).toULongLong();
    if (device.isEmpty() || deviceSize == 0) {
        finish(op, false, tr("The device no longer exists or has no medium."));
        return;
    }
    qint64 total = qint64(deviceSize);
    if (restore) {
        if (!info.value(u"exists"_s).toBool() || !info.value(u"readable"_s).toBool()) {
            finish(op, false, tr("%1 does not exist or cannot be read.").arg(path));
            return;
        }
        const qulonglong imageSize = info.value(u"size"_s).toULongLong();
        if (imageSize == 0) {
            finish(op, false, tr("%1 is empty.").arg(path));
            return;
        }
        if (imageSize > deviceSize) {
            finish(op, false, tr("The image (%1 bytes) is larger than %2 (%3 bytes).").arg(imageSize).arg(device).arg(deviceSize));
            return;
        }
        total = qint64(imageSize);
    } else {
        if (info.value(u"isDirectory"_s).toBool() || !info.value(u"directoryWritable"_s).toBool()) {
            finish(op, false, tr("%1 cannot be written: choose a file in a folder you may write to.").arg(path));
            return;
        }
        if (info.value(u"exists"_s).toBool()) {
            finish(op, false, tr("%1 already exists. Choose another name.").arg(path));
            return;
        }
        if (path.startsWith(u"/dev/"_s)) {
            finish(op, false, tr("An image must be a regular file, not a device."));
            return;
        }
    }

    call(op, blockPath, QString::fromLatin1(kBlock), restore ? u"OpenForRestore"_s : u"OpenForBackup"_s, {QVariantMap()},
         [this, op, restore, blockPath, device, path, total](const QDBusMessage &reply) {
        const QDBusUnixFileDescriptor fd = reply.arguments().value(0).value<QDBusUnixFileDescriptor>();
        if (!fd.isValid()) {
            finish(op, false, tr("The storage service did not hand out the device."));
            return;
        }
        m_image = new ImageWorker(restore, ::dup(fd.fileDescriptor()), path, total);
        m_image->operation = op;
        m_image->blockPath = blockPath;
        m_image->device = device;
        m_imageInfo = QVariantMap{{u"restore"_s, restore}, {u"device"_s, device}, {u"file"_s, path}, {u"label"_s, op}};
        connect(m_image, &QThread::finished, this, [this] {
            ImageWorker *worker = m_image;
            m_image = nullptr;
            m_imageTimer.stop();
            Q_EMIT imageJobChanged();
            if (worker->cancelled()) {
                finish(worker->operation, false, worker->restore()
                    ? tr("Cancelled after %1 of %2 bytes. The content of %3 is now incomplete.").arg(worker->done()).arg(worker->total()).arg(worker->device)
                    : tr("Cancelled; the partial image was removed."));
            } else if (!worker->error().isEmpty()) {
                finish(worker->operation, false, worker->error());
            } else {
                finish(worker->operation, true, worker->restore() ? tr("%1 bytes written from %2").arg(worker->total()).arg(worker->file())
                                                                  : tr("%1 bytes saved to %2").arg(worker->total()).arg(worker->file()));
            }
            if (worker->restore()) {
                // Let the kernel and UDisks see the new content.
                QDBusMessage rescan = QDBusMessage::createMethodCall(kService, worker->blockPath, QString::fromLatin1(kBlock), u"Rescan"_s);
                rescan << QVariantMap();
                QDBusConnection::systemBus().asyncCall(rescan);
            }
            worker->deleteLater();
        });
        m_image->clock.start();
        m_image->start();
        m_imageTimer.start();
        Q_EMIT imageJobChanged();
    });
}

void UDisksClient::cancelImageJob()
{
    if (m_image) {
        m_image->cancel();
        Q_EMIT imageJobChanged();
    }
}

void UDisksClient::cancelJob(const QString &jobPath)
{
    call(tr("Cancel job"), jobPath, QString::fromLatin1(kJob), u"Cancel"_s, {QVariantMap()});
}

QJsonObject UDisksClient::protectedDisksJson() const
{
    QJsonObject o;
    const QHash<QString, QString> disks = SystemDevices::protectedDisks();
    for (auto it = disks.constBegin(); it != disks.constEnd(); ++it)
        o[u"/dev/"_s + it.key()] = it.value();
    return o;
}
