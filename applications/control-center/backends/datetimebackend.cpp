// SPDX-License-Identifier: GPL-3.0-or-later
#include "datetimebackend.h"

#include "dumpregistry.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QSettings>
#include <QStandardPaths>
#include <QTimeZone>

NEXUS_CC_DUMP("datetime", [] {
    DateTimeBackend backend;
    if (!backend.ready()) {
        QEventLoop loop;
        QObject::connect(&backend, &DateTimeBackend::stateChanged, &loop, [&] { if (backend.ready()) loop.quit(); });
        QTimer::singleShot(8000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
const QString kService = QStringLiteral("org.freedesktop.timedate1");
const QString kPath = QStringLiteral("/org/freedesktop/timedate1");
const QString kInterface = QStringLiteral("org.freedesktop.timedate1");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kZoneInfo = QStringLiteral("/usr/share/zoneinfo");
// Authorisation dialogs may stay open for a while.
const int kChangeTimeoutMs = 5 * 60 * 1000;

QSettings shellSettings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("nexus"), QStringLiteral("horizon"));
}

QString errorText(const QDBusError &error)
{
    const QString name = error.name();
    if (name == QLatin1String("org.freedesktop.DBus.Error.AccessDenied")
        || name == QLatin1String("org.freedesktop.DBus.Error.InteractiveAuthorizationRequired")
        || name.endsWith(QLatin1String("NotAuthorized")))
        return DateTimeBackend::tr("The change was not authorised.");
    if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
        || name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")
        || name == QLatin1String("org.freedesktop.DBus.Error.Spawn.ChildExited")
        || name == QLatin1String("org.freedesktop.DBus.Error.Spawn.ExecFailed"))
        return DateTimeBackend::tr("The time service (systemd-timedated) is not running and could not be started.");
    if (name == QLatin1String("org.freedesktop.DBus.Error.NoReply") || name == QLatin1String("org.freedesktop.DBus.Error.Timeout"))
        return DateTimeBackend::tr("The time service did not answer.");
    return error.message().isEmpty() ? name : error.message();
}

// Zone of /etc/localtime when it is a link into the zone database.
QString zoneFromLocaltime()
{
    const QString target = QFile::symLinkTarget(QStringLiteral("/etc/localtime"));
    const int at = target.indexOf(QStringLiteral("/zoneinfo/"));
    if (at >= 0)
        return target.mid(at + 10);
    QFile file(QStringLiteral("/etc/timezone"));
    if (file.open(QIODevice::ReadOnly))
        return QString::fromLatin1(file.readLine(128)).trimmed();
    return QString();
}
} // namespace

DateTimeBackend::DateTimeBackend(QObject *parent)
    : QObject(parent)
{
    loadZones();
    loadPanel();
    watchPanel();
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { loadPanel(); watchPanel(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { loadPanel(); watchPanel(); });

    m_clock.setInterval(1000);
    connect(&m_clock, &QTimer::timeout, this, &DateTimeBackend::tick);
    m_clock.start();

    // NTPSynchronized has no change signal; look again now and then.
    m_poll.setInterval(20000);
    connect(&m_poll, &QTimer::timeout, this, &DateTimeBackend::refresh);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        bus.connect(kService, kPath, kProperties, QStringLiteral("PropertiesChanged"), this,
                    SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    }
    refresh();
}

QString DateTimeBackend::settingsFile() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/nexus/horizon.conf");
}

// ---- time zone list ------------------------------------------------------------------

void DateTimeBackend::loadZones()
{
    QMap<QString, QString> countries;
    QFile iso(kZoneInfo + QStringLiteral("/iso3166.tab"));
    if (iso.open(QIODevice::ReadOnly)) {
        while (!iso.atEnd()) {
            const QString line = QString::fromUtf8(iso.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            const QStringList fields = line.split(QLatin1Char('\t'));
            if (fields.size() >= 2)
                countries.insert(fields.at(0), fields.at(1));
        }
    }

    auto add = [this](const QString &id, const QString &countryNames, const QString &comment) {
        Zone zone;
        zone.id = id;
        const int slash = id.indexOf(QLatin1Char('/'));
        zone.region = slash > 0 ? id.left(slash) : QString();
        zone.city = (slash > 0 ? id.mid(slash + 1) : id);
        zone.city.replace(QLatin1Char('_'), QLatin1Char(' ')).replace(QLatin1Char('/'), QStringLiteral(" / "));
        zone.countries = countryNames;
        zone.comment = comment;
        m_zones << zone;
    };

    QFile tab(kZoneInfo + QStringLiteral("/zone1970.tab"));
    if (tab.open(QIODevice::ReadOnly)) {
        m_zoneSource = tab.fileName();
        while (!tab.atEnd()) {
            const QString line = QString::fromUtf8(tab.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            const QStringList fields = line.split(QLatin1Char('\t'));
            if (fields.size() < 3)
                continue;
            QStringList names;
            const QStringList codes = fields.at(0).split(QLatin1Char(','));
            for (const QString &code : codes)
                names << countries.value(code, code);
            add(fields.at(2), names.join(QStringLiteral(", ")), fields.value(3));
        }
    } else {
        m_zoneSource = tr("the time zones known to Qt");
        const QList<QByteArray> ids = QTimeZone::availableTimeZoneIds();
        for (const QByteArray &raw : ids) {
            const QString id = QString::fromLatin1(raw);
            if (id.contains(QLatin1Char('/')) && !id.startsWith(QLatin1String("Etc/")))
                add(id, QString(), QString());
        }
    }
    // Not a place, so not in zone1970.tab — but the usual choice for servers.
    if (QFileInfo::exists(kZoneInfo + QStringLiteral("/Etc/UTC")) || m_zones.isEmpty()) {
        Zone utc;
        utc.id = QStringLiteral("Etc/UTC");
        utc.region = QStringLiteral("Etc");
        utc.city = QStringLiteral("UTC");
        utc.comment = tr("Coordinated Universal Time");
        m_zones << utc;
    }
    std::sort(m_zones.begin(), m_zones.end(), [](const Zone &a, const Zone &b) { return a.id < b.id; });
    for (const Zone &zone : std::as_const(m_zones)) {
        if (!zone.region.isEmpty() && !m_regions.contains(zone.region))
            m_regions << zone.region;
    }
}

QString DateTimeBackend::offsetText(int seconds)
{
    const int minutes = qAbs(seconds) / 60;
    return QStringLiteral("UTC%1%2:%3")
        .arg(seconds < 0 ? QLatin1Char('-') : QLatin1Char('+'))
        .arg(minutes / 60, 2, 10, QLatin1Char('0'))
        .arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

QVariantList DateTimeBackend::zones() const
{
    QVariantList out;
    const QString needle = m_filter.trimmed().toLower();
    const QString spaced = QString(needle).replace(QLatin1Char('_'), QLatin1Char(' '));
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    for (const Zone &zone : m_zones) {
        if (!m_region.isEmpty() && zone.region != m_region)
            continue;
        QString abbreviation;
        int offset = 0;
        bool known = false;
        auto resolve = [&] {
            if (known)
                return;
            const QTimeZone tz(zone.id.toLatin1());
            if (tz.isValid()) {
                offset = tz.offsetFromUtc(utc);
                abbreviation = tz.abbreviation(utc);
            }
            known = true;
        };
        if (!needle.isEmpty()) {
            bool match = zone.id.toLower().contains(needle) || zone.city.toLower().contains(spaced)
                         || zone.countries.toLower().contains(spaced) || zone.comment.toLower().contains(spaced);
            if (!match) {
                resolve();
                match = abbreviation.toLower() == needle || offsetText(offset).toLower().contains(needle);
            }
            if (!match)
                continue;
        }
        resolve();
        out << QVariantMap{
            {QStringLiteral("id"), zone.id},
            {QStringLiteral("region"), zone.region},
            {QStringLiteral("city"), zone.city},
            {QStringLiteral("countries"), zone.countries},
            {QStringLiteral("comment"), zone.comment},
            {QStringLiteral("offset"), offset},
            {QStringLiteral("offsetText"), offsetText(offset)},
            {QStringLiteral("abbreviation"), abbreviation},
            {QStringLiteral("current"), zone.id == m_timezone},
        };
    }
    return out;
}

void DateTimeBackend::setRegion(const QString &region)
{
    if (region == m_region)
        return;
    m_region = region;
    Q_EMIT zonesChanged();
}

void DateTimeBackend::setFilter(const QString &filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    Q_EMIT zonesChanged();
}

QString DateTimeBackend::timezoneLabel() const
{
    if (m_timezone.isEmpty())
        return tr("Unknown");
    for (const Zone &zone : m_zones) {
        if (zone.id == m_timezone)
            return zone.countries.isEmpty() ? zone.city : tr("%1 (%2)").arg(zone.city, zone.countries);
    }
    return m_timezone;
}

// ---- clock -----------------------------------------------------------------------------

QDateTime DateTimeBackend::now() const
{
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    if (!m_timezone.isEmpty()) {
        const QTimeZone tz(m_timezone.toLatin1());
        if (tz.isValid())
            return utc.toTimeZone(tz);
    }
    return utc.toLocalTime();
}

QString DateTimeBackend::currentTime() const
{
    return QLocale().toString(now().time(), m_clock24h ? QStringLiteral("HH:mm:ss") : QStringLiteral("h:mm:ss AP"));
}

QString DateTimeBackend::currentDate() const
{
    return QLocale().toString(now().date(), QLocale::LongFormat);
}

QString DateTimeBackend::utcOffsetText() const
{
    const QDateTime t = now();
    const QString abbreviation = t.timeZoneAbbreviation();
    const QString offset = offsetText(t.offsetFromUtc());
    return abbreviation.isEmpty() || abbreviation.startsWith(QLatin1String("UTC")) ? offset
                                                                                     : tr("%1, %2").arg(abbreviation, offset);
}

QString DateTimeBackend::isoDate() const { return now().date().toString(Qt::ISODate); }
QString DateTimeBackend::isoTime() const { return now().time().toString(QStringLiteral("HH:mm:ss")); }

// ---- timedate1 ----------------------------------------------------------------------------

void DateTimeBackend::refresh()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        readWithoutService(tr("The system message bus is not reachable, so the time service (systemd-timedated) "
                              "cannot be asked. Time zone and clock settings are shown read-only."));
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kProperties, QStringLiteral("GetAll"));
    message << kInterface;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 25000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError()) {
            readWithoutService(tr("%1 Time zone and clock settings are shown read-only.").arg(errorText(reply.error())));
            return;
        }
        m_available = true;
        m_statusMessage.clear();
        m_localRtcSource = tr("Reported by systemd-timedated");
        applyProperties(reply.value());
        m_ready = true;
        if (!m_poll.isActive())
            m_poll.start();
        Q_EMIT stateChanged();
        Q_EMIT zonesChanged();
        Q_EMIT tick();
    });
}

void DateTimeBackend::applyProperties(const QVariantMap &p)
{
    if (p.contains(QStringLiteral("Timezone")))
        m_timezone = p.value(QStringLiteral("Timezone")).toString();
    if (p.contains(QStringLiteral("LocalRTC"))) {
        m_localRtc = p.value(QStringLiteral("LocalRTC")).toBool();
        m_localRtcKnown = true;
    }
    if (p.contains(QStringLiteral("CanNTP")))
        m_canNtp = p.value(QStringLiteral("CanNTP")).toBool();
    if (p.contains(QStringLiteral("NTP")))
        m_ntp = p.value(QStringLiteral("NTP")).toBool();
    if (p.contains(QStringLiteral("NTPSynchronized")))
        m_ntpSynchronized = p.value(QStringLiteral("NTPSynchronized")).toBool();
}

void DateTimeBackend::readWithoutService(const QString &reason)
{
    m_available = false;
    m_statusMessage = reason;
    m_poll.stop();
    m_canNtp = m_ntp = m_ntpSynchronized = false;

    m_timezone = zoneFromLocaltime();
    if (m_timezone.isEmpty())
        m_timezone = QString::fromLatin1(QTimeZone::systemTimeZoneId());

    // Third line of /etc/adjtime: "UTC" or "LOCAL"; no file means UTC.
    m_localRtcKnown = false;
    m_localRtc = false;
    m_localRtcSource.clear();
    QFile adjtime(QStringLiteral("/etc/adjtime"));
    if (adjtime.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromLatin1(adjtime.readAll()).split(QLatin1Char('\n'));
        if (lines.size() >= 3 && !lines.at(2).trimmed().isEmpty()) {
            m_localRtc = lines.at(2).trimmed() == QLatin1String("LOCAL");
            m_localRtcKnown = true;
            m_localRtcSource = tr("Read from /etc/adjtime");
        }
    }
    m_ready = true;
    Q_EMIT stateChanged();
    Q_EMIT zonesChanged();
    Q_EMIT tick();
}

void DateTimeBackend::onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated)
{
    if (interface != kInterface)
        return;
    if (!invalidated.isEmpty() || !m_available) {
        refresh();
        return;
    }
    applyProperties(changed);
    Q_EMIT stateChanged();
    Q_EMIT zonesChanged();
    Q_EMIT tick();
}

void DateTimeBackend::call(const QString &method, const QVariantList &arguments, const QString &successMessage)
{
    if (!m_available) {
        Q_EMIT failed(m_statusMessage);
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
    message.setArguments(arguments);
    message.setInteractiveAuthorizationAllowed(true);
    m_busy = true;
    Q_EMIT busyChanged();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, kChangeTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, successMessage](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        m_busy = false;
        Q_EMIT busyChanged();
        const QDBusPendingReply<> reply = *w;
        if (reply.isError())
            Q_EMIT failed(errorText(reply.error()));
        else
            Q_EMIT changed(successMessage);
        // Controls show what the service reports, whatever was asked for.
        refresh();
        Q_EMIT stateChanged();
    });
}

void DateTimeBackend::setTimezone(const QString &id)
{
    if (id == m_timezone)
        return;
    bool known = false;
    for (const Zone &zone : std::as_const(m_zones))
        known = known || zone.id == id;
    if (!known) {
        Q_EMIT failed(tr("“%1” is not a time zone of this system.").arg(id));
        return;
    }
    call(QStringLiteral("SetTimezone"), {id, true}, tr("Time zone set to %1").arg(id));
}

void DateTimeBackend::setNtp(bool on)
{
    call(QStringLiteral("SetNTP"), {on, true},
         on ? tr("The clock is now set automatically") : tr("Automatic time setting is off"));
}

QString DateTimeBackend::setDateTime(const QString &dateText, const QString &timeText)
{
    const QDate date = QDate::fromString(dateText.trimmed(), Qt::ISODate);
    if (!date.isValid())
        return tr("The date must be written as year-month-day, for example %1.").arg(isoDate());
    QTime time = QTime::fromString(timeText.trimmed(), QStringLiteral("HH:mm:ss"));
    if (!time.isValid())
        time = QTime::fromString(timeText.trimmed(), QStringLiteral("HH:mm"));
    if (!time.isValid())
        return tr("The time must be written as hours:minutes or hours:minutes:seconds in 24-hour notation.");
    if (date.year() < 1970 || date.year() > 2199)
        return tr("The year must be between 1970 and 2199.");
    if (!m_available)
        return m_statusMessage;
    if (m_ntp)
        return tr("Switch off automatic time first.");

    const QTimeZone tz(m_timezone.toLatin1());
    const QDateTime target = tz.isValid() ? QDateTime(date, time, tz) : QDateTime(date, time);
    if (!target.isValid())
        return tr("That time does not exist in the time zone %1 (clock change).").arg(m_timezone);
    const qlonglong usec = target.toMSecsSinceEpoch() * 1000;
    call(QStringLiteral("SetTime"), {usec, false, true},
         tr("Clock set to %1").arg(QLocale().toString(target, QLocale::ShortFormat)));
    return QString();
}

// ---- panel clock options ----------------------------------------------------------------------

void DateTimeBackend::watchPanel()
{
    const QString path = settingsFile();
    const QString dir = QFileInfo(path).absolutePath();
    QDir().mkpath(dir);
    if (!m_watcher.directories().contains(dir))
        m_watcher.addPath(dir);
    if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
        m_watcher.addPath(path);
}

void DateTimeBackend::loadPanel()
{
    QSettings s = shellSettings();
    s.sync();
    const bool c24 = s.value(QStringLiteral("Panel/Clock24h"), true).toBool();
    const bool secs = s.value(QStringLiteral("Panel/ClockSeconds"), false).toBool();
    const bool date = s.value(QStringLiteral("Panel/ClockDate"), true).toBool();
    if (c24 == m_clock24h && secs == m_clockSeconds && date == m_clockDate)
        return;
    m_clock24h = c24;
    m_clockSeconds = secs;
    m_clockDate = date;
    Q_EMIT panelChanged();
    Q_EMIT tick();
}

void DateTimeBackend::storePanel(const QString &key, bool value)
{
    QSettings s = shellSettings();
    s.setValue(key, value);
    s.sync();
    if (s.status() != QSettings::NoError)
        Q_EMIT failed(tr("%1 could not be written.").arg(settingsFile()));
    loadPanel();
    watchPanel();
}

void DateTimeBackend::setClock24h(bool on) { if (on != m_clock24h) storePanel(QStringLiteral("Panel/Clock24h"), on); }
void DateTimeBackend::setClockSeconds(bool on) { if (on != m_clockSeconds) storePanel(QStringLiteral("Panel/ClockSeconds"), on); }
void DateTimeBackend::setClockDate(bool on) { if (on != m_clockDate) storePanel(QStringLiteral("Panel/ClockDate"), on); }

QJsonObject DateTimeBackend::toJson() const
{
    const QDateTime t = now();
    return {
        {QStringLiteral("serviceAvailable"), m_available},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("timezone"), m_timezone},
        {QStringLiteral("utcOffsetSeconds"), t.offsetFromUtc()},
        {QStringLiteral("localTime"), t.toString(Qt::ISODate)},
        {QStringLiteral("canNtp"), m_canNtp},
        {QStringLiteral("ntp"), m_ntp},
        {QStringLiteral("ntpSynchronized"), m_ntpSynchronized},
        {QStringLiteral("localRtcKnown"), m_localRtcKnown},
        {QStringLiteral("localRtc"), m_localRtc},
        {QStringLiteral("zoneSource"), m_zoneSource},
        {QStringLiteral("zoneCount"), int(m_zones.size())},
        {QStringLiteral("regions"), QJsonArray::fromStringList(m_regions)},
        {QStringLiteral("clock24h"), m_clock24h},
        {QStringLiteral("clockSeconds"), m_clockSeconds},
        {QStringLiteral("clockDate"), m_clockDate},
        {QStringLiteral("settingsFile"), settingsFile()},
    };
}
