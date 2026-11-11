// SPDX-License-Identifier: GPL-3.0-or-later
#include "poweridle.h"
#include "powerinhibitors.h"
#include "powerscreen.h"
#include "powersource.h"
#include "session.h"

#include <KIdleTime>

#include <QDBusConnection>

namespace PowerModule {

IdleController::IdleController(Config *config, Source *source, Inhibitors *inhibitors, Screens *screens,
                               Session *session, QObject *parent)
    : QObject(parent), m_config(config), m_source(source), m_inhibitors(inhibitors), m_screens(screens), m_session(session)
{
    KIdleTime *idleTime = KIdleTime::instance();
    connect(idleTime, &KIdleTime::timeoutReached, this, &IdleController::timeoutReached);
    connect(idleTime, &KIdleTime::resumingFromIdle, this, &IdleController::resumed);

    connect(m_config, &Config::idleChanged, this, [this] { arm(QStringLiteral("settings changed")); });
    const auto sourceChanged = [this] {
        if (!m_armed || effective(m_config, m_source) != m_armedFor)
            arm(QStringLiteral("power source changed"));
    };
    connect(m_source, &Source::onBatteryChanged, this, sourceChanged);
    connect(m_source, &Source::readyChanged, this, sourceChanged);
    connect(m_inhibitors, &Inhibitors::changed, this, &IdleController::inhibitorsChanged);

    QDBusConnection system = QDBusConnection::systemBus();
    if (system.isConnected()) {
        system.connect(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
                       QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("PrepareForSleep"),
                       this, SLOT(prepareForSleep(bool)));
    }
    arm(QStringLiteral("start"));
}

IdleController::~IdleController()
{
    KIdleTime::instance()->removeAllIdleTimeouts();
}

const char *IdleController::name(Stage stage)
{
    static const char *const names[] = {"screenOff", "lock", "suspend"};
    return names[stage];
}

IdleMinutes IdleController::effective(const Config *config, const Source *source)
{
    return source->onBattery() ? config->onBattery() : config->onAc();
}

void IdleController::arm(const QString &why)
{
    ++m_generation;
    KIdleTime *idleTime = KIdleTime::instance();
    idleTime->removeAllIdleTimeouts();
    m_armedFor = effective(m_config, m_source);
    m_armed = true;
    m_armedAt = QDateTime::currentDateTime();
    m_armedWhy = why;
    const int minutes[StageCount] = {m_armedFor.screenOff, m_armedFor.lock, m_armedFor.suspend};
    for (int i = 0; i < StageCount; ++i) {
        StageState &stage = m_stages[i];
        stage = StageState();
        stage.minutes = minutes[i];
        if (stage.minutes <= 0)
            continue;
        stage.timeoutMs = Config::scaled(qint64(stage.minutes) * 60 * 1000);
        stage.id = idleTime->addIdleTimeout(std::chrono::milliseconds(stage.timeoutMs));
        stage.state = QStringLiteral("armed");
    }
    Q_EMIT changed();
}

void IdleController::set(Stage stage, const QString &state, const QString &detail)
{
    m_stages[stage].state = state;
    m_stages[stage].detail = detail;
    m_events.append(QJsonObject{{QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                                {QStringLiteral("action"), QLatin1String(name(stage))},
                                {QStringLiteral("result"), state},
                                {QStringLiteral("detail"), detail}});
    while (m_events.size() > 20)
        m_events.removeFirst();
    Q_EMIT changed();
}

void IdleController::timeoutReached(int id, int)
{
    for (int i = 0; i < StageCount; ++i) {
        if (m_stages[i].id == id && m_stages[i].state == QLatin1String("armed")) {
            fire(Stage(i));
            return;
        }
    }
}

void IdleController::fire(Stage stage)
{
    if (!m_idle) {
        m_idle = true;
        KIdleTime::instance()->catchNextResumeEvent();
    }
    if (stage != Suspend) {
        if (m_inhibitors->blocksIdle())
            set(stage, QStringLiteral("held"), m_inhibitors->blocker(false));
        else
            act(stage);
        return;
    }
    // Suspending is the one action worth a fresh look at logind's inhibitor
    // locks rather than trusting the last change notification.
    const quint64 generation = m_generation;
    m_inhibitors->refreshLogind([this, generation] {
        if (generation != m_generation || !m_idle || m_stages[Suspend].state != QLatin1String("armed"))
            return;         // the user came back, or the timeouts were restarted, meanwhile
        if (m_inhibitors->blocksSuspend())
            set(Suspend, QStringLiteral("held"), m_inhibitors->blocker(true));
        else
            act(Suspend);
    });
}

void IdleController::act(Stage stage)
{
    switch (stage) {
    case ScreenOff:
        if (m_screens->setOff(true)) {
            m_switchedScreensOff = true;
            set(stage, QStringLiteral("done"), QStringLiteral("outputs switched off (org_kde_kwin_dpms)"));
        } else {
            set(stage, QStringLiteral("failed"), m_screens->toJson().value(QLatin1String("unavailableReason")).toString());
        }
        break;
    case Lock:
        if (m_session->canLock()) {
            m_session->lock();
            set(stage, QStringLiteral("done"), QStringLiteral("org.freedesktop.ScreenSaver.Lock"));
        } else {
            set(stage, QStringLiteral("failed"), QStringLiteral("This session has no screen locker (org.freedesktop.ScreenSaver)."));
        }
        break;
    case Suspend:
        if (m_session->canSuspend()) {
            m_session->suspend();
            set(stage, QStringLiteral("done"), QStringLiteral("org.freedesktop.login1.Manager.Suspend"));
        } else {
            set(stage, QStringLiteral("failed"), QStringLiteral("logind does not offer suspend on this computer (CanSuspend)."));
        }
        break;
    case StageCount:
        break;
    }
}

void IdleController::resumed()
{
    if (!m_idle)
        return;
    m_idle = false;
    ++m_generation;
    if (m_switchedScreensOff) {
        // KWin wakes the outputs on input by itself; this covers a resume
        // that was not input to the compositor's seat.
        m_switchedScreensOff = false;
        m_screens->setOff(false);
    }
    for (StageState &stage : m_stages) {
        if (stage.id >= 0) {
            stage.state = QStringLiteral("armed");
            stage.detail.clear();
        }
    }
    Q_EMIT changed();
}

void IdleController::inhibitorsChanged()
{
    if (m_idle) {
        const bool idleFree = !m_inhibitors->blocksIdle(), suspendFree = !m_inhibitors->blocksSuspend();
        const bool release = (idleFree && (m_stages[ScreenOff].state == QLatin1String("held") || m_stages[Lock].state == QLatin1String("held")))
                             || (suspendFree && m_stages[Suspend].state == QLatin1String("held"));
        if (release) {
            // Count the idle time again from the moment the inhibitor went away.
            arm(QStringLiteral("inhibitor released"));
            return;
        }
    }
    Q_EMIT changed();
}

void IdleController::prepareForSleep(bool start)
{
    if (start)
        return;
    // Awake again: a suspended computer was not "idle" in the compositor's
    // clock, and nothing guarantees an input event follows the wake-up.
    m_idle = false;
    if (m_switchedScreensOff) {
        m_switchedScreensOff = false;
        m_screens->setOff(false);
    }
    arm(QStringLiteral("woke up from suspend"));
}

QJsonObject IdleController::toJson() const
{
    QJsonObject stages;
    for (int i = 0; i < StageCount; ++i) {
        const StageState &stage = m_stages[i];
        QJsonObject o{{QStringLiteral("minutes"), stage.minutes}, {QStringLiteral("state"), stage.state}};
        if (stage.id >= 0)
            o.insert(QStringLiteral("timeoutSeconds"), double(stage.timeoutMs) / 1000.0);
        if (!stage.detail.isEmpty())
            o.insert(QStringLiteral("detail"), stage.detail);
        stages.insert(QLatin1String(name(Stage(i))), o);
    }
    return QJsonObject{
        {QStringLiteral("userIdle"), m_idle},
        {QStringLiteral("armedAt"), m_armedAt.toString(Qt::ISODateWithMs)},
        {QStringLiteral("armedBecause"), m_armedWhy},
        {QStringLiteral("timers"), stages},
        {QStringLiteral("events"), m_events},
    };
}

} // namespace PowerModule
