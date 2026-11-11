// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QDBusMessage>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

#include <memory>

// Bluetooth page: adapter state, known and nearby devices, pairing.
//
// Source: BlueZ (org.bluez on the system bus) through KDE's BluezQt —
// adapters, devices and their change signals, pairing through a BluezQt
// agent registered for this application only (it does not replace the
// session's default agent). Two things BluezQt 6.13 does not offer in its
// installed headers are read directly:
//   org.bluez.Battery1.Percentage      battery level of a connected device
//   /sys/class/rfkill                  hard / soft block of the radio
//
// States: "ok" (an adapter exists; it may be switched off), "rfkill-soft"
// (blocked in software — switching Bluetooth on removes the block),
// "rfkill-hard" (blocked by a hardware switch or firmware), "no-adapter",
// "service-not-running" (bluetooth.service / org.bluez is not there),
// "no-system-bus" and "unsupported-build" (compiled without BluezQt,
// SHADOW_HAVE_BLUEZQT).
//
// Rows of the device models: ubi (D-Bus path, the key), address, name,
// kind, deviceIcon, paired, trusted, connected, battery (-1 = not reported),
// rssi, busy ("", "pairing", "connecting", "disconnecting", "removing").
class BluetoothBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)

    Q_PROPERTY(bool powered READ powered NOTIFY adapterChanged)
    Q_PROPERTY(bool discoverable READ discoverable NOTIFY adapterChanged)
    Q_PROPERTY(bool discovering READ discovering NOTIFY adapterChanged)
    Q_PROPERTY(QString adapterName READ adapterName NOTIFY adapterChanged)
    Q_PROPERTY(QString adapterAddress READ adapterAddress NOTIFY adapterChanged)
    Q_PROPERTY(int adapterCount READ adapterCount NOTIFY adapterChanged)
    Q_PROPERTY(int discoverableTimeout READ discoverableTimeout NOTIFY adapterChanged)

    Q_PROPERTY(QAbstractItemModel *pairedDevices READ pairedDevices CONSTANT)
    Q_PROPERTY(QAbstractItemModel *availableDevices READ availableDevices CONSTANT)
    Q_PROPERTY(int pairedCount READ pairedCount NOTIFY devicesChanged)
    Q_PROPERTY(int availableCount READ availableCount NOTIFY devicesChanged)

    // The question a pairing in progress asks, or an empty map.
    //   kind     "confirm" | "pin" | "passkey" | "display-pin" | "display-passkey" | "authorize" | "authorize-service"
    //   device   name of the device, address
    //   code     the PIN / passkey to compare or to type on the device
    //   uuid     service to authorise (authorize-service)
    Q_PROPERTY(QVariantMap prompt READ prompt NOTIFY promptChanged)

public:
    explicit BluetoothBackend(QObject *parent = nullptr);
    ~BluetoothBackend() override;

    bool ready() const { return m_ready; }
    QString state() const { return m_state; }
    bool available() const { return m_state == QLatin1String("ok") || m_state == QLatin1String("rfkill-soft"); }
    QString statusMessage() const { return m_statusMessage; }

    bool powered() const { return m_powered; }
    bool discoverable() const { return m_discoverable; }
    bool discovering() const { return m_discovering; }
    QString adapterName() const { return m_adapterName; }
    QString adapterAddress() const { return m_adapterAddress; }
    int adapterCount() const { return m_adapterCount; }
    int discoverableTimeout() const { return m_discoverableTimeout; }

    QAbstractItemModel *pairedDevices() const;
    QAbstractItemModel *availableDevices() const;
    int pairedCount() const;
    int availableCount() const;
    QVariantMap prompt() const { return m_prompt; }

    Q_INVOKABLE void setPowered(bool powered);
    Q_INVOKABLE void setDiscoverable(bool discoverable);
    Q_INVOKABLE void startScan();
    Q_INVOKABLE void stopScan();
    Q_INVOKABLE void pair(const QString &ubi);
    Q_INVOKABLE void connectDevice(const QString &ubi);
    Q_INVOKABLE void disconnectDevice(const QString &ubi);
    Q_INVOKABLE void setTrusted(const QString &ubi, bool trusted);
    Q_INVOKABLE void removeDevice(const QString &ubi);
    // Answer the current prompt. 'text' carries the PIN or passkey typed by the user.
    Q_INVOKABLE void answerPrompt(bool accept, const QString &text = QString());
    // Ask the system bus to start bluetooth.service (D-Bus activation).
    Q_INVOKABLE void startService();

    QJsonObject toJson() const;

    class Private;

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void adapterChanged();
    void devicesChanged();
    void promptChanged();
    void failed(const QString &message);

private Q_SLOTS:
    void scheduleRebuild();
    void rebuild();
    void onBatteryPropertiesChanged(const QString &interface, const QVariantMap &changed,
                                    const QStringList &invalidated, const QDBusMessage &message);

private:
    friend class Private;

    bool m_ready = false;
    QString m_state;
    QString m_statusMessage;
    bool m_powered = false;
    bool m_discoverable = false;
    bool m_discovering = false;
    QString m_adapterName;
    QString m_adapterAddress;
    int m_adapterCount = 0;
    int m_discoverableTimeout = 0;
    QVariantMap m_prompt;
    std::unique_ptr<Private> d;
};
