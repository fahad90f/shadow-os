// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

// Security page: a read-only rendering of the report printed by
// /usr/sbin/shadow-security-status (sections firewall, apparmor, encryption,
// secureboot, updates, listening, services, logins, audit, hardening and the
// recommendations the tool derives from them).
//
// The tool is run through pkexec (polkit action org.shadow.security.read-status,
// no password for an active local session), directly when the Control Center
// itself runs as root. If polkit refuses or is absent the tool is run without
// privileges; it then reports less and the page says so ("limited").
//
// Nothing is computed here beyond turning the report's fields into text:
// every state and every line of evidence is a field of the report, and the
// recommendations are shown as the tool words them. No scores.
//
// Test hook (development only): SHADOW_SECURITY_STATUS_TOOL replaces the path.
class SecurityBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    Q_PROPERTY(bool privileged READ privileged NOTIFY changed)
    Q_PROPERTY(QString limitedReason READ limitedReason NOTIFY changed)
    Q_PROPERTY(QDateTime generated READ generated NOTIFY changed)
    // [{key, icon, title, state, tone, evidence: [string]}]; tone: success | warning | danger | neutral
    Q_PROPERTY(QVariantList checks READ checks NOTIFY changed)
    // [{id, severity, title, detail, action}] exactly as reported
    Q_PROPERTY(QVariantList recommendations READ recommendations NOTIFY changed)
    Q_PROPERTY(bool securityCenterAvailable READ securityCenterAvailable CONSTANT)
    Q_PROPERTY(bool updaterAvailable READ updaterAvailable CONSTANT)

public:
    explicit SecurityBackend(QObject *parent = nullptr);

    bool loading() const { return m_loading; }
    bool ready() const { return m_ready; }
    bool available() const { return m_available; }
    QString statusMessage() const { return m_statusMessage; }
    bool privileged() const { return m_privileged; }
    QString limitedReason() const { return m_limitedReason; }
    QDateTime generated() const { return m_generated; }
    QVariantList checks() const { return m_checks; }
    QVariantList recommendations() const { return m_recommendations; }
    bool securityCenterAvailable() const;
    bool updaterAvailable() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool openSecurityCenter() const;
    Q_INVOKABLE bool openUpdater() const;

    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();

private:
    void runTool(bool throughPolkit);
    void applyReport(const QJsonObject &report);
    void fail(const QString &message);

    bool m_loading = false;
    bool m_ready = false;
    bool m_available = false;
    bool m_privileged = false;
    QString m_statusMessage, m_limitedReason;
    QDateTime m_generated;
    QVariantList m_checks, m_recommendations;
    QJsonObject m_report;
    int m_generation = 0;
};
