// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QUrl>

class QDateTime;

// Small helpers shared by the back-ends.
namespace Util {

// IEC byte count, same format as Theme.formatBytes() in QML ("1.50 MiB").
QString formatBytes(qulonglong bytes);

// Path for local files, otherwise the URL without password.
QString displayPath(const QUrl &url);

// Name of the last path segment; host or scheme root for top-level URLs
// ("/" → "/", sftp://host/ → "host", trash:/ → "Trash").
QString displayName(const QUrl &url);

// The directory containing `url` (no trailing slash except for the root).
QUrl parentUrl(const QUrl &url);

// `dir` + "/" + `name`
QUrl childUrl(const QUrl &dir, const QString &name);

// Same location ignoring a trailing slash.
bool sameUrl(const QUrl &a, const QUrl &b);

// URL from user input or a command line argument: absolute and relative
// local paths, "~", and URLs with a scheme.
QUrl urlFromUserInput(const QString &input, const QString &workingDirectory = QString());

// "Today, 14:02" / locale short date for older dates; empty for invalid dates.
QString formatDateTime(const QDateTime &dt);

} // namespace Util
