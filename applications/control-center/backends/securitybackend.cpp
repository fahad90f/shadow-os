// SPDX-License-Identifier: GPL-3.0-or-later
#include "securitybackend.h"

#include "dumpregistry.h"

#include <command.h>

#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <unistd.h>

SHADOW_CC_DUMP("security", [] {
    SecurityBackend backend;
    if (!backend.ready()) {
        QEventLoop loop;
        QObject::connect(&backend, &SecurityBackend::changed, &loop, [&] { if (backend.ready()) loop.quit(); });
        QTimer::singleShot(240000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
const QString kSections = QStringLiteral("firewall,apparmor,encryption,secureboot,updates,listening,services,logins,audit,hardening,recommendations");

QString toolPath()
{
    const QString override = qEnvironmentVariable("SHADOW_SECURITY_STATUS_TOOL");
    return override.isEmpty() ? QStringLiteral("/usr/sbin/shadow-security-status") : override;
}

QString tr_(const char *text) { return SecurityBackend::tr(text); }

QString yesNo(const QJsonValue &value)
{
    if (value.isBool())
        return value.toBool() ? tr_("yes") : tr_("no");
    return tr_("unknown");
}

QString localTime(const QString &isoUtc)
{
    const QDateTime time = QDateTime::fromString(isoUtc, Qt::ISODate);
    return time.isValid() ? QLocale().toString(time.toLocalTime(), QLocale::ShortFormat) : isoUtc;
}

struct Check
{
    QString key, icon, title, state, tone = QStringLiteral("neutral");
    QStringList evidence;

    Check(const QString &k, const QString &i, const QString &t) : key(k), icon(i), title(t) {}

    QVariantMap toMap() const
    {
        return {{QStringLiteral("key"), key}, {QStringLiteral("icon"), icon}, {QStringLiteral("title"), title},
                {QStringLiteral("state"), state}, {QStringLiteral("tone"), tone}, {QStringLiteral("evidence"), evidence}};
    }
};

// A section the tool could not examine: {"available": false, "reason": …}.
bool unavailable(const QJsonObject &section, Check &check)
{
    if (section.isEmpty()) {
        check.state = tr_("Not reported");
        check.evidence << tr_("The report does not contain this section.");
        return true;
    }
    if (section.value(QStringLiteral("available")).toBool())
        return false;
    check.state = tr_("Not available");
    const QString reason = section.value(QStringLiteral("reason")).toString();
    if (!reason.isEmpty())
        check.evidence << reason;
    return true;
}

Check firewallCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("firewall"), QStringLiteral("firewall"), tr_("Firewall")};
    if (unavailable(s, c))
        return c;
    const bool active = s.value(QStringLiteral("active")).toBool();
    c.state = active ? tr_("Active") : tr_("Not active");
    c.tone = active ? QStringLiteral("success") : QStringLiteral("danger");
    c.evidence << tr_("Starts at boot: %1").arg(yesNo(s.value(QStringLiteral("enabled_at_boot"))));
    if (active)
        c.evidence << tr_("Inbound policy: %1  ·  forward policy: %2  ·  %3 rule(s) loaded")
                          .arg(s.value(QStringLiteral("input_policy")).toString(tr_("unknown")),
                               s.value(QStringLiteral("forward_policy")).toString(tr_("unknown")))
                          .arg(s.value(QStringLiteral("rule_count")).toInt());
    QStringList ports;
    const QJsonArray allowed = s.value(QStringLiteral("allowed")).toArray();
    for (const QJsonValue &v : allowed) {
        const QJsonObject rule = v.toObject();
        ports << rule.value(QStringLiteral("port")).toVariant().toString() + QLatin1Char('/') + rule.value(QStringLiteral("protocol")).toString();
    }
    c.evidence << (ports.isEmpty() ? tr_("Open ports: none") : tr_("Open ports: %1").arg(ports.join(QStringLiteral(", "))));
    if (!s.value(QStringLiteral("installed")).toBool())
        c.evidence << tr_("/etc/nftables.conf is missing.");
    return c;
}

Check appArmorCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("apparmor"), QStringLiteral("shield"), tr_("AppArmor")};
    if (unavailable(s, c))
        return c;
    const bool enabled = s.value(QStringLiteral("enabled")).toBool();
    c.state = enabled ? tr_("Enabled") : tr_("Not enabled");
    c.tone = enabled ? QStringLiteral("success") : QStringLiteral("danger");
    const QString reason = s.value(QStringLiteral("reason")).toString();
    if (!reason.isEmpty())
        c.evidence << reason;
    const QJsonObject profiles = s.value(QStringLiteral("profiles")).toObject();
    if (!profiles.isEmpty())
        c.evidence << tr_("Profiles: %1 enforcing, %2 complaining, %3 kill, %4 unconfined")
                          .arg(profiles.value(QStringLiteral("enforce")).toInt())
                          .arg(profiles.value(QStringLiteral("complain")).toInt())
                          .arg(profiles.value(QStringLiteral("kill")).toInt())
                          .arg(profiles.value(QStringLiteral("unconfined")).toInt());
    if (s.value(QStringLiteral("confined_processes")).isDouble())
        c.evidence << tr_("Confined processes: %1").arg(s.value(QStringLiteral("confined_processes")).toInt());
    const QJsonValue service = s.value(QStringLiteral("service")).toObject().value(QStringLiteral("active"));
    if (service.isBool())
        c.evidence << tr_("apparmor.service active: %1").arg(yesNo(service));
    return c;
}

Check encryptionCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("encryption"), QStringLiteral("disk-lock"), tr_("Disk encryption")};
    if (unavailable(s, c))
        return c;
    const QJsonObject root = s.value(QStringLiteral("root")).toObject();
    const QJsonValue encrypted = root.value(QStringLiteral("encrypted"));
    if (s.value(QStringLiteral("live_session")).toBool()) {
        c.state = tr_("Live session");
        c.evidence << tr_("The system is running from installation media; nothing is installed on a disk yet.");
    } else if (!encrypted.isBool()) {
        c.state = tr_("Unknown");
        c.evidence << tr_("The device holding the root file system could not be determined.");
    } else {
        c.state = encrypted.toBool() ? tr_("System disk encrypted") : tr_("System disk not encrypted");
        c.tone = encrypted.toBool() ? QStringLiteral("success") : QStringLiteral("warning");
    }
    if (root.value(QStringLiteral("device")).isString())
        c.evidence << tr_("Root file system: %1").arg(root.value(QStringLiteral("device")).toString());
    const QJsonObject home = s.value(QStringLiteral("home")).toObject();
    if (home.value(QStringLiteral("separate")).toBool())
        c.evidence << tr_("Separate /home on %1, encrypted: %2")
                          .arg(home.value(QStringLiteral("device")).toString(), yesNo(home.value(QStringLiteral("encrypted"))));
    const QJsonObject swap = s.value(QStringLiteral("swap")).toObject();
    if (swap.value(QStringLiteral("present")).toBool())
        c.evidence << tr_("Swap encrypted: %1").arg(yesNo(swap.value(QStringLiteral("encrypted"))));
    else if (!swap.isEmpty())
        c.evidence << tr_("No swap space in use.");
    const QJsonArray luks = s.value(QStringLiteral("luks_devices")).toArray();
    for (const QJsonValue &v : luks) {
        const QJsonObject d = v.toObject();
        QStringList parts{d.value(QStringLiteral("device")).toString()};
        if (d.value(QStringLiteral("version")).isDouble())
            parts << QStringLiteral("LUKS%1").arg(d.value(QStringLiteral("version")).toInt());
        if (d.value(QStringLiteral("cipher")).isString())
            parts << d.value(QStringLiteral("cipher")).toString();
        if (d.value(QStringLiteral("pbkdf")).isString())
            parts << d.value(QStringLiteral("pbkdf")).toString();
        parts << (d.value(QStringLiteral("unlocked")).toBool() ? tr_("unlocked") : tr_("locked"));
        c.evidence << tr_("LUKS volume: %1").arg(parts.join(QStringLiteral("  ·  ")));
    }
    return c;
}

Check secureBootCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("secureboot"), QStringLiteral("key"), tr_("Secure Boot")};
    if (unavailable(s, c))
        return c;
    if (s.value(QStringLiteral("firmware")).toString() == QLatin1String("bios")) {
        c.state = tr_("Not supported");
        c.evidence << s.value(QStringLiteral("reason")).toString();
        return c;
    }
    const QJsonValue enabled = s.value(QStringLiteral("enabled"));
    if (!enabled.isBool()) {
        c.state = tr_("Unknown");
        c.evidence << tr_("The firmware does not expose a SecureBoot variable.");
    } else {
        c.state = enabled.toBool() ? tr_("Enabled") : tr_("Disabled");
        c.tone = enabled.toBool() ? QStringLiteral("success") : QStringLiteral("warning");
    }
    c.evidence << tr_("Firmware: UEFI  ·  setup mode: %1").arg(yesNo(s.value(QStringLiteral("setup_mode"))));
    if (s.value(QStringLiteral("kernel_lockdown")).isString())
        c.evidence << tr_("Kernel lockdown: %1").arg(s.value(QStringLiteral("kernel_lockdown")).toString());
    if (s.value(QStringLiteral("shim_installed")).isBool() || s.value(QStringLiteral("mok_keys")).isDouble()) {
        QStringList parts;
        if (s.value(QStringLiteral("shim_installed")).isBool())
            parts << tr_("shim boot loader installed: %1").arg(yesNo(s.value(QStringLiteral("shim_installed"))));
        if (s.value(QStringLiteral("mok_keys")).isDouble())
            parts << tr_("%1 machine owner key(s) enrolled").arg(s.value(QStringLiteral("mok_keys")).toInt());
        c.evidence << parts.join(QStringLiteral("  ·  "));
    }
    if (s.value(QStringLiteral("boot_loader_refresh_failed")).isString()) {
        c.tone = QStringLiteral("danger");
        c.evidence << tr_("The boot loader could not be updated since %1.").arg(s.value(QStringLiteral("boot_loader_refresh_failed")).toString());
    }
    return c;
}

Check updatesCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("updates"), QStringLiteral("arrow-up-circle"), tr_("Software updates")};
    if (unavailable(s, c))
        return c;
    const int upgradable = s.value(QStringLiteral("upgradable")).toInt();
    const int security = s.value(QStringLiteral("security")).toInt();
    if (security > 0) {
        c.state = tr_("%1 security update(s) waiting").arg(security);
        c.tone = QStringLiteral("danger");
    } else if (upgradable > 0) {
        c.state = tr_("%1 update(s) available").arg(upgradable);
        c.tone = QStringLiteral("warning");
    } else {
        c.state = tr_("No updates pending");
        c.tone = s.value(QStringLiteral("lists_present")).toBool() ? QStringLiteral("success") : QStringLiteral("neutral");
    }
    if (security > 0 && upgradable > security)
        c.evidence << tr_("%1 update(s) in total").arg(upgradable);
    if (!s.value(QStringLiteral("lists_present")).toBool())
        c.evidence << tr_("Package lists have never been downloaded, so pending updates cannot be known.");
    else if (s.value(QStringLiteral("last_refresh")).isString())
        c.evidence << tr_("Package lists last refreshed: %1 (%2 day(s) ago)")
                          .arg(localTime(s.value(QStringLiteral("last_refresh")).toString()))
                          .arg(s.value(QStringLiteral("last_refresh_age_days")).toDouble(), 0, 'f', 1);
    if (s.value(QStringLiteral("reboot_required")).toBool()) {
        QStringList packages;
        const QJsonArray array = s.value(QStringLiteral("reboot_required_packages")).toArray();
        for (const QJsonValue &v : array)
            packages << v.toString();
        c.evidence << (packages.isEmpty() ? tr_("A restart is required.")
                                          : tr_("A restart is required for: %1").arg(packages.mid(0, 8).join(QStringLiteral(", "))));
    }
    return c;
}

Check listeningCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("listening"), QStringLiteral("network"), tr_("Listening network services")};
    if (unavailable(s, c))
        return c;
    const int exposed = s.value(QStringLiteral("exposed_count")).toInt();
    c.state = tr_("%1 listening, %2 not limited to this computer").arg(s.value(QStringLiteral("count")).toInt()).arg(exposed);
    QStringList lines;
    const QJsonArray sockets = s.value(QStringLiteral("sockets")).toArray();
    for (const QJsonValue &v : sockets) {
        const QJsonObject socket = v.toObject();
        if (socket.value(QStringLiteral("scope")).toString() == QLatin1String("loopback"))
            continue;
        const QString process = socket.value(QStringLiteral("process")).toString();
        const QString address = socket.value(QStringLiteral("local_address")).toString();
        const QString line = tr_("%1  ·  %2/%3  ·  %4")
            .arg(process.isEmpty() ? tr_("unknown process") : process,
                 socket.value(QStringLiteral("local_port")).toString(),
                 socket.value(QStringLiteral("protocol")).toString(),
                 socket.value(QStringLiteral("scope")).toString() == QLatin1String("all-interfaces") ? tr_("all interfaces") : address);
        if (!lines.contains(line))
            lines << line;
    }
    const int shown = qMin(lines.size(), 12);
    c.evidence << lines.mid(0, shown);
    if (lines.size() > shown)
        c.evidence << tr_("… and %1 more").arg(lines.size() - shown);
    return c;
}

Check servicesCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("services"), QStringLiteral("server"), tr_("System services")};
    if (unavailable(s, c))
        return c;
    QStringList failed;
    const QJsonArray array = s.value(QStringLiteral("failed")).toArray();
    for (const QJsonValue &v : array)
        failed << v.toString();
    c.state = tr_("%1 running, %2 failed").arg(s.value(QStringLiteral("running_count")).toInt()).arg(failed.size());
    if (!failed.isEmpty()) {
        c.tone = QStringLiteral("warning");
        c.evidence << tr_("Failed units: %1").arg(failed.mid(0, 10).join(QStringLiteral(", ")));
    }
    return c;
}

Check loginsCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("logins"), QStringLiteral("users"), tr_("Login activity")};
    if (unavailable(s, c))
        return c;
    const QJsonArray sessions = s.value(QStringLiteral("sessions")).toArray();
    const QJsonValue failedCount = s.value(QStringLiteral("failed_count_7d"));
    c.state = failedCount.isDouble() ? tr_("%1 failed sign-in attempt(s) in 7 days").arg(failedCount.toInt())
                                     : tr_("Sign-in history not available");
    const QJsonObject journal = s.value(QStringLiteral("journal")).toObject();
    if (!journal.isEmpty())
        c.evidence << tr_("The authentication log could not be read: %1").arg(journal.value(QStringLiteral("reason")).toString());
    QStringList users;
    for (const QJsonValue &v : sessions) {
        const QString user = v.toObject().value(QStringLiteral("user")).toString();
        if (!user.isEmpty() && !users.contains(user))
            users << user;
    }
    c.evidence << (sessions.isEmpty() ? tr_("Open sessions: none reported")
                                      : tr_("Open sessions: %1 (%2)").arg(sessions.size()).arg(users.join(QStringLiteral(", "))));
    const QJsonArray failed = s.value(QStringLiteral("failed")).toArray();
    if (!failed.isEmpty()) {
        const QJsonObject last = failed.last().toObject();
        QStringList parts{localTime(last.value(QStringLiteral("time")).toString())};
        for (const char *key : {"service", "user", "source"}) {
            if (last.value(QLatin1String(key)).isString())
                parts << last.value(QLatin1String(key)).toString();
        }
        c.evidence << tr_("Most recent failure: %1").arg(parts.join(QStringLiteral("  ·  ")));
    }
    const QJsonArray recent = s.value(QStringLiteral("recent")).toArray();
    if (!recent.isEmpty()) {
        const QJsonObject last = recent.last().toObject();
        c.evidence << tr_("Most recent session opened: %1 by %2 (%3)")
                          .arg(localTime(last.value(QStringLiteral("time")).toString()),
                               last.value(QStringLiteral("user")).toString(tr_("unknown")),
                               last.value(QStringLiteral("service")).toString(tr_("unknown")));
    }
    return c;
}

Check auditCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("audit"), QStringLiteral("file-search"), tr_("Audit log")};
    if (unavailable(s, c))
        return c;
    const QJsonValue active = s.value(QStringLiteral("service")).toObject().value(QStringLiteral("active"));
    if (active.isBool()) {
        c.state = active.toBool() ? tr_("Recording") : tr_("Service not running");
        c.tone = active.toBool() ? QStringLiteral("success") : QStringLiteral("warning");
    } else {
        c.state = tr_("Service state unknown");
    }
    if (s.value(QStringLiteral("enabled")).isDouble())
        c.evidence << tr_("Kernel auditing enabled: %1").arg(s.value(QStringLiteral("enabled")).toInt() > 0 ? tr_("yes") : tr_("no"));
    if (s.value(QStringLiteral("rules")).isDouble())
        c.evidence << tr_("Rules loaded: %1").arg(s.value(QStringLiteral("rules")).toInt());
    if (s.value(QStringLiteral("lost")).isDouble())
        c.evidence << tr_("Lost events: %1  ·  backlog: %2").arg(s.value(QStringLiteral("lost")).toInt()).arg(s.value(QStringLiteral("backlog")).toInt());
    const QJsonObject keys = s.value(QStringLiteral("events_by_key_7d")).toObject();
    if (!keys.isEmpty()) {
        QStringList parts;
        for (auto it = keys.begin(); it != keys.end() && parts.size() < 8; ++it)
            parts << QStringLiteral("%1: %2").arg(it.key()).arg(it.value().toInt());
        c.evidence << tr_("Events in 7 days by rule key: %1").arg(parts.join(QStringLiteral(", ")));
    }
    return c;
}

Check hardeningCheck(const QJsonObject &s)
{
    Check c{QStringLiteral("hardening"), QStringLiteral("sliders"), tr_("Kernel hardening")};
    if (unavailable(s, c))
        return c;
    const QJsonArray sysctl = s.value(QStringLiteral("sysctl")).toArray();
    QStringList drift;
    for (const QJsonValue &v : sysctl) {
        const QJsonObject item = v.toObject();
        if (!item.value(QStringLiteral("ok")).toBool())
            drift << tr_("%1 = %2 (baseline %3)").arg(item.value(QStringLiteral("key")).toString(),
                                                     item.value(QStringLiteral("actual")).toString(tr_("not present")),
                                                     item.value(QStringLiteral("expected")).toString());
    }
    const QJsonObject baseline = s.value(QStringLiteral("sysctl_baseline")).toObject();
    if (!baseline.isEmpty()) {
        c.state = tr_("Baseline not installed");
        c.evidence << baseline.value(QStringLiteral("reason")).toString();
    } else if (drift.isEmpty()) {
        c.state = tr_("%1 setting(s) match the SHADOW baseline").arg(sysctl.size());
        c.tone = QStringLiteral("success");
    } else {
        c.state = tr_("%1 of %2 setting(s) differ from the baseline").arg(drift.size()).arg(sysctl.size());
        c.tone = QStringLiteral("warning");
        c.evidence << drift.mid(0, 8);
        if (drift.size() > 8)
            c.evidence << tr_("… and %1 more").arg(drift.size() - 8);
    }
    QStringList missing;
    const QJsonArray cmdline = s.value(QStringLiteral("cmdline")).toArray();
    for (const QJsonValue &v : cmdline) {
        if (!v.toObject().value(QStringLiteral("ok")).toBool())
            missing << v.toObject().value(QStringLiteral("parameter")).toString();
    }
    if (!cmdline.isEmpty())
        c.evidence << (missing.isEmpty() ? tr_("All %1 hardening boot parameter(s) are in effect.").arg(cmdline.size())
                                         : tr_("Boot parameters not in effect: %1").arg(missing.join(QLatin1Char(' '))));
    QStringList vulnerable;
    const QJsonArray mitigations = s.value(QStringLiteral("mitigations")).toArray();
    for (const QJsonValue &v : mitigations) {
        if (v.toObject().value(QStringLiteral("vulnerable")).toBool())
            vulnerable << v.toObject().value(QStringLiteral("name")).toString();
    }
    if (!mitigations.isEmpty())
        c.evidence << (vulnerable.isEmpty() ? tr_("CPU vulnerabilities: the kernel reports none of %1 as unmitigated.").arg(mitigations.size())
                                            : tr_("CPU vulnerabilities without mitigation: %1").arg(vulnerable.join(QStringLiteral(", "))));
    if (!vulnerable.isEmpty() && c.tone == QLatin1String("success"))
        c.tone = QStringLiteral("warning");
    QString kernel = tr_("Kernel %1").arg(s.value(QStringLiteral("kernel")).toString());
    if (s.value(QStringLiteral("tainted")).isDouble())
        kernel += QStringLiteral("  ·  ") + (s.value(QStringLiteral("tainted")).toInt() == 0 ? tr_("not tainted")
                                                                                              : tr_("taint flags %1").arg(s.value(QStringLiteral("tainted")).toInt()));
    c.evidence << kernel;
    return c;
}
} // namespace

SecurityBackend::SecurityBackend(QObject *parent)
    : QObject(parent)
{
    refresh();
}

bool SecurityBackend::securityCenterAvailable() const
{
    return !QStandardPaths::findExecutable(QStringLiteral("shadow-security-center")).isEmpty();
}

bool SecurityBackend::updaterAvailable() const
{
    return !QStandardPaths::findExecutable(QStringLiteral("shadow-updater")).isEmpty();
}

bool SecurityBackend::openSecurityCenter() const
{
    return QProcess::startDetached(QStringLiteral("shadow-security-center"), {});
}

bool SecurityBackend::openUpdater() const
{
    return QProcess::startDetached(QStringLiteral("shadow-updater"), {});
}

void SecurityBackend::refresh()
{
    if (m_loading)
        return;
    if (!QFile::exists(toolPath())) {
        fail(tr("The security report tool (%1) is not installed.").arg(toolPath()));
        return;
    }
    m_loading = true;
    m_limitedReason.clear();
    Q_EMIT changed();
    runTool(geteuid() != 0);
}

void SecurityBackend::runTool(bool throughPolkit)
{
    const int generation = ++m_generation;
    const QStringList arguments{QStringLiteral("--section"), kSections};
    auto done = [this, generation, throughPolkit](const Shadow::CommandResult &result) {
        if (generation != m_generation)
            return;
        const QJsonDocument doc = QJsonDocument::fromJson(result.out);
        if (result.ok() && doc.isObject()) {
            applyReport(doc.object());
            return;
        }
        if (throughPolkit) {
            // Without polkit the tool still reports what an ordinary user may see.
            m_limitedReason = result.exitCode == -1 ? tr("pkexec could not be started")
                            : result.exitCode == 126 || result.exitCode == 127 ? tr("polkit did not authorise the request")
                            : result.errText().trimmed().isEmpty() ? tr("pkexec failed with exit code %1").arg(result.exitCode)
                                                                   : result.errText().trimmed();
            runTool(false);
            return;
        }
        m_loading = false;
        fail(result.exitCode == -2 ? tr("The security report tool did not finish in time.")
             : result.errText().trimmed().isEmpty() ? tr("The security report tool failed (exit code %1).").arg(result.exitCode)
                                                    : result.errText().trimmed());
    };
    // The "updates" section simulates an upgrade with apt, which can take a while.
    if (throughPolkit)
        Shadow::Command::run(this, QStringLiteral("pkexec"), QStringList{toolPath()} + arguments, done, 240000);
    else
        Shadow::Command::run(this, toolPath(), arguments, done, 240000);
}

void SecurityBackend::fail(const QString &message)
{
    m_loading = false;
    m_ready = true;
    m_available = false;
    m_statusMessage = message;
    m_checks.clear();
    m_recommendations.clear();
    m_report = {};
    Q_EMIT changed();
}

void SecurityBackend::applyReport(const QJsonObject &report)
{
    m_report = report;
    m_loading = false;
    m_ready = true;
    m_available = true;
    m_statusMessage.clear();
    m_privileged = report.value(QStringLiteral("privileged")).toBool();
    m_generated = QDateTime::fromString(report.value(QStringLiteral("generated")).toString(), Qt::ISODate).toLocalTime();

    const auto section = [&report](const char *name) { return report.value(QLatin1String(name)).toObject(); };
    m_checks = {
        firewallCheck(section("firewall")).toMap(),
        appArmorCheck(section("apparmor")).toMap(),
        encryptionCheck(section("encryption")).toMap(),
        secureBootCheck(section("secureboot")).toMap(),
        updatesCheck(section("updates")).toMap(),
        listeningCheck(section("listening")).toMap(),
        servicesCheck(section("services")).toMap(),
        loginsCheck(section("logins")).toMap(),
        auditCheck(section("audit")).toMap(),
        hardeningCheck(section("hardening")).toMap(),
    };

    m_recommendations.clear();
    const QJsonArray recommendations = report.value(QStringLiteral("recommendations")).toArray();
    for (const QJsonValue &v : recommendations) {
        const QJsonObject r = v.toObject();
        m_recommendations.append(QVariantMap{
            {QStringLiteral("id"), r.value(QStringLiteral("id")).toString()},
            {QStringLiteral("severity"), r.value(QStringLiteral("severity")).toString()},
            {QStringLiteral("title"), r.value(QStringLiteral("title")).toString()},
            {QStringLiteral("detail"), r.value(QStringLiteral("detail")).toString()},
            {QStringLiteral("action"), r.value(QStringLiteral("action")).toString()},
        });
    }
    Q_EMIT changed();
}

QJsonObject SecurityBackend::toJson() const
{
    return {
        {QStringLiteral("available"), m_available},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("privileged"), m_privileged},
        {QStringLiteral("limitedReason"), m_limitedReason},
        {QStringLiteral("securityCenterAvailable"), securityCenterAvailable()},
        {QStringLiteral("checks"), QJsonArray::fromVariantList(m_checks)},
        {QStringLiteral("recommendations"), QJsonArray::fromVariantList(m_recommendations)},
        {QStringLiteral("report"), m_report},
    };
}
