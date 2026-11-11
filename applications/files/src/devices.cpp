// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices.h"

#include "interaction.h"
#include "util.h"

#include <Solid/Block>
#include <Solid/DeviceNotifier>
#include <Solid/NetworkShare>
#include <Solid/OpticalDisc>
#include <Solid/OpticalDrive>
#include <Solid/StorageAccess>
#include <Solid/StorageDrive>
#include <Solid/StorageVolume>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QJsonObject>
#include <QStorageInfo>

using namespace Qt::StringLiterals;

namespace {

// The drive a volume lives on (the volume itself for drive-less devices).
Solid::Device driveOf(const Solid::Device &device)
{
    Solid::Device d = device;
    for (int depth = 0; d.isValid() && depth < 8; ++depth) {
        if (d.is<Solid::StorageDrive>())
            return d;
        d = d.parent();
    }
    return Solid::Device();
}

} // namespace

Devices::Devices(QObject *parent)
    : QObject(parent)
{
    auto *notifier = Solid::DeviceNotifier::instance();
    connect(notifier, &Solid::DeviceNotifier::deviceAdded, this, &Devices::rescan);
    connect(notifier, &Solid::DeviceNotifier::deviceRemoved, this, &Devices::rescan);

    // Free space of mounted volumes changes without any notification.
    m_spaceTimer.setInterval(30000);
    connect(&m_spaceTimer, &QTimer::timeout, this, &Devices::refresh);

    rescan();
}

Devices *Devices::instance()
{
    static Devices *self = new Devices(qApp);
    return self;
}

Devices *Devices::create(QQmlEngine *, QJSEngine *)
{
    Devices *self = instance();
    QQmlEngine::setObjectOwnership(self, QQmlEngine::CppOwnership);
    return self;
}

QString Devices::emptyReason() const
{
    if (!m_list.isEmpty())
        return {};
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected() || !bus.interface())
        return tr("The system message bus is not available, so storage devices cannot be detected.");
    const QString service = u"org.freedesktop.UDisks2"_s;
    bool present = bus.interface()->isServiceRegistered(service);
    if (!present) {
        const QDBusReply<QStringList> activatable = bus.interface()->activatableServiceNames();
        present = activatable.isValid() && activatable.value().contains(service);
    }
    if (!present)
        return tr("The UDisks2 storage service is not running, so drives cannot be listed or mounted.");
    return tr("No storage volumes were found.");
}

void Devices::watch(Solid::Device &device)
{
    auto *access = device.as<Solid::StorageAccess>();
    if (!access || m_watched.contains(device.udi()))
        return;
    m_watched.insert(device.udi());
    // The mount state is the authoritative end of a mount or unmount: not
    // every Solid backend also reports setupDone() / teardownDone().
    connect(access, &Solid::StorageAccess::accessibilityChanged, this, [this](bool accessible, const QString &udi) {
        m_busy.remove(udi);
        refresh();
        if (accessible)
            announceMounted(udi);
        else
            m_mounting.remove(udi);
    });
    connect(access, &Solid::StorageAccess::setupDone, this,
            [this](Solid::ErrorType error, const QVariant &errorData, const QString &udi) {
        operationDone(udi, error, errorData, tr("Could not mount “%1”"));
        if (error == Solid::NoError)
            announceMounted(udi);
        else
            m_mounting.remove(udi);
    });
    connect(access, &Solid::StorageAccess::teardownDone, this,
            [this](Solid::ErrorType error, const QVariant &errorData, const QString &udi) {
        operationDone(udi, error, errorData, tr("Could not unmount “%1”"));
    });
}

// Tell the window that a mount it asked for has finished (once per request).
void Devices::announceMounted(const QString &udi)
{
    if (!m_mounting.contains(udi))
        return;
    Solid::Device volume = m_devices.value(udi);
    const auto *storage = volume.as<Solid::StorageAccess>();
    if (!storage || !storage->isAccessible() || storage->filePath().isEmpty())
        return;
    m_mounting.remove(udi);
    Q_EMIT mounted(udi, QUrl::fromLocalFile(storage->filePath()));
}

void Devices::rescan()
{
    QHash<QString, Solid::Device> found;
    QStringList order;
    const QList<Solid::Device> all = Solid::Device::listFromType(Solid::DeviceInterface::StorageAccess);
    for (const Solid::Device &device : all) {
        const auto *access = device.as<Solid::StorageAccess>();
        if (!access || access->isIgnored())
            continue;
        if (const auto *volume = device.as<Solid::StorageVolume>()) {
            // Only volumes that carry a file system: no partition tables,
            // swap, RAID members or locked encrypted containers.
            if (volume->isIgnored() || volume->usage() != Solid::StorageVolume::FileSystem)
                continue;
        }
        found.insert(device.udi(), device);
        order << device.udi();
    }

    // Keep the Solid::Device objects of volumes that are still present: their
    // interfaces carry the signal connections.
    for (auto it = found.begin(); it != found.end(); ++it) {
        const auto existing = m_devices.constFind(it.key());
        if (existing != m_devices.constEnd())
            it.value() = existing.value();
        watch(it.value());
    }
    m_devices = found;
    m_order = order;
    m_watched.removeIf([this](const QString &udi) { return !m_devices.contains(udi); });
    for (auto it = m_busy.begin(); it != m_busy.end();) {
        if (m_devices.contains(*it))
            ++it;
        else
            it = m_busy.erase(it);
    }
    refresh();
}

QVariantMap Devices::describe(const Solid::Device &device) const
{
    const auto *access = device.as<Solid::StorageAccess>();
    const auto *volume = device.as<Solid::StorageVolume>();
    const auto *block = device.as<Solid::Block>();
    const Solid::Device driveDevice = driveOf(device);
    const auto *drive = driveDevice.as<Solid::StorageDrive>();

    const bool isMounted = access && access->isAccessible();
    const QString path = isMounted ? access->filePath() : QString();
    const bool network = device.is<Solid::NetworkShare>();
    const bool optical = device.is<Solid::OpticalDisc>() || (drive && drive->driveType() == Solid::StorageDrive::CdromDrive);
    const bool removable = drive && (drive->isRemovable() || drive->isHotpluggable());

    QString label = volume ? volume->label() : QString();
    if (label.isEmpty())
        label = device.description();
    if (label.isEmpty())
        label = device.product();
    if (path == "/"_L1 && (volume == nullptr || volume->label().isEmpty()))
        label = tr("System volume");

    double total = volume ? double(volume->size()) : 0.0;
    double free = -1.0;
    if (isMounted) {
        const QStorageInfo storage(path);
        if (storage.isValid() && storage.bytesTotal() > 0) {
            total = double(storage.bytesTotal());
            free = double(storage.bytesAvailable());
        }
    }

    return {
        {u"udi"_s, device.udi()},
        {u"label"_s, label},
        {u"icon"_s, network ? u"server"_s : optical ? u"storage"_s : removable ? u"usb"_s : u"harddrive"_s},
        {u"mounted"_s, isMounted},
        {u"path"_s, path},
        {u"url"_s, isMounted ? QUrl::fromLocalFile(path) : QUrl()},
        {u"device"_s, block ? block->device() : QString()},
        {u"fsType"_s, volume ? volume->fsType() : QString()},
        {u"total"_s, total},
        {u"free"_s, free},
        {u"removable"_s, removable},
        {u"optical"_s, optical},
        {u"network"_s, network},
        {u"busy"_s, m_busy.contains(device.udi())},
    };
}

void Devices::refresh()
{
    QVariantList list;
    bool anyMounted = false;
    for (const QString &udi : std::as_const(m_order)) {
        const QVariantMap map = describe(m_devices.value(udi));
        anyMounted = anyMounted || map.value(u"mounted"_s).toBool();
        list.append(map);
    }
    if (anyMounted && !m_spaceTimer.isActive())
        m_spaceTimer.start();
    else if (!anyMounted)
        m_spaceTimer.stop();
    if (list != m_list) {
        m_list = list;
        Q_EMIT devicesChanged();
    }
}

QString Devices::labelOf(const QString &udi) const
{
    for (const QVariant &v : m_list) {
        const QVariantMap map = v.toMap();
        if (map.value(u"udi"_s).toString() == udi)
            return map.value(u"label"_s).toString();
    }
    return udi;
}

void Devices::operationDone(const QString &udi, Solid::ErrorType error, const QVariant &errorData, const QString &what)
{
    m_busy.remove(udi);
    if (error != Solid::NoError && error != Solid::UserCanceled) {
        QString reason = errorData.toString();
        if (reason.isEmpty()) {
            reason = error == Solid::DeviceBusy ? tr("The device is in use.")
                : error == Solid::UnauthorizedOperation ? tr("You are not authorised to do this.")
                : error == Solid::MissingDriver ? tr("No driver for this file system is installed.")
                : tr("The operation failed.");
        }
        Interaction::instance()->reportError(what.arg(labelOf(udi)), reason);
    }
    refresh();
}

void Devices::mount(const QString &udi)
{
    Solid::Device device = m_devices.value(udi);
    auto *access = device.as<Solid::StorageAccess>();
    if (!access || m_busy.contains(udi))
        return;
    if (access->isAccessible()) {
        Q_EMIT mounted(udi, QUrl::fromLocalFile(access->filePath()));
        return;
    }
    m_busy.insert(udi);
    m_mounting.insert(udi);
    refresh();
    if (!access->setup()) {
        m_mounting.remove(udi);
        operationDone(udi, Solid::OperationFailed, tr("The storage service refused the request."), tr("Could not mount “%1”"));
    }
}

void Devices::unmount(const QString &udi)
{
    Solid::Device device = m_devices.value(udi);
    auto *access = device.as<Solid::StorageAccess>();
    if (!access || !access->isAccessible() || m_busy.contains(udi))
        return;
    m_busy.insert(udi);
    refresh();
    if (!access->teardown())
        operationDone(udi, Solid::OperationFailed, tr("The storage service refused the request."), tr("Could not unmount “%1”"));
}

void Devices::eject(const QString &udi)
{
    Solid::Device drive = driveOf(m_devices.value(udi));
    auto *optical = drive.as<Solid::OpticalDrive>();
    if (!optical || m_busy.contains(udi))
        return;
    // Holding the drive keeps its interface (and this connection) alive until
    // the drive has answered.
    m_ejecting.insert(udi, drive);
    connect(optical, &Solid::OpticalDrive::ejectDone, this,
            [this, udi](Solid::ErrorType error, const QVariant &errorData, const QString &) {
        if (!m_ejecting.contains(udi))
            return;
        operationDone(udi, error, errorData, tr("Could not eject “%1”"));
        QTimer::singleShot(0, this, [this, udi] { m_ejecting.remove(udi); });
    });
    m_busy.insert(udi);
    refresh();
    if (!optical->eject()) {
        operationDone(udi, Solid::OperationFailed, tr("The drive refused the request."), tr("Could not eject “%1”"));
        QTimer::singleShot(0, this, [this, udi] { m_ejecting.remove(udi); });
    }
}

QJsonArray Devices::toJson() const
{
    QJsonArray array;
    for (const QVariant &v : m_list) {
        const QVariantMap map = v.toMap();
        array.append(QJsonObject{
            {u"udi"_s, map.value(u"udi"_s).toString()},
            {u"label"_s, map.value(u"label"_s).toString()},
            {u"device"_s, map.value(u"device"_s).toString()},
            {u"fsType"_s, map.value(u"fsType"_s).toString()},
            {u"mounted"_s, map.value(u"mounted"_s).toBool()},
            {u"mountPoint"_s, map.value(u"path"_s).toString()},
            {u"totalBytes"_s, map.value(u"total"_s).toDouble()},
            {u"freeBytes"_s, map.value(u"free"_s).toDouble()},
            {u"removable"_s, map.value(u"removable"_s).toBool()},
            {u"optical"_s, map.value(u"optical"_s).toBool()},
            {u"network"_s, map.value(u"network"_s).toBool()},
        });
    }
    return array;
}
