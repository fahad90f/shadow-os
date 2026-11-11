// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusObjectPath>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

// Users page.
//
// * Accounts: AccountsService (org.freedesktop.Accounts on the system bus):
//   ListCachedUsers and each user's properties; CreateUser, DeleteUser,
//   User.SetRealName, User.SetAccountType (administrator = member of the
//   "sudo" group on Debian), User.SetIconFile. AccountsService asks polkit
//   itself (org.freedesktop.accounts.user-administration for other accounts,
//   change-own-user-data for one's own name and picture).
// * Passwords never appear on a command line:
//     - one's own password: passwd(1) is run as the user, with the current and
//       the new password on its standard input, so PAM checks the current one;
//     - another account's password (administrator action):
//       "pkexec /usr/libexec/nexus/users-helper set-password" with
//       "<user>:<password>" on standard input, which the helper passes to
//       chpasswd(8) the same way.
// * Automatic login: SDDM's [Autologin] User, read from /usr/lib/sddm/sddm.conf.d,
//   /etc/sddm.conf.d and /etc/sddm.conf in SDDM's order; changed with
//   "pkexec users-helper set-autologin <user> | clear-autologin"
//   (polkit action org.nexus.controlcenter.users.manage).
// * Who is signed in: org.freedesktop.login1 ListUsers (when logind runs).
//
// Blocks that cannot be overridden: the account the Control Center runs as
// and accounts that are signed in cannot be removed; the last administrator
// can neither be removed nor made a standard user.
//
// Test hook (development only): NEXUS_CC_HELPER_DIR replaces /usr/libexec/nexus.
class UsersBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    // [{path, uid, userName, realName, displayName, admin, locked, iconFile, home, current, loggedIn, autoLogin, local, passwordSet}]
    Q_PROPERTY(QVariantList users READ users NOTIFY changed)
    Q_PROPERTY(QString currentUserName READ currentUserName CONSTANT)
    Q_PROPERTY(int adminCount READ adminCount NOTIFY changed)
    Q_PROPERTY(bool sessionsKnown READ sessionsKnown NOTIFY changed)
    Q_PROPERTY(bool sddmInstalled READ sddmInstalled CONSTANT)
    Q_PROPERTY(QString autoLoginUser READ autoLoginUser NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // Changes whenever a picture was replaced, to defeat the image cache.
    Q_PROPERTY(int iconRevision READ iconRevision NOTIFY changed)

public:
    explicit UsersBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    bool available() const { return m_available; }
    QString statusMessage() const { return m_statusMessage; }
    QVariantList users() const { return m_users; }
    QString currentUserName() const { return m_currentUser; }
    int adminCount() const;
    bool sessionsKnown() const { return m_sessionsKnown; }
    bool sddmInstalled() const;
    QString autoLoginUser() const { return m_autoLoginUser; }
    bool busy() const { return m_busy > 0; }
    int iconRevision() const { return m_iconRevision; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE QVariantMap user(const QString &userName) const;

    // Validation: empty string = acceptable, otherwise the reason.
    Q_INVOKABLE QString userNameProblem(const QString &userName) const;
    Q_INVOKABLE QString realNameProblem(const QString &realName) const;
    Q_INVOKABLE QString passwordProblem(const QString &password, const QString &repeated) const;
    // Why the account cannot be removed / lose its administrator role; empty = allowed.
    Q_INVOKABLE QString removeBlockReason(const QString &userName) const;
    Q_INVOKABLE QString demoteBlockReason(const QString &userName) const;
    // True when changing this account's password needs the current one (own account, not root).
    Q_INVOKABLE bool needsCurrentPassword(const QString &userName) const;

    Q_INVOKABLE void addUser(const QString &userName, const QString &realName, bool administrator, const QString &password);
    Q_INVOKABLE void removeUser(const QString &userName, bool removeFiles);
    Q_INVOKABLE void setRealName(const QString &userName, const QString &realName);
    Q_INVOKABLE void setAdministrator(const QString &userName, bool administrator);
    Q_INVOKABLE void setPassword(const QString &userName, const QString &password, const QString &currentPassword);
    Q_INVOKABLE void setAutoLogin(const QString &userName, bool on);
    Q_INVOKABLE void setAvatar(const QString &userName, const QUrl &imageFile);

    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();
    void busyChanged();
    void failed(const QString &message);
    void succeeded(const QString &message);

private Q_SLOTS:
    void scheduleRefresh();

private:
    void finishRefresh();
    void loadAutoLogin();
    void setBusy(bool on);
    QString pathOf(const QString &userName) const;
    void callUser(const QString &userName, const QString &method, const QVariantList &arguments,
                  const QString &successMessage, const QString &failurePrefix);
    void runHelper(const QStringList &arguments, const QByteArray &stdinData,
                   std::function<void(bool ok, const QString &error)> done);

    bool m_ready = false;
    bool m_available = false;
    QString m_statusMessage;
    QVariantList m_users;
    QList<QVariantMap> m_collecting;
    int m_pending = 0;
    int m_generation = 0;
    QSet<qulonglong> m_loggedIn;
    bool m_sessionsKnown = false;
    QString m_currentUser;
    QString m_autoLoginUser;
    int m_busy = 0;
    int m_iconRevision = 0;
    QTimer m_refreshTimer;
};
