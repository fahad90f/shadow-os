// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

namespace StatusModule {

// Calendar arithmetic and locale data for the calendar popup. Everything is
// computed with QDate/QLocale/QTimeZone for the user's locale (LC_TIME) and
// the system time zone; the popup itself only lays the result out.
//
// Dates cross into QML as plain year/month/day numbers, so no time-zone
// conversion of JavaScript Date objects can shift a day.
class CalendarBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int todayYear READ todayYear NOTIFY todayChanged)
    Q_PROPERTY(int todayMonth READ todayMonth NOTIFY todayChanged)
    Q_PROPERTY(int todayDay READ todayDay NOTIFY todayChanged)
    // "Tuesday", "10 November 2026" — in the locale's words and order.
    Q_PROPERTY(QString todayWeekday READ todayWeekday NOTIFY todayChanged)
    Q_PROPERTY(QString todayLongDate READ todayLongDate NOTIFY todayChanged)
    Q_PROPERTY(int todayWeekNumber READ todayWeekNumber NOTIFY todayChanged)
    // Column headers, starting with the locale's first day of the week.
    Q_PROPERTY(QStringList dayNames READ dayNames CONSTANT)
    Q_PROPERTY(QStringList longDayNames READ longDayNames CONSTANT)
    // "Europe/Berlin", "CET", "UTC+01:00"
    Q_PROPERTY(QString timeZoneId READ timeZoneId NOTIFY timeZoneChanged)
    Q_PROPERTY(QString timeZoneAbbreviation READ timeZoneAbbreviation NOTIFY timeZoneChanged)
    Q_PROPERTY(QString utcOffset READ utcOffset NOTIFY timeZoneChanged)

public:
    explicit CalendarBackend(QObject *parent = nullptr);

    int todayYear() const { return m_today.year(); }
    int todayMonth() const { return m_today.month(); }
    int todayDay() const { return m_today.day(); }
    QString todayWeekday() const;
    QString todayLongDate() const;
    int todayWeekNumber() const { return m_today.weekNumber(); }
    QStringList dayNames() const;
    QStringList longDayNames() const;
    QString timeZoneId() const { return m_zoneId; }
    QString timeZoneAbbreviation() const { return m_zoneAbbreviation; }
    QString utcOffset() const { return m_utcOffset; }

    // The six weeks of the grid for a month:
    //   [{ week: <ISO 8601 week number>,
    //      days: [{ year, month, day, inMonth, today, weekend, label }, × 7] }, × 6]
    // 'label' is the full date for screen readers and tooltips. 'today'
    // is as of the call: a binding must also depend on todayDay.
    Q_INVOKABLE QVariantList weeks(int year, int month) const;
    // "November 2026"
    Q_INVOKABLE QString monthTitle(int year, int month) const;
    // Re-reads the date and the time zone (they change behind a sleeping
    // timer when the machine was suspended or the zone was changed).
    Q_INVOKABLE void refresh();

    QJsonObject toJson() const;
    QJsonArray weeksJson(int year, int month) const;

Q_SIGNALS:
    void todayChanged();
    void timeZoneChanged();

private:
    void armMidnightTimer();

    QDate m_today;
    QString m_zoneId;
    QString m_zoneAbbreviation;
    QString m_utcOffset;
    QTimer m_midnight;
};

} // namespace StatusModule
