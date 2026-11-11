// SPDX-License-Identifier: GPL-3.0-or-later
#include "firewallbackend.h"

#include "dumpregistry.h"

#include <command.h>

#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>

#include <unistd.h>

NEXUS_CC_DUMP("firewall", [] {
    FirewallBackend backend;
    if (!backend.ready()) {
        QEventLoop loop;
        QObject::connect(&backend, &FirewallBackend::changed, &loop, [&] { if (backend.ready()) loop.quit(); });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
const QString kRecord = QStringLiteral("/etc/nexus/firewall.json");

QString statusTool()
{
    const QString override = qEnvironmentVariable("NEXUS_SECURITY_STATUS_TOOL");
    return override.isEmpty() ? QStringLiteral("/usr/sbin/nexus-security-status") : override;
}

// Root runs the tool itself; everybody else goes through polkit.
void runPrivileged(QObject *context, const QString &tool, const QStringList &arguments, Nexus::Command::Callback done,
                   int timeoutMs = 300000)
{
    if (geteuid() == 0)
        Nexus::Command::run(context, tool, arguments, std::move(done), timeoutMs);
    else
        Nexus::Command::run(context, QStringLiteral("pkexec"), QStringList{tool} + arguments, std::move(done), timeoutMs);
}

QString failureText(const Nexus::CommandResult &result)
{
    if (geteuid() != 0) {
        if (result.exitCode == -1)
            return FirewallBackend::tr("pkexec could not be started. Is polkit installed?");
        if (result.exitCode == 126)
            return FirewallBackend::tr("The request was not authorised.");
        if (result.exitCode == 127)
            return FirewallBackend::tr("Authentication failed: polkit is not running, or no authentication agent is running in this session.");
    }
    if (result.exitCode == -2)
        return FirewallBackend::tr("The firewall tool did not answer in time.");
    QString text = (result.errText().trimmed().isEmpty() ? result.outText() : result.errText()).trimmed();
    if (text.startsWith(QLatin1String("nexus-firewall: ")))
        text = text.mid(16);
    return text.isEmpty() ? FirewallBackend::tr("The firewall tool failed (exit code %1).").arg(result.exitCode) : text;
}

QVariantList rulesFromJson(const QJsonArray &array)
{
    QVariantList rules;
    for (const QJsonValue &value : array) {
        const QJsonObject o = value.toObject();
        const QString port = o.value(QStringLiteral("port")).toVariant().toString();
        const QString protocol = o.value(QStringLiteral("protocol")).toString();
        if (port.isEmpty() || protocol.isEmpty())
            continue;
        rules.append(QVariantMap{{QStringLiteral("port"), port},
                                 {QStringLiteral("protocol"), protocol},
                                 {QStringLiteral("name"), o.value(QStringLiteral("name")).toString()}});
    }
    return rules;
}
} // namespace

FirewallBackend::FirewallBackend(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QString FirewallBackend::toolPath() const
{
    const QString override = qEnvironmentVariable("NEXUS_FIREWALL_TOOL");
    return override.isEmpty() ? QStringLiteral("/usr/sbin/nexus-firewall") : override;
}

void FirewallBackend::refresh()
{
    const int generation = ++m_generation;
    m_installed = QFile::exists(toolPath());
    if (!m_installed) {
        m_statusKnown = false;
        m_rules.clear();
        m_statusMessage = tr("The NEXUS firewall tool (%1) is not installed.").arg(toolPath());
        finishRefresh();
        return;
    }
    if (geteuid() == 0) {
        Nexus::Command::run(this, toolPath(), {QStringLiteral("status"), QStringLiteral("--json")},
                            [this, generation](const Nexus::CommandResult &result) {
            if (generation != m_generation)
                return;
            const QJsonDocument doc = QJsonDocument::fromJson(result.out);
            // Exit code 3 means "not active" and still carries the document.
            if ((result.exitCode == 0 || result.exitCode == 3) && doc.isObject())
                applyStatus(doc.object());
            else
                readRecordOnly(failureText(result));
        });
        return;
    }
    if (!QFile::exists(statusTool())) {
        readRecordOnly(tr("%1 is not installed, so the live state of the firewall cannot be read without administrator rights.")
                           .arg(statusTool()));
        return;
    }
    runPrivileged(this, statusTool(), {QStringLiteral("--section"), QStringLiteral("firewall")},
                  [this, generation](const Nexus::CommandResult &result) {
        if (generation != m_generation)
            return;
        const QJsonObject section = QJsonDocument::fromJson(result.out).object().value(QStringLiteral("firewall")).toObject();
        if (result.ok() && section.value(QStringLiteral("available")).toBool())
            applyStatus(section);
        else if (result.ok() && !section.isEmpty())
            readRecordOnly(section.value(QStringLiteral("reason")).toString());
        else
            readRecordOnly(failureText(result));
    }, 60000);
}

void FirewallBackend::applyStatus(const QJsonObject &info)
{
    m_statusKnown = true;
    m_statusMessage.clear();
    m_rulesetInstalled = info.value(QStringLiteral("installed")).toBool();
    m_active = info.value(QStringLiteral("active")).toBool();
    m_enabledAtBoot = info.value(QStringLiteral("enabled_at_boot")).toBool();
    m_inputPolicy = info.value(QStringLiteral("input_policy")).toString();
    m_forwardPolicy = info.value(QStringLiteral("forward_policy")).toString();
    m_ruleCount = info.value(QStringLiteral("rule_count")).toInt();
    m_otherTables.clear();
    const QJsonArray tables = info.value(QStringLiteral("other_tables")).toArray();
    for (const QJsonValue &table : tables)
        m_otherTables.append(table.toString());
    m_rules = rulesFromJson(info.value(QStringLiteral("allowed")).toArray());
    finishRefresh();
}

void FirewallBackend::readRecordOnly(const QString &why)
{
    m_statusKnown = false;
    m_active = m_enabledAtBoot = false;
    m_inputPolicy.clear();
    m_forwardPolicy.clear();
    m_ruleCount = 0;
    m_otherTables.clear();
    m_rules.clear();
    m_statusMessage = why;
    QFile record(kRecord);
    if (record.open(QIODevice::ReadOnly))
        m_rules = rulesFromJson(QJsonDocument::fromJson(record.readAll()).object().value(QStringLiteral("allowed")).toArray());
    finishRefresh();
}

void FirewallBackend::finishRefresh()
{
    m_ready = true;
    Q_EMIT changed();
}

void FirewallBackend::runAction(const QStringList &arguments, const QString &successMessage)
{
    if (m_busy || !m_installed)
        return;
    m_busy = true;
    Q_EMIT busyChanged();
    runPrivileged(this, toolPath(), arguments, [this, successMessage](const Nexus::CommandResult &result) {
        m_busy = false;
        Q_EMIT busyChanged();
        if (result.ok())
            Q_EMIT succeeded(successMessage);
        else
            Q_EMIT failed(failureText(result));
        refresh();
    });
}

void FirewallBackend::setEnabled(bool on)
{
    runAction({on ? QStringLiteral("enable") : QStringLiteral("disable")},
              on ? tr("The firewall is active.") : tr("The firewall was disabled."));
}

void FirewallBackend::allow(const QString &ports, const QString &protocol, const QString &name)
{
    const QString spec = ports.trimmed();
    const QString label = name.trimmed();
    if (!portsProblem(spec).isEmpty() || !nameProblem(label).isEmpty()
        || (protocol != QLatin1String("tcp") && protocol != QLatin1String("udp"))) {
        Q_EMIT failed(tr("The rule is not valid."));
        return;
    }
    QStringList arguments{QStringLiteral("allow"), spec + QLatin1Char('/') + protocol};
    if (!label.isEmpty())
        arguments << QStringLiteral("--name") << label;
    runAction(arguments, tr("Inbound %1/%2 is now allowed.").arg(spec, protocol));
}

void FirewallBackend::remove(const QString &ports, const QString &protocol)
{
    if (!portsProblem(ports).isEmpty() || (protocol != QLatin1String("tcp") && protocol != QLatin1String("udp"))) {
        Q_EMIT failed(tr("The rule is not valid."));
        return;
    }
    runAction({QStringLiteral("remove"), ports + QLatin1Char('/') + protocol}, tr("Inbound %1/%2 was closed.").arg(ports, protocol));
}

void FirewallBackend::reload()
{
    runAction({QStringLiteral("reload")}, tr("The firewall rules were reloaded."));
}

void FirewallBackend::reset()
{
    runAction({QStringLiteral("reset")}, tr("All opened ports were closed."));
}

QString FirewallBackend::portsProblem(const QString &ports) const
{
    // Same grammar as nexus-firewall: <port> or <first>-<last>, 1–65535.
    static const QRegularExpression pattern(QStringLiteral("^(\\d{1,5})(?:-(\\d{1,5}))?$"));
    const QString text = ports.trimmed();
    if (text.isEmpty())
        return tr("Enter a port number or a range such as 8000-8100.");
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return tr("Use a port number (22) or a range (8000-8100).");
    const int first = match.captured(1).toInt();
    const int last = match.captured(2).isEmpty() ? first : match.captured(2).toInt();
    if (first < 1 || last > 65535)
        return tr("Ports are numbers between 1 and 65535.");
    if (first > last)
        return tr("A range starts with its lowest port.");
    if (first == last && !match.captured(2).isEmpty())
        return tr("Enter a single port without a range.");
    if (match.captured(1) != QString::number(first) || (!match.captured(2).isEmpty() && match.captured(2) != QString::number(last)))
        return tr("Write port numbers without leading zeros.");
    return QString();
}

QString FirewallBackend::nameProblem(const QString &name) const
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9 ._+()/-]{0,48}$"));
    if (name.trimmed().size() > 48)
        return tr("The description can have at most 48 characters.");
    if (!pattern.match(name.trimmed()).hasMatch())
        return tr("The description may contain letters, digits, spaces and . _ + ( ) / -");
    return QString();
}

bool FirewallBackend::isOpen(const QString &ports, const QString &protocol) const
{
    for (const QVariant &rule : m_rules) {
        const QVariantMap map = rule.toMap();
        if (map.value(QStringLiteral("port")).toString() == ports.trimmed() && map.value(QStringLiteral("protocol")).toString() == protocol)
            return true;
    }
    return false;
}

QJsonObject FirewallBackend::toJson() const
{
    return {
        {QStringLiteral("installed"), m_installed},
        {QStringLiteral("statusKnown"), m_statusKnown},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("rulesetInstalled"), m_rulesetInstalled},
        {QStringLiteral("active"), m_statusKnown ? QJsonValue(m_active) : QJsonValue()},
        {QStringLiteral("enabledAtBoot"), m_statusKnown ? QJsonValue(m_enabledAtBoot) : QJsonValue()},
        {QStringLiteral("inputPolicy"), m_inputPolicy},
        {QStringLiteral("forwardPolicy"), m_forwardPolicy},
        {QStringLiteral("ruleCount"), m_ruleCount},
        {QStringLiteral("otherTables"), QJsonArray::fromStringList(m_otherTables)},
        {QStringLiteral("allowed"), QJsonArray::fromVariantList(m_rules)},
    };
}
