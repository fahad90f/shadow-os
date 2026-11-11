// SPDX-License-Identifier: GPL-3.0-or-later
#include "trayitem.h"

#include "trayhost.h"
#include "traymenu.h"
#include "trayutil.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QJsonArray>
#include <QPainter>
#include <QPointer>

#include <memory>

namespace {

const QString kItemInterface = QStringLiteral("org.kde.StatusNotifierItem");
const QString kPropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

const char *const kProperties[] = {
    "Id", "Title", "Category", "Status", "IconName", "IconThemePath", "IconPixmap", "OverlayIconName",
    "OverlayIconPixmap", "AttentionIconName", "AttentionIconPixmap", "ToolTip", "Menu", "ItemIsMenu",
};

QString text(const QVariantMap &properties, const char *name, int maxLength)
{
    const QVariant value = properties.value(QLatin1String(name));
    return value.typeId() == QMetaType::QString ? TrayUtil::cleanText(value.toString(), maxLength) : QString();
}

// An icon name as items send it: a name of the icon theme or an absolute
// file name. Anything else (relative paths, URLs) is not an icon.
QString iconName(const QVariantMap &properties, const char *name)
{
    const QVariant value = properties.value(QLatin1String(name));
    if (value.typeId() != QMetaType::QString)
        return {};
    const QString raw = value.toString();
    if (raw.size() > TrayLimits::MaxIconNameLength)
        return {};
    const QString clean = TrayUtil::cleanText(raw, TrayLimits::MaxIconNameLength);
    if (clean != raw || (clean.contains(u'/') && !clean.startsWith(u'/')) || clean.contains(QLatin1String("/../")))
        return {};
    return clean;
}

QString validStatus(const QString &status)
{
    if (status == QLatin1String("Passive") || status == QLatin1String("NeedsAttention"))
        return status;
    return QStringLiteral("Active");
}

} // namespace

TrayItem::TrayItem(TrayHost *host, int serial, const QString &service, const QString &path)
    : QObject(host)
    , m_host(host)
    , m_serial(serial)
    , m_service(service)
    , m_path(path)
{
    m_clock.start();
    m_refreshTimer.setSingleShot(true);
    connect(&m_refreshTimer, &QTimer::timeout, this, &TrayItem::refresh);
}

TrayItem::~TrayItem()
{
    delete m_menu;
    m_menu = nullptr;
}

// ---- reading the properties ----------------------------------------------------------------

void TrayItem::start()
{
    resolveOwner();
}

void TrayItem::resolveOwner()
{
    if (m_service.startsWith(u':')) {
        m_owner = m_service;
        Q_EMIT ownerResolved();
        fetch();
        return;
    }
    QDBusMessage ask = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                      QStringLiteral("/org/freedesktop/DBus"),
                                                      QStringLiteral("org.freedesktop.DBus"),
                                                      QStringLiteral("GetNameOwner"));
    ask << m_service;
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(ask, TrayLimits::CallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusPendingReply<QString> reply = *call;
        if (reply.isError() || reply.value().isEmpty()) {
            Q_EMIT failed();            // the service does not exist (any more)
            return;
        }
        m_owner = reply.value();
        Q_EMIT ownerResolved();
        fetch();
    });
}

void TrayItem::refresh()
{
    if (m_owner.isEmpty())
        return;
    if (m_fetching) {
        m_dirty = true;
        return;
    }
    const qint64 now = m_clock.elapsed();
    if (now < m_nextFetch) {
        if (!m_refreshTimer.isActive())
            m_refreshTimer.start(int(m_nextFetch - now));
        return;
    }
    fetch();
}

void TrayItem::fetch()
{
    if (m_fetching)
        return;
    m_fetching = true;
    m_dirty = false;
    if (m_singly) {
        fetchSingly();
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(m_owner, m_path, kPropertiesInterface, QStringLiteral("GetAll"));
    call << kItemInterface;
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, TrayLimits::PropertyTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        const QDBusPendingReply<QVariantMap> reply = *finished;
        if (reply.isError()) {
            fetchFailed(reply.error());
            return;
        }
        QElapsedTimer parse;
        parse.start();
        apply(reply.value());
        fetchFinished(parse.elapsed());
    });
}

// Some items answer GetAll with an error because one of their properties
// cannot be read; each property is then asked for on its own.
void TrayItem::fetchSingly()
{
    struct Collected { QVariantMap properties; int pending = 0; int answered = 0; };
    const auto collected = std::make_shared<Collected>();
    collected->pending = int(std::size(kProperties));
    for (const char *name : kProperties) {
        const QString property = QLatin1String(name);
        QDBusMessage call = QDBusMessage::createMethodCall(m_owner, m_path, kPropertiesInterface, QStringLiteral("Get"));
        call << kItemInterface << property;
        auto *watcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(call, TrayLimits::PropertyTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, collected, property](QDBusPendingCallWatcher *finished) {
            finished->deleteLater();
            const QDBusPendingReply<QDBusVariant> reply = *finished;
            if (!reply.isError()) {
                collected->properties.insert(property, reply.value().variant());
                ++collected->answered;
            }
            if (--collected->pending > 0)
                return;
            if (collected->answered == 0) {
                fetchFailed(QDBusError(QDBusError::NoReply, QStringLiteral("no property could be read")));
                return;
            }
            QElapsedTimer parse;
            parse.start();
            apply(collected->properties);
            fetchFinished(parse.elapsed());
        });
    }
}

void TrayItem::fetchFinished(qint64 parseMs)
{
    m_fetching = false;
    m_failures = 0;
    // An item whose properties are expensive to take in (large pixmaps) is
    // read less often.
    m_nextFetch = m_clock.elapsed() + qMax<qint64>(TrayLimits::RefreshIntervalMs, parseMs * 10);
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
    if (m_dirty) {
        m_dirty = false;
        refresh();
    }
}

void TrayItem::fetchFailed(const QDBusError &error)
{
    m_fetching = false;
    const bool unanswered = error.type() == QDBusError::NoReply || error.type() == QDBusError::Timeout
        || error.type() == QDBusError::TimedOut || error.type() == QDBusError::ServiceUnknown
        || error.type() == QDBusError::Disconnected;
    if (!m_singly && !unanswered) {
        m_singly = true;
        fetch();
        return;
    }
    if (m_ready)
        return;                         // what is known stays; the next change signal tries again
    if (++m_failures >= 3) {
        Q_EMIT failed();
        return;
    }
    m_nextFetch = m_clock.elapsed() + 1000;
    m_refreshTimer.start(1000);
}

void TrayItem::apply(const QVariantMap &properties)
{
    using namespace TrayLimits;

    // ---- identity
    const QString id = text(properties, "Id", MaxIdLength);
    const QString title = text(properties, "Title", MaxTitleLength);
    const QString category = text(properties, "Category", 64);
    if (id != m_id || title != m_title || category != m_category) {
        m_id = id;
        m_title = title;
        m_category = category;
        Q_EMIT identityChanged();
        Q_EMIT toolTipChanged();        // the tool tip title falls back to these
    }

    // ---- status
    const QVariant statusValue = properties.value(QStringLiteral("Status"));
    const QString status = validStatus(statusValue.typeId() == QMetaType::QString ? statusValue.toString() : QString());
    const bool statusChange = status != m_status;
    m_status = status;

    // ---- icons
    QString themePath;
    const QVariant themePathValue = properties.value(QStringLiteral("IconThemePath"));
    if (themePathValue.typeId() == QMetaType::QString) {
        const QString raw = themePathValue.toString();
        if (raw.startsWith(u'/') && raw.size() <= MaxPathLength && TrayUtil::cleanText(raw, MaxPathLength) == raw)
            themePath = raw;
    }
    const auto readIcon = [&properties, &themePath](const char *nameProperty, const char *pixmapProperty) {
        TrayIconSource source;
        source.name = iconName(properties, nameProperty);
        source.themePath = themePath;
        TrayIcons::readPixmaps(properties.value(QLatin1String(pixmapProperty)), source);
        return source;
    };
    const TrayIconSource icon = readIcon("IconName", "IconPixmap");
    const TrayIconSource attention = readIcon("AttentionIconName", "AttentionIconPixmap");
    const TrayIconSource overlay = readIcon("OverlayIconName", "OverlayIconPixmap");
    const bool iconChange = !icon.sameAs(m_icon) || !attention.sameAs(m_attentionIcon)
        || !overlay.sameAs(m_overlayIcon) || themePath != m_iconThemePath
        || (statusChange && !m_attentionIcon.isEmpty());
    m_iconThemePath = themePath;
    if (iconChange || !m_ready) {
        m_icon = icon;
        m_attentionIcon = attention;
        m_overlayIcon = overlay;
        ++m_iconGeneration;
        m_baseImages.clear();
        m_hasIcon = !baseImage(32).isNull();
        Q_EMIT iconChanged();
    }
    if (statusChange)
        Q_EMIT statusChanged();

    // ---- tool tip: (icon name, icon pixmaps, title, description)
    QString toolTipTitle, toolTipText;
    const QVariant toolTip = properties.value(QStringLiteral("ToolTip"));
    if (toolTip.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument argument = toolTip.value<QDBusArgument>();
        if (argument.currentSignature() == QLatin1String("(sa(iiay)ss)")) {
            QString unusedIcon, rawTitle, rawText;
            argument.beginStructure();
            argument >> unusedIcon;
            argument.beginArray();      // the tool tip's own pixmaps are not shown
            argument.endArray();
            argument >> rawTitle >> rawText;
            argument.endStructure();
            toolTipTitle = TrayUtil::cleanText(rawTitle.left(MaxTitleLength * 4), MaxTitleLength);
            toolTipText = TrayUtil::plainText(rawText.left(MaxToolTipLength * 8), MaxToolTipLength, MaxToolTipLines);
        }
    }
    if (toolTipTitle != m_toolTipTitle || toolTipText != m_toolTipText) {
        m_toolTipTitle = toolTipTitle;
        m_toolTipText = toolTipText;
        Q_EMIT toolTipChanged();
    }

    // ---- menu
    QString menuPath;
    const QVariant menu = properties.value(QStringLiteral("Menu"));
    if (menu.userType() == qMetaTypeId<QDBusObjectPath>())
        menuPath = menu.value<QDBusObjectPath>().path();
    else if (menu.typeId() == QMetaType::QString)
        menuPath = menu.toString();
    // "/NO_DBUSMENU" is how some toolkits say that they have none.
    if (!TrayUtil::isValidObjectPath(menuPath) || menuPath == QLatin1String("/")
        || menuPath == QLatin1String("/NO_DBUSMENU"))
        menuPath.clear();
    const QVariant isMenu = properties.value(QStringLiteral("ItemIsMenu"));
    const bool itemIsMenu = isMenu.typeId() == QMetaType::Bool && isMenu.toBool();
    if (menuPath != m_menuPath || itemIsMenu != m_itemIsMenu) {
        if (menuPath != m_menuPath && m_menu) {
            m_menu->deleteLater();
            m_menu = nullptr;
        }
        m_menuPath = menuPath;
        m_itemIsMenu = itemIsMenu;
        Q_EMIT menuChanged();
    }
}

void TrayItem::handleSignal(const QString &member, const QVariantList &arguments)
{
    if (member == QLatin1String("NewStatus") && !arguments.isEmpty()
        && arguments.constFirst().typeId() == QMetaType::QString) {
        // The new status comes with the signal; nothing needs to be read.
        const QString status = validStatus(arguments.constFirst().toString());
        if (status != m_status) {
            m_status = status;
            if (!m_attentionIcon.isEmpty()) {
                ++m_iconGeneration;
                m_baseImages.clear();
                Q_EMIT iconChanged();
            }
            Q_EMIT statusChanged();
        }
        return;
    }
    refresh();
}

// ---- what QML shows ----------------------------------------------------------------------

QString TrayItem::iconSource() const
{
    return QStringLiteral("image://%1/item/%2/%3").arg(TrayImageProvider::scheme()).arg(m_serial).arg(m_iconGeneration);
}

QString TrayItem::toolTipTitle() const
{
    if (!m_toolTipTitle.isEmpty())
        return m_toolTipTitle;
    return m_title.isEmpty() ? m_id : m_title;
}

TrayMenu *TrayItem::menu()
{
    if (m_menuPath.isEmpty() || m_owner.isEmpty())
        return nullptr;
    if (!m_menu)
        m_menu = new TrayMenu(m_host, this, m_owner, m_menuPath);
    return m_menu;
}

QImage TrayItem::baseImage(int side) const
{
    const auto cached = m_baseImages.constFind(side);
    if (cached != m_baseImages.constEnd())
        return *cached;

    QImage image;
    if (needsAttention() && !m_attentionIcon.isEmpty())
        image = m_attentionIcon.render(side);
    if (image.isNull())
        image = m_icon.render(side);
    if (!image.isNull() && !m_overlayIcon.isEmpty()) {
        // The overlay icon: a small emblem in the lower right corner.
        const int emblem = qMax(8, side / 2);
        const QImage overlay = m_overlayIcon.render(emblem);
        if (!overlay.isNull()) {
            QPainter painter(&image);
            painter.drawImage(side - emblem, side - emblem, overlay);
        }
    }
    if (m_baseImages.size() >= 8)
        m_baseImages.clear();
    m_baseImages.insert(side, image);
    return image;
}

QImage TrayItem::image(int side, const QColor &foreground, const QColor &background) const
{
    QImage image = baseImage(side);
    TrayIcons::ensureLegible(image, foreground, background);
    return image;
}

// ---- calls to the item -----------------------------------------------------------------------

void TrayItem::send(const QString &method, const QVariantList &arguments)
{
    if (m_owner.isEmpty())
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(m_owner, m_path, kItemInterface, method);
    call.setArguments(arguments);
    QDBusConnection::sessionBus().asyncCall(call, TrayLimits::CallTimeoutMs);
}

void TrayItem::provideActivationToken(QWindow *origin, const std::function<void()> &then)
{
    const QPointer<TrayItem> self(this);
    m_host->requestToken(origin, m_id, [self, then](const QString &token) {
        if (!self)
            return;
        if (!token.isEmpty())
            self->send(QStringLiteral("ProvideXdgActivationToken"), {token});
        then();
    });
}

void TrayItem::positionCall(const QString &method, int x, int y, QWindow *origin, bool reportUnsupported)
{
    if (m_owner.isEmpty())
        return;
    provideActivationToken(origin, [this, method, x, y, reportUnsupported] {
        QDBusMessage call = QDBusMessage::createMethodCall(m_owner, m_path, kItemInterface, method);
        call << x << y;
        const QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(call, TrayLimits::CallTimeoutMs);
        if (!reportUnsupported)
            return;
        auto *watcher = new QDBusPendingCallWatcher(pending, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *finished) {
            finished->deleteLater();
            if (!finished->isError())
                return;
            const QDBusError::ErrorType type = finished->error().type();
            // An item that does not implement the call; one that merely
            // takes long has probably done what was asked.
            if (type != QDBusError::NoReply && type != QDBusError::Timeout && type != QDBusError::TimedOut)
                Q_EMIT activateUnsupported();
        });
    });
}

void TrayItem::activate(int x, int y, QWindow *origin)
{
    positionCall(QStringLiteral("Activate"), x, y, origin, true);
}

void TrayItem::secondaryActivate(int x, int y, QWindow *origin)
{
    positionCall(QStringLiteral("SecondaryActivate"), x, y, origin, false);
}

void TrayItem::contextMenu(int x, int y, QWindow *origin)
{
    positionCall(QStringLiteral("ContextMenu"), x, y, origin, false);
}

void TrayItem::scroll(int deltaX, int deltaY)
{
    // Whole wheel notches are passed on; a touchpad's small movements add up.
    const auto pass = [this](int &accumulated, int delta, const char *orientation) {
        accumulated = qBound(-12000, accumulated + delta, 12000);
        const int notches = accumulated / 120;
        if (notches == 0)
            return;
        accumulated -= notches * 120;
        send(QStringLiteral("Scroll"), {notches * 120, QString::fromLatin1(orientation)});
    };
    pass(m_scrollY, deltaY, "vertical");
    pass(m_scrollX, deltaX, "horizontal");
}

// ---- diagnostics -----------------------------------------------------------------------------

QJsonObject TrayItem::toJson() const
{
    const auto pixmapSizes = [](const TrayIconSource &source) {
        QJsonArray sizes;
        for (const QImage &pixmap : source.pixmaps)
            sizes << QStringLiteral("%1x%2").arg(pixmap.width()).arg(pixmap.height());
        return sizes;
    };
    QJsonObject o{
        {QStringLiteral("serial"), m_serial},
        {QStringLiteral("service"), m_service},
        {QStringLiteral("owner"), m_owner},
        {QStringLiteral("path"), m_path},
        {QStringLiteral("ready"), m_ready},
        {QStringLiteral("id"), m_id},
        {QStringLiteral("title"), m_title},
        {QStringLiteral("category"), m_category},
        {QStringLiteral("status"), m_status},
        {QStringLiteral("shown"), shown()},
        {QStringLiteral("iconName"), m_icon.name},
        {QStringLiteral("iconThemePath"), m_iconThemePath},
        {QStringLiteral("iconPixmaps"), pixmapSizes(m_icon)},
        {QStringLiteral("attentionIconName"), m_attentionIcon.name},
        {QStringLiteral("attentionIconPixmaps"), pixmapSizes(m_attentionIcon)},
        {QStringLiteral("overlayIconName"), m_overlayIcon.name},
        {QStringLiteral("overlayIconPixmaps"), pixmapSizes(m_overlayIcon)},
        // whether the process that wrote this found a picture for the icon
        // (a diagnostic run may use another icon theme than the shell)
        {QStringLiteral("iconFound"), m_hasIcon},
        {QStringLiteral("toolTipTitle"), toolTipTitle()},
        {QStringLiteral("toolTipText"), m_toolTipText},
        {QStringLiteral("menuPath"), m_menuPath},
        {QStringLiteral("itemIsMenu"), m_itemIsMenu},
        {QStringLiteral("readsPropertiesSingly"), m_singly},
    };
    if (m_menu) {
        o.insert(QStringLiteral("menuState"), m_menu->failure().isEmpty()
                     ? (m_menu->loaded() ? QStringLiteral("loaded") : QStringLiteral("not-read"))
                     : m_menu->failure());
        if (m_menu->loaded())
            o.insert(QStringLiteral("menu"), m_menu->toJson());
    }
    return o;
}
