// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <command.h>

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>

// Encrypted DNS and MAC address policy: a front end for shadow-netprivacy
// (package shadow-hardening; docs/security/private-dns.md).
//
// Reading: "shadow-netprivacy status --json", which runs for any user and
// reports what systemd-resolved and NetworkManager have in force (the live
// values come from resolved over D-Bus), never only what was asked for. The
// document is checked field by field; a value of an unexpected type is
// reported, not guessed.
//
// Changing (pkexec, polkit action org.shadow.netprivacy.manage, administrator,
// kept for a short time; root runs the tool directly):
//   shadow-netprivacy dns {dot|dot-strict|doh|off} [--provider P] --dnssec D
//   shadow-netprivacy mac {per-boot|per-network|hardware}
//   shadow-netprivacy mac-exception {add|remove} <connection UUID>
// Every value is checked here with the tool's own rules before anything is
// started, and passed as one element of the argument vector.
//
// Test hook (development only): SHADOW_NETPRIVACY_TOOL replaces the path of
// the tool (default /usr/sbin/shadow-netprivacy, the path polkit authorises).
class NetPrivacy : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // "unknown" (not read yet) | "ok" | "missing" (tool not installed) | "failed"
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshingChanged)
    Q_PROPERTY(QString toolPath READ toolPath CONSTANT)
    // dns: mode (dot|dot-strict|doh|off|plain|""), provider, servers, dnsOverTls,
    //   dnssec, encrypted (always|opportunistic|partly|no|tor|unknown),
    //   allNamesToProvider, override (""|hide-me|escape), source, resolverInstalled,
    //   resolverActive, resolvConf, nssResolve, live (map or empty), currentServer,
    //   dohInstalled, dohActive
    Q_PROPERTY(QVariantMap dns READ dns NOTIFY changed)
    // mac: mode (per-boot|per-network|hardware|random|custom|""), stableId, wifi,
    //   ethernet, scanRandomization, source, override, fixedDrivers, exceptions
    //   (lower-case UUIDs), exceptionsKnown, dnsPlugin
    Q_PROPERTY(QVariantMap mac READ mac NOTIFY changed)
    Q_PROPERTY(QStringList errors READ errors NOTIFY changed)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY changed)
    Q_PROPERTY(bool escapeHatch READ escapeHatch NOTIFY changed)
    Q_PROPERTY(QString checked READ checked NOTIFY changed)          // ISO time of the last reading
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyOperation READ busyOperation NOTIFY busyChanged)   // dns | mac | mac-exception
    Q_PROPERTY(QString busyTarget READ busyTarget NOTIFY busyChanged)         // the UUID for mac-exception

public:
    explicit NetPrivacy(QObject *parent = nullptr);

    QString status() const { return m_status; }
    QString statusText() const { return m_statusText; }
    bool ready() const { return m_status != QLatin1String("unknown"); }
    bool refreshing() const { return m_refreshing; }
    QString toolPath() const;
    QVariantMap dns() const { return m_dns; }
    QVariantMap mac() const { return m_mac; }
    QStringList errors() const { return m_errors; }
    QStringList warnings() const { return m_warnings; }
    bool escapeHatch() const { return m_escapeHatch; }
    QString checked() const { return m_checked; }
    bool busy() const { return !m_busyOperation.isEmpty(); }
    QString busyOperation() const { return m_busyOperation; }
    QString busyTarget() const { return m_busyTarget; }

    Q_INVOKABLE void refresh();
    // provider: quad9 | mullvad | cloudflare | custom:<ip>#<name>[,...]; ignored for "off".
    Q_INVOKABLE void setDns(const QString &mode, const QString &provider, const QString &dnssec);
    Q_INVOKABLE void setMac(const QString &mode);
    Q_INVOKABLE void setMacException(const QString &uuid, bool keep);

    // Empty when the request would be accepted by shadow-netprivacy, else why not.
    Q_INVOKABLE QString dnsRequestError(const QString &mode, const QString &provider, const QString &dnssec) const;
    // "192.0.2.53#dns.example.net, ..." → empty or the reason it is not valid.
    Q_INVOKABLE QString customServersError(const QString &text) const;
    // "192.0.2.53#dns.example.net, ..." → "custom:192.0.2.53#dns.example.net,..." (normalised).
    Q_INVOKABLE QString customProvider(const QString &text) const;

    QJsonObject dump() const;

    // Pure parts, public for the tests: the status document → the maps above.
    struct Parsed {
        QVariantMap dns, mac;
        QStringList errors, warnings, problems;   // problems: fields of an unexpected type
        bool escapeHatch = false;
        bool readable = false;                     // a status document with "dns" and "mac"
        bool ok = false;                           // readable and every field as expected
    };
    static Parsed parseStatus(const QByteArray &json);

Q_SIGNALS:
    void changed();
    void refreshed();
    void refreshingChanged();
    void busyChanged();
    // operation: dns | mac | mac-exception; target: the UUID for mac-exception.
    void operationFinished(const QString &operation, const QString &target, bool ok, const QString &message);

private:
    void runChange(const QString &operation, const QString &target, const QStringList &arguments);
    QString failureText(const Shadow::CommandResult &result) const;

    QString m_status = QStringLiteral("unknown");
    QString m_statusText;
    bool m_refreshing = false;
    int m_generation = 0;
    QVariantMap m_dns;
    QVariantMap m_mac;
    QStringList m_errors;
    QStringList m_warnings;
    bool m_escapeHatch = false;
    QString m_checked;
    QString m_busyOperation;
    QString m_busyTarget;
};
