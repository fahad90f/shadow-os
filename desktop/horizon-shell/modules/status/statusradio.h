// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

#include <QMap>

class QSocketNotifier;

namespace StatusModule {

// Airplane mode: every radio of the computer switched off at once.
//
// Source: the kernel's rfkill interface, /dev/rfkill (see
// Documentation/driver-api/rfkill.rst). Reading it yields one event per
// radio switch (Wi-Fi, Bluetooth, mobile broadband, …) and every later
// change; writing an RFKILL_OP_CHANGE_ALL event for RFKILL_TYPE_ALL sets
// the soft block of all of them in one step. NetworkManager and BlueZ
// follow rfkill, so their own state (and the Wi-Fi and Bluetooth tiles)
// changes with it, and they restore what was on before when the block is
// lifted.
//
//   airplaneMode   every radio is blocked (by software or a hardware switch)
//
// "Available" needs at least one radio and write access to /dev/rfkill,
// which logind grants to the user of the active local session (udev's
// "uaccess" tag).
class RadioBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(bool airplaneMode READ airplaneMode NOTIFY changed)

public:
    explicit RadioBackend(QObject *parent = nullptr);
    ~RadioBackend() override;

    bool airplaneMode() const { return m_airplaneMode; }

    Q_INVOKABLE void setAirplaneMode(bool on);

Q_SIGNALS:
    void changed();

protected:
    QJsonObject details() const override;

private:
    struct Radio {
        quint8 type = 0;
        bool soft = false;
        bool hard = false;
    };

    void readEvents();
    void evaluate();

    QMap<quint32, Radio> m_radios;
    QSocketNotifier *m_notifier = nullptr;
    int m_fd = -1;
    bool m_writable = false;
    bool m_airplaneMode = false;
};

} // namespace StatusModule
