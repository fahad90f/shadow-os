// SPDX-License-Identifier: GPL-3.0-or-later
#include "mousebackend.h"

#include "dumpregistry.h"
#include "networkcommon.h"

#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QJsonArray>
#include <QRegularExpression>

#include <memory>

using namespace NetHw;

NEXUS_CC_DUMP("mouse", [] {
    MouseBackend backend;
    waitUntilReady(&backend);
    return QJsonValue(backend.toJson());
})

namespace {
const QString kService = QStringLiteral("org.kde.KWin");
const QString kManagerPath = QStringLiteral("/org/kde/KWin/InputDevice");
const QString kManagerInterface = QStringLiteral("org.kde.KWin.InputDeviceManager");
const QString kDeviceInterface = QStringLiteral("org.kde.KWin.InputDevice");

const QStringList kBoolProperties = {
    QStringLiteral("enabled"),
    QStringLiteral("leftHanded"),
    QStringLiteral("middleEmulation"),
    QStringLiteral("naturalScroll"),
    QStringLiteral("pointerAccelerationProfileFlat"),
    QStringLiteral("pointerAccelerationProfileAdaptive"),
    QStringLiteral("tapToClick"),
    QStringLiteral("tapAndDrag"),
    QStringLiteral("tapDragLock"),
    QStringLiteral("lmrTapButtonMap"),
    QStringLiteral("disableWhileTyping"),
    QStringLiteral("disableEventsOnExternalMouse"),
    QStringLiteral("scrollTwoFinger"),
    QStringLiteral("scrollEdge"),
    QStringLiteral("scrollOnButtonDown"),
    QStringLiteral("clickMethodAreas"),
    QStringLiteral("clickMethodClickfinger"),
};

bool validSysName(const QString &sysName)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_]{1,64}$"));
    return pattern.match(sysName).hasMatch();
}
} // namespace

MouseBackend::MouseBackend(QObject *parent)
    : QObject(parent)
{
    m_model = new RowModel({QStringLiteral("sysName"), QStringLiteral("name"), QStringLiteral("touchpad"),
                            QStringLiteral("properties")},
                           QStringLiteral("sysName"), this);

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        auto *watcher = new QDBusServiceWatcher(kService, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &MouseBackend::refresh);
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, &MouseBackend::refresh);
        bus.connect(kService, kManagerPath, kManagerInterface, QStringLiteral("deviceAdded"), this, SLOT(refresh()));
        bus.connect(kService, kManagerPath, kManagerInterface, QStringLiteral("deviceRemoved"), this, SLOT(refresh()));
    }
    refresh();
}

QAbstractItemModel *MouseBackend::devices() const
{
    return m_model;
}

int MouseBackend::deviceCount() const
{
    return m_model->count();
}

void MouseBackend::setState(const QString &state, const QString &message)
{
    if (state == m_state && message == m_statusMessage)
        return;
    m_state = state;
    m_statusMessage = message;
    Q_EMIT stateChanged();
}

void MouseBackend::finish(const QString &state, const QString &message, quint64 generation)
{
    if (generation != m_generation)
        return;                 // a newer refresh is under way
    if (state != QLatin1String("ok")) {
        m_order.clear();
        m_properties.clear();
    }
    publish();
    setState(state, message);
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
}

void MouseBackend::refresh()
{
    const quint64 generation = ++m_generation;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        // Answer through the event loop so the first result is always asynchronous.
        QTimer::singleShot(0, this, [this, generation] {
            finish(QStringLiteral("no-session-bus"),
                   tr("There is no D-Bus session bus, so the compositor cannot be asked for its input devices."),
                   generation);
        });
        return;
    }

    dbusNameHasOwner(bus, kService, this, [this, bus, generation](bool owned) {
        if (!owned) {
            finish(QStringLiteral("no-kwin"),
                   tr("Pointer devices are configured through KWin, which is not running in this session "
                      "(org.kde.KWin is not on the session bus)."),
                   generation);
            return;
        }
        const QDBusMessage get = dbusMethod(kService, kManagerPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                            QStringLiteral("Get"), {kManagerInterface, QStringLiteral("devicesSysNames")});
        dbusCall(bus, get, this, [this, generation](const QDBusMessage &reply) {
            if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
                finish(QStringLiteral("no-input-api"),
                       tr("KWin is running, but it does not expose input devices (%1). That is the case when KWin "
                          "runs as an X11 window manager or without its libinput backend.").arg(dbusErrorText(reply)),
                       generation);
                return;
            }
            loadDevices(dbusPlain(reply.arguments().constFirst()).toStringList(), generation);
        });
    });
}

void MouseBackend::loadDevices(const QStringList &sysNames, quint64 generation)
{
    QStringList valid;
    for (const QString &sysName : sysNames) {
        if (validSysName(sysName))
            valid.append(sysName);
    }
    if (valid.isEmpty()) {
        finish(QStringLiteral("no-devices"), tr("KWin reports no input devices."), generation);
        return;
    }

    auto pending = std::make_shared<int>(int(valid.size()));
    auto collected = std::make_shared<QMap<QString, QVariantMap>>();
    QDBusConnection bus = QDBusConnection::sessionBus();
    for (const QString &sysName : std::as_const(valid)) {
        dbusGetAll(bus, kService, kManagerPath + QLatin1Char('/') + sysName, kDeviceInterface, this,
                   [this, sysName, valid, pending, collected, generation](bool ok, const QVariantMap &properties, const QString &) {
            if (ok && properties.value(QStringLiteral("pointer")).toBool())
                collected->insert(sysName, dbusPlain(properties).toMap());
            if (--*pending > 0 || generation != m_generation)
                return;
            m_order = valid;
            m_properties = *collected;
            if (m_properties.isEmpty()) {
                finish(QStringLiteral("no-devices"),
                       tr("No mouse, touchpad or other pointer device is connected."), generation);
            } else {
                finish(QStringLiteral("ok"), QString(), generation);
            }
        });
    }
}

void MouseBackend::publish()
{
    // Mice first, then touchpads; within each group in KWin's order.
    QVariantList rows;
    for (const bool touchpads : {false, true}) {
        for (const QString &sysName : std::as_const(m_order)) {
            const auto it = m_properties.constFind(sysName);
            if (it == m_properties.constEnd())
                continue;
            const bool touchpad = it->value(QStringLiteral("touchpad")).toBool();
            if (touchpad != touchpads)
                continue;
            QString name = it->value(QStringLiteral("name")).toString();
            if (name.isEmpty())
                name = sysName;
            rows.append(QVariantMap{
                {QStringLiteral("sysName"), sysName},
                {QStringLiteral("name"), name},
                {QStringLiteral("touchpad"), touchpad},
                {QStringLiteral("properties"), *it},
            });
        }
    }
    const int before = m_model->count();
    m_model->setRows(rows);
    if (before != m_model->count())
        Q_EMIT devicesChanged();
}

void MouseBackend::reloadDevice(const QString &sysName)
{
    const quint64 generation = m_generation;
    dbusGetAll(QDBusConnection::sessionBus(), kService, kManagerPath + QLatin1Char('/') + sysName, kDeviceInterface, this,
               [this, sysName, generation](bool ok, const QVariantMap &properties, const QString &) {
        if (generation != m_generation)
            return;
        if (!ok) {
            refresh();          // the device is gone
            return;
        }
        m_properties.insert(sysName, dbusPlain(properties).toMap());
        publish();
    });
}

void MouseBackend::write(const QString &sysName, const QString &property, const QVariant &value)
{
    if (!m_properties.contains(sysName)) {
        Q_EMIT failed(tr("The device is no longer connected."));
        return;
    }
    dbusSet(QDBusConnection::sessionBus(), kService, kManagerPath + QLatin1Char('/') + sysName, kDeviceInterface,
            property, value, this, [this, sysName](bool ok, const QString &error) {
        if (!ok)
            Q_EMIT failed(tr("KWin refused the change: %1").arg(error));
        reloadDevice(sysName);      // show what KWin really applied
    });
}

void MouseBackend::setBool(const QString &sysName, const QString &property, bool value)
{
    if (!kBoolProperties.contains(property)) {
        Q_EMIT failed(tr("“%1” is not a setting this page can change.").arg(property));
        return;
    }
    write(sysName, property, value);
}

void MouseBackend::setReal(const QString &sysName, const QString &property, double value)
{
    if (property == QLatin1String("pointerAcceleration")) {
        value = qBound(-1.0, value, 1.0);
    } else if (property == QLatin1String("scrollFactor")) {
        value = qBound(0.1, value, 10.0);
    } else {
        Q_EMIT failed(tr("“%1” is not a setting this page can change.").arg(property));
        return;
    }
    write(sysName, property, value);
}

QJsonObject MouseBackend::toJson() const
{
    QJsonArray devices;
    const QVariantList rows = m_model->rows();
    for (const QVariant &row : rows) {
        const QVariantMap map = row.toMap();
        devices.append(QJsonObject{
            {QStringLiteral("sysName"), map.value(QStringLiteral("sysName")).toString()},
            {QStringLiteral("name"), map.value(QStringLiteral("name")).toString()},
            {QStringLiteral("touchpad"), map.value(QStringLiteral("touchpad")).toBool()},
            {QStringLiteral("properties"), QJsonObject::fromVariantMap(map.value(QStringLiteral("properties")).toMap())},
        });
    }
    return {
        {QStringLiteral("state"), m_state},
        {QStringLiteral("available"), available()},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("devices"), devices},
    };
}
