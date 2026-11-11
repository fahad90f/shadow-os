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

// Wi-Fi page: radio, access points, saved networks and the active connection.
//
// Sources
//   nmcli -t (terse)        radio state, devices and their IP details, the access
//                           point list, saved Wi-Fi profiles; changes that carry
//                           no secret (radio, rescan, activate / deactivate a saved
//                           profile, delete, autoconnect, MAC address policy)
//   org.freedesktop.NetworkManager (system bus)
//                           AddAndActivateConnection for a new network — the
//                           password is sent inside the D-Bus message, never on a
//                           command line; Device.StateChanged to follow the
//                           attempt; Settings.Connection.Delete to drop the new
//                           profile again when the attempt fails; change signals
//                           that trigger a refresh
//   /sys/class/rfkill       hard / soft block of the WLAN radio
//
// States: "ok", "radio-off" (the switch can turn it on; rfkillSoftBlocked says
// whether a soft block is the reason), "rfkill-hard", "no-adapter",
// "nm-not-running", "nm-missing" (nmcli not installed).
//
// networks rows: key, ssid, strength (0–100), bars (0–4), security (label),
//   securityKind ("open" | "owe" | "psk" | "sae" | "wep" | "enterprise"), band,
//   inUse, saved, savedUuid, connectable, note
// saved rows: uuid, name, ssid, autoconnect, lastUsed (seconds since the epoch), active
class WifiBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(bool radioEnabled READ radioEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool rfkillSoftBlocked READ rfkillSoftBlocked NOTIFY stateChanged)
    Q_PROPERTY(QString device READ device NOTIFY stateChanged)
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY stateChanged)

    Q_PROPERTY(QAbstractItemModel *networks READ networks CONSTANT)
    Q_PROPERTY(int networkCount READ networkCount NOTIFY networksChanged)
    Q_PROPERTY(QAbstractItemModel *saved READ saved CONSTANT)
    Q_PROPERTY(int savedCount READ savedCount NOTIFY networksChanged)
    // The connection of the Wi-Fi device: connected, ssid, stateText, strength,
    // security, band, channel, bssid, speed, ipv4, gateway4, dns, ipv6,
    // gateway6, mac, uuid, macPolicy.
    Q_PROPERTY(QVariantMap details READ details NOTIFY detailsChanged)

    Q_PROPERTY(bool scanning READ scanning NOTIFY busyChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY busyChanged)
    Q_PROPERTY(QString connectingSsid READ connectingSsid NOTIFY busyChanged)

public:
    explicit WifiBackend(QObject *parent = nullptr);
    ~WifiBackend() override;

    bool ready() const { return m_ready; }
    QString state() const { return m_state; }
    bool available() const { return m_state == QLatin1String("ok") || m_state == QLatin1String("radio-off"); }
    QString statusMessage() const { return m_statusMessage; }
    bool radioEnabled() const { return m_state == QLatin1String("ok"); }
    bool rfkillSoftBlocked() const { return m_rfkillSoft; }
    QString device() const { return m_device; }
    int deviceCount() const { return m_deviceCount; }

    QAbstractItemModel *networks() const;
    int networkCount() const;
    QAbstractItemModel *saved() const;
    int savedCount() const;
    QVariantMap details() const { return m_details; }

    bool scanning() const { return m_scanning; }
    bool connecting() const { return m_connecting; }
    QString connectingSsid() const { return m_connectingSsid; }

    Q_INVOKABLE void setRadioEnabled(bool enabled);
    Q_INVOKABLE void scan();
    // Connect to a listed network (a row key). 'password' is needed for a
    // secured network that has no saved profile; it is ignored otherwise.
    Q_INVOKABLE void connectNetwork(const QString &key, const QString &password = QString());
    // securityKind: "open", "psk" (WPA/WPA2 Personal) or "sae" (WPA3 Personal).
    Q_INVOKABLE void connectHidden(const QString &ssid, const QString &securityKind, const QString &password);
    Q_INVOKABLE void disconnectDevice();
    Q_INVOKABLE void forget(const QString &uuid);
    Q_INVOKABLE void setAutoconnect(const QString &uuid, bool autoconnect);
    // policy: "" (NetworkManager's default), "permanent", "random", "stable" or "preserve".
    Q_INVOKABLE void setMacPolicy(const QString &uuid, const QString &policy);
    // Empty if 'password' is acceptable for that kind of network, else why not.
    Q_INVOKABLE QString passwordProblem(const QString &securityKind, const QString &password) const;

    QJsonObject toJson() const;

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void networksChanged();
    void detailsChanged();
    void busyChanged();
    void failed(const QString &message);
    void connected(const QString &ssid);

private Q_SLOTS:
    void onDeviceStateChanged(uint newState, uint oldState, uint reason);

private:
    struct Fetch;
    void publish(const Fetch &fetch);
    void finishRefresh();
    void setUnavailable(const QString &state, const QString &message);
    void activateNew(const QString &ssidText, const QByteArray &ssid, const QString &securityKind,
                     const QString &password, const QString &apPath, bool hidden);
    void endAttempt(bool success, const QString &error);
    void runChange(const QStringList &arguments, const QString &failurePrefix, int timeoutMs = 30000);

    NetHw::Nm::Watcher *m_watcher = nullptr;
    NetHw::RowModel *m_networks = nullptr;
    NetHw::RowModel *m_saved = nullptr;
    QTimer m_poll;

    bool m_ready = false;
    bool m_refreshing = false;
    bool m_refreshAgain = false;
    QString m_state;
    QString m_statusMessage;
    bool m_rfkillSoft = false;
    bool m_rfkillHard = false;
    QString m_device;
    QString m_devicePath;
    int m_deviceCount = 0;
    QVariantMap m_details;

    bool m_scanning = false;
    bool m_connecting = false;
    QString m_connectingSsid;
    // The profile created by the attempt in progress (removed if it fails).
    QString m_attemptConnectionPath;
    bool m_attemptWatched = false;
    bool m_attemptSawActivity = false;
    QTimer m_attemptTimeout;
};
