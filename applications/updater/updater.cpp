// SPDX-License-Identifier: GPL-3.0-or-later
#include "updater.h"

#include <asyncrun.h>
#include <pkhistory.h>
#include <pkservice.h>
#include <pkutil.h>
#include <systemfacts.h>

#include <Daemon>

#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QLocale>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>

#include <limits>
#include <memory>

using ShadowPk::Job;
using ShadowPk::Service;
using T = PackageKit::Transaction;

namespace {

QSettings settings()
{
    // ~/.config/shadow/updater.conf, falling back to /etc/xdg/shadow/updater.conf
    // for distribution defaults.
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("shadow"), QStringLiteral("updater"));
}

int rankOf(const QString &restart)
{
    if (restart == QLatin1String("system")) return 3;
    if (restart == QLatin1String("session")) return 2;
    if (restart == QLatin1String("application")) return 1;
    return 0;
}

} // namespace

Updater::Updater(QObject *parent)
    : QObject(parent)
{
    loadSettings();
    loadLastLog();

    connect(&m_updates, &ShadowPk::UpdateChecker::checked, this, [this] {
        if (m_updates.state() == QLatin1String("ready") || !m_updates.error().isEmpty()) {
            storeCheckResult(m_updates);
            loadSettings();
        }
    });
    // Another program changed the package state (apt in a terminal, the Store).
    connect(Service::instance(), &Service::updatesChanged, this, [this] {
        if (!busy() && !m_updates.busy() && m_updates.state() != QLatin1String("idle"))
            m_updates.check(false);
    });
}

QObject *Updater::service() const
{
    return Service::instance();
}

QString Updater::settingsPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/shadow/updater.conf");
}

QString Updater::lastLogPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
           + QStringLiteral("/shadow/updater/last-transaction.log");
}

// ---------------------------------------------------------------- settings

void Updater::loadSettings()
{
    QSettings s = settings();
    s.sync();
    m_autoCheck = s.value(QStringLiteral("Updates/AutoCheck"), true).toBool();
    m_checkIntervalHours = qBound(1, s.value(QStringLiteral("Updates/CheckIntervalHours"), 24).toInt(), 24 * 30);
    m_lastCheck = QDateTime::fromString(s.value(QStringLiteral("LastCheck/Time")).toString(), Qt::ISODate);
    const QString error = s.value(QStringLiteral("LastCheck/Error")).toString();
    if (!m_lastCheck.isValid()) {
        m_lastCheckResult.clear();
    } else if (!error.isEmpty()) {
        m_lastCheckResult = tr("Failed: %1").arg(error);
    } else {
        const int updates = s.value(QStringLiteral("LastCheck/Updates"), 0).toInt();
        const int security = s.value(QStringLiteral("LastCheck/Security"), 0).toInt();
        m_lastCheckResult = updates == 0 ? tr("No updates")
            : security > 0 ? tr("%1 updates, %2 of them security updates").arg(updates).arg(security)
                           : tr("%1 updates").arg(updates);
    }
    Q_EMIT settingsChanged();
}

void Updater::setAutoCheck(bool on)
{
    if (on == m_autoCheck)
        return;
    m_autoCheck = on;
    QSettings s = settings();
    s.setValue(QStringLiteral("Updates/AutoCheck"), on);
    s.sync();
    Q_EMIT settingsChanged();
}

void Updater::setCheckIntervalHours(int hours)
{
    hours = qBound(1, hours, 24 * 30);
    if (hours == m_checkIntervalHours)
        return;
    m_checkIntervalHours = hours;
    QSettings s = settings();
    s.setValue(QStringLiteral("Updates/CheckIntervalHours"), hours);
    s.sync();
    Q_EMIT settingsChanged();
}

void Updater::storeCheckResult(const ShadowPk::UpdateChecker &checker)
{
    QSettings s = settings();
    s.beginGroup(QStringLiteral("LastCheck"));
    s.setValue(QStringLiteral("Time"), QDateTime::currentDateTime().toString(Qt::ISODate));
    s.setValue(QStringLiteral("Updates"), checker.count());
    s.setValue(QStringLiteral("Security"), checker.securityCount());
    s.setValue(QStringLiteral("Refreshed"), checker.refreshed());
    // The refresh error is kept separate: the list may still be valid (from the last refresh).
    s.setValue(QStringLiteral("Error"), checker.error());
    s.setValue(QStringLiteral("RefreshError"), checker.refreshError());
    s.endGroup();
    s.sync();
}

QJsonObject Updater::settingsJson()
{
    QSettings s = settings();
    s.sync();
    QJsonObject last;
    const QString time = s.value(QStringLiteral("LastCheck/Time")).toString();
    if (!time.isEmpty()) {
        last.insert(QStringLiteral("time"), time);
        last.insert(QStringLiteral("updates"), s.value(QStringLiteral("LastCheck/Updates"), 0).toInt());
        last.insert(QStringLiteral("security"), s.value(QStringLiteral("LastCheck/Security"), 0).toInt());
        last.insert(QStringLiteral("refreshed"), s.value(QStringLiteral("LastCheck/Refreshed"), false).toBool());
        last.insert(QStringLiteral("error"), s.value(QStringLiteral("LastCheck/Error")).toString());
        last.insert(QStringLiteral("refreshError"), s.value(QStringLiteral("LastCheck/RefreshError")).toString());
    }
    return QJsonObject{
        {QStringLiteral("file"), s.fileName()},
        {QStringLiteral("autoCheck"), s.value(QStringLiteral("Updates/AutoCheck"), true).toBool()},
        {QStringLiteral("checkIntervalHours"), qBound(1, s.value(QStringLiteral("Updates/CheckIntervalHours"), 24).toInt(), 24 * 30)},
        {QStringLiteral("lastCheck"), last},
    };
}

// ---------------------------------------------------------------- start-up

void Updater::initialize()
{
    if (m_initialized)
        return;
    m_initialized = true;

    refreshReboot();
    refreshHealth();
    refreshHistory();
    refreshSources();

    Service *service = Service::instance();
    connect(service, &Service::probed, this, [this] {
        Service *s = Service::instance();
        if (!s->available()) {
            // Puts the checker into its "error" state with the exact D-Bus reason.
            m_updates.check(false);
            m_transactionsError = s->error();
            Q_EMIT historyChanged();
            probeLogind();
            return;
        }
        refreshTransactions();
        probeLogind();
        if (m_updates.busy())
            return;
        if (!m_autoCheck) {
            m_updates.check(false);
            return;
        }
        // Automatic checking is on: refresh the lists if PackageKit last did
        // so longer ago than the configured interval, otherwise just read them.
        auto *watcher = new QDBusPendingCallWatcher(PackageKit::Daemon::getTimeSinceAction(T::RoleRefreshCache), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            const QDBusPendingReply<uint> reply = *w;
            const bool stale = reply.isError() || reply.value() == std::numeric_limits<uint>::max()
                               || reply.value() > static_cast<uint>(m_checkIntervalHours) * 3600u;
            if (!m_updates.busy() && !busy())
                m_updates.check(stale);
        });
    });
    service->probe();
}

void Updater::retryService()
{
    Service::instance()->probe();
}

// ---------------------------------------------------------------- updates

void Updater::checkForUpdates()
{
    if (busy())
        return;
    m_updates.check(true);
}

void Updater::installAll()
{
    const QStringList ids = m_updates.installableIds();
    startUpdate(ids, ids.size() == 1 ? tr("Installing 1 update") : tr("Installing all %1 updates").arg(ids.size()));
}

void Updater::installSecurity()
{
    const QStringList ids = m_updates.idsInGroup(QStringLiteral("security"));
    startUpdate(ids, ids.size() == 1 ? tr("Installing 1 security update") : tr("Installing %1 security updates").arg(ids.size()));
}

void Updater::installSelected()
{
    const QStringList ids = m_updates.model()->selectedIds();
    startUpdate(ids, ids.size() == 1 ? tr("Installing 1 selected update") : tr("Installing %1 selected updates").arg(ids.size()));
}

void Updater::startUpdate(const QStringList &ids, const QString &title)
{
    if (ids.isEmpty() || busy())
        return;
    startJob(ShadowPk::Jobs::update(ids, title, this));
}

void Updater::repair()
{
    if (busy())
        return;
    startJob(ShadowPk::Jobs::repair(this));
}

void Updater::startJob(Job *job)
{
    if (m_job)
        m_job->deleteLater();
    m_job = job;
    connect(job, &Job::phaseChanged, this, &Updater::jobChanged);
    connect(job, &Job::finished, this, &Updater::onJobFinished);
    Q_EMIT jobChanged();
    // A changelog download still in flight would keep the transaction waiting.
    m_updates.cancelDetails();
    job->start();
}

void Updater::onJobFinished(bool success)
{
    Job *job = m_job;
    if (!job)
        return;
    saveLastLog(job);

    const int rank = rankOf(job->restart());
    if (rank > 0) {
        m_jobRestartRank = qMax(m_jobRestartRank, rank);
        m_jobRestartSecurity = m_jobRestartSecurity || job->restartForSecurity();
        const QStringList packages = job->restartPackages();
        for (const QString &p : packages) {
            if (!m_jobRestartPackages.contains(p))
                m_jobRestartPackages << p;
        }
    }

    refreshReboot();
    refreshHealth();
    refreshHistory();
    refreshTransactions();
    if (job->kind() != QLatin1String("refresh") && !m_updates.busy())
        m_updates.check(false);
    Q_EMIT jobChanged();
    Q_EMIT jobFinished(success, job->kind());
}

void Updater::dismissJob()
{
    if (!m_job || m_job->active())
        return;
    m_job->deleteLater();
    m_job.clear();
    Q_EMIT jobChanged();
}

// ---------------------------------------------------------------- last log

void Updater::saveLastLog(const Job *job)
{
    QString text;
    QTextStream out(&text);
    out << job->title() << '\n'
        << tr("Finished: %1").arg(QLocale().toString(QDateTime::currentDateTime(), QLocale::LongFormat)) << '\n'
        << tr("Result: %1").arg(job->result()) << '\n';
    if (!job->errorKey().isEmpty()) {
        out << tr("PackageKit error code: %1").arg(job->errorKey()) << '\n'
            << tr("Error: %1").arg(job->errorTitle()) << '\n'
            << tr("Details: %1").arg(job->errorDetails()) << '\n';
    }
    if (job->untrustedAccepted())
        out << tr("Untrusted packages were explicitly allowed by the user.") << '\n';
    if (job->restart() != QLatin1String("none"))
        out << tr("Restart required: %1 (%2)").arg(job->restart(), job->restartPackages().join(QLatin1Char(' '))) << '\n';
    out << '\n' << job->logText() << '\n';

    m_lastLog = text;
    m_lastLogTitle = job->title();
    Q_EMIT lastLogChanged();

    // Keep it across restarts of the application: failed updates are usually
    // looked at (and reported) later.
    const QString path = lastLogPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(text.toUtf8());
        file.commit();
    }
}

void Updater::loadLastLog()
{
    QFile file(lastLogPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    m_lastLog = QString::fromUtf8(file.read(4 << 20));
    m_lastLogTitle = m_lastLog.section(QLatin1Char('\n'), 0, 0);
}

// ---------------------------------------------------------------- history

void Updater::refreshHistory()
{
    if (m_historyLoading)
        return;
    m_historyLoading = true;
    Q_EMIT historyChanged();
    ShadowPk::runAsync(this, [] { return ShadowPk::readAptHistory(500); }, [this](const QJsonObject &result) {
        m_aptHistory = result.value(QStringLiteral("entries")).toArray().toVariantList();
        m_historyError = result.value(QStringLiteral("error")).toString();
        m_historyTruncated = result.value(QStringLiteral("truncated")).toBool();
        m_historyLoading = false;
        rebuildHistory();
    });
}

void Updater::refreshTransactions()
{
    if (m_transactionsLoading)
        return;
    m_transactionsLoading = true;
    ShadowPk::fetchOldTransactions(this, 300, [this](const QVariantList &transactions, const QString &error) {
        m_transactions = transactions;
        m_transactionsError = error;
        m_transactionsLoading = false;
        rebuildHistory();
    });
}

void Updater::rebuildHistory()
{
    // APT's log says what changed; PackageKit's records add who asked for it.
    m_history = ShadowPk::mergeHistory(m_aptHistory, m_transactions);
    Q_EMIT historyChanged();
}

// ---------------------------------------------------------------- health

void Updater::refreshHealth()
{
    if (m_healthChecking)
        return;
    m_healthChecking = true;
    Q_EMIT healthChanged();
    ShadowPk::runAsync(this, [] { return ShadowPk::checkDpkgHealth(); }, [this](const QJsonObject &result) {
        m_health = result.toVariantMap();
        m_healthChecking = false;
        Q_EMIT healthChanged();
    });
}

// ---------------------------------------------------------------- restart

void Updater::refreshReboot()
{
    // A handful of stat() calls and two tiny files in /run: fine on this thread.
    m_reboot = ShadowPk::readRebootState().toVariantMap();
    Q_EMIT rebootChanged();
}

bool Updater::restartNeeded() const
{
    return restartLevel() != QLatin1String("none");
}

QString Updater::restartLevel() const
{
    if (m_reboot.value(QStringLiteral("required")).toBool() || m_jobRestartRank >= 3
        || m_reboot.value(QStringLiteral("newerKernelInstalled")).toBool())
        return QStringLiteral("system");
    if (m_jobRestartRank == 2)
        return QStringLiteral("session");
    if (m_jobRestartRank == 1)
        return QStringLiteral("application");
    return QStringLiteral("none");
}

QString Updater::restartReason() const
{
    QStringList reasons;
    if (m_reboot.value(QStringLiteral("required")).toBool()) {
        const QStringList packages = m_reboot.value(QStringLiteral("packages")).toStringList();
        reasons << (packages.isEmpty()
            ? tr("Installed packages asked for a restart (%1 exists).").arg(m_reboot.value(QStringLiteral("flagFile")).toString())
            : tr("These packages asked for a restart: %1.").arg(packages.join(QStringLiteral(", "))));
    }
    if (m_jobRestartRank > 0) {
        const QString who = m_jobRestartPackages.isEmpty() ? tr("the updates just installed")
                                                           : m_jobRestartPackages.join(QStringLiteral(", "));
        if (m_jobRestartRank >= 3)
            reasons << tr("PackageKit reported that a restart of the computer is needed for: %1.").arg(who);
        else if (m_jobRestartRank == 2)
            reasons << tr("PackageKit reported that you need to log out and back in for: %1.").arg(who);
        else
            reasons << tr("PackageKit reported that running applications must be restarted for: %1.").arg(who);
        if (m_jobRestartSecurity)
            reasons << tr("The restart is needed to complete a security update.");
    }
    if (m_reboot.value(QStringLiteral("newerKernelInstalled")).toBool()) {
        reasons << tr("A newer kernel (%1) is installed than the one running (%2).")
                       .arg(m_reboot.value(QStringLiteral("newestKernel")).toString(),
                            m_reboot.value(QStringLiteral("runningKernel")).toString());
    }
    return reasons.join(QLatin1Char(' '));
}

void Updater::probeLogind()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        m_canReboot.clear();
        m_canRebootError = tr("The system D-Bus is not available: %1").arg(bus.lastError().message());
        Q_EMIT rebootChanged();
        return;
    }
    const QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("CanReboot"));
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 15000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        if (reply.isError()) {
            m_canReboot.clear();
            m_canRebootError = tr("logind is not reachable: %1").arg(reply.error().message());
        } else {
            m_canReboot = reply.value();
            m_canRebootError = m_canReboot == QLatin1String("yes") || m_canReboot == QLatin1String("challenge")
                ? QString() : tr("logind reports that restarting is not possible here (CanReboot: %1).").arg(m_canReboot);
        }
        Q_EMIT rebootChanged();
    });
}

void Updater::restartNow()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        Q_EMIT restartFailed(tr("The system D-Bus is not available: %1").arg(bus.lastError().message()));
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("Reboot"));
    call << true;   // interactive: polkit may ask (other users logged in, inhibitors)
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 120000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError())
            Q_EMIT restartFailed(tr("logind refused to restart the computer: %1").arg(w->error().message()));
    });
}

// ---------------------------------------------------------------- sources

void Updater::refreshSources()
{
    if (m_sourcesLoading)
        return;
    m_sourcesLoading = true;
    Q_EMIT sourcesChanged();
    ShadowPk::runAsync(this, [] { return ShadowPk::readAptSources(); }, [this](const QJsonObject &result) {
        m_sources = result.value(QStringLiteral("sources")).toArray().toVariantList();
        m_sourcesLoading = false;
        Q_EMIT sourcesChanged();
    });
}

// ---------------------------------------------------------------- reporting

void Updater::copyToClipboard(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}

QString Updater::report() const
{
    const Service *s = Service::instance();
    QString text;
    QTextStream out(&text);
    out << "SHADOW Updater " << QCoreApplication::applicationVersion() << " — report of "
        << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";
    out << "System:        " << QSysInfo::prettyProductName() << "\n";
    out << "Kernel:        " << QSysInfo::kernelVersion() << "\n";
    out << "Architecture:  " << ShadowPk::systemArchitecture() << "\n";
    if (s->available()) {
        out << "PackageKit:    " << s->daemonVersion() << ", back-end " << s->backendName()
            << " (" << s->backendDescription() << "), network " << s->networkState() << "\n";
    } else {
        out << "PackageKit:    NOT REACHABLE — " << s->error() << "\n";
    }

    out << "\nUpdates:       ";
    if (m_updates.state() == QLatin1String("ready"))
        out << m_updates.count() << " available, " << m_updates.securityCount() << " security, "
            << m_updates.blockedCount() << " held back\n";
    else if (!m_updates.error().isEmpty())
        out << "could not be read: " << m_updates.error() << "\n";
    else
        out << "not checked\n";
    if (!m_updates.refreshError().isEmpty())
        out << "Refresh error: " << m_updates.refreshError() << "\n";

    out << "\nRestart:       " << restartLevel();
    if (restartNeeded())
        out << " — " << restartReason();
    out << "\n";

    out << "\nPackage database (dpkg): ";
    if (m_health.isEmpty()) {
        out << "not checked yet\n";
    } else if (healthy()) {
        out << "consistent\n";
    } else {
        out << "NEEDS REPAIR\n";
        const QString audit = m_health.value(QStringLiteral("audit")).toMap().value(QStringLiteral("output")).toString();
        if (!audit.isEmpty())
            out << "dpkg --audit:\n" << audit << "\n";
        const QStringList pending = m_health.value(QStringLiteral("interruptedUpdates")).toStringList();
        if (!pending.isEmpty())
            out << "/var/lib/dpkg/updates: " << pending.join(QLatin1Char(' ')) << "\n";
        const QVariantList broken = m_health.value(QStringLiteral("brokenPackages")).toList();
        for (const QVariant &v : broken) {
            const QVariantMap p = v.toMap();
            out << "  " << p.value(QStringLiteral("status")).toString() << "  " << p.value(QStringLiteral("package")).toString()
                << " " << p.value(QStringLiteral("version")).toString() << " — " << p.value(QStringLiteral("state")).toString() << "\n";
        }
    }

    out << "\n---- Last transaction ----\n";
    out << (m_lastLog.isEmpty() ? QStringLiteral("(none recorded)\n") : m_lastLog);
    return text;
}
