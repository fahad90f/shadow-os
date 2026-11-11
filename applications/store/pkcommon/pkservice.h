// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <Transaction>

namespace ShadowPk {

// Reachability and identity of the PackageKit daemon (system D-Bus service
// org.freedesktop.PackageKit).
//
// PackageKit is the only way the SHADOW software tools change the system, so
// both applications first establish — with a real D-Bus round trip — whether
// it can be reached, and keep the exact D-Bus error when it cannot. Nothing
// else in pkcommon touches PackageKit-Qt unless available() is true: without a
// system bus (build chroot, rescue shell) no transaction object is created.
class Service : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString errorName READ errorName NOTIFY changed)
    Q_PROPERTY(QString backendName READ backendName NOTIFY changed)
    Q_PROPERTY(QString backendDescription READ backendDescription NOTIFY changed)
    Q_PROPERTY(QString backendAuthor READ backendAuthor NOTIFY changed)
    Q_PROPERTY(QString daemonVersion READ daemonVersion NOTIFY changed)
    Q_PROPERTY(QString distroId READ distroId NOTIFY changed)
    Q_PROPERTY(QString networkState READ networkState NOTIFY changed)
    Q_PROPERTY(bool offline READ offline NOTIFY changed)
    Q_PROPERTY(bool locked READ locked NOTIFY changed)

public:
    static Service *instance();

    bool available() const { return m_available; }
    bool checking() const { return m_checking; }
    // Human-readable reason PackageKit cannot be used; contains the D-Bus error text verbatim.
    QString error() const { return m_error; }
    // D-Bus error name, e.g. org.freedesktop.DBus.Error.ServiceUnknown.
    QString errorName() const { return m_errorName; }
    QString backendName() const { return m_backendName; }
    QString backendDescription() const { return m_backendDescription; }
    QString backendAuthor() const { return m_backendAuthor; }
    QString daemonVersion() const { return m_daemonVersion; }
    QString distroId() const { return m_distroId; }
    // "unknown", "offline", "online", "wired", "wifi", "mobile"
    QString networkState() const { return m_networkState; }
    bool offline() const { return m_networkState == QLatin1String("offline"); }
    bool locked() const { return m_locked; }
    QStringList roles() const { return m_roles; }
    bool supports(PackageKit::Transaction::Role role) const;

    // Ask the bus (again). probed() fires when the answer is in — always
    // asynchronously, so callers can wait for it.
    Q_INVOKABLE void probe();

    // Whether transactions may make polkit ask the user (default) or must
    // fail instead of prompting (headless update check). Call before probe().
    void setInteractive(bool interactive) { m_interactive = interactive; }
    bool interactive() const { return m_interactive; }

    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();
    void probed();
    // Forwarded from the daemon once it is reachable.
    void updatesChanged();
    void repoListChanged();
    void restartScheduled();
    void daemonQuit();

private Q_SLOTS:
    // org.freedesktop.DBus.Local.Disconnected: the bus connection is gone.
    void busDisconnected();

private:
    explicit Service(QObject *parent = nullptr);
    void setUnavailable(const QString &errorName, const QString &message);
    void applyProperties(const QVariantMap &properties);
    void hookDaemon();

    bool m_available = false;
    bool m_checking = false;
    bool m_daemonHooked = false;
    bool m_watching = false;
    bool m_interactive = true;
    quint64 m_probeSerial = 0;
    QString m_error;
    QString m_errorName;
    QString m_backendName;
    QString m_backendDescription;
    QString m_backendAuthor;
    QString m_daemonVersion;
    QString m_distroId;
    QString m_networkState = QStringLiteral("unknown");
    bool m_locked = false;
    qulonglong m_roleBits = 0;
    QStringList m_roles;
};

} // namespace ShadowPk
