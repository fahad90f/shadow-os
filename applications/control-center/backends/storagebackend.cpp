// SPDX-License-Identifier: GPL-3.0-or-later
#include "storagebackend.h"

#include "dumpregistry.h"

#include <command.h>

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>

#include <fcntl.h>
#include <fts.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <unordered_set>

using InterfaceMap = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceMap>;
Q_DECLARE_METATYPE(InterfaceMap)
Q_DECLARE_METATYPE(ManagedObjects)

namespace {
template <typename Ready>
void waitFor(StorageBackend *backend, Ready ready, int timeoutMs)
{
    if (ready())
        return;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (ready()) loop.quit(); });
    poll.start();
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    Q_UNUSED(backend)
    loop.exec();
}
} // namespace

NEXUS_CC_DUMP("storage", [] {
    StorageBackend backend;
    waitFor(&backend, [&] { return backend.settled(); }, 30000);
    return QJsonValue(backend.toJson());
})
// The walk of the home folder, run to its end (up to two minutes).
NEXUS_CC_DUMP("storage-home", [] {
    StorageBackend backend;
    backend.startHomeScan();
    waitFor(&backend, [&] { return backend.homeState() != QLatin1String("scanning"); }, 120000);
    return QJsonValue(backend.homeToJson());
})

namespace {
const QString kUDisks = QStringLiteral("org.freedesktop.UDisks2");
const QString kUDisksPath = QStringLiteral("/org/freedesktop/UDisks2");
const QString kObjectManager = QStringLiteral("org.freedesktop.DBus.ObjectManager");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kBlock = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString kPartition = QStringLiteral("org.freedesktop.UDisks2.Partition");
const QString kFilesystem = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
const QString kSwap = QStringLiteral("org.freedesktop.UDisks2.Swapspace");
const QString kLoop = QStringLiteral("org.freedesktop.UDisks2.Loop");
const QString kDrive = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QString kAta = QStringLiteral("org.freedesktop.UDisks2.Drive.Ata");
const QString kNvme = QStringLiteral("org.freedesktop.UDisks2.NVMe.Controller");
const int kActionTimeoutMs = 5 * 60 * 1000;

// "ay" with a trailing NUL, as UDisks2 sends device and mount paths.
QString bytesToPath(QByteArray bytes)
{
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    return QString::fromLocal8Bit(bytes);
}

QStringList byteArrayList(const QVariant &value)
{
    QStringList out;
    if (value.canConvert<QDBusArgument>() && value.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument argument = value.value<QDBusArgument>();
        argument.beginArray();
        while (!argument.atEnd()) {
            QByteArray bytes;
            argument >> bytes;
            out << bytesToPath(bytes);
        }
        argument.endArray();
    } else {
        const QByteArrayList list = value.value<QByteArrayList>();
        for (const QByteArray &bytes : list)
            out << bytesToPath(bytes);
    }
    out.removeAll(QString());
    return out;
}

QString objectPath(const QVariant &value)
{
    const QString path = value.value<QDBusObjectPath>().path();
    return path == QLatin1String("/") ? QString() : path;
}

QString errorText(const QDBusError &error)
{
    const QString name = error.name();
    if (name.endsWith(QLatin1String("NotAuthorized")) || name.endsWith(QLatin1String("NotAuthorizedCanObtain"))
        || name.endsWith(QLatin1String("NotAuthorizedDismissed")) || name == QLatin1String("org.freedesktop.DBus.Error.AccessDenied"))
        return StorageBackend::tr("The action was not authorised.");
    if (name.endsWith(QLatin1String("DeviceBusy")))
        return StorageBackend::tr("The device is in use. Close the files and programs that use it and try again.");
    QString message = error.message();
    // "GDBus.Error:org.freedesktop.UDisks2.Error.Failed: Error mounting …"
    const int at = message.indexOf(QStringLiteral(": "));
    if (message.startsWith(QLatin1String("GDBus.Error:")) && at > 0)
        message = message.mid(at + 2);
    return message.isEmpty() ? name : message;
}

QVariantMap smartUnknown(const QString &text)
{
    return {{QStringLiteral("state"), QStringLiteral("unknown")}, {QStringLiteral("text"), text},
            {QStringLiteral("detail"), QString()}};
}

QString hoursText(double hours)
{
    if (hours <= 0)
        return QString();
    return StorageBackend::tr("%1 hours powered on").arg(QLocale().toString(qRound64(hours)));
}

QString temperatureText(double kelvin)
{
    if (kelvin <= 0)
        return QString();
    return StorageBackend::tr("%1 °C").arg(qRound(kelvin - 273.15));
}

QString joinParts(const QStringList &parts)
{
    QStringList clean;
    for (const QString &part : parts) {
        if (!part.isEmpty())
            clean << part;
    }
    return clean.join(QStringLiteral("  ·  "));
}

QString partitionKind(const QString &usage, const QString &type)
{
    if (type == QLatin1String("swap"))
        return QStringLiteral("swap");
    if (usage == QLatin1String("crypto") || type.startsWith(QLatin1String("crypto")))
        return QStringLiteral("encrypted");
    if (type == QLatin1String("LVM2_member"))
        return QStringLiteral("lvm");
    if (usage == QLatin1String("raid") || type.endsWith(QLatin1String("raid_member")))
        return QStringLiteral("raid");
    if (usage == QLatin1String("filesystem") || !type.isEmpty())
        return QStringLiteral("filesystem");
    return QStringLiteral("other");
}

// Usage of a mounted file system, added to a partition entry.
void addUsage(QVariantMap &partition, const QStringList &mountPoints)
{
    partition.insert(QStringLiteral("usageKnown"), false);
    partition.insert(QStringLiteral("used"), 0.0);
    partition.insert(QStringLiteral("total"), 0.0);
    if (mountPoints.isEmpty())
        return;
    const QStorageInfo info(mountPoints.constFirst());
    if (!info.isValid() || !info.isReady() || info.bytesTotal() <= 0)
        return;
    partition.insert(QStringLiteral("usageKnown"), true);
    partition.insert(QStringLiteral("total"), double(info.bytesTotal()));
    partition.insert(QStringLiteral("used"), double(info.bytesTotal() - info.bytesFree()));
}

bool isSystemMount(const QStringList &mountPoints)
{
    static const QStringList system = {QStringLiteral("/"), QStringLiteral("/boot"), QStringLiteral("/boot/efi"), QStringLiteral("/efi"),
                                       QStringLiteral("/home"), QStringLiteral("/usr"), QStringLiteral("/var"), QStringLiteral("/nix")};
    for (const QString &mount : mountPoints) {
        if (system.contains(mount) || mount.startsWith(QLatin1String("/run/live")) || mount.startsWith(QLatin1String("/usr/lib/live")))
            return true;
    }
    return false;
}

QString driveKind(bool removable, const QString &bus, bool optical, int rotationRate)
{
    if (optical)
        return QStringLiteral("optical");
    if (bus == QLatin1String("sdio") || bus == QLatin1String("mmc"))
        return QStringLiteral("card");
    if (bus == QLatin1String("usb"))
        return QStringLiteral("usb");
    if (removable)
        return QStringLiteral("removable");
    if (rotationRate == 0)
        return QStringLiteral("ssd");
    if (rotationRate > 0)
        return QStringLiteral("hdd");
    return QStringLiteral("disk");
}
} // namespace

// ---- home folder walk ------------------------------------------------------------------------

struct StorageBackend::ScanShared
{
    std::atomic_bool cancel{false};
};

namespace {
enum Category { Documents, Downloads, Pictures, Music, Videos, Desktop, AppData, Caches, Trash, Other, CategoryCount };

struct CategoryMeta
{
    const char *id;
    const char *title;
};
const CategoryMeta kCategories[CategoryCount] = {
    {"documents", QT_TRANSLATE_NOOP("StorageBackend", "Documents")},
    {"downloads", QT_TRANSLATE_NOOP("StorageBackend", "Downloads")},
    {"pictures", QT_TRANSLATE_NOOP("StorageBackend", "Pictures")},
    {"music", QT_TRANSLATE_NOOP("StorageBackend", "Music")},
    {"videos", QT_TRANSLATE_NOOP("StorageBackend", "Videos")},
    {"desktop", QT_TRANSLATE_NOOP("StorageBackend", "Desktop")},
    {"appdata", QT_TRANSLATE_NOOP("StorageBackend", "Application data and settings")},
    {"caches", QT_TRANSLATE_NOOP("StorageBackend", "Caches")},
    {"trash", QT_TRANSLATE_NOOP("StorageBackend", "Rubbish bin")},
    {"other", QT_TRANSLATE_NOOP("StorageBackend", "Other files and folders")},
};

struct Totals
{
    quint64 bytes[CategoryCount] = {};
    quint64 files[CategoryCount] = {};
    int unreadable = 0;

    QVariantList toList() const
    {
        QVariantList list;
        for (int i = 0; i < CategoryCount; ++i) {
            if (bytes[i] == 0 && files[i] == 0)
                continue;
            list << QVariantMap{
                {QStringLiteral("id"), QLatin1String(kCategories[i].id)},
                {QStringLiteral("title"), QCoreApplication::translate("StorageBackend", kCategories[i].title)},
                {QStringLiteral("bytes"), double(bytes[i])},
                {QStringLiteral("files"), double(files[i])},
            };
        }
        return list;
    }
    double totalBytes() const
    {
        quint64 sum = 0;
        for (quint64 b : bytes)
            sum += b;
        return double(sum);
    }
    double totalFiles() const
    {
        quint64 sum = 0;
        for (quint64 f : files)
            sum += f;
        return double(sum);
    }
};

struct InodeKey
{
    dev_t dev;
    ino_t ino;
    bool operator==(const InodeKey &o) const { return dev == o.dev && ino == o.ino; }
};
struct InodeHash
{
    size_t operator()(const InodeKey &k) const { return std::hash<quint64>()(quint64(k.ino) * 1000003u ^ quint64(k.dev)); }
};

// Directories with a meaning of their own; everything below inherits it.
QHash<QByteArray, int> categoryRules(const QString &home)
{
    QHash<QByteArray, int> rules;
    auto add = [&](QStandardPaths::StandardLocation location, int category) {
        const QString path = QDir::cleanPath(QStandardPaths::writableLocation(location));
        if (!path.isEmpty() && path != home && path.startsWith(home + QLatin1Char('/')))
            rules.insert(QFile::encodeName(path), category);
    };
    // Later entries do not replace earlier ones (a desktop that is the home
    // folder itself, or two kinds sharing a folder, keep the first meaning).
    add(QStandardPaths::DocumentsLocation, Documents);
    add(QStandardPaths::DownloadLocation, Downloads);
    add(QStandardPaths::PicturesLocation, Pictures);
    add(QStandardPaths::MusicLocation, Music);
    add(QStandardPaths::MoviesLocation, Videos);
    add(QStandardPaths::DesktopLocation, Desktop);
    rules.insert(QFile::encodeName(home + QStringLiteral("/.cache")), Caches);
    rules.insert(QFile::encodeName(home + QStringLiteral("/.local/share/Trash")), Trash);
    return rules;
}
} // namespace

// ---- backend ------------------------------------------------------------------------------------

StorageBackend::StorageBackend(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<InterfaceMap>();
    qDBusRegisterMetaType<ManagedObjects>();

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(400);
    connect(&m_refreshTimer, &QTimer::timeout, this, &StorageBackend::refresh);

    // Free space changes without any event.
    m_usageTimer.setInterval(15000);
    connect(&m_usageTimer, &QTimer::timeout, this, &StorageBackend::loadVolumes);
    m_usageTimer.start();

    // The kernel flags /proc/self/mounts as "exceptional" whenever the mount table changes.
    m_mountsFd = ::open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
    if (m_mountsFd >= 0) {
        m_mountNotifier = new QSocketNotifier(m_mountsFd, QSocketNotifier::Exception, this);
        connect(m_mountNotifier, &QSocketNotifier::activated, this, &StorageBackend::scheduleRefresh);
    }

    QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        bus.connect(kUDisks, kUDisksPath, kObjectManager, QStringLiteral("InterfacesAdded"), this, SLOT(scheduleRefresh()));
        bus.connect(kUDisks, kUDisksPath, kObjectManager, QStringLiteral("InterfacesRemoved"), this, SLOT(scheduleRefresh()));
        bus.connect(kUDisks, QString(), kProperties, QStringLiteral("PropertiesChanged"), this, SLOT(scheduleRefresh()));
    }
    refresh();
}

StorageBackend::~StorageBackend()
{
    stopScanThread();
    if (m_mountsFd >= 0)
        ::close(m_mountsFd);
}

void StorageBackend::scheduleRefresh()
{
    if (!m_refreshTimer.isActive())
        m_refreshTimer.start();
}

bool StorageBackend::diskManagerAvailable() const
{
    return !QStandardPaths::findExecutable(QStringLiteral("nexus-disks")).isEmpty();
}

bool StorageBackend::openDiskManager()
{
    const bool ok = QProcess::startDetached(QStringLiteral("nexus-disks"), {});
    if (!ok)
        Q_EMIT failed(tr("NEXUS Disk Manager could not be started."));
    return ok;
}

void StorageBackend::refresh()
{
    loadVolumes();
    loadUDisks();
}

// ---- mounted file systems ----------------------------------------------------------------------

void StorageBackend::loadVolumes()
{
    static const QSet<QByteArray> pseudo = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "ramfs", "cgroup", "cgroup2", "pstore", "bpf", "debugfs",
        "tracefs", "securityfs", "configfs", "fusectl", "mqueue", "hugetlbfs", "autofs", "binfmt_misc", "efivarfs",
        "rpc_pipefs", "nsfs", "fuse.portal", "fuse.gvfsd-fuse", "squashfs", "overlay", "selinuxfs",
    };
    QVariantList list;
    QSet<QByteArray> seenDevices;
    QList<QStorageInfo> mounted = QStorageInfo::mountedVolumes();
    // Shortest mount point first, so that a file system bind-mounted into
    // several places is listed where it is mounted "for real".
    std::sort(mounted.begin(), mounted.end(), [](const QStorageInfo &a, const QStorageInfo &b) {
        return a.rootPath().size() < b.rootPath().size();
    });
    for (const QStorageInfo &info : std::as_const(mounted)) {
        if (!info.isValid() || !info.isReady() || info.bytesTotal() <= 0)
            continue;
        const bool isRoot = info.rootPath() == QLatin1String("/");
        if (pseudo.contains(info.fileSystemType()) && !isRoot)
            continue;
        if (seenDevices.contains(info.device()))
            continue;
        seenDevices.insert(info.device());
        const double total = double(info.bytesTotal());
        const double free = double(info.bytesFree());
        const double available = double(info.bytesAvailable());
        QString name = info.name();
        if (name.isEmpty())
            name = isRoot ? tr("System") : info.rootPath() == homePath() || info.rootPath() == QLatin1String("/home")
                                               ? tr("Home") : QFileInfo(info.rootPath()).fileName();
        list << QVariantMap{
            {QStringLiteral("mountPoint"), info.rootPath()},
            {QStringLiteral("device"), QString::fromLocal8Bit(info.device())},
            {QStringLiteral("fsType"), QString::fromLatin1(info.fileSystemType())},
            {QStringLiteral("name"), name},
            {QStringLiteral("total"), total},
            {QStringLiteral("used"), total - free},
            {QStringLiteral("free"), available},
            // As df does: used space against what an ordinary user can ever use.
            {QStringLiteral("fraction"), (total - free) + available > 0 ? (total - free) / ((total - free) + available) : 0.0},
            {QStringLiteral("readOnly"), info.isReadOnly()},
        };
    }
    std::sort(list.begin(), list.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("mountPoint")).toString() < b.toMap().value(QStringLiteral("mountPoint")).toString();
    });
    if (list != m_volumes) {
        m_volumes = list;
        Q_EMIT volumesChanged();
    }
}

double StorageBackend::homeVolumeBytes() const
{
    const QStorageInfo info(homePath());
    return info.isValid() && info.isReady() ? double(info.bytesTotal()) : 0.0;
}

// ---- UDisks2 ---------------------------------------------------------------------------------------

void StorageBackend::loadUDisks()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        loadLsblk(tr("The system message bus is not reachable, so the disk service (UDisks2) cannot be asked."));
        return;
    }
    const QDBusMessage message = QDBusMessage::createMethodCall(kUDisks, kUDisksPath, kObjectManager, QStringLiteral("GetManagedObjects"));
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 25000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<ManagedObjects> reply = *w;
        if (reply.isError()) {
            loadLsblk(tr("The disk service (UDisks2) is not available: %1").arg(reply.error().message()));
            return;
        }
        const ManagedObjects objects = reply.value();

        struct BlockInfo
        {
            QString path, device, drive, table, cryptoBacking, usage, type, label;
            double size = 0;
            bool readOnly = false, ignore = false, system = false, hasFilesystem = false, isPartition = false,
                 hasTable = false, isLoop = false, swapActive = false;
            int number = 0;
            QStringList mountPoints;
        };
        QMap<QString, BlockInfo> blocks;
        QMap<QString, InterfaceMap> drives;
        for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
            const InterfaceMap &interfaces = it.value();
            if (interfaces.contains(kDrive))
                drives.insert(it.key().path(), interfaces);
            if (!interfaces.contains(kBlock))
                continue;
            const QVariantMap b = interfaces.value(kBlock);
            BlockInfo info;
            info.path = it.key().path();
            info.device = bytesToPath(b.value(QStringLiteral("Device")).toByteArray());
            info.drive = objectPath(b.value(QStringLiteral("Drive")));
            info.cryptoBacking = objectPath(b.value(QStringLiteral("CryptoBackingDevice")));
            info.usage = b.value(QStringLiteral("IdUsage")).toString();
            info.type = b.value(QStringLiteral("IdType")).toString();
            info.label = b.value(QStringLiteral("IdLabel")).toString();
            info.size = b.value(QStringLiteral("Size")).toDouble();
            info.readOnly = b.value(QStringLiteral("ReadOnly")).toBool();
            info.ignore = b.value(QStringLiteral("HintIgnore")).toBool();
            info.system = b.value(QStringLiteral("HintSystem")).toBool();
            info.hasTable = interfaces.contains(QStringLiteral("org.freedesktop.UDisks2.PartitionTable"));
            info.isLoop = interfaces.contains(kLoop);
            if (interfaces.contains(kPartition)) {
                const QVariantMap p = interfaces.value(kPartition);
                info.isPartition = true;
                info.table = objectPath(p.value(QStringLiteral("Table")));
                info.number = p.value(QStringLiteral("Number")).toInt();
                if (info.label.isEmpty())
                    info.label = p.value(QStringLiteral("Name")).toString();
            }
            if (interfaces.contains(kFilesystem)) {
                info.hasFilesystem = true;
                info.mountPoints = byteArrayList(interfaces.value(kFilesystem).value(QStringLiteral("MountPoints")));
            }
            if (interfaces.contains(kSwap))
                info.swapActive = interfaces.value(kSwap).value(QStringLiteral("Active")).toBool();
            blocks.insert(info.path, info);
        }

        auto partitionEntry = [&](const BlockInfo &block, int depth, bool removable) {
            const QString kind = partitionKind(block.usage, block.type);
            const bool mounted = !block.mountPoints.isEmpty();
            QVariantMap entry{
                {QStringLiteral("device"), block.device},
                {QStringLiteral("objectPath"), block.path},
                {QStringLiteral("size"), block.size},
                {QStringLiteral("fsType"), block.type},
                {QStringLiteral("label"), block.label},
                {QStringLiteral("kind"), kind},
                {QStringLiteral("depth"), depth},
                {QStringLiteral("mountPoints"), block.mountPoints},
                {QStringLiteral("mounted"), mounted || block.swapActive},
                {QStringLiteral("canMount"), block.hasFilesystem && !mounted},
                {QStringLiteral("canUnmount"), block.hasFilesystem && mounted && (removable || !isSystemMount(block.mountPoints))},
                {QStringLiteral("readOnly"), block.readOnly},
            };
            addUsage(entry, block.mountPoints);
            return entry;
        };
        // A partition (or whole-disk file system) followed by what is inside
        // it when it is an unlocked encrypted container.
        std::function<void(const BlockInfo &, int, bool, QVariantList &)> addWithChildren =
            [&](const BlockInfo &block, int depth, bool removable, QVariantList &out) {
                out << partitionEntry(block, depth, removable);
                for (const BlockInfo &child : std::as_const(blocks)) {
                    if (child.cryptoBacking == block.path && depth < 4)
                        addWithChildren(child, depth + 1, removable, out);
                }
            };

        QVariantList disks;
        QSet<QString> used;
        bool smartPending = false;
        for (const BlockInfo &block : std::as_const(blocks)) {
            if (block.isPartition || block.drive.isEmpty() || block.ignore || !drives.contains(block.drive))
                continue;
            const InterfaceMap driveInterfaces = drives.value(block.drive);
            const QVariantMap d = driveInterfaces.value(kDrive);
            const QString connection = d.value(QStringLiteral("ConnectionBus")).toString();
            const bool removable = d.value(QStringLiteral("Removable")).toBool() || d.value(QStringLiteral("MediaRemovable")).toBool()
                                   || connection == QLatin1String("usb") || connection == QLatin1String("sdio");
            const bool mediaAvailable = d.value(QStringLiteral("MediaAvailable"), true).toBool();
            const bool optical = d.value(QStringLiteral("Optical")).toBool()
                                 || d.value(QStringLiteral("Media")).toString().startsWith(QLatin1String("optical"));
            QString name = joinParts({d.value(QStringLiteral("Vendor")).toString().trimmed(), d.value(QStringLiteral("Model")).toString().trimmed()})
                               .replace(QStringLiteral("  ·  "), QStringLiteral(" "));
            if (name.isEmpty())
                name = block.device;

            QVariantList partitions;
            QList<BlockInfo> children;
            for (const BlockInfo &candidate : std::as_const(blocks)) {
                if (candidate.isPartition && candidate.table == block.path)
                    children << candidate;
            }
            std::sort(children.begin(), children.end(), [](const BlockInfo &a, const BlockInfo &b) { return a.number < b.number; });
            for (const BlockInfo &child : std::as_const(children)) {
                addWithChildren(child, 0, removable, partitions);
                used.insert(child.path);
            }
            if (children.isEmpty() && !block.hasTable && (block.hasFilesystem || !block.type.isEmpty()))
                addWithChildren(block, 0, removable, partitions);
            used.insert(block.path);

            // SMART as the service has it.
            QVariantMap smart = smartUnknown(tr("Not reported"));
            if (driveInterfaces.contains(kAta)) {
                const QVariantMap ata = driveInterfaces.value(kAta);
                if (!ata.value(QStringLiteral("SmartSupported")).toBool()) {
                    smart = smartUnknown(tr("Not supported by this drive"));
                } else if (!ata.value(QStringLiteral("SmartEnabled")).toBool()) {
                    smart = smartUnknown(tr("Switched off on this drive"));
                } else if (ata.value(QStringLiteral("SmartUpdated")).toULongLong() == 0) {
                    smart = smartUnknown(tr("Not read yet"));
                    smartPending = true;
                    if (!m_smartUpdateRequested) {
                        // Ask the service to read the data once (does not wake a sleeping disk).
                        QDBusMessage update = QDBusMessage::createMethodCall(kUDisks, block.drive, kAta, QStringLiteral("SmartUpdate"));
                        update << QVariantMap{{QStringLiteral("nowakeup"), true}};
                        QDBusConnection::systemBus().asyncCall(update, 30000);
                    }
                } else {
                    const bool failing = ata.value(QStringLiteral("SmartFailing")).toBool();
                    const qlonglong bad = ata.value(QStringLiteral("SmartNumBadSectors")).toLongLong();
                    smart = QVariantMap{
                        {QStringLiteral("state"), failing ? QStringLiteral("failing") : QStringLiteral("ok")},
                        {QStringLiteral("text"), failing ? tr("The drive reports that it is failing") : tr("Healthy")},
                        {QStringLiteral("detail"), joinParts({temperatureText(ata.value(QStringLiteral("SmartTemperature")).toDouble()),
                                                             hoursText(ata.value(QStringLiteral("SmartPowerOnSeconds")).toDouble() / 3600.0),
                                                             bad > 0 ? tr("%1 bad sectors").arg(bad) : QString()})},
                    };
                }
            } else if (driveInterfaces.contains(kNvme)) {
                const QVariantMap nvme = driveInterfaces.value(kNvme);
                if (nvme.value(QStringLiteral("SmartUpdated")).toULongLong() == 0) {
                    smart = smartUnknown(tr("Not read yet"));
                } else {
                    const QStringList warnings = nvme.value(QStringLiteral("SmartCriticalWarning")).toStringList();
                    smart = QVariantMap{
                        {QStringLiteral("state"), warnings.isEmpty() ? QStringLiteral("ok") : QStringLiteral("failing")},
                        {QStringLiteral("text"), warnings.isEmpty() ? tr("Healthy") : tr("Critical warning: %1").arg(warnings.join(QStringLiteral(", ")))},
                        {QStringLiteral("detail"), joinParts({temperatureText(nvme.value(QStringLiteral("SmartTemperature")).toDouble()),
                                                             hoursText(nvme.value(QStringLiteral("SmartPowerOnHours")).toDouble())})},
                    };
                }
            }

            bool anyMounted = false;
            for (const QVariant &p : std::as_const(partitions))
                anyMounted = anyMounted || p.toMap().value(QStringLiteral("mounted")).toBool();
            const bool canEject = d.value(QStringLiteral("Ejectable")).toBool();
            const bool canPowerOff = d.value(QStringLiteral("CanPowerOff")).toBool();
            disks << QVariantMap{
                {QStringLiteral("id"), block.path},
                {QStringLiteral("device"), block.device},
                {QStringLiteral("name"), name},
                {QStringLiteral("size"), block.size > 0 ? block.size : d.value(QStringLiteral("Size")).toDouble()},
                {QStringLiteral("removable"), removable},
                {QStringLiteral("mediaAvailable"), mediaAvailable},
                {QStringLiteral("kind"), driveKind(removable, connection, optical, d.value(QStringLiteral("RotationRate")).toInt())},
                {QStringLiteral("bus"), connection},
                {QStringLiteral("drivePath"), block.drive},
                {QStringLiteral("canEject"), canEject},
                {QStringLiteral("canPowerOff"), canPowerOff},
                {QStringLiteral("canRemoveSafely"), removable && (canEject || canPowerOff || anyMounted)},
                {QStringLiteral("smart"), smart},
                {QStringLiteral("partitions"), partitions},
            };
        }
        m_smartUpdateRequested = m_smartUpdateRequested || smartPending;

        // Volumes without a drive behind them: LVM logical volumes, RAID
        // arrays, mounted disk images. Unlocked containers were added above.
        for (const BlockInfo &block : std::as_const(blocks)) {
            if (used.contains(block.path) || block.isPartition || !block.drive.isEmpty() || !block.cryptoBacking.isEmpty()
                || block.ignore || block.size <= 0)
                continue;
            if (!block.hasFilesystem && block.type != QLatin1String("swap"))
                continue;
            if (block.isLoop && block.mountPoints.isEmpty())
                continue;
            QVariantList partitions;
            addWithChildren(block, 0, false, partitions);
            disks << QVariantMap{
                {QStringLiteral("id"), block.path},
                {QStringLiteral("device"), block.device},
                {QStringLiteral("name"), block.isLoop ? tr("Disk image") : block.device.startsWith(QLatin1String("/dev/md")) ? tr("RAID array")
                                                                                                                          : tr("Logical volume")},
                {QStringLiteral("size"), block.size},
                {QStringLiteral("removable"), false},
                {QStringLiteral("mediaAvailable"), true},
                {QStringLiteral("kind"), QStringLiteral("virtual")},
                {QStringLiteral("bus"), QString()},
                {QStringLiteral("drivePath"), QString()},
                {QStringLiteral("canEject"), false},
                {QStringLiteral("canPowerOff"), false},
                {QStringLiteral("canRemoveSafely"), false},
                {QStringLiteral("smart"), smartUnknown(QString())},
                {QStringLiteral("partitions"), partitions},
            };
        }
        std::sort(disks.begin(), disks.end(), [](const QVariant &a, const QVariant &b) {
            const QVariantMap x = a.toMap(), y = b.toMap();
            if (x.value(QStringLiteral("removable")).toBool() != y.value(QStringLiteral("removable")).toBool())
                return y.value(QStringLiteral("removable")).toBool();
            return x.value(QStringLiteral("device")).toString() < y.value(QStringLiteral("device")).toString();
        });
        finishDisks(disks, QStringLiteral("udisks"), QString());
    });
}

// ---- lsblk fallback ----------------------------------------------------------------------------------

void StorageBackend::loadLsblk(const QString &reason)
{
    const QStringList arguments = {QStringLiteral("-J"), QStringLiteral("-b"), QStringLiteral("-o"),
                                   QStringLiteral("NAME,PATH,TYPE,SIZE,FSTYPE,LABEL,MOUNTPOINTS,MODEL,VENDOR,RM,HOTPLUG,TRAN,ROTA,RO")};
    Nexus::Command::run(this, QStringLiteral("lsblk"), arguments, [this, reason](const Nexus::CommandResult &result) {
        if (!result.ok()) {
            finishDisks({}, QStringLiteral("none"),
                        tr("%1 The fallback, lsblk, failed as well%2").arg(reason, result.exitCode == -1 ? tr(" (it is not installed).")
                                                                                                     : QStringLiteral(": ") + result.errText().trimmed()));
            return;
        }
        auto truthy = [](const QJsonValue &value) {
            return value.isBool() ? value.toBool() : value.toString() == QLatin1String("1") || value.toInt() == 1;
        };
        auto mountPointsOf = [](const QJsonObject &device) {
            QStringList out;
            const QJsonArray mounts = device.value(QStringLiteral("mountpoints")).toArray();
            for (const QJsonValue &mount : mounts) {
                if (mount.isString() && !mount.toString().isEmpty())
                    out << mount.toString();
            }
            return out;
        };
        std::function<void(const QJsonObject &, int, QVariantList &)> addPartition =
            [&](const QJsonObject &device, int depth, QVariantList &out) {
                const QString type = device.value(QStringLiteral("fstype")).toString();
                const QStringList mounts = mountPointsOf(device);
                const bool swapOn = mounts.contains(QStringLiteral("[SWAP]"));
                QStringList realMounts = mounts;
                realMounts.removeAll(QStringLiteral("[SWAP]"));
                QVariantMap entry{
                    {QStringLiteral("device"), device.value(QStringLiteral("path")).toString()},
                    {QStringLiteral("objectPath"), QString()},
                    {QStringLiteral("size"), device.value(QStringLiteral("size")).toDouble()},
                    {QStringLiteral("fsType"), type},
                    {QStringLiteral("label"), device.value(QStringLiteral("label")).toString()},
                    {QStringLiteral("kind"), partitionKind(QString(), type)},
                    {QStringLiteral("depth"), depth},
                    {QStringLiteral("mountPoints"), realMounts},
                    {QStringLiteral("mounted"), !realMounts.isEmpty() || swapOn},
                    {QStringLiteral("canMount"), false},
                    {QStringLiteral("canUnmount"), false},
                    {QStringLiteral("readOnly"), truthy(device.value(QStringLiteral("ro")))},
                };
                addUsage(entry, realMounts);
                out << entry;
                const QJsonArray children = device.value(QStringLiteral("children")).toArray();
                for (const QJsonValue &child : children) {
                    if (depth < 4)
                        addPartition(child.toObject(), depth + 1, out);
                }
            };

        QVariantList disks;
        const QJsonArray devices = QJsonDocument::fromJson(result.out).object().value(QStringLiteral("blockdevices")).toArray();
        for (const QJsonValue &value : devices) {
            const QJsonObject device = value.toObject();
            const QString type = device.value(QStringLiteral("type")).toString();
            const double size = device.value(QStringLiteral("size")).toDouble();
            const QJsonArray children = device.value(QStringLiteral("children")).toArray();
            const QStringList mounts = mountPointsOf(device);
            if (size <= 0)
                continue;
            if (type == QLatin1String("loop") && mounts.isEmpty() && children.isEmpty())
                continue;
            if (device.value(QStringLiteral("name")).toString().startsWith(QLatin1String("zram")) && mounts.isEmpty())
                continue;
            const QString transport = device.value(QStringLiteral("tran")).toString();
            const bool removable = truthy(device.value(QStringLiteral("rm"))) || truthy(device.value(QStringLiteral("hotplug")))
                                   || transport == QLatin1String("usb");
            QVariantList partitions;
            for (const QJsonValue &child : children)
                addPartition(child.toObject(), 0, partitions);
            if (children.isEmpty() && (!device.value(QStringLiteral("fstype")).toString().isEmpty() || !mounts.isEmpty()))
                addPartition(device, 0, partitions);
            // Virtual disks carry a bare PCI vendor number ("0x1af4"), which is not a name.
            QString vendor = device.value(QStringLiteral("vendor")).toString().trimmed();
            if (vendor.startsWith(QLatin1String("0x")))
                vendor.clear();
            QString name = (vendor + QLatin1Char(' ') + device.value(QStringLiteral("model")).toString().trimmed()).trimmed();
            const QString path = device.value(QStringLiteral("path")).toString();
            if (name.isEmpty())
                name = type == QLatin1String("loop") ? tr("Disk image") : path;
            const QVariant rota = device.value(QStringLiteral("rota")).toVariant();
            disks << QVariantMap{
                {QStringLiteral("id"), path},
                {QStringLiteral("device"), path},
                {QStringLiteral("name"), name},
                {QStringLiteral("size"), size},
                {QStringLiteral("removable"), removable},
                {QStringLiteral("mediaAvailable"), true},
                {QStringLiteral("kind"), type == QLatin1String("loop") ? QStringLiteral("virtual")
                                             : driveKind(removable, transport, type == QLatin1String("rom"),
                                                         rota.isNull() ? -1 : truthy(device.value(QStringLiteral("rota"))) ? 1 : 0)},
                {QStringLiteral("bus"), transport},
                {QStringLiteral("drivePath"), QString()},
                {QStringLiteral("canEject"), false},
                {QStringLiteral("canPowerOff"), false},
                {QStringLiteral("canRemoveSafely"), false},
                {QStringLiteral("smart"), type == QLatin1String("disk") ? smartUnknown(tr("Not reported")) : smartUnknown(QString())},
                {QStringLiteral("partitions"), partitions},
            };
        }
        finishDisks(disks, QStringLiteral("lsblk"),
                    tr("%1 Disks are listed with lsblk instead; mounting, unmounting and ejecting are not available.").arg(reason));
    }, 15000);
}

void StorageBackend::finishDisks(const QVariantList &disks, const QString &source, const QString &message)
{
    // SMART values read with smartctl in this session fill the gaps.
    QVariantList merged;
    for (const QVariant &value : disks) {
        QVariantMap disk = value.toMap();
        const QString device = disk.value(QStringLiteral("device")).toString();
        if (disk.value(QStringLiteral("smart")).toMap().value(QStringLiteral("state")).toString() == QLatin1String("unknown")
            && m_smartctl.contains(device))
            disk.insert(QStringLiteral("smart"), m_smartctl.value(device));
        merged << disk;
    }
    const bool changed = merged != m_disks || source != m_source || message != m_statusMessage || !m_ready;
    m_disks = merged;
    m_source = source;
    m_statusMessage = message;
    m_ready = true;
    if (changed)
        Q_EMIT disksChanged();
    querySmartctl();
}

// smartctl for drives the service has no SMART data for. Without
// administrator rights it normally cannot open the device; then the drive
// stays "not reported" with that reason.
void StorageBackend::querySmartctl()
{
    if (!Nexus::Command::available(QStringLiteral("smartctl")))
        return;
    for (const QVariant &value : std::as_const(m_disks)) {
        const QVariantMap disk = value.toMap();
        const QString device = disk.value(QStringLiteral("device")).toString();
        const QString kind = disk.value(QStringLiteral("kind")).toString();
        if (m_smartctl.contains(device) || kind == QLatin1String("virtual") || kind == QLatin1String("optical")
            || disk.value(QStringLiteral("smart")).toMap().value(QStringLiteral("state")).toString() != QLatin1String("unknown")
            || disk.value(QStringLiteral("smart")).toMap().value(QStringLiteral("text")).toString() != tr("Not reported"))
            continue;
        m_smartctl.insert(device, smartUnknown(tr("Not reported")));      // asked once per device
        ++m_smartctlRunning;
        Nexus::Command::run(this, QStringLiteral("smartctl"),
                            {QStringLiteral("--json"), QStringLiteral("-H"), QStringLiteral("-A"), QStringLiteral("--nocheck=standby"), device},
                            [this, device](const Nexus::CommandResult &result) {
            const QJsonObject json = QJsonDocument::fromJson(result.out).object();
            QVariantMap smart;
            if (json.contains(QStringLiteral("smart_status"))) {
                const bool passed = json.value(QStringLiteral("smart_status")).toObject().value(QStringLiteral("passed")).toBool();
                const double celsius = json.value(QStringLiteral("temperature")).toObject().value(QStringLiteral("current")).toDouble(-1000);
                const double hours = json.value(QStringLiteral("power_on_time")).toObject().value(QStringLiteral("hours")).toDouble();
                smart = QVariantMap{
                    {QStringLiteral("state"), passed ? QStringLiteral("ok") : QStringLiteral("failing")},
                    {QStringLiteral("text"), passed ? tr("Healthy") : tr("The drive reports that it is failing")},
                    {QStringLiteral("detail"), joinParts({celsius > -1000 ? tr("%1 °C").arg(qRound(celsius)) : QString(), hoursText(hours)})},
                };
            } else if (result.exitCode >= 0 && (result.exitCode & 2)) {
                // Bit 1: the device could not be opened.
                smart = smartUnknown(geteuid() == 0 ? tr("Not reported (smartctl cannot open the device)")
                                                    : tr("Not reported (reading it needs administrator rights)"));
            } else {
                smart = smartUnknown(tr("Not reported by this device"));
            }
            m_smartctl.insert(device, smart);
            --m_smartctlRunning;
            finishDisks(m_disks, m_source, m_statusMessage);
        }, 20000);
    }
}

// ---- actions ----------------------------------------------------------------------------------------------

void StorageBackend::setBusy(const QString &device)
{
    if (device == m_busyDevice)
        return;
    m_busyDevice = device;
    Q_EMIT busyChanged();
}

void StorageBackend::udisksCall(const QString &path, const QString &interface, const QString &method, const QString &device,
                                std::function<void(bool, const QString &)> done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kUDisks, path, interface, method);
    message << QVariantMap();
    message.setInteractiveAuthorizationAllowed(true);
    setBusy(device);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, kActionTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const bool ok = !w->isError();
        const QString error = ok ? QString() : errorText(w->error());
        done(ok, error);
    });
}

static QVariantMap findPartition(const QVariantList &disks, const QString &objectPath, QVariantMap *diskOut = nullptr)
{
    for (const QVariant &d : disks) {
        const QVariantMap disk = d.toMap();
        const QVariantList partitions = disk.value(QStringLiteral("partitions")).toList();
        for (const QVariant &p : partitions) {
            if (p.toMap().value(QStringLiteral("objectPath")).toString() == objectPath && !objectPath.isEmpty()) {
                if (diskOut)
                    *diskOut = disk;
                return p.toMap();
            }
        }
    }
    return {};
}

void StorageBackend::mount(const QString &objectPath)
{
    const QVariantMap partition = findPartition(m_disks, objectPath);
    if (partition.isEmpty() || !partition.value(QStringLiteral("canMount")).toBool() || !m_busyDevice.isEmpty())
        return;
    const QString device = partition.value(QStringLiteral("device")).toString();
    udisksCall(objectPath, kFilesystem, QStringLiteral("Mount"), device, [this, device](bool ok, const QString &error) {
        setBusy(QString());
        if (ok)
            Q_EMIT done(tr("%1 is mounted").arg(device));
        else
            Q_EMIT failed(tr("%1 could not be mounted: %2").arg(device, error));
        refresh();
    });
}

void StorageBackend::unmount(const QString &objectPath)
{
    const QVariantMap partition = findPartition(m_disks, objectPath);
    if (partition.isEmpty() || !partition.value(QStringLiteral("canUnmount")).toBool() || !m_busyDevice.isEmpty())
        return;
    const QString device = partition.value(QStringLiteral("device")).toString();
    udisksCall(objectPath, kFilesystem, QStringLiteral("Unmount"), device, [this, device](bool ok, const QString &error) {
        setBusy(QString());
        if (ok)
            Q_EMIT done(tr("%1 is unmounted").arg(device));
        else
            Q_EMIT failed(tr("%1 could not be unmounted: %2").arg(device, error));
        refresh();
    });
}

void StorageBackend::removeSafely(const QString &diskId)
{
    if (!m_busyDevice.isEmpty())
        return;
    removeStep(diskId, 0);
}

// Step 0…n-1: unmount the mounted file systems one after the other (state is
// re-read from the last enumeration each time); then eject; then power off.
void StorageBackend::removeStep(const QString &diskId, int step)
{
    QVariantMap disk;
    for (const QVariant &d : std::as_const(m_disks)) {
        if (d.toMap().value(QStringLiteral("id")).toString() == diskId)
            disk = d.toMap();
    }
    if (disk.isEmpty() || !disk.value(QStringLiteral("canRemoveSafely")).toBool()) {
        setBusy(QString());
        return;
    }
    const QString device = disk.value(QStringLiteral("device")).toString();
    const QString name = disk.value(QStringLiteral("name")).toString();
    const QString drivePath = disk.value(QStringLiteral("drivePath")).toString();
    auto fail = [this, name](const QString &error) {
        setBusy(QString());
        Q_EMIT failed(tr("%1 cannot be removed safely yet: %2").arg(name, error));
        refresh();
    };

    // Innermost volumes first (an unlocked container before its partition).
    QVariantList mounted;
    const QVariantList partitions = disk.value(QStringLiteral("partitions")).toList();
    for (const QVariant &p : partitions) {
        const QVariantMap partition = p.toMap();
        if (!partition.value(QStringLiteral("mountPoints")).toStringList().isEmpty())
            mounted.prepend(partition);
    }
    if (step < mounted.size()) {
        const QVariantMap partition = mounted.at(step).toMap();
        udisksCall(partition.value(QStringLiteral("objectPath")).toString(), kFilesystem, QStringLiteral("Unmount"), device,
                   [this, diskId, step, fail](bool ok, const QString &error) {
            if (!ok) {
                fail(error);
                return;
            }
            removeStep(diskId, step + 1);
        });
        return;
    }
    const bool canEject = disk.value(QStringLiteral("canEject")).toBool();
    const bool canPowerOff = disk.value(QStringLiteral("canPowerOff")).toBool();
    auto finish = [this, name] {
        setBusy(QString());
        Q_EMIT done(tr("%1 can be removed now").arg(name));
        refresh();
    };
    auto powerOff = [this, drivePath, device, canPowerOff, finish, fail] {
        if (!canPowerOff) {
            finish();
            return;
        }
        udisksCall(drivePath, kDrive, QStringLiteral("PowerOff"), device, [finish, fail](bool ok, const QString &error) {
            if (ok)
                finish();
            else
                fail(error);
        });
    };
    if (canEject) {
        udisksCall(drivePath, kDrive, QStringLiteral("Eject"), device, [powerOff, fail](bool ok, const QString &error) {
            if (ok)
                powerOff();
            else
                fail(error);
        });
    } else {
        powerOff();
    }
}

// ---- home folder --------------------------------------------------------------------------------------------

QString StorageBackend::homePath() const
{
    return QDir::cleanPath(QDir::homePath());
}

void StorageBackend::stopScanThread()
{
    if (m_scan)
        m_scan->cancel.store(true);
    if (m_scanThread) {
        m_scanThread->wait();
        delete m_scanThread;
        m_scanThread = nullptr;
    }
    m_scan.reset();
}

void StorageBackend::startHomeScan()
{
    if (m_homeState == QLatin1String("scanning"))
        return;
    stopScanThread();
    const quint64 generation = ++m_scanGeneration;
    m_scan = std::make_shared<ScanShared>();
    m_homeState = QStringLiteral("scanning");
    m_homeCategories.clear();
    m_homeBytes = m_homeFiles = 0;
    m_homeUnreadable = 0;
    Q_EMIT homeChanged();

    const QString home = homePath();
    const QHash<QByteArray, int> rules = categoryRules(home);
    const std::shared_ptr<ScanShared> shared = m_scan;
    const QPointer<StorageBackend> self(this);

    m_scanThread = QThread::create([home, rules, shared, self, generation] {
        Totals totals;
        auto report = [&](bool finished, bool cancelled) {
            const QVariantList list = totals.toList();
            const double bytes = totals.totalBytes(), files = totals.totalFiles();
            const int unreadable = totals.unreadable;
            QMetaObject::invokeMethod(qApp, [self, generation, list, bytes, files, unreadable, finished, cancelled] {
                if (self)
                    self->homeProgress(generation, list, bytes, files, unreadable, finished, cancelled);
            }, Qt::QueuedConnection);
        };

        QByteArray root = QFile::encodeName(home);
        char *paths[] = {root.data(), nullptr};
        FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_XDEV | FTS_NOCHDIR, nullptr);
        if (!fts) {
            totals.unreadable = 1;
            report(true, false);
            return;
        }
        std::unordered_set<InodeKey, InodeHash> hardLinks;
        auto lastReport = std::chrono::steady_clock::now();
        bool cancelled = false;
        while (FTSENT *entry = fts_read(fts)) {
            if (shared->cancel.load(std::memory_order_relaxed)) {
                cancelled = true;
                break;
            }
            int category = Other;
            if (entry->fts_level > 0) {
                category = int(entry->fts_parent->fts_number);
                if (entry->fts_level == 1)
                    category = entry->fts_name[0] == '.' ? AppData : Other;
                if (entry->fts_level <= 4) {
                    const auto rule = rules.constFind(QByteArray::fromRawData(entry->fts_path, int(entry->fts_pathlen)));
                    if (rule != rules.constEnd())
                        category = rule.value();
                }
            }
            entry->fts_number = category;

            switch (entry->fts_info) {
            case FTS_DP:
                break;                              // counted on the way in
            case FTS_DNR:
            case FTS_ERR:
            case FTS_NS:
                ++totals.unreadable;
                break;
            case FTS_D:
                totals.bytes[category] += quint64(entry->fts_statp->st_blocks) * 512u;
                break;
            default: {
                const struct stat *st = entry->fts_statp;
                if (entry->fts_info == FTS_NSOK || !st)
                    break;
                if (st->st_nlink > 1 && !S_ISDIR(st->st_mode)) {
                    if (!hardLinks.insert({st->st_dev, st->st_ino}).second)
                        break;                      // same file under another name
                }
                totals.bytes[category] += quint64(st->st_blocks) * 512u;
                ++totals.files[category];
                break;
            }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport > std::chrono::milliseconds(250)) {
                lastReport = now;
                report(false, false);
            }
        }
        fts_close(fts);
        report(true, cancelled);
    });
    m_scanThread->start(QThread::LowPriority);
}

void StorageBackend::cancelHomeScan()
{
    if (m_homeState != QLatin1String("scanning") || !m_scan)
        return;
    m_scan->cancel.store(true);
}

void StorageBackend::homeProgress(quint64 generation, const QVariantList &categories, double bytes, double files, int unreadable,
                                  bool finished, bool cancelled)
{
    if (generation != m_scanGeneration)
        return;
    m_homeCategories = categories;
    std::sort(m_homeCategories.begin(), m_homeCategories.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("bytes")).toDouble() > b.toMap().value(QStringLiteral("bytes")).toDouble();
    });
    m_homeBytes = bytes;
    m_homeFiles = files;
    m_homeUnreadable = unreadable;
    if (finished)
        m_homeState = cancelled ? QStringLiteral("cancelled") : QStringLiteral("done");
    Q_EMIT homeChanged();
}

// ---- diagnostics ----------------------------------------------------------------------------------------------

QJsonObject StorageBackend::toJson() const
{
    return {
        {QStringLiteral("source"), m_source},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("disks"), QJsonArray::fromVariantList(m_disks)},
        {QStringLiteral("volumes"), QJsonArray::fromVariantList(m_volumes)},
        {QStringLiteral("diskManagerAvailable"), diskManagerAvailable()},
    };
}

QJsonObject StorageBackend::homeToJson() const
{
    return {
        {QStringLiteral("path"), homePath()},
        {QStringLiteral("state"), m_homeState},
        {QStringLiteral("bytes"), m_homeBytes},
        {QStringLiteral("files"), m_homeFiles},
        {QStringLiteral("unreadable"), m_homeUnreadable},
        {QStringLiteral("categories"), QJsonArray::fromVariantList(m_homeCategories)},
    };
}
