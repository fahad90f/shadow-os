// SPDX-License-Identifier: GPL-3.0-or-later
#include "trayhost.h"

#include "trayitem.h"
#include "traymenu.h"
#include "trayutil.h"

#include <KWaylandExtras>
#include <KWindowSystem>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <QWindow>

namespace {

const QString kWatcherService = QStringLiteral("org.kde.StatusNotifierWatcher");
const QString kWatcherPath = QStringLiteral("/StatusNotifierWatcher");
const QString kWatcherInterface = QStringLiteral("org.kde.StatusNotifierWatcher");
const QString kItemInterface = QStringLiteral("org.kde.StatusNotifierItem");
const QString kDefaultItemPath = QStringLiteral("/StatusNotifierItem");
const QString kBusService = QStringLiteral("org.freedesktop.DBus");
const QString kBusPath = QStringLiteral("/org/freedesktop/DBus");

const char *const kItemSignals[] = {"NewTitle", "NewIcon", "NewAttentionIcon", "NewOverlayIcon", "NewToolTip",
                                    "NewStatus", "NewMenu", "NewIconThemePath"};

QDBusMessage busCall(const QString &method)
{
    return QDBusMessage::createMethodCall(kBusService, kBusPath, kBusService, method);
}

} // namespace

TrayHost::TrayHost(QObject *parent)
    : QAbstractListModel(parent)
{
}

TrayHost::~TrayHost()
{
    // Items are children; their menus refer to this host while being destroyed.
    qDeleteAll(m_items);
    m_items.clear();
}

QString TrayHost::watcher() const
{
    switch (m_watcherMode) {
    case OwnWatcher: return QStringLiteral("own");
    case ForeignWatcher: return QStringLiteral("foreign");
    case NoWatcher: break;
    }
    return QStringLiteral("none");
}

int TrayHost::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant TrayHost::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size() || role != ItemRole)
        return {};
    return QVariant::fromValue<QObject *>(m_rows.at(index.row()));
}

QHash<int, QByteArray> TrayHost::roleNames() const
{
    return {{ItemRole, "trayItem"}};
}

// ---- start-up, the watcher name --------------------------------------------------------

void TrayHost::start(Mode mode)
{
    if (m_started)
        return;
    m_started = true;
    m_mode = mode;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qWarning("horizon-shell: tray: no session bus; the system tray is not available");
        return;
    }

    // One subscription for the change signals of all items; they are told
    // apart by sender and path.
    for (const char *signal : kItemSignals) {
        bus.connect(QString(), QString(), kItemInterface, QLatin1String(signal), this,
                    SLOT(itemSignal(QDBusMessage)));
    }
    m_serviceWatcher = new QDBusServiceWatcher(this);
    m_serviceWatcher->setConnection(bus);
    m_serviceWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &TrayHost::serviceGone);

    m_nameWatcher = new QDBusServiceWatcher(kWatcherService, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_nameWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &newOwner) { watcherOwnerChanged(newOwner); });

    if (mode == Shell) {
        new TrayWatcherAdaptor(this);
        if (!bus.registerObject(kWatcherPath, this, QDBusConnection::ExportAdaptors))
            qWarning("horizon-shell: tray: cannot export %s", qPrintable(kWatcherPath));
        // A name of our own, by which a watcher can tell that this host lives.
        m_hostName = QStringLiteral("org.kde.StatusNotifierHost-%1").arg(QCoreApplication::applicationPid());
        QDBusMessage own = busCall(QStringLiteral("RequestName"));
        own << m_hostName << uint(4);       // do not queue
        bus.asyncCall(own, TrayLimits::CallTimeoutMs);
        requestWatcherName();
    } else {
        QDBusMessage ask = busCall(QStringLiteral("GetNameOwner"));
        ask << kWatcherService;
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(ask, TrayLimits::CallTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
            call->deleteLater();
            const QDBusPendingReply<QString> owner = *call;
            if (owner.isError())
                Q_EMIT listed();            // nobody is the watcher
            else
                watcherOwnerChanged(owner.value());
        });
    }
}

void TrayHost::requestWatcherName()
{
    // An existing watcher keeps the name; this shell is a host of it then.
    // "Do not queue": the name must not pass to the shell silently when
    // that watcher leaves — items only register again when they see the
    // name disappear and come back, so it is asked for anew at that point
    // (watcherOwnerChanged).
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage request = busCall(QStringLiteral("RequestName"));
    request << kWatcherService << uint(4);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(request, TrayLimits::CallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusPendingReply<uint> reply = *call;
        QDBusConnection sessionBus = QDBusConnection::sessionBus();
        if (reply.isError()) {
            qWarning("horizon-shell: tray: cannot request %s: %s", qPrintable(kWatcherService),
                     qPrintable(reply.error().message()));
            return;
        }
        if (reply.value() == 1 || reply.value() == 4) {
            watcherOwnerChanged(sessionBus.baseService());
            return;
        }
        QDBusMessage ask = busCall(QStringLiteral("GetNameOwner"));
        ask << kWatcherService;
        auto *ownerWatcher = new QDBusPendingCallWatcher(sessionBus.asyncCall(ask, TrayLimits::CallTimeoutMs), this);
        connect(ownerWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *ownerCall) {
            ownerCall->deleteLater();
            const QDBusPendingReply<QString> owner = *ownerCall;
            if (!owner.isError())
                watcherOwnerChanged(owner.value());
        });
    });
}

void TrayHost::watcherOwnerChanged(const QString &owner)
{
    if (owner == m_watcherOwner)
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (m_watcherMode == ForeignWatcher) {
        bus.disconnect(kWatcherService, kWatcherPath, kWatcherInterface, QStringLiteral("StatusNotifierItemRegistered"),
                       this, SLOT(foreignItemRegistered(QString)));
        bus.disconnect(kWatcherService, kWatcherPath, kWatcherInterface, QStringLiteral("StatusNotifierItemUnregistered"),
                       this, SLOT(foreignItemUnregistered(QString)));
    }
    // Items registered with the watcher that was; they register again with
    // the new one.
    removeAll();
    m_watcherOwner = owner;
    if (owner.isEmpty()) {
        m_watcherMode = NoWatcher;
        if (m_mode == Shell)
            requestWatcherName();       // the watcher has left: take its place
    } else if (m_mode == Shell && owner == bus.baseService()) {
        becomeWatcher();
    } else {
        attachToForeignWatcher(owner);
    }
    Q_EMIT stateChanged();
}

void TrayHost::becomeWatcher()
{
    m_watcherMode = OwnWatcher;
    // Tells items that started before the shell that there is a host now.
    Q_EMIT hostRegistered();
}

void TrayHost::attachToForeignWatcher(const QString &owner)
{
    m_watcherMode = ForeignWatcher;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (m_mode == Shell) {
        qInfo("horizon-shell: tray: %s is provided by another program (%s); showing its items",
              qPrintable(kWatcherService), qPrintable(owner));
        QDBusMessage host = QDBusMessage::createMethodCall(kWatcherService, kWatcherPath, kWatcherInterface,
                                                           QStringLiteral("RegisterStatusNotifierHost"));
        host << m_hostName;
        bus.asyncCall(host, TrayLimits::CallTimeoutMs);
    }
    bus.connect(kWatcherService, kWatcherPath, kWatcherInterface, QStringLiteral("StatusNotifierItemRegistered"),
                this, SLOT(foreignItemRegistered(QString)));
    bus.connect(kWatcherService, kWatcherPath, kWatcherInterface, QStringLiteral("StatusNotifierItemUnregistered"),
                this, SLOT(foreignItemUnregistered(QString)));

    QDBusMessage get = QDBusMessage::createMethodCall(kWatcherService, kWatcherPath,
                                                      QStringLiteral("org.freedesktop.DBus.Properties"),
                                                      QStringLiteral("Get"));
    get << kWatcherInterface << QStringLiteral("RegisteredStatusNotifierItems");
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(get, TrayLimits::PropertyTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, owner](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *call;
        if (owner == m_watcherOwner && !reply.isError()) {
            const QStringList registrations = reply.value().variant().toStringList();
            for (const QString &registration : registrations)
                foreignItemRegistered(registration);
        }
        Q_EMIT listed();
    });
}

// ---- items ------------------------------------------------------------------------------

bool TrayHost::splitRegistration(const QString &registration, QString *service, QString *path)
{
    const qsizetype slash = registration.indexOf(u'/');
    *service = slash < 0 ? registration : registration.left(slash);
    *path = slash < 0 ? kDefaultItemPath : registration.mid(slash);
    return TrayUtil::isValidBusName(*service) && TrayUtil::isValidObjectPath(*path);
}

void TrayHost::foreignItemRegistered(const QString &registration)
{
    QString service, path;
    if (m_watcherMode != ForeignWatcher || !splitRegistration(registration, &service, &path))
        return;
    if (m_items.size() >= TrayLimits::MaxItems || m_byRegistration.contains(service + path))
        return;
    addItem(service, path, QString());
}

void TrayHost::foreignItemUnregistered(const QString &registration)
{
    QString service, path;
    if (m_watcherMode != ForeignWatcher || !splitRegistration(registration, &service, &path))
        return;
    if (TrayItem *item = m_byRegistration.value(service + path))
        removeItem(item, false);
}

void TrayHost::registerItem(const QString &serviceOrPath, const QDBusMessage &message)
{
    const auto refuse = [&message](QDBusError::ErrorType type, const QString &text) {
        message.setDelayedReply(true);
        QDBusConnection::sessionBus().send(message.createErrorReply(type, text));
    };
    if (m_watcherMode != OwnWatcher) {
        refuse(QDBusError::Failed, QStringLiteral("This program is not the StatusNotifierWatcher of the session"));
        return;
    }
    // Either a bus name (the item is at /StatusNotifierItem) or the object
    // path of an item of the calling connection.
    const QString sender = message.service();
    QString service = serviceOrPath, path = kDefaultItemPath;
    if (serviceOrPath.startsWith(u'/')) {
        service = sender;
        path = serviceOrPath;
    }
    if (!TrayUtil::isValidBusName(service) || !TrayUtil::isValidObjectPath(path)) {
        refuse(QDBusError::InvalidArgs, QStringLiteral("Not a bus name or an object path"));
        return;
    }
    if (m_byRegistration.contains(service + path))
        return;                         // registering twice is harmless
    int ofClient = 0;
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        if (*it == sender)
            ++ofClient;
    }
    if (m_items.size() >= TrayLimits::MaxItems || ofClient >= TrayLimits::MaxItemsPerClient) {
        refuse(QDBusError::LimitsExceeded, QStringLiteral("Too many status notifier items"));
        return;
    }
    addItem(service, path, sender);
    Q_EMIT itemRegistered(service + path);
}

void TrayHost::registerHost(const QString &service, const QDBusMessage &message)
{
    Q_UNUSED(message)
    if (m_watcherMode != OwnWatcher || !TrayUtil::isValidBusName(service) || m_hosts.contains(service)
        || m_hosts.size() >= TrayLimits::MaxHosts)
        return;
    m_hosts << service;
    if (m_serviceWatcher)
        m_serviceWatcher->addWatchedService(service);
    Q_EMIT hostRegistered();
}

QStringList TrayHost::registeredItems() const
{
    QStringList out;
    if (m_watcherMode != OwnWatcher)
        return out;
    for (const TrayItem *item : m_items)
        out << item->registration();
    return out;
}

void TrayHost::addItem(const QString &service, const QString &path, const QString &client)
{
    auto *item = new TrayItem(this, m_nextSerial++, service, path);
    m_items << item;
    m_byRegistration.insert(service + path, item);
    m_clients.insert(item, client);
    if (m_serviceWatcher)
        m_serviceWatcher->addWatchedService(service);

    connect(item, &TrayItem::ownerResolved, this, [this, item] {
        m_byOwner.insert(item->owner() + item->path(), item);
    });
    connect(item, &TrayItem::readyChanged, this, [this, item] { itemReady(item); });
    connect(item, &TrayItem::statusChanged, this, &TrayHost::recount);
    connect(item, &TrayItem::failed, this, [this, item] {
        qInfo("horizon-shell: tray: %s does not answer as a status notifier item; dropped",
              qPrintable(item->registration()));
        removeItem(item, true);
    });
    item->start();
}

void TrayHost::itemReady(TrayItem *item)
{
    if (!item->ready() || m_rows.contains(item) || !m_items.contains(item))
        return;
    // Kept in the order of registration.
    qsizetype row = 0;
    while (row < m_rows.size() && m_rows.at(row)->serial() < item->serial())
        ++row;
    beginInsertRows({}, int(row), int(row));
    m_rows.insert(row, item);
    endInsertRows();
    recount();
}

void TrayHost::removeItem(TrayItem *item, bool announce)
{
    if (!m_items.removeOne(item))
        return;
    const qsizetype row = m_rows.indexOf(item);
    if (row >= 0) {
        beginRemoveRows({}, int(row), int(row));
        m_rows.removeAt(row);
        endRemoveRows();
    }
    m_byRegistration.remove(item->registration());
    m_byOwner.remove(item->owner() + item->path());
    m_clients.remove(item);
    const QString service = item->service();
    const bool serviceStillUsed = m_hosts.contains(service)
        || std::any_of(m_items.cbegin(), m_items.cend(),
                       [&service](const TrayItem *other) { return other->service() == service; });
    if (!serviceStillUsed && m_serviceWatcher)
        m_serviceWatcher->removeWatchedService(service);
    if (announce && m_watcherMode == OwnWatcher)
        Q_EMIT itemUnregistered(item->registration());
    Q_EMIT itemRemoved(item);
    if (m_menuItem == item)
        setMenuItem(nullptr);
    item->deleteLater();
    recount();
}

void TrayHost::removeAll()
{
    const QList<TrayItem *> items = m_items;
    for (TrayItem *item : items)
        removeItem(item, false);
    m_hosts.clear();
}

void TrayHost::serviceGone(const QString &service)
{
    const QList<TrayItem *> items = m_items;
    for (TrayItem *item : items) {
        if (item->service() == service)
            removeItem(item, true);
    }
    if (m_hosts.removeAll(service) > 0 && m_serviceWatcher)
        m_serviceWatcher->removeWatchedService(service);
}

void TrayHost::recount()
{
    int shown = 0;
    for (const TrayItem *item : std::as_const(m_rows)) {
        if (item->shown())
            ++shown;
    }
    if (shown != m_shown) {
        m_shown = shown;
        Q_EMIT shownCountChanged();
    }
}

void TrayHost::itemSignal(const QDBusMessage &message)
{
    if (TrayItem *item = m_byOwner.value(message.service() + message.path()))
        item->handleSignal(message.member(), message.arguments());
}

TrayItem *TrayHost::itemBySerial(int serial) const
{
    for (TrayItem *item : m_items) {
        if (item->serial() == serial)
            return item;
    }
    return nullptr;
}

QImage TrayHost::itemImage(int serial, int side, const QColor &foreground, const QColor &background) const
{
    const TrayItem *item = itemBySerial(serial);
    return item ? item->image(side, foreground, background) : QImage();
}

QImage TrayHost::menuImage(int serial, int entryId, int side, const QColor &foreground,
                           const QColor &background) const
{
    const TrayItem *item = itemBySerial(serial);
    const TrayMenu *menu = item ? item->existingMenu() : nullptr;
    return menu ? menu->image(entryId, side, foreground, background) : QImage();
}

void TrayHost::setMenuItem(QObject *item)
{
    if (item == m_menuItem)
        return;
    m_menuItem = item;
    Q_EMIT menuItemChanged();
}

void TrayHost::showMenu(QObject *object, int x)
{
    auto *item = qobject_cast<TrayItem *>(object);
    if (item && m_items.contains(item) && item->hasMenu())
        Q_EMIT menuRequested(item, x);
}

void TrayHost::showToolTip(QObject *object, int x)
{
    auto *item = qobject_cast<TrayItem *>(object);
    if (item && m_items.contains(item))
        Q_EMIT toolTipRequested(item, x);
}

void TrayHost::hideToolTip()
{
    Q_EMIT toolTipDismissed();
}

// ---- activation tokens -------------------------------------------------------------------

void TrayHost::requestToken(QWindow *origin, const QString &appId, const std::function<void(const QString &)> &then)
{
    if (!origin || !KWindowSystem::isPlatformWayland() || m_tokenRequests.size() >= 16) {
        then(QString());
        return;
    }
    const int serial = int(KWaylandExtras::lastInputSerial(origin));
    if (m_tokenRequests.contains(serial)) {
        then(QString());                // one token per input event
        return;
    }
    if (!m_tokenConnected) {
        m_tokenConnected = true;
        connect(KWaylandExtras::self(), &KWaylandExtras::xdgActivationTokenArrived, this,
                [this](int tokenSerial, const QString &token) {
            const auto it = m_tokenRequests.constFind(tokenSerial);
            if (it == m_tokenRequests.constEnd())
                return;
            const TokenRequest request = *it;
            m_tokenRequests.erase(it);
            request.then(token);
        });
    }
    m_tokenRequests.insert(serial, {then});
    KWaylandExtras::requestXdgActivationToken(origin, uint32_t(serial), appId);
    QTimer::singleShot(TrayLimits::TokenTimeoutMs, this, [this, serial] {
        const auto it = m_tokenRequests.constFind(serial);
        if (it == m_tokenRequests.constEnd())
            return;
        const TokenRequest request = *it;
        m_tokenRequests.erase(it);
        request.then(QString());
    });
}

// ---- diagnostics -------------------------------------------------------------------------

bool TrayHost::settled() const
{
    return std::all_of(m_items.cbegin(), m_items.cend(), [](const TrayItem *item) { return item->ready(); });
}

QJsonObject TrayHost::toJson() const
{
    QJsonArray items;
    for (const TrayItem *item : m_items)
        items << item->toJson();
    return QJsonObject{
        {QStringLiteral("available"), available()},
        {QStringLiteral("watcher"), watcher()},
        {QStringLiteral("watcherOwner"), m_watcherOwner},
        {QStringLiteral("hostName"), m_hostName},
        {QStringLiteral("otherHosts"), QJsonArray::fromStringList(m_hosts)},
        {QStringLiteral("shownCount"), m_shown},
        {QStringLiteral("items"), items},
    };
}

// ---- org.kde.StatusNotifierWatcher ---------------------------------------------------------

TrayWatcherAdaptor::TrayWatcherAdaptor(TrayHost *host)
    : QDBusAbstractAdaptor(host)
    , m_host(host)
{
    connect(host, &TrayHost::itemRegistered, this, &TrayWatcherAdaptor::StatusNotifierItemRegistered);
    connect(host, &TrayHost::itemUnregistered, this, &TrayWatcherAdaptor::StatusNotifierItemUnregistered);
    connect(host, &TrayHost::hostRegistered, this, &TrayWatcherAdaptor::StatusNotifierHostRegistered);
}

QStringList TrayWatcherAdaptor::registeredItems() const
{
    return m_host->registeredItems();
}

void TrayWatcherAdaptor::RegisterStatusNotifierItem(const QString &service, const QDBusMessage &message)
{
    m_host->registerItem(service, message);
}

void TrayWatcherAdaptor::RegisterStatusNotifierHost(const QString &service, const QDBusMessage &message)
{
    m_host->registerHost(service, message);
}
