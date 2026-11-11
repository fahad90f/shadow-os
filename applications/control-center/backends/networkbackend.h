// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantMap>

namespace NetHw {
class RowModel;
namespace Nm {
class Watcher;
}
}

// Wired & Proxy page.
//
// Wired interfaces — NetworkManager through nmcli -t:
//   nmcli device show             state, carrier, speed, MAC, addresses, DNS
//   nmcli connection show [uuid…] the profile of each interface: IPv4 / IPv6
//                                 method, addresses, gateway, DNS
//   nmcli device connect|disconnect <ifname>
//   nmcli connection modify uuid … ipv4.* ipv6.*   then
//   nmcli connection up uuid … ifname …            to apply to a live interface
// Every value is validated here before it reaches nmcli (IP addresses with
// inet_pton, prefix lengths, interface names, UUIDs).
//
// Proxy — two places, written together by applyProxy():
//   ~/.config/environment.d/90-shadow-proxy.conf   read by systemd --user at
//       login and inherited by the session: http_proxy, https_proxy,
//       all_proxy (SOCKS), no_proxy and their upper-case twins; auto_proxy
//       with the PAC URL in automatic mode. The file is removed for "none".
//       Running applications keep the environment they were started with —
//       the change reaches everything only after logging out and in again.
//   NetworkManager                proxy.method / proxy.pac-url of the active
//       Ethernet and Wi-Fi profiles (NetworkManager can express "none" and
//       "automatic (PAC)" only, so manual proxies exist in the environment
//       file alone).
// Proxy hosts never carry a user name or password: credentials do not belong
// in an environment file.
//
// devices rows: name, stateCode, stateText, connected, managed, carrier,
//   speed, mac, profile, uuid, ipv4, gateway4, ipv6, gateway6, dns,
//   method4, addresses4, profileGateway4, dns4, method6, addresses6,
//   profileGateway6, dns6
class NetworkBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QAbstractItemModel *devices READ devices CONSTANT)
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY devicesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    // mode ("none" | "manual" | "auto"), httpHost, httpPort, httpsHost,
    // httpsPort, socksHost, socksPort, noProxy, pacUrl
    Q_PROPERTY(QVariantMap proxy READ proxy NOTIFY proxyChanged)
    Q_PROPERTY(QString proxyFile READ proxyFile CONSTANT)
    Q_PROPERTY(QString proxyFileProblem READ proxyFileProblem NOTIFY proxyChanged)
    // Names of the active profiles whose NetworkManager proxy setting is "auto".
    Q_PROPERTY(QStringList proxyProfiles READ proxyProfiles NOTIFY devicesChanged)

public:
    explicit NetworkBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    // "ok", "nm-not-running" or "nm-missing".
    QString state() const { return m_state; }
    bool available() const { return m_state == QLatin1String("ok"); }
    QString statusMessage() const { return m_statusMessage; }
    QAbstractItemModel *devices() const;
    int deviceCount() const;
    bool busy() const { return m_busy; }

    QVariantMap proxy() const { return m_proxy; }
    QString proxyFile() const;
    QString proxyFileProblem() const { return m_proxyFileProblem; }
    QStringList proxyProfiles() const { return m_proxyProfiles; }

    Q_INVOKABLE void setDeviceEnabled(const QString &device, bool enabled);
    // settings: method4 ("auto" | "manual"), addresses4, gateway4, dns4,
    //           method6 ("auto" | "manual" | "disabled"), addresses6, gateway6, dns6.
    // Address lists are separated by commas or spaces; addresses carry a prefix ("192.0.2.10/24").
    Q_INVOKABLE QString ipProblem(const QVariantMap &settings) const;
    Q_INVOKABLE void applyIp(const QString &device, const QVariantMap &settings);

    Q_INVOKABLE QString proxyProblem(const QVariantMap &proxy) const;
    Q_INVOKABLE void applyProxy(const QVariantMap &proxy);

    QJsonObject toJson() const;

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void devicesChanged();
    void busyChanged();
    void proxyChanged();
    void failed(const QString &message);
    void ipApplied(const QString &device);
    // 'note' says what was written and what the user still has to do.
    void proxyApplied(const QString &note);

private:
    struct Fetch;
    void publish(const Fetch &fetch);
    void finishRefresh();
    void setUnavailable(const QString &state, const QString &message);
    void setBusy(bool busy);
    void loadProxyFile();
    bool writeProxyFile(const QVariantMap &proxy, QString *error);
    void applyProxyToProfiles(const QVariantMap &proxy, const QString &note);
    void nextProxyProfile();

    NetHw::Nm::Watcher *m_watcher = nullptr;
    NetHw::RowModel *m_devices = nullptr;
    QTimer m_poll;
    bool m_ready = false;
    bool m_refreshing = false;
    bool m_refreshAgain = false;
    bool m_busy = false;
    QString m_state;
    QString m_statusMessage;

    QVariantMap m_proxy;
    QString m_proxyFileProblem;
    QStringList m_proxyProfiles;
    // Active Ethernet / Wi-Fi profiles: uuid → (name, proxy.method, proxy.pac-url)
    struct ActiveProfile
    {
        QString uuid, name, method, pacUrl;
    };
    QList<ActiveProfile> m_activeProfiles;
    // Profiles still to be updated by the applyProxy() in progress.
    QList<ActiveProfile> m_proxyQueue;
    QStringList m_proxyFailures;
    QString m_proxyMethod, m_proxyPacUrl, m_proxyNote;
};
