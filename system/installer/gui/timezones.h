// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

// Time zones for the installed system, from the tz database of the running
// system (package tzdata):
//
//   /usr/share/zoneinfo/zone.tab       one line per country and zone (country
//                                      code, coordinates, zone name, comment);
//                                      zone1970.tab is used when it is missing
//   /usr/share/zoneinfo/iso3166.tab    country code → country name
//   /usr/share/zoneinfo/<zone>         the zone itself — an entry is only
//                                      offered if this file exists, which is
//                                      also what the engine checks
//
// UTC offsets and the clock preview are computed by QTimeZone from those
// zone files, for the current moment.
class TimeZones : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QVariantList regions READ regions CONSTANT)      // [{ key, name, count }]
    Q_PROPERTY(int count READ count CONSTANT)
    Q_PROPERTY(QString sourceFile READ sourceFile CONSTANT)
    Q_PROPERTY(QString problem READ problem CONSTANT)
    // Zone of the running system (the live session's), if it is in the list.
    Q_PROPERTY(QString systemZone READ systemZone CONSTANT)

public:
    explicit TimeZones(QObject *parent = nullptr);

    QVariantList regions() const { return m_regions; }
    int count() const { return int(m_zones.size()); }
    QString sourceFile() const { return m_sourceFile; }
    QString problem() const { return m_problem; }
    QString systemZone() const { return m_systemZone; }

    // Zones of one region, or — when 'needle' is not empty — every zone whose
    // city, country or name matches it. [{ id, city, detail, region, offsetText }]
    Q_INVOKABLE QVariantList zones(const QString &region, const QString &needle) const;
    Q_INVOKABLE QVariantList zonesOfTerritory(const QString &territory) const;
    Q_INVOKABLE bool contains(const QString &id) const { return m_index.contains(id); }
    Q_INVOKABLE QVariantMap describe(const QString &id) const;
    Q_INVOKABLE QString regionOf(const QString &id) const;
    Q_INVOKABLE QString countryName(const QString &territory) const { return m_countries.value(territory.toUpper()); }
    // The wall-clock time in a zone right now, e.g. "Tuesday, 10 November 2026, 23:14".
    Q_INVOKABLE QString timeIn(const QString &id) const;
    // The zone of a country that has exactly one, or one "main" zone according
    // to the tz database's own comments; empty otherwise. A suggestion only.
    Q_INVOKABLE QString suggestionForTerritory(const QString &territory) const;

    QJsonObject toJson() const;

private:
    struct Zone
    {
        QString id;
        QString region;
        QString city;
        QString detail;         // country and the tz database's comment
        QString comment;
        QStringList territories;
        QString search;
    };
    QVariantMap toMap(const Zone &zone) const;
    static QString offsetText(const QString &id);

    QList<Zone> m_zones;
    QHash<QString, int> m_index;                // id → position in m_zones
    QHash<QString, QString> m_countries;        // "DE" → "Germany"
    QVariantList m_regions;
    QString m_sourceFile;
    QString m_problem;
    QString m_systemZone;
};
