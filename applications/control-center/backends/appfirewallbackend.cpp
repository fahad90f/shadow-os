// SPDX-License-Identifier: GPL-3.0-or-later
#include "appfirewallbackend.h"

#include "dumpregistry.h"

#include <command.h>

#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <unistd.h>

SHADOW_CC_DUMP("appfirewall", [] {
    AppFirewallBackend backend;
    if (!backend.ready()) {
        QEventLoop loop;
        QObject::connect(&backend, &AppFirewallBackend::changed, &loop, [&] { if (backend.ready()) loop.quit(); });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
// The pattern of shadow-appfw (APP_ID): the name of a launcher entry.
bool validId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]{0,199}$"));
    return re.match(id).hasMatch();
}

QString failureText(const Shadow::CommandResult &result)
{
    if (geteuid() != 0) {
        if (result.exitCode == -1)
            return AppFirewallBackend::tr("pkexec could not be started. Is polkit installed?");
        if (result.exitCode == 126)
            return AppFirewallBackend::tr("The request was not authorised.");
        if (result.exitCode == 127)
            return AppFirewallBackend::tr("Authentication failed: polkit is not running, or no authentication agent is running in this session.");
    }
    if (result.exitCode == -2)
        return AppFirewallBackend::tr("The per-application firewall did not answer in time.");
    QString text = (result.errText().trimmed().isEmpty() ? result.outText() : result.errText()).trimmed();
    // The last line is the error; "Note:" lines may come before it.
    text = text.section(QLatin1Char('\n'), -1);
    if (text.startsWith(QLatin1String("shadow-appfw: ")))
        text = text.mid(14);
    return text.isEmpty() ? AppFirewallBackend::tr("The per-application firewall failed (exit code %1).").arg(result.exitCode)
                          : text;
}
} // namespace

AppFirewallBackend::AppFirewallBackend(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QString AppFirewallBackend::toolPath() const
{
    const QString override = qEnvironmentVariable("SHADOW_APPFW_TOOL");
    return override.isEmpty() ? QStringLiteral("/usr/sbin/shadow-appfw") : override;
}

void AppFirewallBackend::finishRefresh()
{
    m_ready = true;
    Q_EMIT changed();
}

void AppFirewallBackend::refresh()
{
    const int generation = ++m_generation;
    m_installed = QFile::exists(toolPath());
    if (!m_installed) {
        m_applications.clear();
        m_blockedCount = 0;
        m_statusMessage = tr("The per-application firewall (%1) is not installed.").arg(toolPath());
        finishRefresh();
        return;
    }
    // Both answer an ordinary user; nothing here needs a password.
    Shadow::Command::run(this, toolPath(), {QStringLiteral("status"), QStringLiteral("--json")},
                        [this, generation](const Shadow::CommandResult &statusResult) {
        if (generation != m_generation)
            return;
        const QJsonObject status = QJsonDocument::fromJson(statusResult.out).object();
        Shadow::Command::run(this, toolPath(), {QStringLiteral("list"), QStringLiteral("--json")},
                            [this, generation, status, statusResult](const Shadow::CommandResult &listResult) {
            if (generation != m_generation)
                return;
            const QJsonObject list = QJsonDocument::fromJson(listResult.out).object();
            if (!listResult.ok() || !list.contains(QStringLiteral("applications"))) {
                m_applications.clear();
                m_blockedCount = 0;
                m_statusMessage = failureText(listResult.ok() ? statusResult : listResult);
                finishRefresh();
                return;
            }
            m_enabled = status.value(QStringLiteral("enabled")).toBool(true);
            m_rejectedPackets = status.value(QStringLiteral("rejected_packets")).toInt();
            QSet<QString> notConfined, running;
            const QJsonArray loose = status.value(QStringLiteral("not_confined")).toArray();
            for (const QJsonValue &value : loose)
                notConfined.insert(value.toString());
            const QJsonArray scopes = status.value(QStringLiteral("running")).toArray();
            for (const QJsonValue &value : scopes)
                running.insert(value.toObject().value(QStringLiteral("app")).toString());

            m_applications.clear();
            m_blockedCount = 0;
            const QJsonArray rows = list.value(QStringLiteral("applications")).toArray();
            for (const QJsonValue &value : rows) {
                const QJsonObject o = value.toObject();
                const QString id = o.value(QStringLiteral("id")).toString();
                if (!validId(id))
                    continue;
                const bool blocked = o.value(QStringLiteral("mode")).toString() == QLatin1String("deny");
                m_blockedCount += blocked ? 1 : 0;
                // A running scope is named after the id with the characters a
                // unit name cannot hold replaced, as shadow-appfw does.
                QString unit = id;
                unit.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.]")), QStringLiteral("_"));
                m_applications.append(QVariantMap{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("name"), o.value(QStringLiteral("name")).toString(id)},
                    {QStringLiteral("icon"), o.value(QStringLiteral("icon")).toString()},
                    {QStringLiteral("hidden"), o.value(QStringLiteral("hidden")).toBool()},
                    {QStringLiteral("blocked"), blocked},
                    {QStringLiteral("missing"), o.value(QStringLiteral("missing")).toBool()},
                    {QStringLiteral("notConfined"), blocked && notConfined.contains(id)},
                    {QStringLiteral("running"), running.contains(unit)}});
            }
            // What the tool could not read (its configuration file) is said, not hidden.
            m_statusMessage = list.value(QStringLiteral("error")).toString();
            finishRefresh();
        }, 30000);
    }, 30000);
}

void AppFirewallBackend::setBlocked(const QString &application, bool blocked)
{
    if (m_busy || !m_installed)
        return;
    if (!validId(application)) {
        Q_EMIT failed(tr("\"%1\" is not the name of an application.").arg(application));
        return;
    }
    QString name = application;
    for (const QVariant &row : std::as_const(m_applications)) {
        const QVariantMap map = row.toMap();
        if (map.value(QStringLiteral("id")).toString() == application)
            name = map.value(QStringLiteral("name")).toString();
    }
    const QStringList arguments{QStringLiteral("set"), application,
                                blocked ? QStringLiteral("deny") : QStringLiteral("allow")};
    const QString done = blocked
        ? tr("%1 has no network access from the next time it is started.").arg(name)
        : tr("%1 may use the network again from the next time it is started.").arg(name);
    m_busy = true;
    Q_EMIT busyChanged();
    const auto finished = [this, done](const Shadow::CommandResult &result) {
        m_busy = false;
        Q_EMIT busyChanged();
        if (result.ok())
            Q_EMIT succeeded(done);
        else
            Q_EMIT failed(failureText(result));
        refresh();
    };
    // Root runs the tool itself; everybody else goes through polkit.
    if (geteuid() == 0)
        Shadow::Command::run(this, toolPath(), arguments, finished, 300000);
    else
        Shadow::Command::run(this, QStringLiteral("pkexec"), QStringList{toolPath()} + arguments, finished, 300000);
}

QVariantList AppFirewallBackend::filtered(const QString &text, bool withHidden) const
{
    const QString needle = text.trimmed();
    QVariantList rows;
    for (const QVariant &row : m_applications) {
        const QVariantMap map = row.toMap();
        // A blocked application is always listed: it must be possible to allow it again.
        if (map.value(QStringLiteral("hidden")).toBool() && !withHidden && !map.value(QStringLiteral("blocked")).toBool())
            continue;
        if (!needle.isEmpty()
            && !map.value(QStringLiteral("name")).toString().contains(needle, Qt::CaseInsensitive)
            && !map.value(QStringLiteral("id")).toString().contains(needle, Qt::CaseInsensitive))
            continue;
        rows.append(row);
    }
    return rows;
}

QJsonObject AppFirewallBackend::toJson() const
{
    return {{QStringLiteral("installed"), m_installed},
            {QStringLiteral("enabled"), m_enabled},
            {QStringLiteral("statusMessage"), m_statusMessage},
            {QStringLiteral("blockedCount"), m_blockedCount},
            {QStringLiteral("rejectedPackets"), m_rejectedPackets},
            {QStringLiteral("applications"), QJsonArray::fromVariantList(m_applications)}};
}
