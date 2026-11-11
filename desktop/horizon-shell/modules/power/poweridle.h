// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "powerconfig.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

class Session;

namespace PowerModule {

class Inhibitors;
class Screens;
class Source;

// The three idle actions: screens off, lock, suspend.
//
// Idleness comes from KIdleTime, which on Wayland asks the compositor
// (ext-idle-notify-v1): one idle timeout per action that is switched on, with
// the minutes of power.conf for the current power source. The compositor
// does not report idleness while a window holds a Wayland idle inhibitor.
//
// When a timeout arrives
//   screens off  the outputs are switched off through KWin's DPMS protocol
//   lock         Session::lock() → org.freedesktop.ScreenSaver.Lock
//   suspend      Session::suspend() → logind Suspend. The screen locker
//                itself locks on logind's PrepareForSleep when
//                kscreenlockerrc [Daemon] LockOnResume is true, so the
//                module does not lock a second time.
// unless an inhibitor holds the action back (see Inhibitors). An action that
// was held back is not lost: when its inhibitor goes away while the user is
// still idle, all timeouts start again from that moment.
//
// Timeouts also start again when the settings or the power source change and
// when the computer wakes up from suspend.
class IdleController : public QObject
{
    Q_OBJECT

public:
    enum Stage { ScreenOff = 0, Lock = 1, Suspend = 2, StageCount = 3 };

    IdleController(Config *config, Source *source, Inhibitors *inhibitors, Screens *screens, Session *session,
                   QObject *parent = nullptr);
    ~IdleController() override;

    static IdleMinutes effective(const Config *config, const Source *source);
    bool idle() const { return m_idle; }

    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void timeoutReached(int id, int ms);
    void resumed();
    void prepareForSleep(bool start);

private:
    struct StageState {
        int minutes = 0;
        qint64 timeoutMs = 0;
        int id = -1;
        QString state = QStringLiteral("off");  // off | armed | done | held | failed
        QString detail;
    };

    void arm(const QString &why);
    void fire(Stage stage);
    void act(Stage stage);
    void set(Stage stage, const QString &state, const QString &detail);
    void inhibitorsChanged();
    static const char *name(Stage stage);

    Config *m_config;
    Source *m_source;
    Inhibitors *m_inhibitors;
    Screens *m_screens;
    Session *m_session;
    StageState m_stages[StageCount];
    IdleMinutes m_armedFor;
    bool m_armed = false;
    bool m_idle = false;
    bool m_switchedScreensOff = false;
    quint64 m_generation = 0;
    QDateTime m_armedAt;
    QString m_armedWhy;
    QJsonArray m_events;
};

} // namespace PowerModule
