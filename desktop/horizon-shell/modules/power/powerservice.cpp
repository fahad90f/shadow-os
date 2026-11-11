// SPDX-License-Identifier: GPL-3.0-or-later
#include "powerservice.h"
#include "powerconfig.h"
#include "poweridle.h"
#include "powerinhibitors.h"
#include "powernotifier.h"
#include "powerscreen.h"
#include "powerupdates.h"
#include "session.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace PowerModule {

namespace {
const QString kBatteryKey = QStringLiteral("battery");
}

Service::Service(Session *session, QObject *parent)
    : QObject(parent), m_session(session)
{
    m_config = new Config(this);
    m_source = new Source(this);
    m_inhibitors = new Inhibitors(session != nullptr, this);
    connect(m_source, &Source::readyChanged, this, &Service::readyChanged);
    connect(m_inhibitors, &Inhibitors::readyChanged, this, &Service::readyChanged);
    if (!session)
        return;

    m_screens = new Screens(this);
    m_notifier = new Notifier(this);
    m_idle = new IdleController(m_config, m_source, m_inhibitors, m_screens, session, this);
    m_updates = new UpdateChecker(m_config, m_notifier, this);
    connect(m_updates, &UpdateChecker::openUpdaterRequested, this, [this] {
        m_session->run(QStringLiteral("nexus-updater"));
    });
    connect(m_source, &Source::batteryChanged, this, &Service::batteryChanged);
    connect(m_source, &Source::readyChanged, this, &Service::batteryChanged);

    // Anything that changes what toJson() says.
    for (QObject *object : {static_cast<QObject *>(m_idle), static_cast<QObject *>(m_inhibitors),
                            static_cast<QObject *>(m_screens), static_cast<QObject *>(m_updates)})
        connect(object, SIGNAL(changed()), this, SIGNAL(changed()));
    connect(m_source, &Source::batteryChanged, this, &Service::changed);
    connect(m_source, &Source::readyChanged, this, &Service::changed);
    connect(m_inhibitors, &Inhibitors::sessionInhibitionsChanged, this, &Service::changed);
    connect(m_config, &Config::idleChanged, this, &Service::changed);

    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(100);
    connect(&m_stateTimer, &QTimer::timeout, this, &Service::writeState);
    connect(this, &Service::changed, &m_stateTimer, qOverload<>(&QTimer::start));
    connect(qApp, &QCoreApplication::aboutToQuit, this, [] { QFile::remove(stateFile()); });
    m_stateTimer.start();
    batteryChanged();
}

bool Service::onBattery() const { return m_source->onBattery(); }
QString Service::batteryWarning() const { return Source::warningName(m_source->warningLevel()); }
bool Service::idleInhibited() const { return m_inhibitors->blocksIdle(); }
bool Service::userIdle() const { return m_idle && m_idle->idle(); }
bool Service::ready() const { return m_source->ready() && m_inhibitors->ready(); }

QString Service::stateFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/nexus-horizon-power.json");
}

// Low battery. The levels and the action are UPower's (UPower.conf:
// PercentageLow/Critical/Action, CriticalPowerAction); UPower performs the
// action itself — here it is only announced.
void Service::batteryChanged()
{
    const Source::Warning level = m_source->warningLevel();
    if (level < Source::WarningLow) {
        if (m_notifiedLevel >= Source::WarningLow)
            m_notifier->close(kBatteryKey);
        m_notifiedLevel = Source::WarningNone;
        return;
    }
    if (level == m_notifiedLevel)
        return;             // one notice per level, not one per percent
    m_notifiedLevel = level;

    const QString action = m_source->criticalAction();
    Notifier::Message message;
    message.key = kBatteryKey;
    message.appName = tr("Power");
    message.icon = QStringLiteral("battery-caution");
    const QString remaining = tr("%1% remaining.").arg(m_source->percentage());
    if (level == Source::WarningLow) {
        message.urgency = Notifier::Normal;
        message.summary = tr("Battery low");
        message.body = remaining + QLatin1Char(' ') + tr("Connect the charger soon.");
    } else if (level == Source::WarningCritical) {
        message.urgency = Notifier::Critical;
        message.resident = true;
        message.summary = tr("Battery critically low");
        QString what;
        if (action == QLatin1String("PowerOff"))
            what = tr("If it is not connected to power, the computer will shut down when the battery is almost empty.");
        else if (action == QLatin1String("Hibernate"))
            what = tr("If it is not connected to power, the computer will hibernate when the battery is almost empty.");
        else if (action == QLatin1String("HybridSleep"))
            what = tr("If it is not connected to power, the computer will suspend (with a hibernation image) when the battery is almost empty.");
        else if (action == QLatin1String("Suspend"))
            what = tr("If it is not connected to power, the computer will suspend when the battery is almost empty.");
        else if (action == QLatin1String("Ignore"))
            what = tr("No automatic action is configured: connect the charger or save your work now.");
        else
            what = tr("Connect the charger or save your work now.");
        message.body = remaining + QLatin1Char(' ') + what;
    } else {
        message.urgency = Notifier::Critical;
        message.resident = true;
        message.summary = tr("Battery empty");
        if (action == QLatin1String("PowerOff"))
            message.body = tr("The computer is shutting down now.");
        else if (action == QLatin1String("Hibernate"))
            message.body = tr("The computer is hibernating now.");
        else if (action == QLatin1String("HybridSleep") || action == QLatin1String("Suspend"))
            message.body = tr("The computer is suspending now.");
        else
            message.body = tr("The battery is empty and no automatic action is configured. Save your work now.");
    }
    m_notifier->send(message);
}

void Service::writeState()
{
    QJsonObject state = toJson();
    state.insert(QStringLiteral("pid"), qint64(QCoreApplication::applicationPid()));
    state.insert(QStringLiteral("writtenAt"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    QSaveFile file(stateFile());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
    file.commit();
}

QJsonObject Service::toJson() const
{
    const IdleMinutes effective = IdleController::effective(m_config, m_source);
    QJsonObject o{
        {QStringLiteral("settings"), m_config->toJson()},
        {QStringLiteral("powerSource"), m_source->toJson()},
        {QStringLiteral("effective"), QJsonObject{
            {QStringLiteral("source"), m_source->onBattery() ? QStringLiteral("battery") : QStringLiteral("ac")},
            {QStringLiteral("screenOffMinutes"), effective.screenOff},
            {QStringLiteral("lockMinutes"), effective.lock},
            {QStringLiteral("suspendMinutes"), effective.suspend},
        }},
        {QStringLiteral("inhibitors"), m_inhibitors->toJson()},
    };
    if (m_session) {
        o.insert(QStringLiteral("idle"), m_idle->toJson());
        o.insert(QStringLiteral("screens"), m_screens->toJson());
        o.insert(QStringLiteral("updates"), m_updates->toJson());
        o.insert(QStringLiteral("notifications"), m_notifier->toJson());
        o.insert(QStringLiteral("batteryNotice"), Source::warningName(m_notifiedLevel));
        o.insert(QStringLiteral("canLock"), m_session->canLock());
        o.insert(QStringLiteral("canSuspend"), m_session->canSuspend());
    } else {
        o.insert(QStringLiteral("updates"), QJsonObject{
            {QStringLiteral("settingsFile"), m_config->updaterFile()},
            {QStringLiteral("autoCheck"), m_config->updatesAutoCheck()},
            {QStringLiteral("checkIntervalHours"), m_config->updatesIntervalHours()},
            {QStringLiteral("updaterInstalled"), !UpdateChecker::updaterProgram().isEmpty()},
        });
    }
    return o;
}

} // namespace PowerModule
