// SPDX-License-Identifier: GPL-3.0-or-later
#include "powerconfig.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QStandardPaths>

namespace PowerModule {

namespace {

QString configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/shadow");
}

// A whole number of minutes from 0 to 1440; anything else is the default.
int minutes(const QSettings &settings, const char *key, int fallback)
{
    const QVariant value = settings.value(QStringLiteral("Power/") + QLatin1String(key));
    if (!value.isValid())
        return fallback;
    bool ok = false;
    const int number = value.toString().trimmed().toInt(&ok);
    return ok && number >= 0 && number <= 1440 ? number : fallback;
}

} // namespace

Config::Config(QObject *parent)
    : QObject(parent)
{
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(250);
    connect(&m_reloadTimer, &QTimer::timeout, this, &Config::reload);

    // The directory may not exist before the first settings change; creating
    // it (empty) is what lets us notice that change.
    QDir().mkpath(configDir());
    // The directory tells about files that are created or replaced (the
    // writers replace atomically); the files themselves about edits in place.
    m_watcher = new QFileSystemWatcher(this);
    m_watcher->addPath(configDir());
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
    connect(m_watcher, &QFileSystemWatcher::fileChanged, &m_reloadTimer, qOverload<>(&QTimer::start));

    reload();
}

QString Config::powerFile() const { return configDir() + QStringLiteral("/power.conf"); }
QString Config::updaterFile() const { return configDir() + QStringLiteral("/updater.conf"); }

double Config::timeScale()
{
    static const double scale = [] {
        bool ok = false;
        const double value = qEnvironmentVariable("SHADOW_POWER_TIME_SCALE").toDouble(&ok);
        return ok && value > 0 ? value : 1.0;
    }();
    return scale;
}

qint64 Config::scaled(qint64 ms)
{
    return qMax<qint64>(1, qRound64(double(ms) / timeScale()));
}

void Config::reload()
{
    // A replaced file is a new inode: watch it again.
    for (const QString &file : {powerFile(), updaterFile()}) {
        if (QFileInfo::exists(file) && !m_watcher->files().contains(file))
            m_watcher->addPath(file);
    }

    // QSettings falls back, per key, to /etc/xdg/shadow/<name>.conf.
    const QSettings power(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("shadow"), QStringLiteral("power"));
    IdleMinutes ac, battery;
    ac.screenOff = minutes(power, "IdleScreenOffMinutes", 10);
    ac.lock = minutes(power, "IdleLockMinutes", 15);
    ac.suspend = minutes(power, "IdleSuspendMinutes", 0);
    battery.screenOff = minutes(power, "IdleScreenOffMinutesOnBattery", 5);
    battery.lock = minutes(power, "IdleLockMinutesOnBattery", 10);
    battery.suspend = minutes(power, "IdleSuspendMinutesOnBattery", 20);
    if (ac != m_ac || battery != m_battery) {
        m_ac = ac;
        m_battery = battery;
        Q_EMIT idleChanged();
    }

    const QSettings updater(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("shadow"), QStringLiteral("updater"));
    const bool autoCheck = updater.value(QStringLiteral("Updates/AutoCheck"), true).toBool();
    const int interval = qBound(1, updater.value(QStringLiteral("Updates/CheckIntervalHours"), 24).toInt(), 24 * 30);
    if (autoCheck != m_autoCheck || interval != m_intervalHours) {
        m_autoCheck = autoCheck;
        m_intervalHours = interval;
        Q_EMIT updatesChanged();
    }
}

QJsonObject Config::toJson() const
{
    const auto idle = [](const IdleMinutes &m) {
        return QJsonObject{{QStringLiteral("screenOffMinutes"), m.screenOff},
                           {QStringLiteral("lockMinutes"), m.lock},
                           {QStringLiteral("suspendMinutes"), m.suspend}};
    };
    return QJsonObject{
        {QStringLiteral("file"), powerFile()},
        {QStringLiteral("fileExists"), QFileInfo::exists(powerFile())},
        {QStringLiteral("onAc"), idle(m_ac)},
        {QStringLiteral("onBattery"), idle(m_battery)},
        {QStringLiteral("timeScale"), timeScale()},
    };
}

} // namespace PowerModule
