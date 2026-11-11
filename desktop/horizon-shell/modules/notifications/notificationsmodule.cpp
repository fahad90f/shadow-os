// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "notifications" shell module: the session's notification service
// (org.freedesktop.Notifications), popups, the notification centre and its
// panel button. See README.md.
#include "module.h"
#include "notificationimage.h"
#include "notificationserver.h"

#include <shadowapp.h>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

static void init(Horizon::ShellContext &ctx)
{
    auto *server = new NotificationServer(ctx.applications, ctx.parent());
    ctx.app->engine().addImageProvider(NotificationImageProvider::scheme(), new NotificationImageProvider(server));
    ctx.expose(QStringLiteral("notifications"), server);
    ctx.addPanelItem(QStringLiteral("NotificationButton.qml"), 40);
    ctx.addSurface(QStringLiteral("NotificationPopups.qml"));
    ctx.addSurface(QStringLiteral("NotificationCenter.qml"));
    // Asynchronous: asks for the bus name and never waits for an answer here.
    server->start();
}
HORIZON_SHELL_MODULE(notifications, init)

// horizon-shell --dump notifications
//
// Runs in a process of its own, next to the shell (if one is running): it
// reports the configuration, what this implementation offers, who owns the
// service in the session and — when that is a Shadow Horizon shell — the
// state the shell reports about itself.
static QJsonObject dumpNotifications()
{
    const QString name = QStringLiteral("org.freedesktop.Notifications");
    const QString path = QStringLiteral("/org/freedesktop/Notifications");

    QJsonObject service{{QStringLiteral("busAvailable"), false}, {QStringLiteral("owner"), QString()},
                        {QStringLiteral("serverInformation"), QJsonValue()}, {QStringLiteral("isHorizon"), false}};
    QJsonValue state;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        service[QStringLiteral("busAvailable")] = true;
        // GetNameOwner rather than a call to the name: that would start a
        // D-Bus-activated notification daemon just to ask it who it is.
        QDBusMessage ask = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("/org/freedesktop/DBus"),
                                                          QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("GetNameOwner"));
        ask << name;
        const QDBusMessage owner = bus.call(ask, QDBus::Block, 2000);
        if (owner.type() == QDBusMessage::ReplyMessage && !owner.arguments().isEmpty()) {
            const QString unique = owner.arguments().constFirst().toString();
            service[QStringLiteral("owner")] = unique;
            const QDBusMessage info = bus.call(QDBusMessage::createMethodCall(unique, path, name,
                                                                              QStringLiteral("GetServerInformation")),
                                               QDBus::Block, 2000);
            if (info.type() == QDBusMessage::ReplyMessage && info.arguments().size() == 4) {
                QJsonArray values;
                for (const QVariant &value : info.arguments())
                    values << value.toString();
                service[QStringLiteral("serverInformation")] = values;
                if (info.arguments().constFirst().toString() == QLatin1String("Shadow Horizon")) {
                    service[QStringLiteral("isHorizon")] = true;
                    const QDBusMessage reply = bus.call(
                        QDBusMessage::createMethodCall(unique, path, QStringLiteral("org.shadow.horizon.Notifications"),
                                                       QStringLiteral("GetState")),
                        QDBus::Block, 2000);
                    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
                        state = QJsonDocument::fromJson(reply.arguments().constFirst().toString().toUtf8()).object();
                }
            }
        }
    }

    using namespace NotificationLimits;
    return QJsonObject{
        {QStringLiteral("config"), QJsonObject{
            {QStringLiteral("path"), NotificationServer::configPath()},
            {QStringLiteral("doNotDisturb"), NotificationServer::readDoNotDisturb()},
        }},
        {QStringLiteral("specVersion"), QStringLiteral("1.2")},
        {QStringLiteral("capabilities"), QJsonArray::fromStringList(NotificationServer::capabilities())},
        {QStringLiteral("limits"), QJsonObject{
            {QStringLiteral("visiblePopups"), MaxVisiblePopups},
            {QStringLiteral("queuedPopups"), MaxQueuedPopups},
            {QStringLiteral("history"), MaxHistory},
            {QStringLiteral("historyPerApplication"), MaxHistoryPerApp},
            {QStringLiteral("actions"), MaxActions},
            {QStringLiteral("appNameLength"), NotificationText::MaxAppNameLength},
            {QStringLiteral("summaryLength"), NotificationText::MaxSummaryLength},
            {QStringLiteral("bodyLength"), NotificationText::MaxBodyLength},
            {QStringLiteral("actionLabelLength"), NotificationText::MaxActionLabelLength},
            {QStringLiteral("links"), NotificationText::MaxLinks},
            {QStringLiteral("rawImageSide"), NotificationImage::MaxRawSide},
            {QStringLiteral("imageFileBytes"), NotificationImage::MaxFileBytes},
            {QStringLiteral("keptImageSide"), NotificationImage::KeptSide},
            {QStringLiteral("popupBurstPerClient"), PopupBurst},
            {QStringLiteral("popupRefillMs"), PopupRefillMs},
            {QStringLiteral("minTimeoutMs"), MinTimeoutMs},
            {QStringLiteral("maxTimeoutMs"), MaxTimeoutMs},
        }},
        {QStringLiteral("service"), service},
        // null: no Shadow Horizon shell is the notification service of this session
        {QStringLiteral("state"), state},
    };
}
HORIZON_SHELL_DUMP(notifications, "notifications", dumpNotifications)
