// SPDX-License-Identifier: GPL-3.0-or-later
#include "powerupdates.h"
#include "powerconfig.h"
#include "powerdbus.h"
#include "powernotifier.h"

#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>

namespace PowerModule {

namespace {
const QString kNm = QStringLiteral("org.freedesktop.NetworkManager");
const QString kNmPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString kKey = QStringLiteral("updates");

constexpr qint64 kMinute = 60 * 1000;
constexpr qint64 kAfterLogin = 3 * kMinute;     // first check after the shell started
constexpr qint64 kNetworkRetry = 5 * kMinute;   // while the network is down
constexpr qint64 kCheckTimeout = 30 * kMinute;  // a check that hangs is given up (real time)

// A default route in the kernel's tables (used where NetworkManager is absent).
bool hasDefaultRoute()
{
    QFile v4(QStringLiteral("/proc/net/route"));
    if (v4.open(QIODevice::ReadOnly)) {
        v4.readLine();      // header
        while (true) {
            const QByteArray line = v4.readLine();
            if (line.isEmpty())
                break;
            const QList<QByteArray> fields = line.simplified().split(' ');
            // Iface Destination Gateway Flags ...; RTF_UP = 1
            if (fields.size() > 7 && fields.at(1) == "00000000" && fields.at(7) == "00000000"
                && (fields.at(3).toUInt(nullptr, 16) & 1))
                return true;
        }
    }
    QFile v6(QStringLiteral("/proc/net/ipv6_route"));
    if (v6.open(QIODevice::ReadOnly)) {
        while (true) {
            const QByteArray line = v6.readLine();
            if (line.isEmpty())
                break;
            const QList<QByteArray> fields = line.simplified().split(' ');
            // dest prefixlen src srcprefix nexthop metric refcnt use flags iface
            if (fields.size() > 9 && fields.at(0) == QByteArray(32, '0') && fields.at(1) == "00"
                && fields.at(9) != "lo" && (fields.at(8).toUInt(nullptr, 16) & 1))
                return true;
        }
    }
    return false;
}
} // namespace

UpdateChecker::UpdateChecker(Config *config, Notifier *notifier, QObject *parent)
    : QObject(parent), m_config(config), m_notifier(notifier), m_system(QDBusConnection::systemBus())
{
    m_timer.setSingleShot(true);
    m_retryTimer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &UpdateChecker::due);
    connect(&m_retryTimer, &QTimer::timeout, this, &UpdateChecker::due);
    connect(m_config, &Config::updatesChanged, this, [this] { reschedule(false); });
    connect(m_notifier, &Notifier::actionInvoked, this, [this](const QString &key, const QString &) {
        if (key == kKey) {
            m_notifier->close(kKey);
            Q_EMIT openUpdaterRequested();
        }
    });
    if (m_system.isConnected())
        m_system.connect(kNm, kNmPath, kNm, QStringLiteral("StateChanged"), this, SLOT(networkStateChanged(uint)));
    reschedule(false);
}

QString UpdateChecker::updaterProgram()
{
    return QStandardPaths::findExecutable(QStringLiteral("nexus-updater"));
}

void UpdateChecker::reschedule(bool afterCheck)
{
    m_timer.stop();
    m_retryTimer.stop();
    m_waitingForNetwork = false;
    m_nextCheck = QDateTime();
    if (m_config->updatesAutoCheck()) {
        // The first check comes a few minutes after login; later ones (and a
        // changed interval) count from now.
        const qint64 configured = afterCheck || m_firstDone ? qint64(m_config->updatesIntervalHours()) * 60 * kMinute : kAfterLogin;
        const qint64 wait = Config::scaled(configured);
        m_timer.start(std::chrono::milliseconds(wait));
        m_nextCheck = QDateTime::currentDateTime().addMSecs(wait);
    }
    Q_EMIT changed();
}

void UpdateChecker::due()
{
    if (!m_config->updatesAutoCheck() || m_process)
        return;
    if (updaterProgram().isEmpty()) {
        // Nothing to run and nothing to say; look again after the interval
        // (the Updater may get installed).
        m_firstDone = true;
        m_lastResult = QStringLiteral("not-run");
        m_lastError = QStringLiteral("nexus-updater is not installed.");
        reschedule(true);
        return;
    }
    withNetwork([this](bool up, const QString &how) {
        m_networkHow = how;
        if (!up) {
            m_waitingForNetwork = true;
            const qint64 wait = Config::scaled(kNetworkRetry);
            m_retryTimer.start(std::chrono::milliseconds(wait));
            m_nextCheck = QDateTime::currentDateTime().addMSecs(wait);
            Q_EMIT changed();
            return;
        }
        m_waitingForNetwork = false;
        startCheck();
    });
}

void UpdateChecker::withNetwork(const std::function<void(bool, const QString &)> &then)
{
    if (!m_system.isConnected()) {
        const bool up = hasDefaultRoute();
        then(up, up ? QStringLiteral("default route (no system bus)") : QStringLiteral("no default route (no system bus)"));
        return;
    }
    QDBusMessage get = dbusGet(kNm, kNmPath, kNm, QStringLiteral("State"));
    get.setAutoStartService(false);
    dbusCall(m_system, get, this, [then](const QDBusMessage &reply) {
        if (reply.type() != QDBusMessage::ReplyMessage) {
            const bool up = hasDefaultRoute();
            then(up, up ? QStringLiteral("default route (NetworkManager is not running)")
                        : QStringLiteral("no default route (NetworkManager is not running)"));
            return;
        }
        // NM_STATE_CONNECTED_SITE = 60, NM_STATE_CONNECTED_GLOBAL = 70
        const uint state = dbusUnwrap(reply.arguments().value(0)).toUInt();
        then(state >= 60, QStringLiteral("NetworkManager state %1").arg(state));
    }, 5000);
}

void UpdateChecker::networkStateChanged(uint state)
{
    if (m_waitingForNetwork && state >= 60 && !m_process) {
        m_retryTimer.stop();
        due();
    }
}

void UpdateChecker::startCheck()
{
    if (m_process)
        return;
    m_process = new QProcess(this);
    m_process->setProgram(updaterProgram());
    m_process->setArguments({QStringLiteral("--check")});
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_process->setStandardErrorFile(QProcess::nullDevice());
    m_process->setStandardInputFile(QProcess::nullDevice());
    auto *watchdog = new QTimer(m_process);
    watchdog->setSingleShot(true);
    connect(watchdog, &QTimer::timeout, m_process, &QProcess::kill);
    watchdog->start(std::chrono::milliseconds(kCheckTimeout));
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        QProcess *process = m_process;
        m_process = nullptr;
        process->deleteLater();
        finished(exitCode, status != QProcess::NormalExit, process->readAllStandardOutput(), QString());
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_process)
            return;
        QProcess *process = m_process;
        m_process = nullptr;
        process->deleteLater();
        finished(-1, false, QByteArray(), process->errorString());
    });
    m_nextCheck = QDateTime();
    Q_EMIT changed();
    m_process->start();
}

void UpdateChecker::finished(int exitCode, bool crashed, const QByteArray &output, const QString &startError)
{
    const QJsonObject result = QJsonDocument::fromJson(output).object();
    m_firstDone = true;
    ++m_runs;
    m_lastRun = QDateTime::currentDateTime();
    m_lastExitCode = crashed ? -1 : exitCode;
    m_lastUpdates = result.value(QLatin1String("updates")).toInt();
    m_lastSecurity = result.value(QLatin1String("securityUpdates")).toInt();
    m_lastError.clear();

    if (!startError.isEmpty() || crashed || (exitCode != 0 && exitCode != 10 && exitCode != 11)) {
        m_lastResult = QStringLiteral("failed");
        m_lastError = !startError.isEmpty() ? startError
                      : crashed ? QStringLiteral("nexus-updater --check was killed or crashed.")
                      : result.value(QLatin1String("error")).toString(QStringLiteral("nexus-updater --check exited with status %1.").arg(exitCode));
        qWarning("horizon-shell power: update check failed: %s", qPrintable(m_lastError));
    } else if (exitCode == 0) {
        m_lastResult = QStringLiteral("none");
        m_notifier->close(kKey);
    } else {
        const bool security = exitCode == 11;
        m_lastResult = security ? QStringLiteral("security") : QStringLiteral("updates");
        Notifier::Message message;
        message.key = kKey;
        message.appName = tr("Software updates");
        message.icon = QStringLiteral("system-software-update");
        message.desktopEntry = QStringLiteral("org.nexus.updater");
        message.urgency = Notifier::Normal;
        message.summary = security ? tr("Security updates are available") : tr("Updates are available");
        // The counts are the updater's; without them (unexpected output) the
        // exit status alone is reported.
        if (m_lastUpdates > 0) {
            message.body = m_lastUpdates == 1 ? tr("1 update can be installed.")
                                              : tr("%1 updates can be installed.").arg(m_lastUpdates);
            if (m_lastSecurity == 1)
                message.body += QLatin1Char(' ') + tr("1 of them fixes a security problem.");
            else if (m_lastSecurity > 1)
                message.body += QLatin1Char(' ') + tr("%1 of them fix security problems.").arg(m_lastSecurity);
        } else {
            message.body = security ? tr("Open NEXUS Updater to review and install them.")
                                    : tr("Open NEXUS Updater to review them.");
        }
        if (result.value(QLatin1String("restartRequired")).toBool())
            message.body += QLatin1Char(' ') + tr("A restart is pending from an earlier update.");
        message.actions = {QStringLiteral("default"), tr("Open Updater"), QStringLiteral("open"), tr("Open Updater")};
        m_notifier->send(message);
    }
    reschedule(true);
}

QJsonObject UpdateChecker::toJson() const
{
    const QString program = updaterProgram();
    QJsonObject o{
        {QStringLiteral("settingsFile"), m_config->updaterFile()},
        {QStringLiteral("autoCheck"), m_config->updatesAutoCheck()},
        {QStringLiteral("checkIntervalHours"), m_config->updatesIntervalHours()},
        {QStringLiteral("updaterInstalled"), !program.isEmpty()},
        {QStringLiteral("checkRunning"), m_process != nullptr},
        {QStringLiteral("waitingForNetwork"), m_waitingForNetwork},
        {QStringLiteral("checksRun"), m_runs},
    };
    if (!program.isEmpty())
        o.insert(QStringLiteral("updater"), program);
    if (!m_networkHow.isEmpty())
        o.insert(QStringLiteral("network"), m_networkHow);
    if (m_nextCheck.isValid()) {
        o.insert(QStringLiteral("nextCheck"), m_nextCheck.toString(Qt::ISODate));
        o.insert(QStringLiteral("nextCheckInSeconds"), qMax<qint64>(0, QDateTime::currentDateTime().secsTo(m_nextCheck)));
    }
    if (!m_lastResult.isEmpty()) {
        QJsonObject last{{QStringLiteral("result"), m_lastResult}};
        if (m_lastRun.isValid()) {
            last.insert(QStringLiteral("time"), m_lastRun.toString(Qt::ISODate));
            last.insert(QStringLiteral("exitCode"), m_lastExitCode);
            last.insert(QStringLiteral("updates"), m_lastUpdates);
            last.insert(QStringLiteral("securityUpdates"), m_lastSecurity);
        }
        if (!m_lastError.isEmpty())
            last.insert(QStringLiteral("error"), m_lastError);
        o.insert(QStringLiteral("lastCheck"), last);
    }
    return o;
}

} // namespace PowerModule
