// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>

// TCP and UDP sockets of this machine, from `ss -H -O -t -u -n -a -p`
// (iproute2): one socket per line, numeric addresses, owning processes.
//
// The kernel only tells a user which processes own *their* sockets. For the
// sockets of other users the process column is empty; showAllProcesses()
// re-reads the list through the privileged helper (polkit action
// org.nexus.networkcenter.sockets), which runs exactly the same `ss` call as
// root.
class SocketList : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList sockets READ sockets NOTIFY socketsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool administratorView READ administratorView NOTIFY viewChanged)
    Q_PROPERTY(bool runningAsRoot READ runningAsRoot CONSTANT)
    Q_PROPERTY(QString updatedAt READ updatedAt NOTIFY socketsChanged)
    Q_PROPERTY(int total READ total NOTIFY socketsChanged)
    Q_PROPERTY(int listening READ listening NOTIFY socketsChanged)
    Q_PROPERTY(int exposed READ exposed NOTIFY socketsChanged)
    Q_PROPERTY(int established READ established NOTIFY socketsChanged)
    Q_PROPERTY(int withoutProcess READ withoutProcess NOTIFY socketsChanged)

public:
    explicit SocketList(QObject *parent = nullptr);

    QVariantList sockets() const { return m_sockets; }
    bool loading() const { return m_loading; }
    bool ready() const { return m_ready; }
    QString error() const { return m_error; }
    bool administratorView() const { return m_adminView; }
    bool runningAsRoot() const;
    QString updatedAt() const { return m_updatedAt; }
    int total() const { return int(m_sockets.size()); }
    int listening() const { return m_listening; }
    int exposed() const { return m_exposed; }
    int established() const { return m_established; }
    int withoutProcess() const { return m_withoutProcess; }

    // Re-read the list the way it is currently shown (own view, or again
    // through the helper while the administrator view is on).
    Q_INVOKABLE void refresh();
    // Read the list as root through the helper; asks for authentication.
    Q_INVOKABLE void showAllProcesses();
    Q_INVOKABLE void showOwnView();

    static QVariantList parse(const QStringList &lines);
    QJsonObject dump() const;

Q_SIGNALS:
    void socketsChanged();
    void loadingChanged();
    void readyChanged();
    void errorChanged();
    void viewChanged();
    void refreshed();
    // The administrator view could not be obtained (cancelled, no polkit, …).
    void elevationFailed(const QString &message);

private:
    void runOwn();
    void runElevated();
    void setSockets(const QVariantList &sockets);
    void setError(const QString &error);
    void finish();

    QVariantList m_sockets;
    bool m_loading = false;
    bool m_ready = false;
    bool m_adminView = false;
    QString m_error;
    QString m_updatedAt;
    int m_listening = 0, m_exposed = 0, m_established = 0, m_withoutProcess = 0;
};
