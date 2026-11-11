// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QStringList>

#include <functional>

class QDBusServiceWatcher;

namespace PowerModule {

// (ss) of org.kde.Solid.PowerManagement.PolicyAgent: application, reason.
struct InhibitionInfo {
    QString application;
    QString reason;
};

// What keeps the idle actions from happening.
//
// 1. Applications ("a film is playing"). They call
//    org.freedesktop.ScreenSaver.Inhibit, a service KWin's screen locker
//    provides. The locker forwards every such request to the session's power
//    manager as org.kde.Solid.PowerManagement.PolicyAgent.AddInhibition — a
//    public D-Bus interface — and that power manager is this module: it owns
//    the name org.kde.Solid.PowerManagement.PolicyAgent on the session bus
//    and keeps the list. Applications may also call the PolicyAgent
//    directly. Types (a bit mask): 1 = do not suspend, 4 = keep the screen on
//    (which implies 1); 2 (power profile) does not concern idle actions.
//    An inhibition ends with ReleaseInhibition or when its caller leaves the bus.
//
// 2. systemd-logind inhibitor locks in "block" mode (systemd-inhibit):
//    Manager.BlockInhibited on the system bus. "idle" holds back every idle
//    action, "sleep" the suspend.
//
// Wayland idle-inhibit surfaces (zwp_idle_inhibit_manager_v1) are not seen
// here: the compositor itself stops reporting idleness while one is visible.
class Inhibitors : public QObject
{
    Q_OBJECT

public:
    enum Type { InterruptSession = 1, ChangeProfile = 2, ChangeScreenSettings = 4 };

    // 'serve' = false: only read logind (for --dump), own no bus name.
    explicit Inhibitors(bool serve, QObject *parent = nullptr);

    // True once logind has answered (or is known to be unreachable).
    bool ready() const { return m_ready; }

    // Screen off and lock are held back.
    bool blocksIdle() const;
    // Suspend is held back.
    bool blocksSuspend() const;
    // One line for logs and the dump; empty when nothing blocks 'suspend ? suspend : idle'.
    QString blocker(bool suspend) const;

    // Asks logind again and calls 'then' when the answer is in.
    void refreshLogind(const std::function<void()> &then = {});

    // PolicyAgent
    uint add(uint types, const QString &application, const QString &reason, const QString &caller);
    void release(uint cookie);
    QList<InhibitionInfo> list() const;
    bool has(uint types) const;

    QJsonObject toJson() const;

Q_SIGNALS:
    void readyChanged();
    void changed();
    void sessionInhibitionsChanged(const QList<PowerModule::InhibitionInfo> &added, const QStringList &removed);

private Q_SLOTS:
    void logindPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);

private:
    struct Entry { uint cookie; uint types; QString application, reason, caller; };
    struct LogindLock { QString what, who, why, mode; uint uid = 0, pid = 0; };

    QList<Entry> m_entries;
    uint m_lastCookie = 0;
    QDBusServiceWatcher *m_callers = nullptr;
    bool m_serving = false;
    QString m_serviceError;

    QDBusConnection m_system;
    bool m_ready = false;
    bool m_logindAvailable = false;
    QString m_logindError;
    QStringList m_blockInhibited;
    QList<LogindLock> m_logindLocks;
    quint64 m_generation = 0;
};

// The D-Bus face of Inhibitors: org.kde.Solid.PowerManagement.PolicyAgent.
class PolicyAgentAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.Solid.PowerManagement.PolicyAgent")

public:
    explicit PolicyAgentAdaptor(Inhibitors *inhibitors);

public Q_SLOTS:
    uint AddInhibition(uint types, const QString &app_name, const QString &reason, const QDBusMessage &message);
    void ReleaseInhibition(uint cookie);
    QList<PowerModule::InhibitionInfo> ListInhibitions();
    bool HasInhibition(uint types);

Q_SIGNALS:
    void InhibitionsChanged(const QList<PowerModule::InhibitionInfo> &added, const QStringList &removed);

private:
    Inhibitors *m_inhibitors;
};

} // namespace PowerModule

Q_DECLARE_METATYPE(PowerModule::InhibitionInfo)
