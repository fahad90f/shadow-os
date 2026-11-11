// SPDX-License-Identifier: GPL-3.0-or-later
#include "statuscalendar.h"

#include <QDateTime>
#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>
#include <QVariantMap>

namespace StatusModule {

namespace {

QString capitalised(const QLocale &locale, const QString &text)
{
    return text.isEmpty() ? text : locale.toUpper(text.left(1)) + text.mid(1);
}

// The locale's long date format without the day of the week, which the
// popup shows on a line of its own.
QString dateFormatWithoutWeekday(const QLocale &locale)
{
    // U+060C and U+3001 are the Arabic and the ideographic comma.
    static const QRegularExpression weekday(QStringLiteral("\\s*dddd[,.\\x{060C}\\x{3001}]?\\s*"));
    static const QRegularExpression edges(QStringLiteral("^[\\s,.\\x{060C}\\x{3001}]+|[\\s,\\x{060C}\\x{3001}]+$"));
    QString format = locale.dateFormat(QLocale::LongFormat);
    format.replace(weekday, QStringLiteral(" "));
    format.remove(edges);
    return format;
}

QString offsetText(int seconds)
{
    if (seconds == 0)
        return QStringLiteral("UTC");
    const int minutes = qAbs(seconds) / 60;
    return QStringLiteral("UTC%1%2:%3")
        .arg(seconds < 0 ? QChar(0x2212) : QLatin1Char('+'))
        .arg(minutes / 60, 2, 10, QLatin1Char('0'))
        .arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

CalendarBackend::CalendarBackend(QObject *parent)
    : QObject(parent)
{
    m_midnight.setSingleShot(true);
    connect(&m_midnight, &QTimer::timeout, this, &CalendarBackend::refresh);
    refresh();
}

void CalendarBackend::refresh()
{
    const QDateTime now = QDateTime::currentDateTime();
    if (now.date() != m_today) {
        m_today = now.date();
        Q_EMIT todayChanged();
    }

    const QTimeZone zone = QTimeZone::systemTimeZone();
    const QString id = QString::fromLatin1(zone.id());
    const QString abbreviation = now.timeZoneAbbreviation();
    const QString offset = offsetText(now.offsetFromUtc());
    if (id != m_zoneId || abbreviation != m_zoneAbbreviation || offset != m_utcOffset) {
        m_zoneId = id;
        m_zoneAbbreviation = abbreviation;
        m_utcOffset = offset;
        Q_EMIT timeZoneChanged();
    }
    armMidnightTimer();
}

void CalendarBackend::armMidnightTimer()
{
    const qint64 untilTomorrow = QDateTime::currentDateTime().msecsTo(m_today.addDays(1).startOfDay()) + 1000;
    m_midnight.start(int(qBound(qint64(1000), untilTomorrow, qint64(25) * 60 * 60 * 1000)));
}

QString CalendarBackend::todayWeekday() const
{
    const QLocale locale;
    return capitalised(locale, locale.standaloneDayName(m_today.dayOfWeek(), QLocale::LongFormat));
}

QString CalendarBackend::todayLongDate() const
{
    const QLocale locale;
    return locale.toString(m_today, dateFormatWithoutWeekday(locale));
}

QStringList CalendarBackend::dayNames() const
{
    const QLocale locale;
    QStringList names;
    for (int i = 0; i < 7; ++i)
        names << locale.standaloneDayName((int(locale.firstDayOfWeek()) - 1 + i) % 7 + 1, QLocale::ShortFormat);
    return names;
}

QStringList CalendarBackend::longDayNames() const
{
    const QLocale locale;
    QStringList names;
    for (int i = 0; i < 7; ++i)
        names << locale.standaloneDayName((int(locale.firstDayOfWeek()) - 1 + i) % 7 + 1, QLocale::LongFormat);
    return names;
}

QVariantList CalendarBackend::weeks(int year, int month) const
{
    QVariantList rows;
    const QDate first(year, month, 1);
    if (!first.isValid())
        return rows;
    const QLocale locale;
    const QList<Qt::DayOfWeek> workdays = locale.weekdays();
    const int lead = (first.dayOfWeek() - int(locale.firstDayOfWeek()) + 7) % 7;
    QDate day = first.addDays(-lead);
    for (int row = 0; row < 6; ++row) {
        // ISO 8601 numbers weeks from Monday; a week belongs to the year its
        // Thursday is in. A row that does not start on Monday straddles two
        // such weeks, and its Thursday names the one most of it lies in.
        const int week = day.addDays((int(Qt::Thursday) - day.dayOfWeek() + 7) % 7).weekNumber();
        QVariantList days;
        for (int column = 0; column < 7; ++column, day = day.addDays(1)) {
            days.append(QVariantMap{
                {QStringLiteral("year"), day.year()},
                {QStringLiteral("month"), day.month()},
                {QStringLiteral("day"), day.day()},
                {QStringLiteral("inMonth"), day.month() == month},
                {QStringLiteral("today"), day == m_today},
                {QStringLiteral("weekend"), !workdays.contains(Qt::DayOfWeek(day.dayOfWeek()))},
                {QStringLiteral("label"), locale.toString(day, QLocale::LongFormat)},
            });
        }
        rows.append(QVariantMap{{QStringLiteral("week"), week}, {QStringLiteral("days"), days}});
    }
    return rows;
}

QString CalendarBackend::monthTitle(int year, int month) const
{
    QLocale locale;
    locale.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    return capitalised(locale, locale.standaloneMonthName(month, QLocale::LongFormat))
           + QLatin1Char(' ') + locale.toString(year);
}

QJsonObject CalendarBackend::toJson() const
{
    const QLocale locale;
    return {
        {QStringLiteral("locale"), locale.name()},
        {QStringLiteral("today"), m_today.toString(Qt::ISODate)},
        {QStringLiteral("weekday"), todayWeekday()},
        {QStringLiteral("longDate"), todayLongDate()},
        {QStringLiteral("isoWeek"), todayWeekNumber()},
        {QStringLiteral("firstDayOfWeek"), int(locale.firstDayOfWeek())},
        {QStringLiteral("dayNames"), QJsonArray::fromStringList(dayNames())},
        {QStringLiteral("monthTitle"), monthTitle(m_today.year(), m_today.month())},
        {QStringLiteral("timeZone"), m_zoneId},
        {QStringLiteral("timeZoneAbbreviation"), m_zoneAbbreviation},
        {QStringLiteral("utcOffset"), m_utcOffset},
        {QStringLiteral("weeks"), weeksJson(m_today.year(), m_today.month())},
    };
}

// The grid of a month as the popup shows it, dates in ISO form.
QJsonArray CalendarBackend::weeksJson(int year, int month) const
{
    QJsonArray out;
    const QVariantList rows = weeks(year, month);
    for (const QVariant &row : rows) {
        const QVariantMap map = row.toMap();
        QJsonArray dates;
        const QVariantList days = map.value(QStringLiteral("days")).toList();
        for (const QVariant &entry : days) {
            const QVariantMap day = entry.toMap();
            dates.append(QDate(day.value(QStringLiteral("year")).toInt(), day.value(QStringLiteral("month")).toInt(),
                               day.value(QStringLiteral("day")).toInt()).toString(Qt::ISODate));
        }
        out.append(QJsonObject{{QStringLiteral("week"), map.value(QStringLiteral("week")).toInt()},
                               {QStringLiteral("days"), dates}});
    }
    return out;
}

} // namespace StatusModule
