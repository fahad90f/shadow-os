// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

// Firewall page: a front end for /usr/sbin/shadow-firewall (nftables).
//
// Reading: "shadow-firewall status --json" needs root. An ordinary user gets
// the same document through "pkexec shadow-security-status --section firewall"
// (polkit action org.shadow.security.read-status: no password for an active
// local session). If neither works, the opened ports are read from the
// world-readable record /etc/shadow-os/firewall.json and the live state is
// reported as unknown.
//
// Changing: "pkexec shadow-firewall enable|disable|allow|remove|reload|reset"
// (polkit action org.shadow.firewall.manage, administrator authentication).
// Arguments are validated here with the same patterns the tool uses, and are
// passed as an argument vector.
//
// Test hooks (development only): SHADOW_FIREWALL_TOOL and
// SHADOW_SECURITY_STATUS_TOOL replace the two program paths.
class FirewallBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool installed READ installed NOTIFY changed)
    Q_PROPERTY(bool statusKnown READ statusKnown NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    Q_PROPERTY(bool rulesetInstalled READ rulesetInstalled NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool enabledAtBoot READ enabledAtBoot NOTIFY changed)
    Q_PROPERTY(QString inputPolicy READ inputPolicy NOTIFY changed)
    Q_PROPERTY(QString forwardPolicy READ forwardPolicy NOTIFY changed)
    Q_PROPERTY(int ruleCount READ ruleCount NOTIFY changed)
    Q_PROPERTY(QStringList otherTables READ otherTables NOTIFY changed)
    Q_PROPERTY(QVariantList rules READ rules NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString toolPath READ toolPath CONSTANT)

public:
    explicit FirewallBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    bool installed() const { return m_installed; }
    bool statusKnown() const { return m_statusKnown; }
    QString statusMessage() const { return m_statusMessage; }
    bool rulesetInstalled() const { return m_rulesetInstalled; }
    bool active() const { return m_active; }
    bool enabledAtBoot() const { return m_enabledAtBoot; }
    QString inputPolicy() const { return m_inputPolicy; }
    QString forwardPolicy() const { return m_forwardPolicy; }
    int ruleCount() const { return m_ruleCount; }
    QStringList otherTables() const { return m_otherTables; }
    QVariantList rules() const { return m_rules; }
    bool busy() const { return m_busy; }
    QString toolPath() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setEnabled(bool on);
    // ports: "<port>" or "<first>-<last>"; protocol: "tcp" | "udp".
    Q_INVOKABLE void allow(const QString &ports, const QString &protocol, const QString &name);
    Q_INVOKABLE void remove(const QString &ports, const QString &protocol);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void reset();

    // Empty when valid, otherwise the reason shown under the field.
    Q_INVOKABLE QString portsProblem(const QString &ports) const;
    Q_INVOKABLE QString nameProblem(const QString &name) const;
    Q_INVOKABLE bool isOpen(const QString &ports, const QString &protocol) const;

    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();
    void busyChanged();
    void failed(const QString &message);
    void succeeded(const QString &message);

private:
    void applyStatus(const QJsonObject &info);
    void readRecordOnly(const QString &why);
    void finishRefresh();
    void runAction(const QStringList &arguments, const QString &successMessage);

    bool m_ready = false;
    bool m_installed = false;
    bool m_statusKnown = false;
    QString m_statusMessage;
    bool m_rulesetInstalled = false;
    bool m_active = false;
    bool m_enabledAtBoot = false;
    QString m_inputPolicy, m_forwardPolicy;
    int m_ruleCount = 0;
    QStringList m_otherTables;
    QVariantList m_rules;
    bool m_busy = false;
    int m_generation = 0;
};
