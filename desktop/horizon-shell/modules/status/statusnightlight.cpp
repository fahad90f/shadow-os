// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusnightlight.h"

#include "statusdbus.h"

#include <KConfig>
#include <KConfigGroup>

#include <QTimer>

namespace StatusModule {

namespace {

const QString kKWin = QStringLiteral("org.kde.KWin");
const QString kPath = QStringLiteral("/org/kde/KWin/NightLight");
const QString kInterface = QStringLiteral("org.kde.KWin.NightLight");

QString modeName(int mode)
{
    switch (mode) {     // KWin::NightLightMode
    case 0: return QStringLiteral("automatic");
    case 1: return QStringLiteral("location");
    case 2: return QStringLiteral("times");
    case 3: return QStringLiteral("constant");
    default: return QStringLiteral("unknown");
    }
}

} // namespace

NightLightBackend::NightLightBackend(QObject *parent)
    : Backend(parent)
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    m_kwin = new DBusProperties(bus, kKWin, kPath, kInterface, this);
    connect(m_kwin, &DBusProperties::changed, this, &NightLightBackend::recompute);
    dbusWatchService(bus, kKWin, this, [this] { m_kwin->load(); }, [this] { m_kwin->clear(); });
    m_kwin->load();
}

void NightLightBackend::recompute()
{
    if (!m_kwin->isLoaded())
        return;
    const bool valid = m_kwin->isValid();
    const bool enabled = valid && m_kwin->boolean(QStringLiteral("enabled"));
    const bool running = valid && m_kwin->boolean(QStringLiteral("running"));
    const bool inhibited = valid && m_kwin->boolean(QStringLiteral("inhibited"));
    const int temperature = valid ? int(m_kwin->integer(QStringLiteral("currentTemperature"))) : 0;
    const QString mode = valid ? modeName(int(m_kwin->integer(QStringLiteral("mode"), -1))) : QString();
    if (enabled != m_enabled || running != m_running || inhibited != m_inhibited || temperature != m_temperature
        || mode != m_mode) {
        m_enabled = enabled;
        m_running = running;
        m_inhibited = inhibited;
        m_temperature = temperature;
        m_mode = mode;
        Q_EMIT changed();
    }

    if (!valid)
        setAvailable(false, tr("KWin's Night Light is not running in this session (%1).").arg(m_kwin->errorMessage()));
    else if (!m_kwin->boolean(QStringLiteral("available")))
        setAvailable(false, tr("KWin reports that Night Light is not supported on this screen."));
    else
        setAvailable(true);
}

void NightLightBackend::setEnabled(bool enabled)
{
    if (!isAvailable())
        return;
    {
        KConfig config(QStringLiteral("kwinrc"), KConfig::NoGlobals);
        KConfigGroup group = config.group(QStringLiteral("NightColor"));
        group.writeEntry("Active", enabled, KConfig::Notify);
        // "Automatic" takes sunrise and sunset from a location service that
        // feeds KWin, which NEXUS does not run; fixed times work everywhere.
        if (enabled && m_mode == QLatin1String("automatic"))
            group.writeEntry("Mode", QStringLiteral("Times"), KConfig::Notify);
        if (!config.sync()) {
            Q_EMIT failed(tr("The Night Light setting could not be written to KWin's configuration."));
            return;
        }
    }
    // KWin announces the new state itself; read it once more in case the
    // notification was sent before it finished reconfiguring.
    QTimer::singleShot(600, m_kwin, &DBusProperties::load);
}

QJsonObject NightLightBackend::details() const
{
    QJsonObject json;
    if (!m_kwin->isValid())
        return json;
    json.insert(QStringLiteral("supported"), m_kwin->boolean(QStringLiteral("available")));
    json.insert(QStringLiteral("enabled"), m_enabled);
    json.insert(QStringLiteral("running"), m_running);
    json.insert(QStringLiteral("inhibited"), m_inhibited);
    json.insert(QStringLiteral("mode"), m_mode);
    json.insert(QStringLiteral("currentTemperature"), m_temperature);
    json.insert(QStringLiteral("targetTemperature"), int(m_kwin->integer(QStringLiteral("targetTemperature"))));
    return json;
}

} // namespace StatusModule
