// SPDX-License-Identifier: GPL-3.0-or-later
#include "powerinhibitors.h"
#include "powerdbus.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusServiceWatcher>
#include <QJsonArray>

QDBusArgument &operator<<(QDBusArgument &argument, const PowerModule::InhibitionInfo &info)
{
    argument.beginStructure();
    argument << info.application << info.reason;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PowerModule::InhibitionInfo &info)
{
    argument.beginStructure();
    argument >> info.application >> info.reason;
    argument.endStructure();
    return argument;
}

namespace PowerModule {

namespace {
const QString kAgentService = QStringLiteral("org.kde.Solid.PowerManagement.PolicyAgent");
const QString kAgentPath = QStringLiteral("/org/kde/Solid/PowerManagement/PolicyAgent");
const QString kLogin1 = QStringLiteral("org.freedesktop.login1");
const QString kLogin1Path = QStringLiteral("/org/freedesktop/login1");
const QString kLogin1Manager = QStringLiteral("org.freedesktop.login1.Manager");
} // namespace

Inhibitors::Inhibitors(bool serve, QObject *parent)
    : QObject(parent), m_system(QDBusConnection::systemBus())
{
    qDBusRegisterMetaType<InhibitionInfo>();
    qDBusRegisterMetaType<QList<InhibitionInfo>>();

    if (serve) {
        QDBusConnection session = QDBusConnection::sessionBus();
        m_callers = new QDBusServiceWatcher(this);
        m_callers->setConnection(session);
        m_callers->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
        connect(m_callers, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &caller) {
            const QList<Entry> entries = m_entries;
            for (const Entry &entry : entries) {
                if (entry.caller == caller)
                    release(entry.cookie);
            }
        });
        auto *adaptor = new PolicyAgentAdaptor(this);
        connect(this, &Inhibitors::sessionInhibitionsChanged, adaptor, &PolicyAgentAdaptor::InhibitionsChanged);
        if (!session.isConnected()) {
            m_serviceError = QStringLiteral("The session bus is not reachable.");
        } else if (!session.registerObject(kAgentPath, this)) {
            m_serviceError = QStringLiteral("The object %1 could not be registered.").arg(kAgentPath);
        } else if (!session.registerService(kAgentService)) {
            m_serviceError = QStringLiteral("The name %1 is owned by another power manager.").arg(kAgentService);
            session.unregisterObject(kAgentPath);
        } else {
            m_serving = true;
        }
        if (!m_serving)
            qWarning("horizon-shell power: application idle inhibitors are not visible: %s", qPrintable(m_serviceError));
    }

    if (!m_system.isConnected()) {
        m_logindError = QStringLiteral("The system bus is not reachable.");
        m_ready = true;
        return;
    }
    m_system.connect(kLogin1, kLogin1Path, QStringLiteral("org.freedesktop.DBus.Properties"),
                     QStringLiteral("PropertiesChanged"), this,
                     SLOT(logindPropertiesChanged(QString,QVariantMap,QStringList)));
    auto *watcher = new QDBusServiceWatcher(kLogin1, m_system, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { refreshLogind(); });
    refreshLogind();
}

void Inhibitors::logindPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated)
{
    if (interface != kLogin1Manager)
        return;
    if (changed.contains(QStringLiteral("BlockInhibited")) || invalidated.contains(QStringLiteral("BlockInhibited")))
        refreshLogind();
}

void Inhibitors::refreshLogind(const std::function<void()> &then)
{
    if (!m_system.isConnected()) {
        if (then)
            then();
        return;
    }
    const quint64 generation = ++m_generation;
    dbusCall(m_system, dbusGet(kLogin1, kLogin1Path, kLogin1Manager, QStringLiteral("BlockInhibited")), this,
             [this, generation, then](const QDBusMessage &reply) {
        if (generation != m_generation) {
            // A newer query is under way; its answer is the one that counts,
            // but whoever asked must still be told once it is safe to decide.
            if (then)
                refreshLogind(then);
            return;
        }
        const bool ok = reply.type() == QDBusMessage::ReplyMessage;
        const QStringList blocked = ok ? dbusUnwrap(reply.arguments().value(0)).toString().split(QLatin1Char(':'), Qt::SkipEmptyParts)
                                       : QStringList();
        const QString error = ok ? QString() : reply.errorMessage();
        // The list is only for display; the decision uses BlockInhibited.
        dbusCall(m_system, dbusMethod(kLogin1, kLogin1Path, kLogin1Manager, QStringLiteral("ListInhibitors")), this,
                 [this, generation, ok, blocked, error, then](const QDBusMessage &listReply) {
            if (generation == m_generation) {
                QList<LogindLock> locks;
                if (listReply.type() == QDBusMessage::ReplyMessage && !listReply.arguments().isEmpty()) {
                    const QDBusArgument argument = listReply.arguments().constFirst().value<QDBusArgument>();
                    argument.beginArray();
                    while (!argument.atEnd()) {
                        LogindLock lock;
                        argument.beginStructure();
                        argument >> lock.what >> lock.who >> lock.why >> lock.mode >> lock.uid >> lock.pid;
                        argument.endStructure();
                        locks.append(lock);
                    }
                    argument.endArray();
                }
                const bool wasIdle = blocksIdle(), wasSuspend = blocksSuspend();
                m_logindAvailable = ok;
                m_logindError = error;
                m_blockInhibited = blocked;
                m_logindLocks = locks;
                if (!m_ready) {
                    m_ready = true;
                    Q_EMIT readyChanged();
                }
                if (wasIdle != blocksIdle() || wasSuspend != blocksSuspend())
                    Q_EMIT changed();
            }
            if (then)
                then();
        });
    });
}

bool Inhibitors::blocksIdle() const
{
    return has(ChangeScreenSettings) || m_blockInhibited.contains(QLatin1String("idle"));
}

bool Inhibitors::blocksSuspend() const
{
    return has(ChangeScreenSettings | InterruptSession) || m_blockInhibited.contains(QLatin1String("idle"))
           || m_blockInhibited.contains(QLatin1String("sleep"));
}

QString Inhibitors::blocker(bool suspend) const
{
    const uint mask = suspend ? (ChangeScreenSettings | InterruptSession) : uint(ChangeScreenSettings);
    for (const Entry &entry : m_entries) {
        if (entry.types & mask)
            return QStringLiteral("application \"%1\" (%2)").arg(entry.application, entry.reason);
    }
    for (const QString &what : {QStringLiteral("idle"), QStringLiteral("sleep")}) {
        if (!m_blockInhibited.contains(what) || (what == QLatin1String("sleep") && !suspend))
            continue;
        for (const LogindLock &lock : m_logindLocks) {
            if (lock.mode == QLatin1String("block") && lock.what.split(QLatin1Char(':')).contains(what))
                return QStringLiteral("logind %1 inhibitor of \"%2\" (%3)").arg(what, lock.who, lock.why);
        }
        return QStringLiteral("logind %1 inhibitor").arg(what);
    }
    return {};
}

uint Inhibitors::add(uint types, const QString &application, const QString &reason, const QString &caller)
{
    const bool wasIdle = blocksIdle(), wasSuspend = blocksSuspend();
    if (++m_lastCookie == 0)
        ++m_lastCookie;
    m_entries.append({m_lastCookie, types, application, reason, caller});
    if (m_callers && !caller.isEmpty())
        m_callers->addWatchedService(caller);
    Q_EMIT sessionInhibitionsChanged({InhibitionInfo{application, reason}}, {});
    if (wasIdle != blocksIdle() || wasSuspend != blocksSuspend())
        Q_EMIT changed();
    return m_lastCookie;
}

void Inhibitors::release(uint cookie)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).cookie != cookie)
            continue;
        const bool wasIdle = blocksIdle(), wasSuspend = blocksSuspend();
        const Entry entry = m_entries.takeAt(i);
        bool callerHasMore = false;
        for (const Entry &other : std::as_const(m_entries))
            callerHasMore = callerHasMore || other.caller == entry.caller;
        if (m_callers && !callerHasMore && !entry.caller.isEmpty())
            m_callers->removeWatchedService(entry.caller);
        Q_EMIT sessionInhibitionsChanged({}, {entry.application});
        if (wasIdle != blocksIdle() || wasSuspend != blocksSuspend())
            Q_EMIT changed();
        return;
    }
}

QList<InhibitionInfo> Inhibitors::list() const
{
    QList<InhibitionInfo> out;
    for (const Entry &entry : m_entries)
        out.append({entry.application, entry.reason});
    return out;
}

bool Inhibitors::has(uint types) const
{
    for (const Entry &entry : m_entries) {
        if (entry.types & types)
            return true;
    }
    return false;
}

QJsonObject Inhibitors::toJson() const
{
    QJsonArray applications;
    for (const Entry &entry : m_entries) {
        QJsonArray types;
        if (entry.types & ChangeScreenSettings) types.append(QStringLiteral("screen"));
        if (entry.types & (ChangeScreenSettings | InterruptSession)) types.append(QStringLiteral("suspend"));
        applications.append(QJsonObject{{QStringLiteral("cookie"), qint64(entry.cookie)},
                                        {QStringLiteral("application"), entry.application},
                                        {QStringLiteral("reason"), entry.reason},
                                        {QStringLiteral("holdsBack"), types}});
    }
    QJsonArray locks;
    for (const LogindLock &lock : m_logindLocks) {
        locks.append(QJsonObject{{QStringLiteral("what"), lock.what}, {QStringLiteral("who"), lock.who},
                                 {QStringLiteral("why"), lock.why}, {QStringLiteral("mode"), lock.mode},
                                 {QStringLiteral("uid"), qint64(lock.uid)}, {QStringLiteral("pid"), qint64(lock.pid)}});
    }
    QJsonObject logind{{QStringLiteral("available"), m_logindAvailable},
                       {QStringLiteral("blockInhibited"), QJsonArray::fromStringList(m_blockInhibited)},
                       {QStringLiteral("inhibitors"), locks}};
    if (!m_logindAvailable)
        logind.insert(QStringLiteral("unavailableReason"), m_logindError);
    QJsonObject o{{QStringLiteral("logind"), logind},
                  {QStringLiteral("idleHeldBackBy"), blocker(false)},
                  {QStringLiteral("suspendHeldBackBy"), blocker(true)}};
    if (m_callers) {
        QJsonObject agent{{QStringLiteral("serving"), m_serving}, {QStringLiteral("inhibitions"), applications}};
        if (!m_serving)
            agent.insert(QStringLiteral("unavailableReason"), m_serviceError);
        o.insert(QStringLiteral("applications"), agent);
    }
    return o;
}

// ---- org.kde.Solid.PowerManagement.PolicyAgent ------------------------------

PolicyAgentAdaptor::PolicyAgentAdaptor(Inhibitors *inhibitors)
    : QDBusAbstractAdaptor(inhibitors), m_inhibitors(inhibitors)
{
}

uint PolicyAgentAdaptor::AddInhibition(uint types, const QString &app_name, const QString &reason, const QDBusMessage &message)
{
    return m_inhibitors->add(types, app_name, reason, message.service());
}

void PolicyAgentAdaptor::ReleaseInhibition(uint cookie)
{
    m_inhibitors->release(cookie);
}

QList<InhibitionInfo> PolicyAgentAdaptor::ListInhibitions()
{
    return m_inhibitors->list();
}

bool PolicyAgentAdaptor::HasInhibition(uint types)
{
    return m_inhibitors->has(types);
}

} // namespace PowerModule
