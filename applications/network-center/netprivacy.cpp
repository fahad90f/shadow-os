// SPDX-License-Identifier: GPL-3.0-or-later
#include "netprivacy.h"

#include "netutil.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

#include <unistd.h>

namespace {

constexpr int kStatusTimeoutMs = 60000;     // nmcli once per saved profile, busctl, systemctl
constexpr int kChangeTimeoutMs = 300000;    // includes the time the password dialog is open
constexpr int kMaxCustomServers = 8;        // shadow-netprivacy MAX_CUSTOM_SERVERS

const QStringList &dnsModes()
{
    static const QStringList modes{QStringLiteral("dot"), QStringLiteral("dot-strict"), QStringLiteral("doh"), QStringLiteral("off")};
    return modes;
}
const QStringList &dnssecValues()
{
    static const QStringList values{QStringLiteral("allow-downgrade"), QStringLiteral("yes"), QStringLiteral("no")};
    return values;
}
const QStringList &macModes()
{
    static const QStringList modes{QStringLiteral("per-boot"), QStringLiteral("per-network"), QStringLiteral("hardware")};
    return modes;
}
const QStringList &providers()
{
    static const QStringList ids{QStringLiteral("quad9"), QStringLiteral("mullvad"), QStringLiteral("cloudflare")};
    return ids;
}

// The tool's HOSTNAME_RE: a fully qualified name (at least two labels).
const QRegularExpression &tlsNamePattern()
{
    static const QRegularExpression re(QStringLiteral(
        "^(?=.{1,253}$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\\.)+[A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$"));
    return re;
}

// One "<ip>#<tls name>" → normalised text, or empty with *error set.
QString parseServer(const QString &spec, QString *error)
{
    const QStringList parts = spec.split(QLatin1Char('#'));
    if (parts.size() != 2) {
        *error = NetPrivacy::tr("“%1”: write address#TLS name, for example 192.0.2.53#dns.example.net.").arg(spec.left(80));
        return {};
    }
    const QString address = parts.at(0).trimmed();
    const QString name = parts.at(1).trimmed();
    QHostAddress host;
    if (!Net::isIpLiteral(address, false) || !host.setAddress(address)) {
        *error = NetPrivacy::tr("“%1” is not an IP address. Host names are not accepted: the server must be reachable before any name can be looked up.").arg(address.left(80));
        return {};
    }
    if (host.isMulticast() || host == QHostAddress(QHostAddress::AnyIPv4) || host == QHostAddress(QHostAddress::AnyIPv6)) {
        *error = NetPrivacy::tr("%1 cannot be a DNS server.").arg(address);
        return {};
    }
    if (host.isLoopback()) {
        *error = NetPrivacy::tr("%1 is this computer; a local resolver would loop back to systemd-resolved.").arg(address);
        return {};
    }
    if (!tlsNamePattern().match(name).hasMatch()) {
        *error = NetPrivacy::tr("“%1” is not a valid TLS server name (a full host name such as dns.example.net).").arg(name.left(80));
        return {};
    }
    return host.toString() + QLatin1Char('#') + name.toLower();
}

// "a#x, b#y" → normalised list; empty with *error on the first problem.
QStringList parseServers(const QString &text, QString *error)
{
    QStringList out;
    for (const QString &raw : text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)) {
        const QString server = parseServer(raw, error);
        if (server.isEmpty())
            return {};
        if (!out.contains(server))
            out << server;
    }
    if (out.isEmpty()) {
        *error = NetPrivacy::tr("Enter at least one server as address#TLS name, for example 192.0.2.53#dns.example.net.");
        return {};
    }
    if (out.size() > kMaxCustomServers) {
        *error = NetPrivacy::tr("At most %1 servers.").arg(kMaxCustomServers);
        return {};
    }
    return out;
}

// ---- reading the status document ------------------------------------------------------

struct Reader
{
    QStringList *problems;
    QString text(const QJsonObject &o, const char *key, const QString &where, const QString &fallback = {}) const
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isString())
            return v.toString().left(500);
        if (!v.isUndefined() && !v.isNull())
            problems->append(QStringLiteral("%1.%2 is not text").arg(where, QLatin1String(key)));
        return fallback;
    }
    bool flag(const QJsonObject &o, const char *key, const QString &where, bool fallback = false) const
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isBool())
            return v.toBool();
        if (!v.isUndefined() && !v.isNull())
            problems->append(QStringLiteral("%1.%2 is not true or false").arg(where, QLatin1String(key)));
        return fallback;
    }
    QStringList texts(const QJsonObject &o, const char *key, const QString &where, int max = 64) const
    {
        QStringList out;
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined() || v.isNull())
            return out;
        if (!v.isArray()) {
            problems->append(QStringLiteral("%1.%2 is not a list").arg(where, QLatin1String(key)));
            return out;
        }
        for (const QJsonValue &item : v.toArray()) {
            if (!item.isString())
                problems->append(QStringLiteral("%1.%2 has an entry that is not text").arg(where, QLatin1String(key)));
            else if (out.size() < max)
                out << item.toString().left(500);
        }
        return out;
    }
    QJsonObject object(const QJsonObject &o, const char *key, const QString &where) const
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (!v.isObject() && !v.isUndefined() && !v.isNull())
            problems->append(QStringLiteral("%1.%2 is not an object").arg(where, QLatin1String(key)));
        return v.toObject();
    }
};

} // namespace

NetPrivacy::NetPrivacy(QObject *parent)
    : QObject(parent)
{
}

QString NetPrivacy::toolPath() const
{
    const QString override = qEnvironmentVariable("SHADOW_NETPRIVACY_TOOL");
    return override.isEmpty() ? QStringLiteral("/usr/sbin/shadow-netprivacy") : override;
}

NetPrivacy::Parsed NetPrivacy::parseStatus(const QByteArray &json)
{
    Parsed p;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        p.problems << (pe.error != QJsonParseError::NoError ? QStringLiteral("not valid JSON (%1)").arg(pe.errorString())
                                                            : QStringLiteral("not a JSON object"));
        return p;
    }
    const QJsonObject root = doc.object();
    const Reader r{&p.problems};
    const QJsonValue version = root.value(QLatin1String("version"));
    if (version.toInt(-1) != 1) {
        const QByteArray text = QJsonDocument(QJsonArray{version}).toJson(QJsonDocument::Compact);
        p.problems << QStringLiteral("unknown status version %1").arg(QString::fromUtf8(text.mid(1, text.size() - 2)).left(40));
    }
    p.escapeHatch = r.flag(root, "escape_hatch", QStringLiteral("status"));
    p.errors = r.texts(root, "errors", QStringLiteral("status"));
    p.warnings = r.texts(root, "warnings", QStringLiteral("status"));

    const QJsonObject dns = r.object(root, "dns", QStringLiteral("status"));
    if (dns.isEmpty())
        p.problems << QStringLiteral("status.dns is missing");
    p.readable = !dns.isEmpty();
    const QJsonObject live = r.object(dns, "live", QStringLiteral("dns"));
    const QJsonObject doh = r.object(dns, "doh", QStringLiteral("dns"));
    const QJsonObject resolv = r.object(dns, "resolv_conf", QStringLiteral("dns"));
    QVariantMap liveMap;
    if (!live.isEmpty()) {
        liveMap = {{QStringLiteral("dnsOverTls"), r.text(live, "dns_over_tls", QStringLiteral("dns.live"))},
                   {QStringLiteral("dnssec"), r.text(live, "dnssec", QStringLiteral("dns.live"))},
                   {QStringLiteral("currentServer"), r.text(live, "current_server", QStringLiteral("dns.live"))}};
    }
    p.dns = {
        {QStringLiteral("mode"), r.text(dns, "mode", QStringLiteral("dns"))},
        {QStringLiteral("provider"), r.text(dns, "provider", QStringLiteral("dns"))},
        {QStringLiteral("servers"), r.texts(dns, "servers", QStringLiteral("dns"))},
        {QStringLiteral("dnsOverTls"), r.text(dns, "dns_over_tls", QStringLiteral("dns"))},
        {QStringLiteral("dnssec"), r.text(dns, "dnssec", QStringLiteral("dns"))},
        {QStringLiteral("allNamesToProvider"), r.flag(dns, "all_names_to_provider", QStringLiteral("dns"))},
        {QStringLiteral("fallbackServers"), r.texts(dns, "fallback_servers", QStringLiteral("dns"))},
        {QStringLiteral("encrypted"), r.text(dns, "encrypted", QStringLiteral("dns"), QStringLiteral("unknown"))},
        {QStringLiteral("override"), r.text(dns, "override", QStringLiteral("dns"))},
        {QStringLiteral("source"), r.text(dns, "source", QStringLiteral("dns"))},
        {QStringLiteral("resolverInstalled"), r.flag(dns, "resolver_installed", QStringLiteral("dns"))},
        {QStringLiteral("resolverActive"), r.flag(dns, "resolver_active", QStringLiteral("dns"))},
        {QStringLiteral("resolvConf"), r.text(resolv, "kind", QStringLiteral("dns.resolv_conf"))},
        {QStringLiteral("nssResolve"), r.flag(dns, "nss_resolve", QStringLiteral("dns"))},
        {QStringLiteral("live"), liveMap},
        {QStringLiteral("currentServer"), liveMap.value(QStringLiteral("currentServer")).toString()},
        {QStringLiteral("dohInstalled"), r.flag(doh, "installed", QStringLiteral("dns.doh"))},
        {QStringLiteral("dohActive"), r.flag(doh, "active", QStringLiteral("dns.doh"))},
    };

    const QJsonObject mac = r.object(root, "mac", QStringLiteral("status"));
    if (mac.isEmpty())
        p.problems << QStringLiteral("status.mac is missing");
    p.readable = p.readable && !mac.isEmpty();
    QStringList exceptions;
    for (const QString &uuid : r.texts(mac, "exceptions", QStringLiteral("mac"), 1024)) {
        if (Net::isUuid(uuid))
            exceptions << uuid.toLower();
        else
            p.problems << QStringLiteral("mac.exceptions has an entry that is not a connection UUID");
    }
    p.mac = {
        {QStringLiteral("mode"), r.text(mac, "mode", QStringLiteral("mac"))},
        {QStringLiteral("stableId"), r.text(mac, "stable_id", QStringLiteral("mac"))},
        {QStringLiteral("wifi"), r.text(mac, "wifi", QStringLiteral("mac"))},
        {QStringLiteral("ethernet"), r.text(mac, "ethernet", QStringLiteral("mac"))},
        {QStringLiteral("scanRandomization"), r.flag(mac, "scan_randomization", QStringLiteral("mac"))},
        {QStringLiteral("source"), r.text(mac, "source", QStringLiteral("mac"))},
        {QStringLiteral("override"), r.text(mac, "override", QStringLiteral("mac"))},
        {QStringLiteral("fixedDrivers"), r.texts(mac, "fixed_drivers", QStringLiteral("mac"))},
        {QStringLiteral("exceptions"), exceptions},
        {QStringLiteral("exceptionsKnown"), r.flag(mac, "exceptions_known", QStringLiteral("mac"))},
        {QStringLiteral("dnsPlugin"), r.text(mac, "dns_plugin", QStringLiteral("mac"))},
    };
    p.ok = p.problems.isEmpty();
    return p;
}

void NetPrivacy::refresh()
{
    const int generation = ++m_generation;
    const QString tool = toolPath();
    if (!QFileInfo(tool).isExecutable()) {
        m_status = QStringLiteral("missing");
        m_statusText = tr("shadow-netprivacy (%1, package shadow-hardening) is not installed, so encrypted DNS and the address policy cannot be read or changed here.").arg(tool);
        m_dns.clear();
        m_mac.clear();
        m_errors.clear();
        m_warnings.clear();
        m_escapeHatch = false;
        m_checked = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        Q_EMIT changed();
        Q_EMIT refreshed();
        return;
    }
    if (!m_refreshing) {
        m_refreshing = true;
        Q_EMIT refreshingChanged();
    }
    Shadow::Command::run(this, tool, {QStringLiteral("status"), QStringLiteral("--json")},
                         [this, generation, tool](const Shadow::CommandResult &result) {
        if (generation != m_generation)
            return;
        m_refreshing = false;
        Q_EMIT refreshingChanged();
        m_checked = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        const Parsed parsed = result.ok() ? parseStatus(result.out) : Parsed{};
        if (!result.ok()) {
            m_status = QStringLiteral("failed");
            const QString detail = Net::cleanMessage(result.errText()).section(QLatin1Char('\n'), 0, 2);
            m_statusText = result.exitCode == -1 ? tr("%1 could not be started.").arg(tool)
                         : result.exitCode == -2 ? tr("shadow-netprivacy status did not answer within %1 seconds.").arg(kStatusTimeoutMs / 1000)
                         : detail.isEmpty() ? tr("shadow-netprivacy status failed (exit status %1).").arg(result.exitCode)
                                            : tr("shadow-netprivacy status failed: %1").arg(detail);
        } else if (!parsed.readable) {
            m_status = QStringLiteral("failed");
            m_statusText = tr("shadow-netprivacy status gave an answer this version cannot read: %1.").arg(parsed.problems.join(QStringLiteral("; ")));
        } else {
            m_status = QStringLiteral("ok");
            m_statusText = parsed.problems.isEmpty() ? QString()
                         : tr("Parts of the status could not be read: %1.").arg(parsed.problems.join(QStringLiteral("; ")));
        }
        if (m_status == QLatin1String("ok")) {
            m_dns = parsed.dns;
            m_mac = parsed.mac;
            m_errors = parsed.errors;
            m_warnings = parsed.warnings;
            m_escapeHatch = parsed.escapeHatch;
        } else {
            m_dns.clear();
            m_mac.clear();
            m_errors.clear();
            m_warnings.clear();
            m_escapeHatch = false;
        }
        Q_EMIT changed();
        Q_EMIT refreshed();
    }, kStatusTimeoutMs);
}

QString NetPrivacy::customServersError(const QString &text) const
{
    QString error;
    parseServers(text, &error);
    return error;
}

QString NetPrivacy::customProvider(const QString &text) const
{
    QString error;
    const QStringList servers = parseServers(text, &error);
    return servers.isEmpty() ? QString() : QStringLiteral("custom:") + servers.join(QLatin1Char(','));
}

QString NetPrivacy::dnsRequestError(const QString &mode, const QString &provider, const QString &dnssec) const
{
    if (!dnsModes().contains(mode))
        return tr("Unknown DNS mode “%1”.").arg(mode.left(40));
    if (!dnssecValues().contains(dnssec))
        return tr("Unknown DNSSEC setting “%1”.").arg(dnssec.left(40));
    if (mode == QLatin1String("off"))
        return {};
    if (provider.startsWith(QLatin1String("custom:"))) {
        if (mode == QLatin1String("doh"))
            return tr("DNS-over-HTTPS works with Quad9, Mullvad and Cloudflare; a server of your own needs Automatic or Strict DoT.");
        QString error;
        parseServers(provider.mid(7), &error);
        return error;
    }
    if (!providers().contains(provider))
        return tr("Unknown provider “%1”.").arg(provider.left(40));
    if (mode == QLatin1String("doh") && m_status == QLatin1String("ok") && !m_dns.value(QStringLiteral("dohInstalled")).toBool())
        return tr("DNS-over-HTTPS needs the dnscrypt-proxy package (sudo apt install dnscrypt-proxy).");
    return {};
}

void NetPrivacy::setDns(const QString &mode, const QString &provider, const QString &dnssec)
{
    const QString error = dnsRequestError(mode, provider, dnssec);
    if (!error.isEmpty()) {
        Q_EMIT operationFinished(QStringLiteral("dns"), QString(), false, error);
        return;
    }
    QStringList arguments{QStringLiteral("dns"), mode};
    if (mode != QLatin1String("off")) {
        QString spec = provider;
        if (spec.startsWith(QLatin1String("custom:")))
            spec = customProvider(spec.mid(7));
        arguments << QStringLiteral("--provider") << spec;
    }
    arguments << QStringLiteral("--dnssec") << dnssec;
    runChange(QStringLiteral("dns"), QString(), arguments);
}

void NetPrivacy::setMac(const QString &mode)
{
    if (!macModes().contains(mode)) {
        Q_EMIT operationFinished(QStringLiteral("mac"), QString(), false, tr("Unknown address policy “%1”.").arg(mode.left(40)));
        return;
    }
    runChange(QStringLiteral("mac"), QString(), {QStringLiteral("mac"), mode});
}

void NetPrivacy::setMacException(const QString &uuid, bool keep)
{
    if (!Net::isUuid(uuid)) {
        Q_EMIT operationFinished(QStringLiteral("mac-exception"), uuid, false, tr("“%1” is not a connection UUID.").arg(uuid.left(60)));
        return;
    }
    runChange(QStringLiteral("mac-exception"), uuid.toLower(),
              {QStringLiteral("mac-exception"), keep ? QStringLiteral("add") : QStringLiteral("remove"), uuid.toLower()});
}

QString NetPrivacy::failureText(const Shadow::CommandResult &result) const
{
    const QString pk = Net::pkexecFailure(result.exitCode, result.errText());
    if (!pk.isEmpty())
        return pk;
    if (result.exitCode == -1)
        return tr("%1 could not be started.").arg(toolPath());
    if (result.exitCode == -2)
        return tr("shadow-netprivacy did not finish in time.");
    QStringList lines;
    for (QString line : Net::cleanMessage(result.errText()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        line = line.trimmed();
        if (line.startsWith(QLatin1String("shadow-netprivacy: ")))
            line = line.mid(19);
        if (!line.isEmpty())
            lines << line;
    }
    return lines.isEmpty() ? tr("shadow-netprivacy failed (exit status %1).").arg(result.exitCode)
                           : lines.join(QLatin1Char(' ')).left(1000);
}

void NetPrivacy::runChange(const QString &operation, const QString &target, const QStringList &arguments)
{
    if (busy()) {
        Q_EMIT operationFinished(operation, target, false, tr("Another change is still running."));
        return;
    }
    const QString tool = toolPath();
    if (!QFileInfo(tool).isExecutable()) {
        Q_EMIT operationFinished(operation, target, false, tr("%1 is not installed (package shadow-hardening).").arg(tool));
        return;
    }
    QString program, problem;
    QStringList programArguments;
    if (!Net::elevatedCommand(tool, arguments, &program, &programArguments, &problem)) {
        Q_EMIT operationFinished(operation, target, false, problem);
        return;
    }
    m_busyOperation = operation;
    m_busyTarget = target;
    Q_EMIT busyChanged();
    Shadow::Command::run(this, program, programArguments, [this, operation, target](const Shadow::CommandResult &result) {
        m_busyOperation.clear();
        m_busyTarget.clear();
        Q_EMIT busyChanged();
        // The tool's own confirmation (what is now in force, and notes such
        // as "applies from the next connection") is the success message.
        const QString message = result.ok() ? Net::cleanMessage(result.outText()).replace(QLatin1Char('\n'), QLatin1Char(' '))
                                            : failureText(result);
        Q_EMIT operationFinished(operation, target, result.ok(), message.isEmpty() ? tr("Done.") : message);
        refresh();
    }, kChangeTimeoutMs);
}

QJsonObject NetPrivacy::dump() const
{
    return {
        {QStringLiteral("toolPath"), toolPath()},
        {QStringLiteral("status"), m_status},
        {QStringLiteral("statusText"), m_statusText},
        {QStringLiteral("escapeHatch"), m_escapeHatch},
        {QStringLiteral("dns"), QJsonObject::fromVariantMap(m_dns)},
        {QStringLiteral("mac"), QJsonObject::fromVariantMap(m_mac)},
        {QStringLiteral("errors"), QJsonArray::fromStringList(m_errors)},
        {QStringLiteral("warnings"), QJsonArray::fromStringList(m_warnings)},
    };
}
