// SPDX-License-Identifier: GPL-3.0-or-later
#include "updatesbackend.h"

#include "dumpregistry.h"

#include <command.h>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <unistd.h>

NEXUS_CC_DUMP("updates", [] {
    UpdatesBackend backend;
    if (!backend.systemReady()) {
        QEventLoop loop;
        QObject::connect(&backend, &UpdatesBackend::systemChanged, &loop, [&] { if (backend.systemReady()) loop.quit(); });
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
const QString kAutoUpgrades = QStringLiteral("/etc/apt/apt.conf.d/20auto-upgrades");

// The same store NEXUS Updater uses (applications/updater/updater.cpp).
QSettings updaterSettings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("nexus"), QStringLiteral("updater"));
}

QString helperPath()
{
    const QString dir = qEnvironmentVariable("NEXUS_CC_HELPER_DIR");
    return (dir.isEmpty() ? QStringLiteral("/usr/libexec/nexus") : dir) + QStringLiteral("/updates-helper");
}
} // namespace

UpdatesBackend::UpdatesBackend(QObject *parent)
    : QObject(parent)
{
    loadSettings();
    // NEXUS Updater and "nexus-updater --check" write the same file.
    const QString dir = QFileInfo(settingsFile()).absolutePath();
    QDir().mkpath(dir);
    m_watcher.addPath(dir);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &UpdatesBackend::loadSettings);
    loadSystem();
}

QString UpdatesBackend::settingsFile() const
{
    return updaterSettings().fileName();
}

void UpdatesBackend::loadSettings()
{
    QSettings s = updaterSettings();
    s.sync();
    m_autoCheck = s.value(QStringLiteral("Updates/AutoCheck"), true).toBool();
    m_interval = qBound(1, s.value(QStringLiteral("Updates/CheckIntervalHours"), 24).toInt(), 24 * 30);
    m_lastCheck = QDateTime::fromString(s.value(QStringLiteral("LastCheck/Time")).toString(), Qt::ISODate);
    m_lastUpdates = s.value(QStringLiteral("LastCheck/Updates"), 0).toInt();
    m_lastSecurity = s.value(QStringLiteral("LastCheck/Security"), 0).toInt();
    m_lastError = s.value(QStringLiteral("LastCheck/Error")).toString();
    m_lastRefreshError = s.value(QStringLiteral("LastCheck/RefreshError")).toString();
    Q_EMIT settingsChanged();
}

void UpdatesBackend::setAutoCheck(bool on)
{
    if (on == m_autoCheck)
        return;
    QSettings s = updaterSettings();
    s.setValue(QStringLiteral("Updates/AutoCheck"), on);
    s.sync();
    if (s.status() != QSettings::NoError)
        Q_EMIT failed(tr("%1 could not be written.").arg(s.fileName()));
    loadSettings();
}

void UpdatesBackend::setCheckIntervalHours(int hours)
{
    hours = qBound(1, hours, 24 * 30);
    if (hours == m_interval)
        return;
    QSettings s = updaterSettings();
    s.setValue(QStringLiteral("Updates/CheckIntervalHours"), hours);
    s.sync();
    if (s.status() != QSettings::NoError)
        Q_EMIT failed(tr("%1 could not be written.").arg(s.fileName()));
    loadSettings();
}

bool UpdatesBackend::updaterAvailable() const
{
    return !QStandardPaths::findExecutable(QStringLiteral("nexus-updater")).isEmpty();
}

bool UpdatesBackend::openUpdater() const
{
    return QProcess::startDetached(QStringLiteral("nexus-updater"), {});
}

bool UpdatesBackend::rebootRequired() const
{
    return QFile::exists(QStringLiteral("/run/reboot-required"));
}

bool UpdatesBackend::unattendedInstalled() const
{
    return QFile::exists(QStringLiteral("/usr/bin/unattended-upgrade"));
}

void UpdatesBackend::checkNow()
{
    if (m_checking || !updaterAvailable())
        return;
    m_checking = true;
    Q_EMIT checkingChanged();
    Nexus::Command::run(this, QStringLiteral("nexus-updater"), {QStringLiteral("--check")}, [this](const Nexus::CommandResult &result) {
        m_checking = false;
        Q_EMIT checkingChanged();
        loadSettings();
        Q_EMIT systemChanged();     // /run/reboot-required may have appeared
        // Exit codes of "nexus-updater --check": 0 none, 10 updates, 11 security updates, 1 failed.
        const QJsonObject reply = QJsonDocument::fromJson(result.out).object();
        if (result.exitCode == 0 || result.exitCode == 10 || result.exitCode == 11) {
            const int updates = reply.value(QStringLiteral("updates")).toInt();
            const int security = reply.value(QStringLiteral("securityUpdates")).toInt();
            Q_EMIT succeeded(updates == 0 ? tr("No updates are available.")
                             : security > 0 ? tr("%1 update(s) available, %2 of them security updates.").arg(updates).arg(security)
                                            : tr("%1 update(s) available.").arg(updates));
        } else {
            QString error = reply.value(QStringLiteral("error")).toString();
            if (error.isEmpty())
                error = result.exitCode == -2 ? tr("the check did not finish in time") : result.errText().trimmed();
            Q_EMIT failed(error.isEmpty() ? tr("The update check failed (exit code %1).").arg(result.exitCode)
                                          : tr("The update check failed: %1").arg(error));
        }
    }, 600000);
}

void UpdatesBackend::reload()
{
    loadSettings();
    loadSystem();
}

void UpdatesBackend::applyPeriodic(const QString &text, const QString &source)
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*APT::Periodic::(Update-Package-Lists|Unattended-Upgrade)\\s+\"([^\"]*)\"\\s*;"),
        QRegularExpression::MultilineOption);
    m_updateLists.clear();
    m_unattended.clear();
    auto it = pattern.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        (match.captured(1) == QLatin1String("Update-Package-Lists") ? m_updateLists : m_unattended) = match.captured(2);
    }
    m_periodicKnown = true;
    m_periodicSource = source;
}

void UpdatesBackend::loadSystem()
{
    if (m_pending > 0)
        return;
    m_pending = 1;
    const auto done = [this] {
        if (--m_pending == 0) {
            m_systemReady = true;
            Q_EMIT systemChanged();
        }
    };
    if (Nexus::Command::available(QStringLiteral("apt-config"))) {
        Nexus::Command::run(this, QStringLiteral("apt-config"), {QStringLiteral("dump")}, [this, done](const Nexus::CommandResult &result) {
            if (result.ok()) {
                applyPeriodic(result.outText(), QStringLiteral("apt-config"));
            } else {
                m_periodicKnown = false;
                m_periodicSource = result.errText().trimmed();
            }
            done();
        });
    } else {
        QFile file(kAutoUpgrades);
        if (file.open(QIODevice::ReadOnly))
            applyPeriodic(QString::fromUtf8(file.readAll()), kAutoUpgrades);
        else
            applyPeriodic(QString(), kAutoUpgrades);   // no file: nothing is configured
        QTimer::singleShot(0, this, done);
    }
    // The values only have an effect while apt's daily timer is enabled.
    m_timerState.clear();
    if (QFileInfo(QStringLiteral("/run/systemd/system")).isDir()) {
        ++m_pending;
        Nexus::Command::run(this, QStringLiteral("systemctl"), {QStringLiteral("is-enabled"), QStringLiteral("apt-daily-upgrade.timer")},
                            [this, done](const Nexus::CommandResult &result) {
            m_timerState = result.outText().trimmed().section(QLatin1Char('\n'), 0, 0);
            done();
        });
    }
}

void UpdatesBackend::setAutomatic(bool updateLists, bool unattendedUpgrade)
{
    if (m_busy)
        return;
    if (!QFile::exists(helperPath())) {
        Q_EMIT failed(tr("The helper %1 is not installed.").arg(helperPath()));
        return;
    }
    m_busy = true;
    Q_EMIT busyChanged();
    const QStringList arguments{QStringLiteral("set-auto-upgrades"), updateLists ? QStringLiteral("1") : QStringLiteral("0"),
                                unattendedUpgrade ? QStringLiteral("1") : QStringLiteral("0")};
    const auto done = [this](const Nexus::CommandResult &result) {
        m_busy = false;
        Q_EMIT busyChanged();
        if (!result.ok()) {
            QString error = QJsonDocument::fromJson(result.out).object().value(QStringLiteral("error")).toString();
            if (error.isEmpty())
                error = result.exitCode == 126 ? tr("The request was not authorised.")
                      : result.exitCode == 127 ? tr("Authentication failed, or no authentication agent is running in this session.")
                      : result.exitCode == -1 ? tr("pkexec could not be started.")
                      : result.errText().trimmed();
            Q_EMIT failed(tr("The automatic update settings were not changed: %1").arg(error));
        } else {
            Q_EMIT succeeded(tr("Automatic update settings saved."));
        }
        loadSystem();
    };
    if (geteuid() == 0)
        Nexus::Command::run(this, helperPath(), arguments, done, 300000);
    else
        Nexus::Command::run(this, QStringLiteral("pkexec"), QStringList{helperPath()} + arguments, done, 300000);
}

QJsonObject UpdatesBackend::toJson() const
{
    QJsonObject last;
    if (m_lastCheck.isValid()) {
        last = {{QStringLiteral("time"), m_lastCheck.toString(Qt::ISODate)},
                {QStringLiteral("updates"), m_lastUpdates},
                {QStringLiteral("security"), m_lastSecurity},
                {QStringLiteral("error"), m_lastError},
                {QStringLiteral("refreshError"), m_lastRefreshError}};
    }
    return {
        {QStringLiteral("settingsFile"), settingsFile()},
        {QStringLiteral("autoCheck"), m_autoCheck},
        {QStringLiteral("checkIntervalHours"), m_interval},
        {QStringLiteral("lastCheck"), last},
        {QStringLiteral("updaterAvailable"), updaterAvailable()},
        {QStringLiteral("rebootRequired"), rebootRequired()},
        {QStringLiteral("unattendedUpgradesInstalled"), unattendedInstalled()},
        {QStringLiteral("periodicKnown"), m_periodicKnown},
        {QStringLiteral("periodicSource"), m_periodicSource},
        {QStringLiteral("updatePackageLists"), m_updateLists},
        {QStringLiteral("unattendedUpgrade"), m_unattended},
        {QStringLiteral("autoUpgradesFileExists"), QFile::exists(kAutoUpgrades)},
        {QStringLiteral("aptDailyUpgradeTimer"), m_timerState},
    };
}
