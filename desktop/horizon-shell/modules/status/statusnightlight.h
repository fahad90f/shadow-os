// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

namespace StatusModule {

class DBusProperties;

// Night Light: the compositor shifting the screen to warmer colours.
//
// State: KWin's Night Light plugin on the session bus — org.kde.KWin,
// /org/kde/KWin/NightLight, interface org.kde.KWin.NightLight (properties
// available, enabled, running, inhibited, currentTemperature, mode),
// followed through PropertiesChanged. What is shown is always what KWin
// reports, never what was last written.
//
// Switch: Night Light has no "enable" method; it is a setting of KWin. The
// switch writes ~/.config/kwinrc [NightColor] Active through KConfig with
// its change notification, which KWin's own configuration watcher reacts
// to — the same mechanism the Control Center's display page uses. When
// Night Light is switched on while its schedule is "automatic" (sunrise and
// sunset from a location service, which SHADOW does not run) the schedule is
// changed to fixed times, like the Control Center does.
//
//   enabled   Night Light is switched on in KWin's settings
//   running   the screen is being tinted right now (enabled, and night)
class NightLightBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool inhibited READ inhibited NOTIFY changed)
    Q_PROPERTY(int currentTemperature READ currentTemperature NOTIFY changed)
    // "automatic" | "location" | "times" | "constant"
    Q_PROPERTY(QString mode READ mode NOTIFY changed)

public:
    explicit NightLightBackend(QObject *parent = nullptr);

    bool enabled() const { return m_enabled; }
    bool running() const { return m_running; }
    bool inhibited() const { return m_inhibited; }
    int currentTemperature() const { return m_temperature; }
    QString mode() const { return m_mode; }

    Q_INVOKABLE void setEnabled(bool enabled);

Q_SIGNALS:
    void changed();

protected:
    QJsonObject details() const override;

private:
    void recompute();

    DBusProperties *m_kwin = nullptr;
    bool m_enabled = false;
    bool m_running = false;
    bool m_inhibited = false;
    int m_temperature = 0;
    QString m_mode;
};

} // namespace StatusModule
