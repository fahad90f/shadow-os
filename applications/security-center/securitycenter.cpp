// SPDX-License-Identifier: GPL-3.0-or-later
#include "securitycenter.h"

#include <command.h>

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

struct Config
{
    QString statusTool = QStringLiteral("/usr/sbin/nexus-security-status");
    QString firewallTool = QStringLiteral("/usr/sbin/nexus-firewall");
    QString initialPage;
    bool direct = false;
};

Config &config()
{
    static Config c;
    return c;
}

// Sections that cost a few milliseconds each; everything else (apt-get -s,
// journalctl over a week, aureport) only runs on a full refresh.
const QStringList kCheapSections{QStringLiteral("firewall"), QStringLiteral("listening"),
                                 QStringLiteral("connections"), QStringLiteral("services")};

const int kStatusTimeoutMs = 6 * 60 * 1000;     // apt-get -s alone may take two minutes on a slow disk
const int kFirewallTimeoutMs = 5 * 60 * 1000;   // includes the time the user spends in the polkit dialog
const int kAutoRefreshMs = 30 * 1000;

QString scalarText(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return QStringLiteral("not determined");
    case QJsonValue::Bool:
        return v.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
    case QJsonValue::Double: {
        const double d = v.toDouble();
        return d == qint64(d) ? QString::number(qint64(d)) : QString::number(d);
    }
    case QJsonValue::String:
        return v.toString();
    default:
        return {};
    }
}

bool isScalar(const QJsonValue &v) { return !v.isObject() && !v.isArray(); }

// Generic, loss-free rendering of one JSON value as indented text.
void renderValue(QString &out, const QString &key, const QJsonValue &v, int depth)
{
    const QString pad(depth * 2, QLatin1Char(' '));
    if (isScalar(v)) {
        const QString text = scalarText(v);
        if (text.contains(QLatin1Char('\n'))) {
            out += pad + key + QStringLiteral(":\n");
            for (const QString &line : text.split(QLatin1Char('\n')))
                out += pad + QStringLiteral("    ") + line + QLatin1Char('\n');
        } else {
            out += pad + key + QStringLiteral(": ") + text + QLatin1Char('\n');
        }
        return;
    }
    if (v.isObject()) {
        out += pad + key + QStringLiteral(":\n");
        const QJsonObject o = v.toObject();
        if (o.isEmpty())
            out += pad + QStringLiteral("  (none)\n");
        for (auto it = o.begin(); it != o.end(); ++it)
            renderValue(out, it.key(), it.value(), depth + 1);
        return;
    }
    const QJsonArray a = v.toArray();
    if (a.isEmpty()) {
        out += pad + key + QStringLiteral(": (none)\n");
        return;
    }
    bool scalars = true;
    for (const QJsonValue &e : a)
        scalars = scalars && isScalar(e);
    if (scalars) {
        QStringList items;
        for (const QJsonValue &e : a)
            items << scalarText(e);
        out += pad + key + QStringLiteral(": ") + items.join(QStringLiteral(", ")) + QLatin1Char('\n');
        return;
    }
    out += pad + key + QStringLiteral(" (") + QString::number(a.size()) + QStringLiteral("):\n");
    for (const QJsonValue &e : a) {
        if (!e.isObject()) {
            renderValue(out, QStringLiteral("-"), e, depth + 1);
            continue;
        }
        QStringList fields;
        const QJsonObject o = e.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (isScalar(it.value()))
                fields << it.key() + QLatin1Char('=') + scalarText(it.value());
            else
                fields << it.key() + QLatin1Char('=')
                              + QString::fromUtf8(QJsonDocument::fromVariant(it.value().toVariant()).toJson(QJsonDocument::Compact));
        }
        out += pad + QStringLiteral("  - ") + fields.join(QStringLiteral("  ")) + QLatin1Char('\n');
    }
}

QString expandHome(QString path)
{
    path = path.trimmed();
    if (path == QLatin1String("~"))
        return QDir::homePath();
    if (path.startsWith(QLatin1String("~/")))
        return QDir::homePath() + path.mid(1);
    return path;
}

} // namespace

// ---------------------------------------------------------------------------------- configuration
void SecurityCenter::configure(const QString &toolsDirArgument, const QString &initialPage)
{
    Config &c = config();
    c.initialPage = initialPage;

    QString dir = toolsDirArgument;
    if (dir.isEmpty())
        dir = qEnvironmentVariable("NEXUS_SECURITY_TOOLS_DIR");
    if (!dir.isEmpty()) {
        const QDir d(dir);
        const auto pick = [&d](const QString &sub, const QString &name) {
            const QString nested = d.absoluteFilePath(sub + QLatin1Char('/') + name);
            return QFileInfo::exists(nested) ? nested : d.absoluteFilePath(name);
        };
        c.statusTool = pick(QStringLiteral("audit"), QStringLiteral("nexus-security-status"));
        c.firewallTool = pick(QStringLiteral("firewall"), QStringLiteral("nexus-firewall"));
        c.direct = true;
    }
    const QString status = qEnvironmentVariable("NEXUS_SECURITY_STATUS_TOOL");
    const QString firewall = qEnvironmentVariable("NEXUS_FIREWALL_TOOL");
    if (!status.isEmpty()) {
        c.statusTool = QFileInfo(status).absoluteFilePath();
        c.direct = true;
    }
    if (!firewall.isEmpty()) {
        c.firewallTool = QFileInfo(firewall).absoluteFilePath();
        c.direct = true;
    }
    if (c.direct) {
        // nexus-security-status finds nexus-firewall through PATH.
        const QByteArray path = QFile::encodeName(QFileInfo(c.firewallTool).absolutePath()) + ':' + qgetenv("PATH");
        qputenv("PATH", path);
    }
}

QStringList SecurityCenter::sectionNames()
{
    return {QStringLiteral("firewall"), QStringLiteral("apparmor"), QStringLiteral("encryption"),
            QStringLiteral("secureboot"), QStringLiteral("updates"), QStringLiteral("listening"),
            QStringLiteral("connections"), QStringLiteral("services"), QStringLiteral("logins"),
            QStringLiteral("audit"), QStringLiteral("hardening"), QStringLiteral("recommendations")};
}

QJsonObject SecurityCenter::toolsJson()
{
    const Config &c = config();
    return QJsonObject{
        {QStringLiteral("mode"), c.direct ? QStringLiteral("direct") : QStringLiteral("pkexec")},
        {QStringLiteral("statusTool"), c.statusTool},
        {QStringLiteral("statusToolInstalled"), QFileInfo(c.statusTool).isExecutable()},
        {QStringLiteral("firewallTool"), c.firewallTool},
        {QStringLiteral("firewallToolInstalled"), QFileInfo(c.firewallTool).isExecutable()},
        {QStringLiteral("pkexecInstalled"), Nexus::Command::available(QStringLiteral("pkexec"))},
        {QStringLiteral("updaterInstalled"), Nexus::Command::available(QStringLiteral("nexus-updater"))},
    };
}

bool SecurityCenter::direct() const { return config().direct; }
QString SecurityCenter::statusTool() const { return config().statusTool; }
QString SecurityCenter::firewallTool() const { return config().firewallTool; }
QString SecurityCenter::initialPage() const { return config().initialPage; }
bool SecurityCenter::updaterInstalled() const { return Nexus::Command::available(QStringLiteral("nexus-updater")); }

// ---------------------------------------------------------------------------------- status tool
SecurityCenter::SecurityCenter(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(kAutoRefreshMs);
    connect(&m_timer, &QTimer::timeout, this, &SecurityCenter::refreshCheap);
}

void SecurityCenter::setAutoRefresh(bool on)
{
    if (m_autoRefresh == on)
        return;
    m_autoRefresh = on;
    if (on)
        m_timer.start();
    else
        m_timer.stop();
    Q_EMIT autoRefreshChanged();
}

void SecurityCenter::setError(const QString &error)
{
    if (m_error == error)
        return;
    m_error = error;
    Q_EMIT errorChanged();
}

QString SecurityCenter::describeFailure(const QString &tool, int exitCode, const QString &stderrText, bool viaPkexec)
{
    const QString name = QFileInfo(tool).fileName();
    const QString detail = stderrText.trimmed();
    if (exitCode == -1)
        return viaPkexec ? tr("pkexec could not be started. Install the packages pkexec and polkitd.")
                         : tr("%1 could not be started.").arg(tool);
    if (exitCode == -2)
        return tr("%1 did not finish in time and was stopped.").arg(name);
    if (viaPkexec && exitCode == 126)
        return tr("Authorisation was not granted: the request was dismissed, or this session is not allowed to do this.")
               + (detail.isEmpty() ? QString() : QLatin1Char('\n') + detail);
    if (viaPkexec && exitCode == 127)
        return tr("Authorisation failed. polkit needs a running authentication agent and an active local session.")
               + (detail.isEmpty() ? QString() : QLatin1Char('\n') + detail);
    if (!detail.isEmpty())
        return detail;
    return tr("%1 failed with exit code %2 and no message.").arg(name).arg(exitCode);
}

void SecurityCenter::refresh()
{
    if (m_running) {
        m_pendingFull = true;
        return;
    }
    startRun({}, true);
}

void SecurityCenter::refreshCheap()
{
    if (m_running || m_firewallBusy) {
        if (!m_runningFull)
            m_pendingCheap = true;
        return;
    }
    // Before the first complete report there is nothing to update.
    if (m_generated.isEmpty()) {
        startRun({}, true);
        return;
    }
    startRun(kCheapSections, false);
}

void SecurityCenter::refreshSections(const QStringList &sections)
{
    if (!m_running)
        startRun(sections, false);
}

void SecurityCenter::startRun(const QStringList &sections, bool full)
{
    const Config &c = config();
    if (!QFileInfo(c.statusTool).isExecutable()) {
        setError(tr("%1 is not installed. It is part of the nexus-hardening package.").arg(c.statusTool));
        Q_EMIT refreshFinished(false);
        return;
    }
    QString program = c.statusTool;
    QStringList arguments;
    if (!c.direct) {
        program = QStringLiteral("pkexec");
        arguments << c.statusTool;
    }
    if (!sections.isEmpty())
        arguments << QStringLiteral("--section") << sections.join(QLatin1Char(','));

    m_running = true;
    m_runningFull = full;
    Q_EMIT loadingChanged();

    const bool viaPkexec = !c.direct;
    const QString tool = c.statusTool;
    Nexus::Command::run(this, program, arguments, [this, full, viaPkexec, tool](const Nexus::CommandResult &r) {
        bool ok = false;
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(r.out, &parseError);
        if (r.exitCode == 0 && doc.isObject()) {
            const QJsonObject o = doc.object();
            const QString generated = o.value(QStringLiteral("generated")).toString();
            for (auto it = o.begin(); it != o.end(); ++it) {
                if (it.key() == QLatin1String("generated") || it.key() == QLatin1String("privileged"))
                    continue;
                m_report.insert(it.key(), it.value());
                m_sectionTimes.insert(it.key(), generated);
            }
            m_privileged = o.value(QStringLiteral("privileged")).toBool();
            if (full)
                m_generated = generated;
            ok = true;
            setError({});
            Q_EMIT reportChanged();
        } else if (r.exitCode == 0) {
            setError(tr("%1 printed something that is not a JSON report (%2).")
                         .arg(QFileInfo(tool).fileName(), parseError.errorString()));
        } else {
            setError(describeFailure(tool, r.exitCode, r.errText(), viaPkexec));
        }

        m_running = false;
        const bool nextFull = m_pendingFull;
        const bool nextCheap = m_pendingCheap && !nextFull;
        m_pendingFull = m_pendingCheap = false;
        Q_EMIT loadingChanged();
        Q_EMIT refreshFinished(ok);
        if (nextFull)
            startRun({}, true);
        else if (nextCheap)
            startRun(kCheapSections, false);
    }, kStatusTimeoutMs);
}

// ---------------------------------------------------------------------------------- firewall
QString SecurityCenter::portError(const QString &port) const
{
    static const QRegularExpression re(QStringLiteral("^(\\d{1,5})(?:-(\\d{1,5}))?$"));
    const QString text = port.trimmed();
    if (text.isEmpty())
        return tr("Enter a port number (1–65535) or a range such as 6000-6010.");
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return tr("“%1” is not a port number or a range such as 6000-6010.").arg(text);
    const int lo = m.captured(1).toInt();
    const int hi = m.captured(2).isEmpty() ? lo : m.captured(2).toInt();
    if (lo < 1 || hi > 65535)
        return tr("Ports must be between 1 and 65535.");
    if (lo > hi)
        return tr("In a range, the lower port comes first.");
    return {};
}

QString SecurityCenter::nameError(const QString &name) const
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9 ._+()/-]{0,48}$"));
    if (!re.match(name).hasMatch())
        return tr("The description may contain letters, digits, spaces and . _ + ( ) / - (at most 48 characters).");
    return {};
}

void SecurityCenter::clearFirewallError()
{
    if (m_firewallError.isEmpty())
        return;
    m_firewallError.clear();
    Q_EMIT firewallErrorChanged();
}

void SecurityCenter::runFirewall(const QString &action, const QStringList &toolArguments)
{
    if (m_firewallBusy)
        return;
    const Config &c = config();
    clearFirewallError();
    if (!QFileInfo(c.firewallTool).isExecutable()) {
        m_firewallError = tr("%1 is not installed. It is part of the nexus-hardening package.").arg(c.firewallTool);
        Q_EMIT firewallErrorChanged();
        Q_EMIT firewallFinished(false, action, m_firewallError);
        return;
    }
    QString program = c.firewallTool;
    QStringList arguments = toolArguments;
    if (!c.direct) {
        program = QStringLiteral("pkexec");
        arguments.prepend(c.firewallTool);
    }
    m_firewallBusy = true;
    m_firewallAction = action;
    Q_EMIT firewallBusyChanged();

    const bool viaPkexec = !c.direct;
    const QString tool = c.firewallTool;
    Nexus::Command::run(this, program, arguments, [this, action, viaPkexec, tool](const Nexus::CommandResult &r) {
        m_firewallBusy = false;
        m_firewallAction.clear();
        Q_EMIT firewallBusyChanged();
        QString message;
        if (r.ok()) {
            message = r.outText().trimmed();
        } else {
            message = describeFailure(tool, r.exitCode, r.errText(), viaPkexec);
            // The tool prefixes its own errors with its name.
            if (message.startsWith(QLatin1String("nexus-firewall: ")))
                message = message.mid(16);
            m_firewallError = message;
            Q_EMIT firewallErrorChanged();
        }
        Q_EMIT firewallFinished(r.ok(), action, message);
        // Show the new state at once, then recompute the recommendations.
        m_pendingFull = true;
        if (!m_running)
            startRun(kCheapSections, false);
    }, kFirewallTimeoutMs);
}

void SecurityCenter::firewallEnable() { runFirewall(QStringLiteral("enable"), {QStringLiteral("enable")}); }
void SecurityCenter::firewallDisable() { runFirewall(QStringLiteral("disable"), {QStringLiteral("disable")}); }
void SecurityCenter::firewallReload() { runFirewall(QStringLiteral("reload"), {QStringLiteral("reload")}); }
void SecurityCenter::firewallReset() { runFirewall(QStringLiteral("reset"), {QStringLiteral("reset")}); }

void SecurityCenter::firewallAllow(const QString &port, const QString &protocol, const QString &name)
{
    const QString proto = protocol.toLower();
    QString problem = portError(port);
    if (problem.isEmpty() && proto != QLatin1String("tcp") && proto != QLatin1String("udp"))
        problem = tr("The protocol must be TCP or UDP.");
    if (problem.isEmpty())
        problem = nameError(name.trimmed());
    if (!problem.isEmpty()) {
        m_firewallError = problem;
        Q_EMIT firewallErrorChanged();
        Q_EMIT firewallFinished(false, QStringLiteral("allow"), problem);
        return;
    }
    QStringList arguments{QStringLiteral("allow"), port.trimmed() + QLatin1Char('/') + proto};
    if (!name.trimmed().isEmpty())
        arguments << QStringLiteral("--name=") + name.trimmed();
    runFirewall(QStringLiteral("allow"), arguments);
}

void SecurityCenter::firewallRemove(const QString &port, const QString &protocol)
{
    const QString proto = protocol.toLower();
    if (!portError(port).isEmpty() || (proto != QLatin1String("tcp") && proto != QLatin1String("udp")))
        return;
    runFirewall(QStringLiteral("remove"), {QStringLiteral("remove"), port.trimmed() + QLatin1Char('/') + proto});
}

// ---------------------------------------------------------------------------------- export
QString SecurityCenter::reportAsJson() const
{
    QJsonObject out{{QStringLiteral("generated"), m_generated}, {QStringLiteral("privileged"), m_privileged}};
    for (const QString &name : sectionNames()) {
        if (m_report.contains(name))
            out.insert(name, m_report.value(name));
    }
    return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented));
}

QString SecurityCenter::reportAsText() const
{
    QString out;
    out += QStringLiteral("NEXUS OS security report\n");
    out += QStringLiteral("Host: %1\n").arg(QSysInfo::machineHostName());
    out += QStringLiteral("Generated: %1 (by nexus-security-status, %2)\n")
               .arg(m_generated.isEmpty() ? QStringLiteral("no complete report yet") : m_generated,
                    m_privileged ? QStringLiteral("run as root") : QStringLiteral("run WITHOUT root: some checks are incomplete"));
    out += QLatin1Char('\n');

    const QJsonArray recs = m_report.value(QStringLiteral("recommendations")).toArray();
    out += QStringLiteral("== Recommendations ==\n");
    if (!m_report.contains(QStringLiteral("recommendations")))
        out += QStringLiteral("  not available\n");
    else if (recs.isEmpty())
        out += QStringLiteral("  none\n");
    for (const QJsonValue &v : recs) {
        const QJsonObject r = v.toObject();
        out += QStringLiteral("  [%1] %2\n").arg(r.value(QStringLiteral("severity")).toString().toUpper(),
                                                r.value(QStringLiteral("title")).toString());
        const QString detail = r.value(QStringLiteral("detail")).toString();
        if (!detail.isEmpty())
            out += QStringLiteral("      %1\n").arg(detail);
    }
    for (const QString &name : sectionNames()) {
        if (name == QLatin1String("recommendations") || !m_report.contains(name))
            continue;
        out += QStringLiteral("\n== %1 (as of %2) ==\n").arg(name, m_sectionTimes.value(name).toString());
        const QJsonObject section = m_report.value(name).toObject();
        for (auto it = section.begin(); it != section.end(); ++it)
            renderValue(out, it.key(), it.value(), 1);
    }
    return out;
}

void SecurityCenter::copyText(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}

QString SecurityCenter::defaultSavePath(const QString &format) const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        dir = QDir::homePath();
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"));
    return QStringLiteral("%1/nexus-security-report-%2.%3")
        .arg(dir, stamp, format == QLatin1String("json") ? QStringLiteral("json") : QStringLiteral("txt"));
}

bool SecurityCenter::fileExists(const QString &path) const
{
    return QFileInfo::exists(expandHome(path));
}

QString SecurityCenter::saveReport(const QString &pathArgument, const QString &format) const
{
    const QString path = expandHome(pathArgument);
    if (path.isEmpty())
        return tr("Enter a file name.");
    if (!QDir::isAbsolutePath(path))
        return tr("Enter an absolute path, for example %1.").arg(defaultSavePath(format));
    if (QFileInfo(path).isDir())
        return tr("%1 is a folder.").arg(path);
    if (!QFileInfo(QFileInfo(path).absolutePath()).isDir())
        return tr("The folder %1 does not exist.").arg(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return tr("%1 could not be written: %2").arg(path, file.errorString());
    file.write((format == QLatin1String("json") ? reportAsJson() : reportAsText()).toUtf8());
    if (!file.commit())
        return tr("%1 could not be written: %2").arg(path, file.errorString());
    // The report lists listening services and login activity.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return {};
}

bool SecurityCenter::openUpdater() const
{
    return QProcess::startDetached(QStringLiteral("nexus-updater"), {});
}

QString SecurityCenter::localTime(const QString &iso, bool withDate) const
{
    const QDateTime t = QDateTime::fromString(iso, Qt::ISODate);
    if (!t.isValid())
        return iso;
    return QLocale().toString(t.toLocalTime(), withDate ? QStringLiteral("yyyy-MM-dd HH:mm") : QStringLiteral("HH:mm:ss"));
}
