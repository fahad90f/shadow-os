// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

// "Applications" on the Firewall page: a front end for /usr/sbin/shadow-appfw,
// the per-application firewall (docs/security/app-firewall.md).
//
// Reading: "shadow-appfw list --json" and "shadow-appfw status --json" answer
// every user (the list of blocked applications is a world-readable file, the
// live state is what root last recorded in /run/shadow/appfw.json).
//
// Changing: "pkexec shadow-appfw set <application> allow|deny" (polkit action
// org.shadow.appfw.manage, administrator authentication). The application's
// name is validated here with the pattern the tool uses and passed as one
// argument. A change applies the next time the application is started.
//
// Test hook (development only): SHADOW_APPFW_TOOL replaces the program path.
class AppFirewallBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool installed READ installed NOTIFY changed)
    // False with shadow.appfw=off on the kernel command line.
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    // [{id, name, icon, hidden, blocked, missing, notConfined, running}]
    Q_PROPERTY(QVariantList applications READ applications NOTIFY changed)
    Q_PROPERTY(int blockedCount READ blockedCount NOTIFY changed)
    Q_PROPERTY(int rejectedPackets READ rejectedPackets NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString toolPath READ toolPath CONSTANT)

public:
    explicit AppFirewallBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    bool installed() const { return m_installed; }
    bool enabled() const { return m_enabled; }
    QString statusMessage() const { return m_statusMessage; }
    QVariantList applications() const { return m_applications; }
    int blockedCount() const { return m_blockedCount; }
    int rejectedPackets() const { return m_rejectedPackets; }
    bool busy() const { return m_busy; }
    QString toolPath() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setBlocked(const QString &application, bool blocked);
    // The applications whose name or id contains 'text'; background ones
    // (no launcher entry of their own) only with 'withHidden'.
    Q_INVOKABLE QVariantList filtered(const QString &text, bool withHidden) const;

    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();
    void busyChanged();
    void failed(const QString &message);
    void succeeded(const QString &message);

private:
    void finishRefresh();

    bool m_ready = false;
    bool m_installed = false;
    bool m_enabled = true;
    QString m_statusMessage;
    QVariantList m_applications;
    int m_blockedCount = 0;
    int m_rejectedPackets = 0;
    bool m_busy = false;
    int m_generation = 0;
};
