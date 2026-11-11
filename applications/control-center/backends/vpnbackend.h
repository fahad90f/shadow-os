// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

namespace NetHw {
class RowModel;
namespace Nm {
class Watcher;
}
}

// VPN page: NetworkManager's VPN (plug-in) and WireGuard profiles.
//
// Sources
//   nmcli -t connection show [uuid…]    profiles, their state, type, VPN
//                                       service and connection.secondaries
//   nmcli connection up|down|delete|modify|import
//   /usr/lib/NetworkManager/VPN/*.name, /etc/NetworkManager/VPN/*.name
//                                       the VPN plug-ins that are installed
//
// Connect automatically:
//   * WireGuard profiles are ordinary NetworkManager devices — the switch is
//     connection.autoconnect.
//   * NetworkManager does not auto-connect plug-in VPNs by themselves
//     (nm-settings: "autoconnect is not implemented for VPN profiles"). It
//     starts them together with a network connection that lists them in
//     connection.secondaries — so the switch adds the VPN to (or removes it
//     from) the profiles of the networks that are connected right now.
//
// Importing
//   .conf → "nmcli connection import type wireguard", .ovpn → "type openvpn"
//   (needs the network-manager-openvpn plug-in). NetworkManager derives the
//   WireGuard interface name from the file name; a file whose name is not a
//   valid interface name is imported from a private temporary copy with a
//   sanitised name and renamed afterwards. Imported WireGuard profiles are
//   set to not connect automatically, so that importing never reroutes
//   traffic by itself.
//
// profiles rows: uuid, name, kind ("wireguard" | "vpn"), typeLabel, stateId
//   ("active" | "activating" | "deactivating" | "inactive"), stateText,
//   active, autoconnect, autoconnectNote, problem (missing plug-in), busy
class VpnBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QAbstractItemModel *profiles READ profiles CONSTANT)
    Q_PROPERTY(int profileCount READ profileCount NOTIFY profilesChanged)
    // Installed NetworkManager VPN plug-ins: [{ name, service, label }].
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged)
    Q_PROPERTY(bool openvpnInstalled READ openvpnInstalled NOTIFY pluginsChanged)
    Q_PROPERTY(bool importing READ importing NOTIFY busyChanged)

public:
    explicit VpnBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    // "ok", "nm-not-running" or "nm-missing".
    QString state() const { return m_state; }
    bool available() const { return m_state == QLatin1String("ok"); }
    QString statusMessage() const { return m_statusMessage; }
    QAbstractItemModel *profiles() const;
    int profileCount() const;
    QVariantList plugins() const { return m_plugins; }
    bool openvpnInstalled() const;
    bool importing() const { return m_importing; }

    Q_INVOKABLE void connectProfile(const QString &uuid);
    Q_INVOKABLE void disconnectProfile(const QString &uuid);
    Q_INVOKABLE void removeProfile(const QString &uuid);
    Q_INVOKABLE void setAutoconnect(const QString &uuid, bool autoconnect);
    // A WireGuard .conf or OpenVPN .ovpn file chosen by the user.
    Q_INVOKABLE void importFile(const QUrl &file);

    QJsonObject toJson() const;

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void profilesChanged();
    void pluginsChanged();
    void busyChanged();
    void failed(const QString &message);
    void imported(const QString &name);

private:
    struct Fetch;
    void publish(const Fetch &fetch);
    void finishRefresh();
    void setUnavailable(const QString &state, const QString &message);
    void loadPlugins();
    void setBusy(const QString &uuid, bool busy);
    // Run nmcli calls one after the other; stop at the first that fails.
    void runSteps(const QString &uuid, QList<QStringList> steps, const QString &failurePrefix);
    void nextStep();
    QString friendlyError(const QString &error) const;
    void finishImport(bool ok, const QString &out, const QString &error, const QString &wantedName, bool wireguard);

    NetHw::Nm::Watcher *m_watcher = nullptr;
    NetHw::RowModel *m_profiles = nullptr;
    QTimer m_poll;
    bool m_ready = false;
    bool m_refreshing = false;
    bool m_refreshAgain = false;
    QString m_state;
    QString m_statusMessage;
    QVariantList m_plugins;
    QSet<QString> m_busy;
    bool m_importing = false;

    // What the last refresh learnt about the other profiles.
    struct Base
    {
        QString uuid, name;
        bool active = false;
        QStringList secondaries;
    };
    QList<Base> m_bases;

    // The queue of nmcli calls of the change in progress.
    QList<QStringList> m_steps;
    QString m_stepsUuid;
    QString m_stepsPrefix;
};
