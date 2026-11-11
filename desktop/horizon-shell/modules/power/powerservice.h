// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "powersource.h"

#include <QJsonObject>
#include <QObject>
#include <QTimer>

class Session;

namespace PowerModule {

class Config;
class IdleController;
class Inhibitors;
class Notifier;
class Screens;
class UpdateChecker;

// The "power" shell module: idle actions, low-battery notices and update
// notices. Exposed to the shell's QML as the context property "power".
//
// With a Session it is the working service of the running shell. Without
// one (horizon-shell --dump power) it only reads: settings, power source and
// logind's inhibitors — no idle timeouts, no bus name, no notifications.
class Service : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool onBattery READ onBattery NOTIFY changed)
    // "none" | "low" | "critical" | "action" (and "unknown" without a battery)
    Q_PROPERTY(QString batteryWarning READ batteryWarning NOTIFY changed)
    // Something keeps the screens on (an application or a logind inhibitor).
    Q_PROPERTY(bool idleInhibited READ idleInhibited NOTIFY changed)
    Q_PROPERTY(bool userIdle READ userIdle NOTIFY changed)

public:
    explicit Service(Session *session, QObject *parent = nullptr);

    bool onBattery() const;
    QString batteryWarning() const;
    bool idleInhibited() const;
    bool userIdle() const;

    // Source and logind have answered.
    bool ready() const;
    QJsonObject toJson() const;

    // Where the running shell keeps a copy of toJson() for --dump power.
    static QString stateFile();

Q_SIGNALS:
    void changed();
    void readyChanged();

private:
    void batteryChanged();
    void writeState();

    Session *m_session;
    Config *m_config;
    Source *m_source;
    Inhibitors *m_inhibitors;
    Screens *m_screens = nullptr;
    Notifier *m_notifier = nullptr;
    IdleController *m_idle = nullptr;
    UpdateChecker *m_updates = nullptr;
    Source::Warning m_notifiedLevel = Source::WarningNone;
    QTimer m_stateTimer;
};

} // namespace PowerModule
