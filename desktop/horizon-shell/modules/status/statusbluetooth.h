// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

namespace BluezQt {
class Manager;
}

namespace StatusModule {

// Bluetooth on/off and the number of connected devices.
//
// Source: BlueZ (org.bluez on the system bus) through BluezQt, which mirrors
// BlueZ's object tree and reports every change as a signal.
//
//   enabled          at least one adapter is powered
//   blocked          the Bluetooth radio is blocked by rfkill (airplane mode
//                    or a hardware switch)
//   connectedCount   devices with an active connection; connectedName is the
//                    name of the first of them
//
// "Available" means the BlueZ service runs and the computer has an adapter.
// Switching on lifts a soft rfkill block first (BluezQt writes /dev/rfkill)
// and then powers every adapter; switching off powers them all down.
class BluetoothBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool blocked READ blocked NOTIFY changed)
    Q_PROPERTY(int connectedCount READ connectedCount NOTIFY changed)
    Q_PROPERTY(QString connectedName READ connectedName NOTIFY changed)

public:
    explicit BluetoothBackend(QObject *parent = nullptr);

    bool enabled() const { return m_enabled; }
    bool blocked() const { return m_blocked; }
    int connectedCount() const { return m_connectedCount; }
    QString connectedName() const { return m_connectedName; }

    Q_INVOKABLE void setEnabled(bool enabled);

Q_SIGNALS:
    void changed();

protected:
    QJsonObject details() const override;

private:
    void update();
    void powerAdapters(bool on);

    BluezQt::Manager *m_manager = nullptr;
    bool m_initialized = false;
    QString m_initError;
    bool m_enabled = false;
    bool m_blocked = false;
    int m_connectedCount = 0;
    QString m_connectedName;
};

} // namespace StatusModule
