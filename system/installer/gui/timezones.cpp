// SPDX-License-Identifier: GPL-3.0-or-later
#include "timezones.h"

#include <QCollator>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QMap>
#include <QTimeZone>

#include <algorithm>

namespace {

const QString kZoneDir = QStringLiteral("/usr/share/zoneinfo");

QString pretty(QString text)
{
    return text.replace(QLatin1Char('_'), QLatin1Char(' '));
}

QString regionName(const QString &key)
{
    if (key == QLatin1String("America"))
        return TimeZones::tr("Americas");
    if (key == QLatin1String("Atlantic"))
        return TimeZones::tr("Atlantic Ocean");
    if (key == QLatin1String("Indian"))
        return TimeZones::tr("Indian Ocean");
    if (key == QLatin1String("Pacific"))
        return TimeZones::tr("Pacific Ocean");
    if (key == QLatin1String("Etc"))
        return TimeZones::tr("UTC");
    return key;
}

} // namespace

TimeZones::TimeZones(QObject *parent)
    : QObject(parent)
{
    // Country names.
    QFile countries(kZoneDir + QStringLiteral("/iso3166.tab"));
    if (countries.open(QIODevice::ReadOnly)) {
        while (!countries.atEnd()) {
            const QString line = QString::fromUtf8(countries.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            const QStringList fields = line.split(QLatin1Char('\t'));
            if (fields.size() >= 2)
                m_countries.insert(fields.at(0), fields.at(1));
        }
    }

    QFile table;
    for (const QString &name : {QStringLiteral("/zone.tab"), QStringLiteral("/zone1970.tab")}) {
        table.setFileName(kZoneDir + name);
        if (table.open(QIODevice::ReadOnly)) {
            m_sourceFile = table.fileName();
            break;
        }
    }
    if (!table.isOpen()) {
        m_problem = tr("The time zone table %1 is missing (package “tzdata”). The installed system will use UTC; "
                       "the time zone can be changed afterwards in the Control Center.")
                        .arg(kZoneDir + QStringLiteral("/zone.tab"));
        return;
    }

    const auto add = [this](const QString &id, const QStringList &territories, const QString &comment) {
        if (m_index.contains(id) || !QFileInfo(kZoneDir + QLatin1Char('/') + id).isFile())
            return;
        Zone zone;
        zone.id = id;
        zone.region = id.section(QLatin1Char('/'), 0, 0);
        zone.city = pretty(id.section(QLatin1Char('/'), -1));
        // "America/Argentina/Salta" → city "Salta", middle part goes to the detail
        const QString middle = id.count(QLatin1Char('/')) == 2 ? pretty(id.section(QLatin1Char('/'), 1, 1)) : QString();
        zone.territories = territories;
        zone.comment = comment;
        QStringList countryNames;
        for (const QString &code : territories) {
            const QString name = m_countries.value(code);
            countryNames << (name.isEmpty() ? code : name);
        }
        QStringList detail;
        if (!countryNames.isEmpty())
            detail << countryNames.join(QStringLiteral(", "));
        else if (!middle.isEmpty())
            detail << middle;
        if (!comment.isEmpty())
            detail << comment;
        zone.detail = detail.join(QStringLiteral(" — "));
        zone.search = (zone.city + QLatin1Char(' ') + zone.detail + QLatin1Char(' ') + id + QLatin1Char(' ')
                       + pretty(id)).toCaseFolded();
        m_index.insert(id, int(m_zones.size()));
        m_zones.append(zone);
    };

    while (!table.atEnd()) {
        const QString line = QString::fromUtf8(table.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() < 3)
            continue;
        add(fields.at(2), fields.at(0).split(QLatin1Char(','), Qt::SkipEmptyParts), fields.value(3));
    }
    // Not a place, so not in the table — but a real zone file and the engine's default.
    add(QStringLiteral("Etc/UTC"), {}, tr("Coordinated Universal Time, no daylight saving"));

    if (m_zones.isEmpty()) {
        m_problem = tr("%1 lists no time zone that exists under %2.").arg(m_sourceFile, kZoneDir);
        return;
    }

    QCollator collator(QLocale(QLocale::English));
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(m_zones.begin(), m_zones.end(), [&collator](const Zone &a, const Zone &b) {
        const int c = collator.compare(a.city, b.city);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    m_index.clear();
    QMap<QString, int> counts;
    for (int i = 0; i < m_zones.size(); ++i) {
        m_index.insert(m_zones.at(i).id, i);
        ++counts[m_zones.at(i).region];
    }
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        if (it.key() == QLatin1String("Etc"))
            continue;
        m_regions.append(QVariantMap{{QStringLiteral("key"), it.key()},
                                     {QStringLiteral("name"), regionName(it.key())},
                                     {QStringLiteral("count"), it.value()}});
    }
    if (counts.contains(QStringLiteral("Etc")))
        m_regions.append(QVariantMap{{QStringLiteral("key"), QStringLiteral("Etc")},
                                     {QStringLiteral("name"), regionName(QStringLiteral("Etc"))},
                                     {QStringLiteral("count"), counts.value(QStringLiteral("Etc"))}});

    QString system = QString::fromLatin1(QTimeZone::systemTimeZoneId());
    if (system == QLatin1String("UTC"))
        system = QStringLiteral("Etc/UTC");
    if (m_index.contains(system))
        m_systemZone = system;
}

QString TimeZones::offsetText(const QString &id)
{
    const QTimeZone zone(id.toLatin1());
    if (!zone.isValid())
        return QString();
    const int seconds = zone.offsetFromUtc(QDateTime::currentDateTimeUtc());
    const int minutes = qAbs(seconds) / 60;
    return QStringLiteral("UTC%1%2:%3")
        .arg(seconds < 0 ? QStringLiteral("−") : QStringLiteral("+"))
        .arg(minutes / 60, 2, 10, QLatin1Char('0'))
        .arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

QVariantMap TimeZones::toMap(const Zone &zone) const
{
    return {
        {QStringLiteral("id"), zone.id},
        {QStringLiteral("city"), zone.city},
        {QStringLiteral("detail"), zone.detail},
        {QStringLiteral("region"), zone.region},
        {QStringLiteral("offsetText"), offsetText(zone.id)},
    };
}

QVariantList TimeZones::zones(const QString &region, const QString &needle) const
{
    const QStringList words = needle.trimmed().toCaseFolded().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QVariantList out;
    for (const Zone &zone : m_zones) {
        if (words.isEmpty()) {
            if (zone.region != region)
                continue;
        } else if (!std::all_of(words.cbegin(), words.cend(),
                                [&zone](const QString &w) { return zone.search.contains(w); })) {
            continue;
        }
        out.append(toMap(zone));
    }
    return out;
}

QVariantList TimeZones::zonesOfTerritory(const QString &territory) const
{
    QVariantList out;
    const QString code = territory.toUpper();
    if (code.isEmpty())
        return out;
    for (const Zone &zone : m_zones) {
        if (zone.territories.contains(code))
            out.append(toMap(zone));
    }
    return out;
}

QVariantMap TimeZones::describe(const QString &id) const
{
    const auto it = m_index.constFind(id);
    return it == m_index.cend() ? QVariantMap() : toMap(m_zones.at(*it));
}

QString TimeZones::regionOf(const QString &id) const
{
    const auto it = m_index.constFind(id);
    return it == m_index.cend() ? QString() : m_zones.at(*it).region;
}

QString TimeZones::timeIn(const QString &id) const
{
    const QTimeZone zone(id.toLatin1());
    if (!zone.isValid())
        return QString();
    const QDateTime now = QDateTime::currentDateTimeUtc().toTimeZone(zone);
    return QLocale(QLocale::English, QLocale::UnitedKingdom).toString(now, QStringLiteral("dddd, d MMMM yyyy, HH:mm"));
}

QString TimeZones::suggestionForTerritory(const QString &territory) const
{
    const QString code = territory.toUpper();
    if (code.isEmpty())
        return QString();
    QStringList all, main;
    for (const Zone &zone : m_zones) {
        if (zone.territories.isEmpty() || zone.territories.constFirst() != code)
            continue;
        all << zone.id;
        const QString comment = zone.comment.toCaseFolded();
        if (comment.startsWith(QLatin1String("most of")) || comment.contains(QLatin1String("(mainland)"))
            || comment.contains(QLatin1String("(most areas)")))
            main << zone.id;
    }
    if (all.size() == 1)
        return all.constFirst();
    return main.size() == 1 ? main.constFirst() : QString();
}

QJsonObject TimeZones::toJson() const
{
    QJsonArray zones;
    for (const Zone &zone : m_zones) {
        zones.append(QJsonObject{
            {QStringLiteral("id"), zone.id},
            {QStringLiteral("city"), zone.city},
            {QStringLiteral("detail"), zone.detail},
            {QStringLiteral("territories"), QJsonArray::fromStringList(zone.territories)},
            {QStringLiteral("offset"), offsetText(zone.id)},
        });
    }
    return {
        {QStringLiteral("source"), m_sourceFile},
        {QStringLiteral("problem"), m_problem},
        {QStringLiteral("systemZone"), m_systemZone},
        {QStringLiteral("regions"), QJsonArray::fromVariantList(m_regions)},
        {QStringLiteral("zones"), zones},
    };
}
