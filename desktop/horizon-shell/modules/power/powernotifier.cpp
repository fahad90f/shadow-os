// SPDX-License-Identifier: GPL-3.0-or-later
#include "powernotifier.h"
#include "powerdbus.h"

namespace PowerModule {

namespace {
const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");
} // namespace

Notifier::Notifier(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kService, QStringLiteral("ActionInvoked"), this, SLOT(onActionInvoked(uint,QString)));
    bus.connect(kService, kPath, kService, QStringLiteral("NotificationClosed"), this, SLOT(onClosed(uint,uint)));
}

void Notifier::send(const Message &message)
{
    QVariantMap hints{{QStringLiteral("urgency"), QVariant::fromValue(uchar(message.urgency))}};
    if (!message.desktopEntry.isEmpty())
        hints.insert(QStringLiteral("desktop-entry"), message.desktopEntry);
    if (message.resident)
        hints.insert(QStringLiteral("resident"), true);

    const quint64 serial = ++m_serial;
    Shown &shown = m_shown[message.key];
    const uint replaces = shown.id;
    shown.serial = serial;

    QDBusMessage call = dbusMethod(kService, kPath, kService, QStringLiteral("Notify"));
    call << message.appName << replaces << message.icon << message.summary << message.body << message.actions
         << hints << (message.resident ? 0 : -1);
    const QString key = message.key;
    m_lastSummary = message.summary;
    ++m_sent;
    dbusCall(QDBusConnection::sessionBus(), call, this, [this, key, serial](const QDBusMessage &reply) {
        if (reply.type() != QDBusMessage::ReplyMessage) {
            m_lastError = reply.errorMessage();
            qWarning("horizon-shell power: the notification could not be shown: %s", qPrintable(m_lastError));
            if (m_shown.value(key).serial == serial)
                m_shown.remove(key);
            return;
        }
        m_lastError.clear();
        const uint id = reply.arguments().value(0).toUInt();
        auto it = m_shown.find(key);
        if (it != m_shown.end() && it->serial == serial) {
            it->id = id;
        } else {
            // close(key) was called before the daemon answered.
            QDBusMessage closeCall = dbusMethod(kService, kPath, kService, QStringLiteral("CloseNotification"));
            closeCall << id;
            QDBusConnection::sessionBus().asyncCall(closeCall);
        }
    });
}

void Notifier::close(const QString &key)
{
    const Shown shown = m_shown.take(key);
    if (shown.id == 0)
        return;
    QDBusMessage call = dbusMethod(kService, kPath, kService, QStringLiteral("CloseNotification"));
    call << shown.id;
    QDBusConnection::sessionBus().asyncCall(call);
}

void Notifier::onActionInvoked(uint id, const QString &action)
{
    for (auto it = m_shown.constBegin(); it != m_shown.constEnd(); ++it) {
        if (it->id == id) {
            Q_EMIT actionInvoked(it.key(), action);
            return;
        }
    }
}

void Notifier::onClosed(uint id, uint)
{
    for (auto it = m_shown.begin(); it != m_shown.end(); ++it) {
        if (it->id == id) {
            it->id = 0;         // the next send() of this key is a new notification
            return;
        }
    }
}

QJsonObject Notifier::toJson() const
{
    QJsonObject o{{QStringLiteral("sent"), m_sent}, {QStringLiteral("lastSummary"), m_lastSummary}};
    if (!m_lastError.isEmpty())
        o.insert(QStringLiteral("lastError"), m_lastError);
    return o;
}

} // namespace PowerModule
