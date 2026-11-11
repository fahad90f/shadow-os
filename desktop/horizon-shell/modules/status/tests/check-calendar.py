#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the calendar part of "horizon-shell --dump status" against Python's
own date arithmetic.

    horizon-shell --dump status | check-calendar.py [expected-first-day]

expected-first-day: ISO number of the day a week starts with in the locale
the shell ran in (1 = Monday ... 7 = Sunday); checked when given.
Exit status 0 when everything agrees; every disagreement is printed.
"""
import datetime
import json
import sys


def main():
    cal = json.load(sys.stdin)["calendar"]
    errors = []

    def expect(what, got, wanted):
        if got != wanted:
            errors.append(f"{what}: got {got!r}, expected {wanted!r}")

    today = datetime.date.fromisoformat(cal["today"])
    expect("ISO week of today", cal["isoWeek"], today.isocalendar()[1])
    first_day = cal["firstDayOfWeek"]
    if len(sys.argv) > 1:
        expect("first day of the week", first_day, int(sys.argv[1]))
    expect("number of day names", len(cal["dayNames"]), 7)

    weeks = cal["weeks"]
    expect("rows", len(weeks), 6)
    days = [datetime.date.fromisoformat(d) for week in weeks for d in week["days"]]
    expect("cells", len(days), 42)
    for previous, day in zip(days, days[1:]):
        expect(f"day after {previous}", day, previous + datetime.timedelta(days=1))
    expect("weekday of the first column", days[0].isoweekday(), first_day)
    month_start = today.replace(day=1)
    if not days[0] <= month_start < days[0] + datetime.timedelta(days=7):
        errors.append(f"the 1st ({month_start}) is not in the first row (starts {days[0]})")
    if today not in days:
        errors.append("today is not in the grid")
    for week in weeks:
        row = [datetime.date.fromisoformat(d) for d in week["days"]]
        thursday = next(d for d in row if d.isoweekday() == 4)
        expect(f"week number of the row starting {row[0]}", week["week"], thursday.isocalendar()[1])

    for error in errors:
        print("FAIL ", error)
    print(f"calendar: locale {cal['locale']}, first day {first_day}, today {cal['today']} (week {cal['isoWeek']}), "
          f"\"{cal['weekday']}, {cal['longDate']}\", \"{cal['monthTitle']}\", {cal['dayNames']}, "
          f"zone {cal['timeZone']} {cal['timeZoneAbbreviation']} {cal['utcOffset']}: "
          + ("OK" if not errors else f"{len(errors)} error(s)"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
