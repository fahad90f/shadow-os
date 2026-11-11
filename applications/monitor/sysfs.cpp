// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysfs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <chrono>

#include <fcntl.h>
#include <unistd.h>

namespace Sys {

double monotonicSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool readFile(const char *path, QByteArray &out, qsizetype limit)
{
    out.resize(0);
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char chunk[16384];
    bool ok = true;
    while (out.size() < limit) {
        const ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (n == 0)
            break;
        out.append(chunk, qMin<qsizetype>(n, limit - out.size()));
    }
    ::close(fd);
    return ok;
}

QByteArray readFile(const QString &path, qsizetype limit)
{
    QByteArray out;
    if (!readFile(QFile::encodeName(path).constData(), out, limit))
        out.clear();
    return out;
}

qsizetype readOnce(const char *path, char *buffer, qsizetype capacity)
{
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n;
    do {
        n = ::read(fd, buffer, size_t(capacity - 1));
    } while (n < 0 && errno == EINTR);
    ::close(fd);
    if (n < 0)
        return -1;
    buffer[n] = '\0';
    return n;
}

QString readText(const QString &path)
{
    return QString::fromUtf8(readFile(path, 65536)).trimmed();
}

std::optional<qint64> readInteger(const QString &path)
{
    const QByteArray data = readFile(path, 64).trimmed();
    if (data.isEmpty())
        return std::nullopt;
    bool ok = false;
    const qint64 value = data.toLongLong(&ok, 10);
    if (!ok)
        return std::nullopt;
    return value;
}

bool exists(const QString &path)
{
    return QFileInfo::exists(path);
}

bool naturalLess(const QString &a, const QString &b)
{
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a.at(i).isDigit() && b.at(j).isDigit()) {
            qsizetype si = i, sj = j;
            while (i < a.size() && a.at(i).isDigit())
                ++i;
            while (j < b.size() && b.at(j).isDigit())
                ++j;
            // Compare the digit runs as numbers: strip leading zeros, then by length, then by text.
            while (si < i - 1 && a.at(si) == QLatin1Char('0'))
                ++si;
            while (sj < j - 1 && b.at(sj) == QLatin1Char('0'))
                ++sj;
            if (i - si != j - sj)
                return i - si < j - sj;
            const int c = QStringView(a).mid(si, i - si).compare(QStringView(b).mid(sj, j - sj));
            if (c != 0)
                return c < 0;
        } else {
            if (a.at(i) != b.at(j))
                return a.at(i) < b.at(j);
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

QStringList listDirectory(const QString &path)
{
    QStringList names = QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System, QDir::NoSort);
    std::sort(names.begin(), names.end(), naturalLess);
    return names;
}

QString canonicalPath(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

QString linkName(const QString &path)
{
    return canonicalPath(path).section(QLatin1Char('/'), -1);
}

} // namespace Sys
