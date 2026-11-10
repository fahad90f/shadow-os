// SPDX-License-Identifier: GPL-3.0-or-later
#include "workspaces.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>

namespace {
const QString kService = QStringLiteral("org.kde.KWin");
const QString kPath = QStringLiteral("/VirtualDesktopManager");
const QString kIface = QStringLiteral("org.kde.KWin.VirtualDesktopManager");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");

struct DesktopData { int position; QString id; QString name; };

const QDBusArgument &operator>>(const QDBusArgument &arg, DesktopData &d)
{
    arg.beginStructure();
    arg >> d.position >> d.id >> d.name;
    arg.endStructure();
    return arg;
}
} // namespace

Workspaces::Workspaces(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    auto *watcher = new QDBusServiceWatcher(kService, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &Workspaces::refresh);
    for (const char *sig : {"desktopCreated", "desktopRemoved", "desktopDataChanged", "countChanged"})
        bus.connect(kService, kPath, kIface, QString::fromLatin1(sig), this, SLOT(refresh()));
    bus.connect(kService, kPath, kIface, QStringLiteral("currentChanged"), this, SLOT(onCurrentChanged(QString)));
    refresh();
}

void Workspaces::refresh()
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kProps, QStringLiteral("GetAll"));
    call << kIface;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QVariantMap> reply = *w;
        const bool ok = !reply.isError();
        QVariantList list;
        QString current;
        if (ok) {
            const QVariantMap props = reply.value();
            current = props.value(QStringLiteral("current")).toString();
            const QDBusArgument arg = props.value(QStringLiteral("desktops")).value<QDBusArgument>();
            arg.beginArray();
            while (!arg.atEnd()) {
                DesktopData d;
                arg >> d;
                list << QVariantMap{{QStringLiteral("id"), d.id}, {QStringLiteral("name"), d.name},
                                    {QStringLiteral("position"), d.position}};
            }
            arg.endArray();
        }
        if (ok != m_available) { m_available = ok; Q_EMIT availableChanged(); }
        if (list != m_list) { m_list = list; Q_EMIT listChanged(); }
        if (current != m_current) { m_current = current; Q_EMIT currentChanged(); }
    });
}

void Workspaces::onCurrentChanged(const QString &id)
{
    if (id != m_current) { m_current = id; Q_EMIT currentChanged(); }
}

int Workspaces::currentIndex() const
{
    for (int i = 0; i < m_list.size(); ++i) {
        if (m_list.at(i).toMap().value(QStringLiteral("id")).toString() == m_current)
            return i;
    }
    return -1;
}

void Workspaces::switchTo(const QString &id)
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kProps, QStringLiteral("Set"));
    call << kIface << QStringLiteral("current") << QVariant::fromValue(QDBusVariant(id));
    QDBusConnection::sessionBus().asyncCall(call);
}

void Workspaces::switchToIndex(int index)
{
    if (index >= 0 && index < m_list.size())
        switchTo(m_list.at(index).toMap().value(QStringLiteral("id")).toString());
}

void Workspaces::next()
{
    if (!m_list.isEmpty())
        switchToIndex((currentIndex() + 1) % int(m_list.size()));
}

void Workspaces::previous()
{
    if (!m_list.isEmpty())
        switchToIndex((currentIndex() - 1 + int(m_list.size())) % int(m_list.size()));
}

void Workspaces::add()
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("createDesktop"));
    call << uint(m_list.size()) << tr("Workspace %1").arg(m_list.size() + 1);
    QDBusConnection::sessionBus().asyncCall(call);
}

void Workspaces::remove(const QString &id)
{
    if (m_list.size() <= 1)
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("removeDesktop"));
    call << id;
    QDBusConnection::sessionBus().asyncCall(call);
}
