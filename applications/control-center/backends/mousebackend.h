// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>

namespace NetHw {
class RowModel;
}

// Mouse & Touchpad page: the pointer devices of the KWin Wayland session.
//
// Source: KWin's libinput device API on the session bus —
//   org.kde.KWin  /org/kde/KWin/InputDevice
//       org.kde.KWin.InputDeviceManager: devicesSysNames, deviceAdded, deviceRemoved
//   org.kde.KWin  /org/kde/KWin/InputDevice/<sysName>
//       org.kde.KWin.InputDevice: one property per libinput setting plus a
//       supports… flag (names checked against KWin 6.3.6)
// Values are written with org.freedesktop.DBus.Properties.Set; KWin applies
// them to libinput and stores them in kcminputrc itself.
//
// KWin does not announce changes of device properties on the bus, so a
// device is re-read after every write and when devices come and go.
//
// Each row of 'devices' has: sysName, name, touchpad, properties (map with
// every property of org.kde.KWin.InputDevice that D-Bus can carry).
class MouseBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QAbstractItemModel *devices READ devices CONSTANT)
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY devicesChanged)

public:
    explicit MouseBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    // "ok", "no-session-bus", "no-kwin", "no-input-api" or "no-devices".
    QString state() const { return m_state; }
    bool available() const { return m_state == QLatin1String("ok"); }
    QString statusMessage() const { return m_statusMessage; }
    QAbstractItemModel *devices() const;
    int deviceCount() const;

    // Write one boolean / real property of a device. Only the settings this
    // page offers are accepted.
    Q_INVOKABLE void setBool(const QString &sysName, const QString &property, bool value);
    Q_INVOKABLE void setReal(const QString &sysName, const QString &property, double value);

    QJsonObject toJson() const;

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void devicesChanged();
    void failed(const QString &message);

private:
    void setState(const QString &state, const QString &message);
    void finish(const QString &state, const QString &message, quint64 generation);
    void loadDevices(const QStringList &sysNames, quint64 generation);
    void reloadDevice(const QString &sysName);
    void publish();
    void write(const QString &sysName, const QString &property, const QVariant &value);

    bool m_ready = false;
    QString m_state;
    QString m_statusMessage;
    quint64 m_generation = 0;
    QStringList m_order;                        // sysNames as KWin lists them
    QMap<QString, QVariantMap> m_properties;    // sysName → properties
    NetHw::RowModel *m_model = nullptr;
};
