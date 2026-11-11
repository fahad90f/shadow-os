// SPDX-License-Identifier: GPL-3.0-or-later
#include "powersource.h"
#include "powerdbus.h"

#include <QDBusServiceWatcher>

namespace PowerModule {

namespace {
const QString kService = QStringLiteral("org.freedesktop.UPower");
const QString kPath = QStringLiteral("/org/freedesktop/UPower");
const QString kDisplayPath = QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice");
const QString kDevice = QStringLiteral("org.freedesktop.UPower.Device");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
} // namespace

Source::Source(QObject *parent)
    : QObject(parent), m_bus(QDBusConnection::systemBus())
{
    if (!m_bus.isConnected()) {
        gone(QStringLiteral("The system bus is not reachable."));
        return;
    }
    m_bus.connect(kService, kPath, kProperties, QStringLiteral("PropertiesChanged"), this,
                  SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
    m_bus.connect(kService, kDisplayPath, kProperties, QStringLiteral("PropertiesChanged"), this,
                  SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
    auto *watcher = new QDBusServiceWatcher(kService, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &Source::load);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        ++m_generation;
        gone(QStringLiteral("UPower is not running."));
    });
    load();
}

void Source::load()
{
    const quint64 generation = ++m_generation;
    m_pending = 3;
    dbusCall(m_bus, dbusGetAll(kService, kPath, kService), this, [this, generation](const QDBusMessage &reply) {
        if (generation != m_generation)
            return;
        if (reply.type() != QDBusMessage::ReplyMessage) {
            ++m_generation;         // the other answers no longer matter
            gone(QStringLiteral("UPower did not answer: %1").arg(reply.errorMessage()));
            return;
        }
        m_daemon = dbusMap(reply.arguments().value(0));
        m_available = true;
        m_reason.clear();
        stepDone();
    });
    dbusCall(m_bus, dbusGetAll(kService, kDisplayPath, kDevice), this, [this, generation](const QDBusMessage &reply) {
        if (generation != m_generation)
            return;
        if (reply.type() == QDBusMessage::ReplyMessage)
            m_display = dbusMap(reply.arguments().value(0));
        stepDone();
    });
    dbusCall(m_bus, dbusMethod(kService, kPath, kService, QStringLiteral("GetCriticalAction")), this,
             [this, generation](const QDBusMessage &reply) {
        if (generation != m_generation)
            return;
        m_criticalAction = reply.type() == QDBusMessage::ReplyMessage ? reply.arguments().value(0).toString() : QString();
        stepDone();
    });
}

void Source::stepDone()
{
    if (--m_pending > 0)
        return;
    recompute();
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
}

void Source::gone(const QString &reason)
{
    m_daemon.clear();
    m_display.clear();
    m_criticalAction.clear();
    m_available = false;
    m_reason = reason;
    m_pending = 0;
    recompute();
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
}

void Source::propertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated)
{
    if (!m_available || m_pending > 0)
        return;                 // a GetAll is on its way and will have the new values
    if (!invalidated.isEmpty()) {
        load();
        return;
    }
    QVariantMap &target = interface == kDevice ? m_display : m_daemon;
    for (auto it = changed.constBegin(); it != changed.constEnd(); ++it)
        target.insert(it.key(), dbusUnwrap(it.value()));
    recompute();
}

void Source::recompute()
{
    const bool onBattery = m_daemon.value(QStringLiteral("OnBattery")).toBool();
    // Type 2 = battery, 3 = UPS; IsPresent tells whether there is one at all.
    const uint type = m_display.value(QStringLiteral("Type")).toUInt();
    const bool present = (type == 2 || type == 3) && m_display.value(QStringLiteral("IsPresent")).toBool();
    const int percentage = present ? qBound(0, qRound(m_display.value(QStringLiteral("Percentage")).toDouble()), 100) : 0;
    const uint level = m_display.value(QStringLiteral("WarningLevel")).toUInt();
    const Warning warning = present && level <= WarningAction ? Warning(level) : WarningUnknown;

    const bool sourceChanged = onBattery != m_onBattery;
    const bool batteryChange = present != m_present || percentage != m_percentage || warning != m_warning;
    m_onBattery = onBattery;
    m_present = present;
    m_percentage = percentage;
    m_warning = warning;
    if (sourceChanged)
        Q_EMIT onBatteryChanged();
    if (batteryChange || sourceChanged)
        Q_EMIT batteryChanged();
}

QString Source::warningName(Warning level)
{
    switch (level) {
    case WarningNone: return QStringLiteral("none");
    case WarningDischarging: return QStringLiteral("discharging");
    case WarningLow: return QStringLiteral("low");
    case WarningCritical: return QStringLiteral("critical");
    case WarningAction: return QStringLiteral("action");
    case WarningUnknown: break;
    }
    return QStringLiteral("unknown");
}

QJsonObject Source::toJson() const
{
    QJsonObject o{
        {QStringLiteral("upowerAvailable"), m_available},
        {QStringLiteral("onBattery"), onBattery()},
        {QStringLiteral("source"), onBattery() ? QStringLiteral("battery") : QStringLiteral("ac")},
        {QStringLiteral("batteryPresent"), m_present},
    };
    if (!m_available)
        o.insert(QStringLiteral("unavailableReason"), m_reason);
    if (m_present) {
        o.insert(QStringLiteral("percentage"), m_percentage);
        o.insert(QStringLiteral("warningLevel"), warningName(m_warning));
    }
    if (m_available)
        o.insert(QStringLiteral("criticalAction"), m_criticalAction);
    return o;
}

} // namespace PowerModule
