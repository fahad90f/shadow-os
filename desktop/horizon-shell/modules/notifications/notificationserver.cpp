// SPDX-License-Identifier: GPL-3.0-or-later
#include "notificationserver.h"

#include "applications.h"
#include "notificationimage.h"

#include <KWaylandExtras>
#include <KWindowSystem>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QWindow>

#include <algorithm>

namespace {

const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
const QString kInterface = QStringLiteral("org.freedesktop.Notifications");
const QString kDefaultAction = QStringLiteral("default");

QString imageUrl(const char *kind, const Notification &n)
{
    return QStringLiteral("image://%1/%2/%3/%4")
        .arg(NotificationImageProvider::scheme(), QLatin1String(kind))
        .arg(n.id)
        .arg(n.revision);
}

QSettings config()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("nexus"),
                     QStringLiteral("notifications"));
}

// The first of 'keys' that the hints contain.
QVariant hint(const QVariantMap &hints, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const auto it = hints.constFind(QLatin1String(key));
        if (it != hints.constEnd())
            return *it;
    }
    return {};
}

QString hintString(const QVariantMap &hints, std::initializer_list<const char *> keys)
{
    const QVariant value = hint(hints, keys);
    return value.typeId() == QMetaType::QString ? value.toString() : QString();
}

bool hintBool(const QVariantMap &hints, const char *key)
{
    const QVariant value = hint(hints, {key});
    switch (value.typeId()) {
    case QMetaType::Bool: return value.toBool();
    case QMetaType::Int: case QMetaType::UInt: case QMetaType::UChar: case QMetaType::Short:
    case QMetaType::UShort: case QMetaType::LongLong: case QMetaType::ULongLong:
        return value.toLongLong() != 0;
    default: return false;
    }
}

// A whole number sent as any of the D-Bus integer types; 'fallback' otherwise.
int hintInt(const QVariantMap &hints, const char *key, int fallback)
{
    const QVariant value = hint(hints, {key});
    switch (value.typeId()) {
    case QMetaType::Int: case QMetaType::UInt: case QMetaType::UChar: case QMetaType::Short:
    case QMetaType::UShort: case QMetaType::LongLong: case QMetaType::ULongLong:
        return int(qBound(qlonglong(-1000000), value.toLongLong(), qlonglong(1000000)));
    case QMetaType::Double:
        return int(qBound(-1000000.0, value.toDouble(), 1000000.0));
    default: return fallback;
    }
}

// The details of notifications (their text) are only given to diagnostics
// when the shell was started for testing: every program allowed to talk to
// the notification service could otherwise read everybody's notifications.
bool debugDetails()
{
    static const bool enabled = qEnvironmentVariableIntValue("NEXUS_NOTIFICATIONS_DEBUG") == 1;
    return enabled;
}

} // namespace

// ---- NotificationModel ----------------------------------------------------------------

NotificationModel::NotificationModel(NotificationServer *server)
    : QAbstractListModel(server)
    , m_server(server)
{
}

int NotificationModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_ids.size());
}

QHash<int, QByteArray> NotificationModel::roleNames() const
{
    return {
        {IdRole, "notificationId"}, {AppKeyRole, "appKey"}, {AppNameRole, "appName"}, {SummaryRole, "summary"},
        {BodyRole, "body"}, {BodyPlainRole, "bodyPlain"}, {UrgencyRole, "urgency"}, {CriticalRole, "critical"},
        {ActionsRole, "actions"}, {HasDefaultActionRole, "hasDefaultAction"}, {ImageSourceRole, "imageSource"},
        {IconSourceRole, "iconSource"}, {ProgressRole, "progress"}, {TimestampRole, "timestamp"},
        {UnreadRole, "unread"}, {ResidentRole, "resident"}, {InteractiveRole, "interactive"},
    };
}

QVariant NotificationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_ids.size())
        return {};
    // A notification that was closed stays in the list until the next sync;
    // it then answers with empty values of the right types.
    static const Notification gone;
    const Notification *n = m_server->notification(m_ids.at(index.row()));
    if (!n)
        n = &gone;
    switch (role) {
    case IdRole: return n->id;
    case AppKeyRole: return n->appKey;
    case AppNameRole: return n->appName;
    case SummaryRole: case Qt::DisplayRole: return n->summary;
    case BodyRole: return n->body.styled;
    case BodyPlainRole: return n->body.plain;
    case UrgencyRole: return n->urgency;
    case CriticalRole: return n->urgency >= 2;
    case ActionsRole: {
        QVariantList actions;
        // Nobody would hear the action of a client that has left the bus.
        if (!n->clientGone) {
            for (const NotificationAction &action : n->actions)
                actions << QVariantMap{{QStringLiteral("key"), action.key}, {QStringLiteral("label"), action.label}};
        }
        return actions;
    }
    case HasDefaultActionRole: return n->hasDefaultAction;
    case ImageSourceRole: return n->image.isNull() ? QString() : imageUrl("image", *n);
    case IconSourceRole: return n->appIcon.isNull() ? QString() : imageUrl("icon", *n);
    case ProgressRole: return n->progress;
    case TimestampRole: return double(n->timestamp);
    case UnreadRole: return n->unread;
    case ResidentRole: return n->resident;
    case InteractiveRole: return n->hasDefaultAction && !n->clientGone;
    }
    return {};
}

void NotificationModel::setIds(const QList<uint> &ids)
{
    if (ids == m_ids)
        return;
    const qsizetype before = m_ids.size();
    const auto reset = [this, &ids] {
        beginResetModel();
        m_ids = ids;
        endResetModel();
    };
    const int fewChanges = 8;

    // 1. rows that are gone
    const QSet<uint> wanted(ids.constBegin(), ids.constEnd());
    int removals = 0;
    for (const uint id : std::as_const(m_ids)) {
        if (!wanted.contains(id))
            ++removals;
    }
    if (removals > fewChanges) {
        reset();
    } else {
        for (qsizetype row = m_ids.size() - 1; row >= 0 && removals > 0; --row) {
            if (!wanted.contains(m_ids.at(row))) {
                beginRemoveRows({}, int(row), int(row));
                m_ids.removeAt(row);
                endRemoveRows();
                --removals;
            }
        }
    }

    // 2. new rows, possibly with one group of rows having moved to the top
    //    (a new notification brings its application's group to the front)
    if (m_ids != ids) {
        const QSet<uint> have(m_ids.constBegin(), m_ids.constEnd());
        QList<uint> rest;
        QList<qsizetype> insertedRows;
        for (qsizetype row = 0; row < ids.size(); ++row) {
            if (have.contains(ids.at(row)))
                rest << ids.at(row);
            else
                insertedRows << row;
        }
        bool done = false;
        if (insertedRows.size() <= fewChanges && rest.size() == m_ids.size()) {
            if (rest != m_ids && !rest.isEmpty()) {
                const qsizetype from = m_ids.indexOf(rest.constFirst());
                qsizetype length = 0;
                while (from > 0 && length < rest.size() && from + length < m_ids.size()
                       && rest.at(length) == m_ids.at(from + length))
                    ++length;
                if (length > 0) {
                    QList<uint> moved = m_ids.mid(from, length) + m_ids.mid(0, from) + m_ids.mid(from + length);
                    if (moved == rest
                        && beginMoveRows({}, int(from), int(from + length - 1), {}, 0)) {
                        m_ids = moved;
                        endMoveRows();
                    }
                }
            }
            if (rest == m_ids) {
                for (const qsizetype row : std::as_const(insertedRows)) {
                    beginInsertRows({}, int(row), int(row));
                    m_ids.insert(row, ids.at(row));
                    endInsertRows();
                }
                done = true;
            }
        }
        if (!done)
            reset();
    }
    if (before != m_ids.size())
        Q_EMIT countChanged();
}

void NotificationModel::notifyChanged(uint id)
{
    const qsizetype row = m_ids.indexOf(id);
    if (row >= 0)
        Q_EMIT dataChanged(index(int(row)), index(int(row)));
}

// ---- NotificationServer -----------------------------------------------------------------

NotificationServer::NotificationServer(Applications *applications, QObject *parent)
    : QObject(parent)
    , m_applications(applications)
    , m_popupModel(this)
    , m_historyModel(this)
{
    m_clock.start();
    m_historySync.setSingleShot(true);
    m_historySync.setInterval(30);
    connect(&m_historySync, &QTimer::timeout, this, &NotificationServer::syncHistory);
    m_tick.setInterval(250);
    connect(&m_tick, &QTimer::timeout, this, &NotificationServer::tick);

    loadConfig();
    watchConfig();
    connect(&m_configWatcher, &QFileSystemWatcher::fileChanged, this, [this] { loadConfig(); watchConfig(); });
    connect(&m_configWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { loadConfig(); watchConfig(); });
}

NotificationServer::~NotificationServer() = default;

QString NotificationServer::configPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/nexus/notifications.conf");
}

bool NotificationServer::readDoNotDisturb()
{
    QSettings settings = config();
    settings.sync();
    return settings.value(QStringLiteral("Notifications/DoNotDisturb"), false).toBool();
}

void NotificationServer::loadConfig()
{
    const bool on = readDoNotDisturb();
    if (on != m_doNotDisturb) {
        m_doNotDisturb = on;
        Q_EMIT doNotDisturbChanged();
    }
}

void NotificationServer::watchConfig()
{
    const QString path = configPath();
    const QString dir = QFileInfo(path).absolutePath();
    QDir().mkpath(dir);
    if (!m_configWatcher.directories().contains(dir))
        m_configWatcher.addPath(dir);
    if (QFileInfo::exists(path) && !m_configWatcher.files().contains(path))
        m_configWatcher.addPath(path);
}

void NotificationServer::setDoNotDisturb(bool on)
{
    if (on == m_doNotDisturb)
        return;
    m_doNotDisturb = on;
    {
        QSettings settings = config();
        settings.setValue(QStringLiteral("Notifications/DoNotDisturb"), on);
        settings.sync();
    }
    watchConfig();
    Q_EMIT doNotDisturbChanged();

    if (on) {
        // What is on screen or waiting goes to the centre, except critical popups.
        QList<uint> expired;
        const QList<uint> shown = m_visible + m_queue;
        for (const uint id : shown) {
            const auto it = m_notifications.find(id);
            if (it == m_notifications.end() || it->second->urgency >= 2)
                continue;
            withdrawPopup(*it->second);
            if (!it->second->inHistory)
                expired << id;
        }
        for (const uint id : std::as_const(expired))
            close(id, Expired);
        promoteQueued();
        syncPopups();
        updateTicking();
    }
}

QString NotificationServer::unavailableReason() const
{
    if (m_available)
        return {};
    if (!m_busConnected)
        return QStringLiteral("no-bus");
    return m_owner.isEmpty() ? QStringLiteral("starting") : QStringLiteral("other-service");
}

void NotificationServer::start()
{
    if (m_started)
        return;
    m_started = true;
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_busConnected = bus.isConnected();
    if (!m_busConnected) {
        qWarning("horizon-shell: notifications: no session bus; the notification service is not available");
        Q_EMIT availableChanged();
        return;
    }
    new NotificationAdaptor(this);
    new NotificationDebugAdaptor(this);
    if (!bus.registerObject(kPath, this, QDBusConnection::ExportAdaptors))
        qWarning("horizon-shell: notifications: cannot export %s", qPrintable(kPath));

    m_clientWatcher = new QDBusServiceWatcher(this);
    m_clientWatcher->setConnection(bus);
    m_clientWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_clientWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &NotificationServer::clientGone);

    m_nameWatcher = new QDBusServiceWatcher(kService, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_nameWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &newOwner) { setOwner(newOwner); });
    requestName();
}

void NotificationServer::requestName()
{
    // No flags: the name is not taken away from a service that already has
    // it; this shell waits in the queue and gets the name if that service
    // ever leaves.
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("/org/freedesktop/DBus"),
                                                          QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("RequestName"));
    request << kService << uint(0);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(request, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusPendingReply<uint> reply = *call;
        QDBusConnection sessionBus = QDBusConnection::sessionBus();
        if (reply.isError()) {
            qWarning("horizon-shell: notifications: cannot request the bus name: %s",
                     qPrintable(reply.error().message()));
            return;
        }
        if (reply.value() == 1 || reply.value() == 4) {         // primary owner / already owner
            setOwner(sessionBus.baseService());
            return;
        }
        // In the queue: somebody else is the notification service.
        QDBusMessage ask = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("/org/freedesktop/DBus"),
                                                          QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("GetNameOwner"));
        ask << kService;
        auto *ownerWatcher = new QDBusPendingCallWatcher(sessionBus.asyncCall(ask, 5000), this);
        connect(ownerWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *ownerCall) {
            ownerCall->deleteLater();
            const QDBusPendingReply<QString> owner = *ownerCall;
            if (!owner.isError())
                setOwner(owner.value());
        });
    });
}

void NotificationServer::setOwner(const QString &owner)
{
    const QString self = QDBusConnection::sessionBus().baseService();
    const bool available = !owner.isEmpty() && owner == self;
    if (owner == m_owner && available == m_available)
        return;
    m_owner = owner;
    if (!available && !owner.isEmpty() && owner != self) {
        qWarning("horizon-shell: notifications: %s is owned by another program (%s); "
                 "this shell does not show notifications until it leaves",
                 qPrintable(kService), qPrintable(owner));
    }
    m_available = available;
    Q_EMIT availableChanged();
}

// ---- lookups ---------------------------------------------------------------------------

const Notification *NotificationServer::notification(uint id) const
{
    const auto it = m_notifications.find(id);
    return it == m_notifications.end() ? nullptr : it->second.get();
}

QImage NotificationServer::image(uint id) const
{
    const Notification *n = notification(id);
    return n ? n->image : QImage();
}

QImage NotificationServer::appIcon(uint id) const
{
    const Notification *n = notification(id);
    return n ? n->appIcon : QImage();
}

QVariantMap NotificationServer::group(const QString &appKey) const
{
    QString name, icon;
    int count = 0;
    // The history is ordered: the first notification of a group is its newest.
    for (const uint id : m_historyModel.ids()) {
        const Notification *n = notification(id);
        if (!n || n->appKey != appKey)
            continue;
        if (count++ == 0)
            name = n->appName;
        if (icon.isEmpty() && !n->appIcon.isNull())
            icon = imageUrl("icon", *n);
    }
    return {{QStringLiteral("name"), name}, {QStringLiteral("count"), count}, {QStringLiteral("iconSource"), icon}};
}

uint NotificationServer::nextId()
{
    do {
        ++m_lastId;
    } while (m_lastId == 0 || m_notifications.count(m_lastId));
    return m_lastId;
}

QStringList NotificationServer::capabilities()
{
    return {QStringLiteral("actions"), QStringLiteral("body"), QStringLiteral("body-hyperlinks"),
            QStringLiteral("body-markup"), QStringLiteral("icon-static"), QStringLiteral("persistence")};
}

// ---- Notify ----------------------------------------------------------------------------

uint NotificationServer::notify(const QString &sender, const QString &appName, uint replacesId,
                                const QString &appIcon, const QString &summary, const QString &body,
                                const QStringList &actions, const QVariantMap &hints, int timeout)
{
    ++m_stats.received;
    QElapsedTimer spent;
    spent.start();
    Notification *n = nullptr;
    bool replaced = false;
    if (replacesId != 0) {
        // Only the client a notification belongs to may replace it — or
        // anybody once that client has left the bus, which is what scripts
        // do that run "notify-send --replace-id" again and again.
        const auto it = m_notifications.find(replacesId);
        if (it != m_notifications.end() && (it->second->sender == sender || it->second->clientGone)) {
            n = it->second.get();
            if (n->sender != sender) {
                untrackClient(n->sender);
                n->sender = sender;
                n->clientGone = false;
                trackClient(sender);
            }
            replaced = true;
            ++m_stats.replaced;
        }
    }
    if (!n) {
        auto created = std::make_unique<Notification>();
        created->id = nextId();
        created->sender = sender;
        n = created.get();
        m_notifications.emplace(n->id, std::move(created));
        trackClient(sender);
    }

    const QString oldSummary = n->summary, oldBody = n->body.plain;
    fill(*n, appName, appIcon, summary, body, actions, hints, timeout);
    n->timestamp = QDateTime::currentMSecsSinceEpoch();
    n->inHistory = !n->transient;
    // An update that only moves a progress bar does not come back as a popup
    // once its popup has gone, and does not count as new.
    const bool sameText = replaced && oldSummary == n->summary && oldBody == n->body.plain;
    if (!sameText)
        n->unread = true;
    if (!sameText || n->popup != Notification::NoPopup)
        present(*n);
    m_popupModel.notifyChanged(n->id);
    m_historyModel.notifyChanged(n->id);

    const uint id = n->id;
    if (!n->inHistory && n->popup == Notification::NoPopup) {
        // A transient notification that cannot be shown now is over. Closed
        // after this call has returned, so that the client knows the id.
        QMetaObject::invokeMethod(this, [this, id] {
            const Notification *later = notification(id);
            if (later && !later->inHistory && later->popup == Notification::NoPopup)
                close(id, Expired);
        }, Qt::QueuedConnection);
    }
    trimHistory();
    scheduleHistorySync();
    recount();
    m_stats.slowestNotifyUs = qMax(m_stats.slowestNotifyUs, quint64(spent.nsecsElapsed() / 1000));
    return id;
}

void NotificationServer::fill(Notification &n, const QString &appName, const QString &appIcon,
                              const QString &summary, const QString &body, const QStringList &actions,
                              const QVariantMap &hints, int timeout)
{
    using namespace NotificationText;

    n.appName = singleLine(appName, MaxAppNameLength);
    n.desktopEntry.clear();
    QString entryIcon;
    const QString desktopEntry = iconName(hintString(hints, {"desktop-entry"}));
    if (!desktopEntry.isEmpty() && m_applications) {
        // The name and icon of an installed application are taken from its
        // desktop entry rather than from what the client says about itself.
        const QString storageId = desktopEntry.endsWith(QLatin1String(".desktop"))
            ? desktopEntry : desktopEntry + QLatin1String(".desktop");
        if (const AppEntry *entry = m_applications->entry(storageId)) {
            n.desktopEntry = storageId;
            if (!entry->name.isEmpty())
                n.appName = singleLine(entry->name, MaxAppNameLength);
            entryIcon = entry->icon;
        }
    }
    if (!n.desktopEntry.isEmpty())
        n.appKey = n.desktopEntry;
    else if (!n.appName.isEmpty())
        n.appKey = QStringLiteral("name:") + n.appName.toLower();
    else
        n.appKey = QStringLiteral("unknown");
    if (n.appName.isEmpty())
        n.appName = tr("Unknown application");

    n.summary = singleLine(summary, MaxSummaryLength);
    n.body = sanitizeBody(body);
    // Nothing readable at all (empty, or only white space): say so rather
    // than show an empty box.
    if (n.summary.isEmpty() && n.body.plain.isEmpty())
        n.summary = tr("Notification without text");
    n.category = singleLine(hintString(hints, {"category"}), MaxCategoryLength);
    n.urgency = qBound(0, hintInt(hints, "urgency", 1), 2);
    n.transient = hintBool(hints, "transient");
    n.resident = hintBool(hints, "resident");
    const int value = hintInt(hints, "value", -1000001);     // a sentinel: no (numeric) "value" hint
    n.progress = value == -1000001 ? -1 : qBound(0, value, 100);

    // Actions: key, label, key, label, ...
    n.actions.clear();
    n.hasDefaultAction = false;
    const qsizetype pairs = qMin<qsizetype>(actions.size() / 2, 64);
    for (qsizetype i = 0; i < pairs; ++i) {
        const QString &key = actions.at(i * 2);
        if (key.isEmpty() || key.size() > MaxActionKeyLength)
            continue;
        if (key == kDefaultAction) {
            n.hasDefaultAction = true;
            continue;
        }
        if (n.actions.size() >= NotificationLimits::MaxActions)
            continue;
        const bool duplicate = std::any_of(n.actions.cbegin(), n.actions.cend(),
                                           [&key](const NotificationAction &a) { return a.key == key; });
        if (duplicate)
            continue;
        QString label = singleLine(actions.at(i * 2 + 1), MaxActionLabelLength);
        if (label.isEmpty())
            label = singleLine(key, MaxActionLabelLength);
        if (label.isEmpty())
            continue;
        n.actions.append({key, label});
    }

    // Pictures, in the order of preference of the specification.
    QImage image = NotificationImage::fromRawData(hint(hints, {"image-data", "image_data"}));
    if (image.isNull())
        image = NotificationImage::fromNameOrPath(hintString(hints, {"image-path", "image_path"}));
    QImage icon = NotificationImage::fromNameOrPath(appIcon);
    if (image.isNull() && icon.isNull())
        image = NotificationImage::fromRawData(hint(hints, {"icon_data"}));
    if (icon.isNull() && !entryIcon.isEmpty())
        icon = NotificationImage::fromNameOrPath(entryIcon);
    n.image = image;
    n.appIcon = icon;
    ++n.revision;

    // How long the popup stays. Critical notifications do not expire.
    if (n.urgency >= 2 || timeout == 0)
        n.timeoutMs = 0;
    else if (timeout < 0)
        n.timeoutMs = n.urgency == 0 ? NotificationLimits::DefaultTimeoutLowMs : NotificationLimits::DefaultTimeoutMs;
    else
        n.timeoutMs = qBound(NotificationLimits::MinTimeoutMs, timeout, NotificationLimits::MaxTimeoutMs);
}

// ---- popups ----------------------------------------------------------------------------

bool NotificationServer::mayPopup(const Notification &n)
{
    if (m_centerOpen)
        return false;               // it appears in the open centre
    if (m_doNotDisturb && n.urgency < 2) {
        ++m_stats.heldByDoNotDisturb;
        return false;
    }
    // A client gets a burst of popups and then a slow trickle; what it sends
    // beyond that is in the centre only.
    if (m_buckets.size() > NotificationLimits::MaxTrackedClients)
        m_buckets.clear();
    const qint64 now = m_clock.elapsed();
    Bucket &bucket = m_buckets[n.sender];
    if (bucket.stamp == 0)
        bucket.tokens = NotificationLimits::PopupBurst;
    else
        bucket.tokens = qMin<double>(NotificationLimits::PopupBurst,
                                     bucket.tokens + double(now - bucket.stamp) / NotificationLimits::PopupRefillMs);
    bucket.stamp = qMax<qint64>(1, now);
    if (bucket.tokens < 1.0) {
        ++m_stats.heldByRateLimit;
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

void NotificationServer::present(Notification &n)
{
    if (n.popup == Notification::Visible) {
        n.remainingMs = n.timeoutMs;        // replaced while showing: stays, starts over
        updateTicking();
        return;
    }
    if (n.popup == Notification::Queued || !mayPopup(n))
        return;
    if (m_visible.size() < NotificationLimits::MaxVisiblePopups) {
        m_visible.prepend(n.id);
        n.popup = Notification::Visible;
        n.remainingMs = n.timeoutMs;
        ++m_stats.popupsShown;
    } else if (m_queue.size() < NotificationLimits::MaxQueuedPopups) {
        if (n.urgency >= 2)
            m_queue.prepend(n.id);
        else
            m_queue.append(n.id);
        n.popup = Notification::Queued;
    } else {
        ++m_stats.heldByQueue;
    }
    syncPopups();
    updateTicking();
}

void NotificationServer::withdrawPopup(Notification &n)
{
    m_visible.removeAll(n.id);
    m_queue.removeAll(n.id);
    n.popup = Notification::NoPopup;
}

void NotificationServer::promoteQueued()
{
    while (m_visible.size() < NotificationLimits::MaxVisiblePopups && !m_queue.isEmpty()) {
        const uint id = m_queue.takeFirst();
        const auto it = m_notifications.find(id);
        if (it == m_notifications.end())
            continue;
        Notification &n = *it->second;
        m_visible.prepend(id);
        n.popup = Notification::Visible;
        n.remainingMs = n.timeoutMs;
        ++m_stats.popupsShown;
    }
}

void NotificationServer::syncPopups()
{
    m_popupModel.setIds(m_visible);
}

void NotificationServer::updateTicking()
{
    bool needed = false;
    if (!m_popupsHovered) {
        for (const uint id : std::as_const(m_visible)) {
            const Notification *n = notification(id);
            if (n && n->timeoutMs > 0) {
                needed = true;
                break;
            }
        }
    }
    if (needed && !m_tick.isActive()) {
        m_tickClock.start();
        m_tick.start();
    } else if (!needed && m_tick.isActive()) {
        m_tick.stop();
    }
}

void NotificationServer::tick()
{
    const int elapsed = int(qMin<qint64>(m_tickClock.restart(), 2000));
    if (m_popupsHovered)
        return;
    QList<uint> withdrawn, expired;
    for (const uint id : std::as_const(m_visible)) {
        const auto it = m_notifications.find(id);
        if (it == m_notifications.end() || it->second->timeoutMs <= 0)
            continue;
        it->second->remainingMs -= elapsed;
        if (it->second->remainingMs <= 0)
            (it->second->inHistory ? withdrawn : expired) << id;
    }
    for (const uint id : std::as_const(withdrawn)) {
        const auto it = m_notifications.find(id);
        if (it != m_notifications.end())
            withdrawPopup(*it->second);     // stays in the centre, unread
    }
    for (const uint id : std::as_const(expired))
        close(id, Expired);
    if (!withdrawn.isEmpty()) {
        promoteQueued();
        syncPopups();
    }
    updateTicking();
}

void NotificationServer::setPopupsHovered(bool hovered)
{
    if (hovered == m_popupsHovered)
        return;
    m_popupsHovered = hovered;
    if (!hovered) {
        // Do not let a popup vanish the moment the pointer leaves it.
        for (const uint id : std::as_const(m_visible)) {
            const auto it = m_notifications.find(id);
            if (it != m_notifications.end() && it->second->timeoutMs > 0)
                it->second->remainingMs = qMax(it->second->remainingMs, NotificationLimits::MinTimeoutMs);
        }
    }
    updateTicking();
    Q_EMIT popupsHoveredChanged();
}

void NotificationServer::setCenterOpen(bool open)
{
    if (open == m_centerOpen)
        return;
    m_centerOpen = open;
    if (open) {
        // Everything is in the list now; popups would only cover it.
        QList<uint> expired;
        const QList<uint> shown = m_visible + m_queue;
        for (const uint id : shown) {
            const auto it = m_notifications.find(id);
            if (it == m_notifications.end())
                continue;
            withdrawPopup(*it->second);
            if (!it->second->inHistory)
                expired << id;
        }
        m_visible.clear();
        m_queue.clear();
        for (const uint id : std::as_const(expired))
            close(id, Expired);
        syncPopups();
        updateTicking();
    } else {
        markAllRead();
    }
    Q_EMIT centerOpenChanged();
}

// ---- closing -----------------------------------------------------------------------------

void NotificationServer::emitClosed(const Notification &n, CloseReason reason)
{
    switch (reason) {
    case Expired: ++m_stats.closedExpired; break;
    case Dismissed: ++m_stats.closedDismissed; break;
    case ClosedByClient: ++m_stats.closedByClient; break;
    case Undefined: ++m_stats.closedUndefined; break;
    }
    if (n.clientGone || n.sender.isEmpty() || !m_busConnected)
        return;
    // Sent to the client the notification belongs to, not to everybody.
    QDBusMessage signal = QDBusMessage::createTargetedSignal(n.sender, kPath, kInterface,
                                                             QStringLiteral("NotificationClosed"));
    signal << n.id << uint(reason);
    QDBusConnection::sessionBus().send(signal);
}

void NotificationServer::close(uint id, CloseReason reason)
{
    const auto it = m_notifications.find(id);
    if (it == m_notifications.end())
        return;
    const std::unique_ptr<Notification> n = std::move(it->second);
    m_notifications.erase(it);
    const bool wasPopup = n->popup != Notification::NoPopup;
    m_visible.removeAll(id);
    m_queue.removeAll(id);
    emitClosed(*n, reason);
    untrackClient(n->sender);
    if (wasPopup) {
        promoteQueued();
        syncPopups();
        updateTicking();
    }
    if (n->inHistory) {
        // The row disappears with the next sync; until then the model
        // answers for it with empty data.
        scheduleHistorySync();
        recount();
    }
}

void NotificationServer::closeByClient(const QString &sender, uint id)
{
    // A client closes its own notifications, and those nobody owns any more.
    const Notification *n = notification(id);
    if (n && (n->sender == sender || n->clientGone))
        close(id, ClosedByClient);
}

void NotificationServer::dismiss(uint id)
{
    close(id, Dismissed);
}

void NotificationServer::dismissApp(const QString &appKey)
{
    QList<uint> ids;
    for (const auto &entry : m_notifications) {
        if (entry.second->appKey == appKey)
            ids << entry.first;
    }
    for (const uint id : std::as_const(ids))
        close(id, Dismissed);
}

void NotificationServer::clearAll()
{
    QList<uint> ids;
    ids.reserve(qsizetype(m_notifications.size()));
    for (const auto &entry : m_notifications)
        ids << entry.first;
    for (const uint id : std::as_const(ids))
        close(id, Dismissed);
}

void NotificationServer::markAllRead()
{
    bool changed = false;
    for (const auto &entry : m_notifications) {
        if (entry.second->unread && entry.second->inHistory) {
            entry.second->unread = false;
            changed = true;
            m_historyModel.notifyChanged(entry.first);
        }
    }
    if (changed)
        recount();
}

// ---- actions ---------------------------------------------------------------------------

void NotificationServer::activate(uint id, QWindow *origin)
{
    const Notification *n = notification(id);
    if (!n)
        return;
    if (n->hasDefaultAction && !n->clientGone)
        invokeAction(id, kDefaultAction, origin);
    else
        close(id, Dismissed);
}

void NotificationServer::invokeAction(uint id, const QString &key, QWindow *origin)
{
    const Notification *n = notification(id);
    if (!n || n->clientGone)
        return;
    const bool known = key == kDefaultAction
        ? n->hasDefaultAction
        : std::any_of(n->actions.cbegin(), n->actions.cend(),
                      [&key](const NotificationAction &a) { return a.key == key; });
    if (!known)
        return;

    // The application may want to bring a window to the front: give it an
    // activation token made from the user's click, if the compositor hands
    // one out quickly. The action is delivered either way.
    if (origin && KWindowSystem::isPlatformWayland() && m_tokenRequests.size() < 16) {
        const int serial = int(KWaylandExtras::lastInputSerial(origin));
        if (!m_tokenRequests.contains(serial)) {
            static bool connected = false;
            if (!connected) {
                connected = true;
                connect(KWaylandExtras::self(), &KWaylandExtras::xdgActivationTokenArrived, this,
                        [this](int tokenSerial, const QString &token) {
                    const auto it = m_tokenRequests.constFind(tokenSerial);
                    if (it == m_tokenRequests.constEnd())
                        return;
                    const TokenRequest request = *it;
                    m_tokenRequests.erase(it);
                    deliverAction(request.id, request.key, token);
                });
            }
            m_tokenRequests.insert(serial, {id, key});
            QString appId = n->desktopEntry;
            if (appId.endsWith(QLatin1String(".desktop")))
                appId.chop(8);
            KWaylandExtras::requestXdgActivationToken(origin, uint32_t(serial), appId);
            QTimer::singleShot(300, this, [this, serial] {
                const auto it = m_tokenRequests.constFind(serial);
                if (it == m_tokenRequests.constEnd())
                    return;
                const TokenRequest request = *it;
                m_tokenRequests.erase(it);
                deliverAction(request.id, request.key, QString());
            });
            return;
        }
    }
    deliverAction(id, key, QString());
}

void NotificationServer::deliverAction(uint id, const QString &key, const QString &token)
{
    const Notification *n = notification(id);
    if (!n)
        return;
    if (!n->clientGone && !n->sender.isEmpty() && m_busConnected) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!token.isEmpty()) {
            QDBusMessage activation = QDBusMessage::createTargetedSignal(n->sender, kPath, kInterface,
                                                                         QStringLiteral("ActivationToken"));
            activation << id << token;
            bus.send(activation);
        }
        QDBusMessage invoked = QDBusMessage::createTargetedSignal(n->sender, kPath, kInterface,
                                                                  QStringLiteral("ActionInvoked"));
        invoked << id << key;
        bus.send(invoked);
        ++m_stats.actionsInvoked;
    }
    if (!n->resident)
        close(id, Dismissed);
}

void NotificationServer::openLink(uint id, const QString &link)
{
    const Notification *n = notification(id);
    bool ok = false;
    const int index = link.toInt(&ok);
    if (!n || !ok || index < 0 || index >= n->body.links.size())
        return;
    // Only http, https and mailto targets are ever stored (NotificationText::linkTarget).
    QDesktopServices::openUrl(QUrl(n->body.links.at(index), QUrl::StrictMode));
}

// ---- history ---------------------------------------------------------------------------

void NotificationServer::trimHistory()
{
    for (;;) {
        // The oldest notification overall and of every application.
        QHash<QString, int> perApp;
        QHash<QString, const Notification *> oldestOfApp;
        const Notification *oldest = nullptr;
        int total = 0;
        const auto older = [](const Notification *a, const Notification *b) {
            return !b || a->timestamp < b->timestamp || (a->timestamp == b->timestamp && a->id < b->id);
        };
        for (const auto &entry : m_notifications) {
            const Notification *n = entry.second.get();
            if (!n->inHistory)
                continue;
            ++total;
            ++perApp[n->appKey];
            if (older(n, oldest))
                oldest = n;
            if (older(n, oldestOfApp.value(n->appKey)))
                oldestOfApp.insert(n->appKey, n);
        }
        const Notification *victim = nullptr;
        if (total > NotificationLimits::MaxHistory) {
            victim = oldest;
        } else {
            for (auto it = perApp.constBegin(); it != perApp.constEnd(); ++it) {
                if (*it > NotificationLimits::MaxHistoryPerApp) {
                    victim = oldestOfApp.value(it.key());
                    break;
                }
            }
        }
        if (!victim)
            return;
        close(victim->id, Expired);
    }
}

void NotificationServer::scheduleHistorySync()
{
    if (!m_historySync.isActive())
        m_historySync.start();
}

void NotificationServer::syncHistory()
{
    QList<const Notification *> all;
    all.reserve(qsizetype(m_notifications.size()));
    for (const auto &entry : m_notifications) {
        if (entry.second->inHistory)
            all << entry.second.get();
    }
    std::sort(all.begin(), all.end(), [](const Notification *a, const Notification *b) {
        return a->timestamp != b->timestamp ? a->timestamp > b->timestamp : a->id > b->id;
    });
    // Groups in the order of their newest notification; newest first inside.
    QStringList groupOrder;
    QHash<QString, QList<uint>> groups;
    for (const Notification *n : std::as_const(all)) {
        auto it = groups.find(n->appKey);
        if (it == groups.end()) {
            groupOrder << n->appKey;
            it = groups.insert(n->appKey, {});
        }
        it->append(n->id);
    }
    QList<uint> ids;
    ids.reserve(all.size());
    for (const QString &key : std::as_const(groupOrder))
        ids += groups.value(key);
    m_historyModel.setIds(ids);
    ++m_revision;
    Q_EMIT revisionChanged();
}

void NotificationServer::recount()
{
    int unread = 0, total = 0;
    for (const auto &entry : m_notifications) {
        if (!entry.second->inHistory)
            continue;
        ++total;
        if (entry.second->unread)
            ++unread;
    }
    if (total != m_historyCount) {
        m_historyCount = total;
        Q_EMIT countChanged();
    }
    if (unread != m_unread) {
        m_unread = unread;
        Q_EMIT unreadCountChanged();
    }
}

// ---- clients ---------------------------------------------------------------------------

void NotificationServer::trackClient(const QString &sender)
{
    if (sender.isEmpty() || !m_clientWatcher)
        return;
    if (++m_clients[sender] == 1)
        m_clientWatcher->addWatchedService(sender);
}

void NotificationServer::untrackClient(const QString &sender)
{
    const auto it = m_clients.find(sender);
    if (it == m_clients.end())
        return;
    if (--*it <= 0) {
        m_clients.erase(it);
        if (m_clientWatcher)
            m_clientWatcher->removeWatchedService(sender);
    }
}

void NotificationServer::clientGone(const QString &sender)
{
    // Its notifications stay readable; their actions have nobody to go to.
    for (const auto &entry : m_notifications) {
        Notification &n = *entry.second;
        if (n.sender != sender || n.clientGone)
            continue;
        n.clientGone = true;
        m_popupModel.notifyChanged(n.id);
        m_historyModel.notifyChanged(n.id);
    }
    m_buckets.remove(sender);
}

// ---- diagnostics -------------------------------------------------------------------------

QJsonObject NotificationServer::state() const
{
    const bool details = debugDetails();
    QJsonArray list;
    QList<uint> ids;
    for (const auto &entry : m_notifications)
        ids << entry.first;
    std::sort(ids.begin(), ids.end());
    for (const uint id : std::as_const(ids)) {
        const Notification &n = *m_notifications.at(id);
        QJsonArray actions;
        for (const NotificationAction &action : n.actions)
            actions << (details ? QJsonValue(QJsonObject{{QStringLiteral("key"), action.key},
                                                         {QStringLiteral("label"), action.label}})
                                : QJsonValue(action.key.size()));
        QJsonObject o{
            {QStringLiteral("id"), qint64(n.id)},
            {QStringLiteral("urgency"), n.urgency},
            {QStringLiteral("actions"), actions},
            {QStringLiteral("hasDefaultAction"), n.hasDefaultAction},
            {QStringLiteral("hasImage"), !n.image.isNull()},
            {QStringLiteral("hasAppIcon"), !n.appIcon.isNull()},
            {QStringLiteral("transient"), n.transient},
            {QStringLiteral("resident"), n.resident},
            {QStringLiteral("progress"), n.progress},
            {QStringLiteral("timeoutMs"), n.timeoutMs},
            {QStringLiteral("unread"), n.unread},
            {QStringLiteral("inHistory"), n.inHistory},
            {QStringLiteral("clientGone"), n.clientGone},
            {QStringLiteral("popup"), n.popup == Notification::Visible ? QStringLiteral("visible")
                                      : n.popup == Notification::Queued ? QStringLiteral("queued")
                                                                        : QStringLiteral("none")},
            {QStringLiteral("summaryLength"), n.summary.size()},
            {QStringLiteral("bodyLength"), n.body.plain.size()},
            {QStringLiteral("bodyTruncated"), n.body.truncated},
            {QStringLiteral("links"), n.body.links.size()},
        };
        if (details) {
            o.insert(QStringLiteral("sender"), n.sender);
            o.insert(QStringLiteral("appKey"), n.appKey);
            o.insert(QStringLiteral("appName"), n.appName);
            o.insert(QStringLiteral("desktopEntry"), n.desktopEntry);
            o.insert(QStringLiteral("category"), n.category);
            o.insert(QStringLiteral("summary"), n.summary);
            o.insert(QStringLiteral("body"), n.body.styled);
            o.insert(QStringLiteral("bodyPlain"), n.body.plain);
            o.insert(QStringLiteral("linkTargets"), QJsonArray::fromStringList(n.body.links));
            if (!n.image.isNull())
                o.insert(QStringLiteral("imageSize"), QStringLiteral("%1x%2").arg(n.image.width()).arg(n.image.height()));
        }
        list << o;
    }
    QJsonArray visible, queued, history;
    for (const uint id : m_visible) visible << qint64(id);
    for (const uint id : m_queue) queued << qint64(id);
    for (const uint id : m_historyModel.ids()) history << qint64(id);
    return QJsonObject{
        {QStringLiteral("available"), m_available},
        {QStringLiteral("unavailableReason"), unavailableReason()},
        {QStringLiteral("owner"), m_owner},
        {QStringLiteral("doNotDisturb"), m_doNotDisturb},
        {QStringLiteral("centerOpen"), m_centerOpen},
        {QStringLiteral("unreadCount"), m_unread},
        {QStringLiteral("count"), m_historyCount},
        {QStringLiteral("details"), details},
        {QStringLiteral("visiblePopups"), visible},
        {QStringLiteral("queuedPopups"), queued},
        {QStringLiteral("history"), history},
        {QStringLiteral("clients"), m_clients.size()},
        {QStringLiteral("notifications"), list},
        {QStringLiteral("statistics"), QJsonObject{
            {QStringLiteral("received"), qint64(m_stats.received)},
            {QStringLiteral("replaced"), qint64(m_stats.replaced)},
            {QStringLiteral("popupsShown"), qint64(m_stats.popupsShown)},
            {QStringLiteral("heldByDoNotDisturb"), qint64(m_stats.heldByDoNotDisturb)},
            {QStringLiteral("heldByRateLimit"), qint64(m_stats.heldByRateLimit)},
            {QStringLiteral("heldByQueue"), qint64(m_stats.heldByQueue)},
            {QStringLiteral("closedExpired"), qint64(m_stats.closedExpired)},
            {QStringLiteral("closedDismissed"), qint64(m_stats.closedDismissed)},
            {QStringLiteral("closedByClient"), qint64(m_stats.closedByClient)},
            {QStringLiteral("closedUndefined"), qint64(m_stats.closedUndefined)},
            {QStringLiteral("actionsInvoked"), qint64(m_stats.actionsInvoked)},
            // the longest the shell was busy with one Notify call, in microseconds
            {QStringLiteral("slowestNotifyUs"), qint64(m_stats.slowestNotifyUs)},
        }},
    };
}

// ---- D-Bus adaptors ----------------------------------------------------------------------

NotificationAdaptor::NotificationAdaptor(NotificationServer *server)
    : QDBusAbstractAdaptor(server)
    , m_server(server)
{
}

QStringList NotificationAdaptor::GetCapabilities()
{
    return NotificationServer::capabilities();
}

uint NotificationAdaptor::Notify(const QString &appName, uint replacesId, const QString &appIcon,
                                 const QString &summary, const QString &body, const QStringList &actions,
                                 const QVariantMap &hints, int expireTimeout, const QDBusMessage &message)
{
    return m_server->notify(message.service(), appName, replacesId, appIcon, summary, body, actions, hints,
                            expireTimeout);
}

void NotificationAdaptor::CloseNotification(uint id, const QDBusMessage &message)
{
    m_server->closeByClient(message.service(), id);
}

QString NotificationAdaptor::GetServerInformation(QString &vendor, QString &version, QString &specVersion)
{
    vendor = QStringLiteral("NEXUS");
    version = QCoreApplication::applicationVersion();
    if (version.isEmpty())
        version = QStringLiteral("1.0");
    specVersion = QStringLiteral("1.2");
    return QStringLiteral("NEXUS HORIZON");
}

NotificationDebugAdaptor::NotificationDebugAdaptor(NotificationServer *server)
    : QDBusAbstractAdaptor(server)
    , m_server(server)
{
}

QString NotificationDebugAdaptor::GetState()
{
    return QString::fromUtf8(QJsonDocument(m_server->state()).toJson(QJsonDocument::Compact));
}
