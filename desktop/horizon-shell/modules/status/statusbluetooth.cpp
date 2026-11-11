// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusbluetooth.h"

#include <BluezQt/Adapter>
#include <BluezQt/Device>
#include <BluezQt/InitManagerJob>
#include <BluezQt/Manager>
#include <BluezQt/PendingCall>
#include <BluezQt/Rfkill>

#include <QDBusConnection>
#include <QJsonArray>
#include <QTimer>

namespace StatusModule {

BluetoothBackend::BluetoothBackend(QObject *parent)
    : Backend(parent)
{
    m_manager = new BluezQt::Manager(this);
    connect(m_manager, &BluezQt::Manager::operationalChanged, this, &BluetoothBackend::update);
    connect(m_manager, &BluezQt::Manager::bluetoothOperationalChanged, this, &BluetoothBackend::update);
    connect(m_manager, &BluezQt::Manager::bluetoothBlockedChanged, this, &BluetoothBackend::update);
    connect(m_manager, &BluezQt::Manager::adapterAdded, this, &BluetoothBackend::update);
    connect(m_manager, &BluezQt::Manager::adapterRemoved, this, &BluetoothBackend::update);
    connect(m_manager, &BluezQt::Manager::adapterChanged, this, &BluetoothBackend::update);
    connect(m_manager, &BluezQt::Manager::deviceAdded, this, &BluetoothBackend::update);
    connect(m_manager, &BluezQt::Manager::deviceRemoved, this, &BluetoothBackend::update);
    connect(m_manager, &BluezQt::Manager::deviceChanged, this, &BluetoothBackend::update);

    if (!QDBusConnection::systemBus().isConnected()) {
        // BluezQt's start-up never finishes without a bus to ask.
        m_initialized = true;
        m_initError = tr("The system bus is not available.");
        QTimer::singleShot(0, this, &BluetoothBackend::update);
        return;
    }
    // BlueZ answering slowly must not leave the state "unknown" for ever;
    // the answer is still used when it arrives.
    QTimer::singleShot(6000, this, [this] {
        if (!m_initialized)
            setAvailable(false, tr("The Bluetooth service (BlueZ) has not answered yet."));
    });

    BluezQt::InitManagerJob *job = m_manager->init();
    connect(job, &BluezQt::InitManagerJob::result, this, [this](BluezQt::InitManagerJob *finished) {
        m_initialized = true;
        if (finished->error())
            m_initError = tr("BlueZ cannot be reached: %1").arg(finished->errorText());
        update();
    });
    job->start();
}

void BluetoothBackend::update()
{
    if (!m_initialized)
        return;

    bool enabled = false;
    int connected = 0;
    QString connectedName;
    const QList<BluezQt::AdapterPtr> adapters = m_manager->adapters();
    for (const BluezQt::AdapterPtr &adapter : adapters)
        enabled = enabled || adapter->isPowered();
    const QList<BluezQt::DevicePtr> devices = m_manager->devices();
    for (const BluezQt::DevicePtr &device : devices) {
        if (!device->isConnected())
            continue;
        if (connected++ == 0)
            connectedName = device->name();
    }
    const bool blocked = m_manager->isBluetoothBlocked();

    if (enabled != m_enabled || blocked != m_blocked || connected != m_connectedCount
        || connectedName != m_connectedName) {
        m_enabled = enabled;
        m_blocked = blocked;
        m_connectedCount = connected;
        m_connectedName = connectedName;
        Q_EMIT changed();
    }

    if (!m_initError.isEmpty())
        setAvailable(false, m_initError);
    else if (!m_manager->isOperational())
        setAvailable(false, tr("The Bluetooth service (BlueZ) is not running."));
    else if (adapters.isEmpty())
        setAvailable(false, tr("This computer has no Bluetooth adapter."));
    else
        setAvailable(true);
}

void BluetoothBackend::setEnabled(bool enabled)
{
    if (!isAvailable())
        return;
    if (enabled && m_manager->isBluetoothBlocked()) {
        if (m_manager->rfkill()->state() == BluezQt::Rfkill::HardBlocked) {
            Q_EMIT failed(tr("Bluetooth is switched off by a hardware switch."));
            return;
        }
        // Blocked by software (airplane mode): lift the block, then give
        // BlueZ a moment to see it before the adapters are powered.
        m_manager->setBluetoothBlocked(false);
        QTimer::singleShot(400, this, [this] { powerAdapters(true); });
        return;
    }
    powerAdapters(enabled);
}

void BluetoothBackend::powerAdapters(bool on)
{
    const QList<BluezQt::AdapterPtr> adapters = m_manager->adapters();
    for (const BluezQt::AdapterPtr &adapter : adapters) {
        if (adapter->isPowered() == on)
            continue;
        BluezQt::PendingCall *call = adapter->setPowered(on);
        connect(call, &BluezQt::PendingCall::finished, this, [this, on](BluezQt::PendingCall *finished) {
            if (!finished->error())
                return;
            Q_EMIT failed(on ? tr("Bluetooth could not be switched on: %1").arg(finished->errorText())
                             : tr("Bluetooth could not be switched off: %1").arg(finished->errorText()));
        });
    }
}

QJsonObject BluetoothBackend::details() const
{
    QJsonObject json;
    if (!m_initialized)
        return json;
    json.insert(QStringLiteral("serviceRunning"), m_manager->isOperational());
    QJsonArray adapterList;
    const QList<BluezQt::AdapterPtr> adapters = m_manager->adapters();
    for (const BluezQt::AdapterPtr &adapter : adapters) {
        adapterList.append(QJsonObject{
            {QStringLiteral("name"), adapter->name()},
            {QStringLiteral("powered"), adapter->isPowered()},
        });
    }
    json.insert(QStringLiteral("adapters"), adapterList);
    json.insert(QStringLiteral("enabled"), m_enabled);
    json.insert(QStringLiteral("blocked"), m_blocked);
    json.insert(QStringLiteral("connectedCount"), m_connectedCount);
    QJsonArray connectedList;
    const QList<BluezQt::DevicePtr> devices = m_manager->devices();
    for (const BluezQt::DevicePtr &device : devices) {
        if (device->isConnected())
            connectedList.append(device->name());
    }
    json.insert(QStringLiteral("connectedDevices"), connectedList);
    return json;
}

} // namespace StatusModule
