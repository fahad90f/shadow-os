// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

#include <QFileSystemWatcher>

namespace StatusModule {

// Brightness of the built-in screen.
//
// Read: /sys/class/backlight/<device>/{brightness,max_brightness,type}.
// When the kernel lists several devices the one closest to the firmware wins
// (type "firmware", then "platform", then "raw"), as the kernel's own
// documentation recommends. The kernel announces changes made by the
// firmware (brightness keys handled by the embedded controller) or by other
// programs on these files, which are watched; nothing is polled.
//
// Write: org.freedesktop.login1.Session.SetBrightness("backlight", device,
// value) on the caller's own session — logind writes the value for the user
// of the session, so no root helper and no udev rule is needed.
//
// "Available" needs a backlight device and a logind session. External
// monitors (DDC/CI) are not covered.
//
//   level   0–1 of the device's maximum
class BrightnessBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(qreal level READ level NOTIFY changed)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY changed)

public:
    explicit BrightnessBackend(QObject *parent = nullptr);

    qreal level() const { return m_max > 0 ? qreal(m_value) / qreal(m_max) : 0; }
    QString deviceName() const { return m_device; }

    Q_INVOKABLE void setLevel(qreal level);
    // Brightness keys: one step of 5 % up (> 0) or down (< 0).
    Q_INVOKABLE void step(int direction);
    // Reads the current value again (and looks for a device if there was none).
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void changed();

protected:
    QJsonObject details() const override;

private:
    void findDevice();
    void probeSession();
    void updateAvailability();
    void readValue();
    void write(int value);
    int lowest() const;

    QString m_root;
    QString m_device;
    QString m_type;
    int m_max = 0;
    int m_value = 0;
    bool m_sessionKnown = false;
    bool m_sessionOk = false;
    QString m_sessionError;
    bool m_inFlight = false;
    int m_pending = -1;
    QFileSystemWatcher m_watcher;
};

} // namespace StatusModule
