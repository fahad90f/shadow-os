// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

// Small helpers for the kernel's pseudo file systems (/proc and /sys).
//
// Their files report a size of 0 and produce the content on demand, so they
// are read with plain read(2) until EOF instead of through QFile's size-based
// logic. Every function fails softly: a missing or unreadable attribute is an
// empty result, never an error — whether a value exists is part of the data.
namespace Sys {

// Monotonic clock in seconds: the time base of every rate the samplers compute.
double monotonicSeconds();

// Whole content of 'path' into 'out' (its capacity is reused between calls).
bool readFile(const char *path, QByteArray &out, qsizetype limit = 1 << 20);
QByteArray readFile(const QString &path, qsizetype limit = 1 << 20);

// One read(2) into a caller-provided buffer, NUL-terminated. For files the
// kernel produces in a single piece (/proc/<pid>/stat, status, cmdline).
// Returns the number of bytes read, or -1.
qsizetype readOnce(const char *path, char *buffer, qsizetype capacity);

// Trimmed UTF-8 text, or an empty string.
QString readText(const QString &path);

// The decimal integer stored in a sysfs attribute; empty if the attribute is
// missing, unreadable (some sensors return an error while idle) or not a number.
std::optional<qint64> readInteger(const QString &path);

bool exists(const QString &path);

// Entry names of a directory without "." and "..", in natural order
// ("hwmon2" before "hwmon10").
QStringList listDirectory(const QString &path);

// Resolved path of a sysfs entry, and the last component of it (used for
// <device>/driver and <class>/<name>/device links). Empty if it does not exist.
QString canonicalPath(const QString &path);
QString linkName(const QString &path);

// Natural ("numeric aware") string order.
bool naturalLess(const QString &a, const QString &b);

} // namespace Sys
