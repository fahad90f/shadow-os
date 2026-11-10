// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQmlEngine>

// Session and power actions.
//
// Power actions go to systemd-logind (org.freedesktop.login1) with
// interactive authorisation, so polkit decides — e.g. it asks for
// authentication when other users are logged in. Locking is delegated to the
// compositor's screen locker (org.freedesktop.ScreenSaver), which is the
// component that can actually guarantee nothing is shown or accepts input
// while locked.
class Session : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the shell as the 'session' context property")
    Q_PROPERTY(QString userName READ userName CONSTANT)
    Q_PROPERTY(QString fullName READ fullName CONSTANT)
    Q_PROPERTY(QString hostName READ hostName CONSTANT)
    Q_PROPERTY(bool liveSession READ liveSession CONSTANT)
    Q_PROPERTY(bool canSuspend READ canSuspend NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool canHibernate READ canHibernate NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool canReboot READ canReboot NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool canPowerOff READ canPowerOff NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool canLock READ canLock NOTIFY capabilitiesChanged)

public:
    explicit Session(QObject *parent = nullptr);

    QString userName() const { return m_userName; }
    QString fullName() const { return m_fullName; }
    QString hostName() const { return m_hostName; }
    bool liveSession() const { return m_live; }
    bool canSuspend() const { return m_canSuspend; }
    bool canHibernate() const { return m_canHibernate; }
    bool canReboot() const { return m_canReboot; }
    bool canPowerOff() const { return m_canPowerOff; }
    bool canLock() const { return m_canLock; }

    // What the boot menu asked the live session to open ("installer",
    // "recovery" or empty). Returned once, then forgotten.
    Q_INVOKABLE QString takeAutostartRequest();

    Q_INVOKABLE void lock();
    Q_INVOKABLE void logout();
    Q_INVOKABLE void suspend();
    Q_INVOKABLE void hibernate();
    Q_INVOKABLE void reboot();
    Q_INVOKABLE void powerOff();
    // Start a program by name with an argument vector (shortcuts, panel buttons).
    Q_INVOKABLE bool run(const QString &program, const QStringList &arguments = {});
    Q_INVOKABLE bool programExists(const QString &program) const;

Q_SIGNALS:
    void capabilitiesChanged();
    void actionFailed(const QString &action, const QString &error);

private:
    void queryCapabilities();
    void logind(const QString &method, const QString &what);

    QString m_userName, m_fullName, m_hostName;
    bool m_live = false;
    bool m_canSuspend = false, m_canHibernate = false, m_canReboot = false, m_canPowerOff = false, m_canLock = false;
};
