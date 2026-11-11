// SPDX-License-Identifier: GPL-3.0-or-later
#include "traymenu.h"

#include "trayhost.h"
#include "trayicons.h"
#include "trayitem.h"
#include "trayutil.h"

#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QIcon>
#include <QJsonObject>
#include <QPointer>

namespace {

const QString kMenuInterface = QStringLiteral("com.canonical.dbusmenu");

bool isUnanswered(const QDBusError &error)
{
    return error.type() == QDBusError::NoReply || error.type() == QDBusError::Timeout
        || error.type() == QDBusError::TimedOut;
}

} // namespace

TrayMenu::TrayMenu(TrayHost *host, TrayItem *item, const QString &owner, const QString &path)
    : QObject(item)
    , m_host(host)
    , m_item(item)
    , m_owner(owner)
    , m_path(path)
{
    m_fetchTimer.setSingleShot(true);
    m_fetchTimer.setInterval(TrayLimits::MenuRefreshIntervalMs);
    connect(&m_fetchTimer, &QTimer::timeout, this, &TrayMenu::fetchLayout);
}

TrayMenu::~TrayMenu()
{
    if (m_open)
        listen(false);
}

// ---- talking to the application ---------------------------------------------------------------

void TrayMenu::listen(bool on)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    for (const char *signal : {"LayoutUpdated", "ItemsPropertiesUpdated"}) {
        if (on)
            bus.connect(m_owner, m_path, kMenuInterface, QLatin1String(signal), this, SLOT(dbusSignal(QDBusMessage)));
        else
            bus.disconnect(m_owner, m_path, kMenuInterface, QLatin1String(signal), this, SLOT(dbusSignal(QDBusMessage)));
    }
}

void TrayMenu::dbusSignal(const QDBusMessage &message)
{
    handleSignal(message);
}

void TrayMenu::sendEvent(int id, const QString &event)
{
    QDBusMessage call = QDBusMessage::createMethodCall(m_owner, m_path, kMenuInterface, QStringLiteral("Event"));
    call << id << event << QVariant::fromValue(QDBusVariant(QVariant(QString()))) << uint(0);
    QDBusConnection::sessionBus().asyncCall(call, TrayLimits::CallTimeoutMs);
}

void TrayMenu::aboutToShow(int id)
{
    QDBusMessage call = QDBusMessage::createMethodCall(m_owner, m_path, kMenuInterface, QStringLiteral("AboutToShow"));
    call << id;
    const QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(call, TrayLimits::MenuTimeoutMs);
    if (id == 0)
        return;         // the layout is read right after anyway
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        const QDBusPendingReply<bool> reply = *finished;
        // "true": the application changed the submenu because it is shown.
        if (!reply.isError() && reply.value() && m_open)
            scheduleFetch();
    });
}

void TrayMenu::scheduleFetch()
{
    if (m_fetching)
        m_fetchAgain = true;
    else if (!m_fetchTimer.isActive())
        m_fetchTimer.start();
}

void TrayMenu::fetchLayout()
{
    if (m_fetching) {
        m_fetchAgain = true;
        return;
    }
    m_fetching = true;
    m_fetchAgain = false;
    QDBusMessage call = QDBusMessage::createMethodCall(m_owner, m_path, kMenuInterface, QStringLiteral("GetLayout"));
    // The whole menu at once, but no deeper than is ever shown.
    call << 0 << TrayLimits::MaxMenuDepth << QStringList();
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, TrayLimits::MenuTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        m_fetching = false;
        if (finished->isError()) {
            // A menu that was read before stays usable; it is just not current.
            setState(false, isUnanswered(finished->error()) ? QStringLiteral("timeout") : QStringLiteral("error"));
        } else if (layoutArrived(finished->reply())) {
            m_loaded = true;
            m_ready = true;
            setState(false, QString());
            bump();
        } else {
            setState(false, QStringLiteral("error"));
        }
        if (m_fetchAgain && m_open)
            scheduleFetch();
        m_fetchAgain = false;
    });
}

bool TrayMenu::layoutArrived(const QDBusMessage &reply)
{
    // (u revision, (ia{sv}av) layout)
    const QVariantList arguments = reply.arguments();
    if (arguments.size() < 2 || arguments.at(1).userType() != qMetaTypeId<QDBusArgument>())
        return false;
    const QDBusArgument layout = arguments.at(1).value<QDBusArgument>();
    if (layout.currentSignature() != QLatin1String("(ia{sv}av)"))
        return false;
    QHash<int, Entry> entries;
    int root = 0;
    if (!readEntry(layout, -1, 0, entries, &root))
        return false;
    if (root != 0) {
        // The root is entry 0 by specification; be lenient about its number.
        Entry entry = entries.take(root);
        entry.id = 0;
        for (const int child : std::as_const(entry.children))
            entries[child].parent = 0;
        entries.insert(0, entry);
    }
    m_entries = entries;
    return true;
}

bool TrayMenu::readEntry(const QDBusArgument &argument, int parent, int depth, QHash<int, Entry> &into, int *id) const
{
    Entry entry;
    QVariantMap properties;
    argument.beginStructure();
    argument >> entry.id >> properties;
    entry.parent = parent;
    const bool accepted = into.size() < TrayLimits::MaxMenuEntries && !into.contains(entry.id);
    if (accepted) {
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it)
            setProperty(entry, it.key(), it.value());
        into.insert(entry.id, entry);
    }
    QList<int> children;
    argument.beginArray();
    // Children beyond the limits are not looked at; leaving the array early
    // is fine, the reader continues behind it.
    while (accepted && !argument.atEnd() && depth < TrayLimits::MaxMenuDepth
           && children.size() < TrayLimits::MaxMenuChildren && into.size() < TrayLimits::MaxMenuEntries) {
        QDBusVariant child;
        argument >> child;
        const QVariant value = child.variant();
        if (value.userType() != qMetaTypeId<QDBusArgument>())
            continue;
        const QDBusArgument childArgument = value.value<QDBusArgument>();
        if (childArgument.currentSignature() != QLatin1String("(ia{sv}av)"))
            continue;
        int childId = 0;
        if (readEntry(childArgument, entry.id, depth + 1, into, &childId))
            children << childId;
    }
    argument.endArray();
    argument.endStructure();
    if (accepted) {
        into[entry.id].children = children;
        *id = entry.id;
    }
    return accepted;
}

void TrayMenu::setProperty(Entry &entry, const QString &name, const QVariant &value)
{
    const bool isString = value.typeId() == QMetaType::QString;
    if (name == QLatin1String("label")) {
        entry.label = isString ? TrayUtil::menuLabel(value.toString().left(TrayLimits::MaxLabelLength * 4)) : QString();
    } else if (name == QLatin1String("enabled")) {
        entry.enabled = value.typeId() != QMetaType::Bool || value.toBool();
    } else if (name == QLatin1String("visible")) {
        entry.visible = value.typeId() != QMetaType::Bool || value.toBool();
    } else if (name == QLatin1String("type")) {
        entry.separator = isString && value.toString() == QLatin1String("separator");
    } else if (name == QLatin1String("children-display")) {
        entry.submenu = isString && value.toString() == QLatin1String("submenu");
    } else if (name == QLatin1String("toggle-type")) {
        const QString type = isString ? value.toString() : QString();
        entry.toggle = type == QLatin1String("checkmark") ? 1 : type == QLatin1String("radio") ? 2 : 0;
    } else if (name == QLatin1String("toggle-state")) {
        const int state = value.typeId() == QMetaType::Int ? value.toInt() : 2;
        entry.toggleState = state == 0 || state == 1 ? state : 2;
    } else if (name == QLatin1String("icon-name")) {
        // Names of the icon theme only; a menu entry does not get to name files.
        const QString icon = isString ? value.toString() : QString();
        const bool plain = !icon.isEmpty() && icon.size() <= 128 && !icon.contains(u'/')
            && TrayUtil::cleanText(icon, 128) == icon;
        entry.iconName = plain ? icon : QString();
        entry.hasIcon = -1;
    } else if (name == QLatin1String("icon-data")) {
        entry.iconImage = value.typeId() == QMetaType::QByteArray
            ? TrayIcons::fromData(value.toByteArray(), TrayLimits::KeptMenuIconSide) : QImage();
        entry.hasIcon = -1;
    }
    // "shortcut", "disposition" and unknown properties are not shown.
}

void TrayMenu::resetProperty(Entry &entry, const QString &name)
{
    // The defaults of the specification.
    if (name == QLatin1String("label")) entry.label.clear();
    else if (name == QLatin1String("enabled")) entry.enabled = true;
    else if (name == QLatin1String("visible")) entry.visible = true;
    else if (name == QLatin1String("type")) entry.separator = false;
    else if (name == QLatin1String("children-display")) entry.submenu = false;
    else if (name == QLatin1String("toggle-type")) entry.toggle = 0;
    else if (name == QLatin1String("toggle-state")) entry.toggleState = 2;
    else if (name == QLatin1String("icon-name")) { entry.iconName.clear(); entry.hasIcon = -1; }
    else if (name == QLatin1String("icon-data")) { entry.iconImage = QImage(); entry.hasIcon = -1; }
}

void TrayMenu::handleSignal(const QDBusMessage &message)
{
    if (message.member() == QLatin1String("LayoutUpdated")) {
        if (m_open)
            scheduleFetch();
        return;
    }
    if (message.member() != QLatin1String("ItemsPropertiesUpdated") || message.arguments().size() < 2)
        return;
    const QVariantList arguments = message.arguments();
    if (arguments.at(0).userType() != qMetaTypeId<QDBusArgument>()
        || arguments.at(1).userType() != qMetaTypeId<QDBusArgument>())
        return;
    bool changed = false;
    int budget = TrayLimits::MaxMenuEntries;

    // a(ia{sv}): properties that have new values
    const QDBusArgument updated = arguments.at(0).value<QDBusArgument>();
    if (updated.currentSignature() == QLatin1String("a(ia{sv})")) {
        updated.beginArray();
        while (!updated.atEnd() && budget-- > 0) {
            int id = 0;
            QVariantMap properties;
            updated.beginStructure();
            updated >> id >> properties;
            updated.endStructure();
            const auto it = m_entries.find(id);
            if (it == m_entries.end())
                continue;
            for (auto property = properties.constBegin(); property != properties.constEnd(); ++property)
                setProperty(*it, property.key(), property.value());
            changed = true;
        }
        updated.endArray();
    }
    // a(ias): properties that are back to their defaults
    const QDBusArgument removed = arguments.at(1).value<QDBusArgument>();
    if (removed.currentSignature() == QLatin1String("a(ias)")) {
        removed.beginArray();
        while (!removed.atEnd() && budget-- > 0) {
            int id = 0;
            QStringList names;
            removed.beginStructure();
            removed >> id >> names;
            removed.endStructure();
            const auto it = m_entries.find(id);
            if (it == m_entries.end())
                continue;
            for (const QString &name : std::as_const(names))
                resetProperty(*it, name);
            changed = true;
        }
        removed.endArray();
    }
    if (changed)
        bump();
}

// ---- state ---------------------------------------------------------------------------------

void TrayMenu::setState(bool loading, const QString &failure)
{
    if (loading == m_loading && failure == m_failure)
        return;
    m_loading = loading;
    m_failure = failure;
    Q_EMIT stateChanged();
}

void TrayMenu::bump()
{
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT stateChanged();
}

void TrayMenu::open()
{
    if (m_open)
        return;
    m_open = true;
    ++m_session;
    m_ready = false;
    m_openSubmenus.clear();
    listen(true);
    m_loading = false;              // so that the change is announced
    setState(true, QString());
    // In this order: the application prepares the menu, then hands it over.
    aboutToShow(0);
    fetchLayout();
    sendEvent(0, QStringLiteral("opened"));
}

void TrayMenu::close()
{
    if (!m_open)
        return;
    m_open = false;
    listen(false);
    m_fetchTimer.stop();
    QList<int> closed;
    for (auto it = m_openSubmenus.crbegin(); it != m_openSubmenus.crend(); ++it)
        closed << *it;
    closed << 0;
    m_openSubmenus.clear();
    if (m_triggering) {
        m_closedLater += closed;    // after the click that is still on its way
    } else {
        for (const int id : std::as_const(closed))
            sendEvent(id, QStringLiteral("closed"));
    }
    m_ready = false;
    m_loading = false;
    Q_EMIT stateChanged();
}

void TrayMenu::peek()
{
    if (!m_fetching) {
        setState(true, QString());
        fetchLayout();
    }
}

void TrayMenu::openSubmenu(int id)
{
    if (!m_open || !isSubmenu(id) || m_openSubmenus.contains(id) || m_openSubmenus.size() >= TrayLimits::MaxMenuDepth)
        return;
    m_openSubmenus << id;
    aboutToShow(id);
    sendEvent(id, QStringLiteral("opened"));
}

void TrayMenu::closeSubmenu(int id)
{
    if (!m_open || !m_openSubmenus.removeOne(id))
        return;
    sendEvent(id, QStringLiteral("closed"));
}

void TrayMenu::trigger(int id, QWindow *origin)
{
    const auto it = m_entries.constFind(id);
    if (id == 0 || it == m_entries.constEnd() || !it->enabled || !it->visible || it->separator || m_triggering)
        return;
    m_triggering = true;
    // The click comes with an activation token, so that the application may
    // bring a window to the front.
    const QPointer<TrayMenu> self(this);
    m_item->provideActivationToken(origin, [self, id] {
        if (!self)
            return;
        self->sendEvent(id, QStringLiteral("clicked"));
        self->m_triggering = false;
        for (const int closed : std::as_const(self->m_closedLater))
            self->sendEvent(closed, QStringLiteral("closed"));
        self->m_closedLater.clear();
    });
}

// ---- what QML shows ------------------------------------------------------------------------

bool TrayMenu::isSubmenu(int id) const
{
    const auto it = m_entries.constFind(id);
    return id != 0 && it != m_entries.constEnd() && it->submenu;
}

bool TrayMenu::entryHasIcon(const Entry &entry) const
{
    if (entry.hasIcon < 0) {
        entry.hasIcon = !entry.iconImage.isNull() || (!entry.iconName.isEmpty() && QIcon::hasThemeIcon(entry.iconName))
            ? 1 : 0;
    }
    return entry.hasIcon == 1;
}

QVariantMap TrayMenu::row(const Entry &entry) const
{
    QString icon;
    if (!entry.separator && entryHasIcon(entry)) {
        icon = QStringLiteral("image://%1/menu/%2/%3/%4")
            .arg(TrayImageProvider::scheme()).arg(m_item->serial()).arg(entry.id).arg(m_revision);
    }
    return {
        {QStringLiteral("id"), entry.id},
        {QStringLiteral("label"), entry.label},
        {QStringLiteral("enabled"), entry.enabled},
        {QStringLiteral("separator"), entry.separator},
        {QStringLiteral("toggle"), entry.toggle == 1 ? QStringLiteral("check")
                                   : entry.toggle == 2 ? QStringLiteral("radio") : QString()},
        {QStringLiteral("state"), entry.toggleState},
        {QStringLiteral("submenu"), entry.submenu},
        {QStringLiteral("icon"), icon},
    };
}

QVariantList TrayMenu::entries(int parentId) const
{
    QVariantList out;
    const auto parent = m_entries.constFind(parentId);
    if (parent == m_entries.constEnd())
        return out;
    bool afterSeparator = true;         // nothing yet: a separator would lead
    for (const int id : parent->children) {
        const auto it = m_entries.constFind(id);
        if (it == m_entries.constEnd() || !it->visible)
            continue;
        if (it->separator) {
            if (afterSeparator)
                continue;
            afterSeparator = true;
        } else {
            afterSeparator = false;
        }
        out << row(*it);
    }
    if (!out.isEmpty() && afterSeparator)
        out.removeLast();               // a separator at the end
    return out;
}

QImage TrayMenu::image(int id, int side, const QColor &foreground, const QColor &background) const
{
    const auto it = m_entries.constFind(id);
    if (it == m_entries.constEnd())
        return {};
    QImage image;
    if (!it->iconImage.isNull()) {
        image = TrayIcons::fitted(it->iconImage, side);
    } else if (!it->iconName.isEmpty()) {
        const QIcon icon = QIcon::fromTheme(it->iconName);
        if (!icon.isNull())
            image = TrayIcons::fitted(icon.pixmap(QSize(side, side), 1.0).toImage(), side);
    }
    TrayIcons::ensureLegible(image, foreground, background);
    return image;
}

QJsonArray TrayMenu::toJson(int parentId) const
{
    QJsonArray out;
    const auto parent = m_entries.constFind(parentId);
    if (parent == m_entries.constEnd())
        return out;
    for (const int id : parent->children) {
        const auto it = m_entries.constFind(id);
        if (it == m_entries.constEnd())
            continue;
        QJsonObject o{
            {QStringLiteral("id"), it->id},
            {QStringLiteral("label"), it->label},
            {QStringLiteral("enabled"), it->enabled},
            {QStringLiteral("visible"), it->visible},
            {QStringLiteral("separator"), it->separator},
            {QStringLiteral("toggle"), it->toggle == 1 ? QStringLiteral("check")
                                       : it->toggle == 2 ? QStringLiteral("radio") : QString()},
            {QStringLiteral("state"), it->toggleState},
            {QStringLiteral("submenu"), it->submenu},
            {QStringLiteral("iconName"), it->iconName},
            {QStringLiteral("hasIconData"), !it->iconImage.isNull()},
        };
        if (!it->children.isEmpty())
            o.insert(QStringLiteral("children"), toJson(id));
        out << o;
    }
    return out;
}
