// SPDX-License-Identifier: GPL-3.0-or-later
#include "aboutbackend.h"

#include "dumpregistry.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#include <sys/utsname.h>

NEXUS_CC_DUMP("about", [] { return QJsonValue(AboutBackend().toJson()); })

AboutBackend::AboutBackend(QObject *parent)
    : QObject(parent)
{
    QFile f(QStringLiteral("/etc/os-release"));
    if (!f.open(QIODevice::ReadOnly)) {
        f.setFileName(QStringLiteral("/usr/lib/os-release"));
        f.open(QIODevice::ReadOnly);
    }
    if (f.isOpen()) {
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0 || line.startsWith(QLatin1Char('#')))
                continue;
            QString value = line.mid(eq + 1);
            if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))))
                value = value.mid(1, value.size() - 2);
            m_values.insert(line.left(eq), value);
        }
    }

    struct utsname u {};
    if (uname(&u) == 0) {
        m_kernel = QString::fromLocal8Bit(u.release);
        m_architecture = QString::fromLocal8Bit(u.machine);
        m_hostname = QString::fromLocal8Bit(u.nodename);
    }
}

bool AboutBackend::systemInformationAvailable() const
{
    return !QStandardPaths::findExecutable(QStringLiteral("nexus-sysinfo")).isEmpty();
}

bool AboutBackend::openSystemInformation() const
{
    return QProcess::startDetached(QStringLiteral("nexus-sysinfo"), {});
}

QJsonObject AboutBackend::toJson() const
{
    return {
        {QStringLiteral("osName"), osName()},
        {QStringLiteral("osVersion"), osVersion()},
        {QStringLiteral("osCodename"), osCodename()},
        {QStringLiteral("osChannel"), osChannel()},
        {QStringLiteral("baseSystem"), baseSystem()},
        {QStringLiteral("kernel"), kernel()},
        {QStringLiteral("architecture"), architecture()},
        {QStringLiteral("hostname"), hostname()},
    };
}
