// SPDX-License-Identifier: GPL-3.0-or-later
#include "util.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QLocale>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace Util {

QString formatBytes(qulonglong bytes)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = double(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        ++i;
    }
    const int decimals = i == 0 ? 0 : (v < 10.0 ? 2 : 1);
    return QString::number(v, 'f', decimals) + u' ' + QLatin1String(units[i]);
}

QString displayPath(const QUrl &url)
{
    if (url.isLocalFile())
        return url.toLocalFile();
    return url.toDisplayString(QUrl::RemovePassword | QUrl::PreferLocalFile);
}

QString displayName(const QUrl &url)
{
    const QString name = url.adjusted(QUrl::StripTrailingSlash).fileName();
    if (!name.isEmpty())
        return name;
    if (url.isLocalFile())
        return u"/"_s;
    if (url.scheme() == "trash"_L1)
        return QCoreApplication::translate("Util", "Trash");
    if (!url.host().isEmpty())
        return url.host();
    return url.scheme() + u":/"_s;
}

QUrl parentUrl(const QUrl &url)
{
    QUrl u = url.adjusted(QUrl::StripTrailingSlash);
    u = u.adjusted(QUrl::RemoveFilename);
    if (u.path().size() > 1)
        u = u.adjusted(QUrl::StripTrailingSlash);
    if (u.path().isEmpty())
        u.setPath(u"/"_s);
    return u;
}

QUrl childUrl(const QUrl &dir, const QString &name)
{
    QUrl u(dir);
    QString path = u.path();
    if (!path.endsWith(u'/'))
        path += u'/';
    u.setPath(path + name);
    return u;
}

bool sameUrl(const QUrl &a, const QUrl &b)
{
    return a.matches(b, QUrl::StripTrailingSlash | QUrl::NormalizePathSegments);
}

QUrl urlFromUserInput(const QString &input, const QString &workingDirectory)
{
    QString text = input.trimmed();
    if (text.isEmpty())
        return {};
    if (text == u'~' || text.startsWith("~/"_L1))
        text = QDir::homePath() + text.mid(1);
    if (text.startsWith(u'/'))
        return QUrl::fromLocalFile(QDir::cleanPath(text));
    // "scheme:/..." — but not a relative file name that happens to contain a colon.
    static const QRegularExpression schemePrefix(u"^[A-Za-z][A-Za-z0-9+.-]+:/"_s);
    if (schemePrefix.match(text).hasMatch()) {
        const QUrl url(text, QUrl::TolerantMode);
        if (url.isValid() && !url.scheme().isEmpty()) {
            if (url.isLocalFile())
                return QUrl::fromLocalFile(QDir::cleanPath(url.toLocalFile()));
            QUrl u(url);
            if (u.path().isEmpty())
                u.setPath(u"/"_s);
            return u;
        }
    }
    const QString base = workingDirectory.isEmpty() ? QDir::currentPath() : workingDirectory;
    return QUrl::fromLocalFile(QDir::cleanPath(QDir(base).absoluteFilePath(text)));
}

QString formatDateTime(const QDateTime &dt)
{
    if (!dt.isValid())
        return {};
    const QDateTime local = dt.toLocalTime();
    const QDate today = QDate::currentDate();
    const QLocale locale;
    // Hours and minutes are enough in a file list; some locales (and "C")
    // include seconds in their short time format.
    QString timeFormat = locale.timeFormat(QLocale::ShortFormat);
    timeFormat.remove(u":ss"_s).remove(u".ss"_s);
    const QString time = locale.toString(local.time(), timeFormat);
    const qint64 days = local.date().daysTo(today);
    if (days == 0)
        return QCoreApplication::translate("Util", "Today, %1").arg(time);
    if (days == 1)
        return QCoreApplication::translate("Util", "Yesterday, %1").arg(time);
    return locale.toString(local.date(), QLocale::ShortFormat) + u", "_s + time;
}

} // namespace Util
