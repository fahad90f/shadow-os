// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Solid/Device>
#include <Solid/SolidNamespace>

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

// Storage volumes known to Solid (UDisks2 on a running system): internal and
// removable partitions, optical media and mounted network shares.
//
// Each entry of `devices`:
//   udi, label, icon, mounted, url, path, device, fsType,
//   total, free (bytes; free is -1 while unmounted), removable, optical, busy
class Devices : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    // Why the list is empty (no storage service, or simply no volumes).
    Q_PROPERTY(QString emptyReason READ emptyReason NOTIFY devicesChanged)

public:
    static Devices *instance();
    static Devices *create(QQmlEngine *, QJSEngine *);

    QVariantList devices() const { return m_list; }
    QString emptyReason() const;

    Q_INVOKABLE void refresh();
    // Mount the volume; mounted() follows on success.
    Q_INVOKABLE void mount(const QString &udi);
    // Unmount; Solid also ejects or powers off removable drives once their
    // last volume is unmounted.
    Q_INVOKABLE void unmount(const QString &udi);
    // Open the tray of an optical drive.
    Q_INVOKABLE void eject(const QString &udi);

    QJsonArray toJson() const;

Q_SIGNALS:
    void devicesChanged();
    void mounted(const QString &udi, const QUrl &url);

private:
    explicit Devices(QObject *parent = nullptr);
    void rescan();
    void announceMounted(const QString &udi);
    void watch(Solid::Device &device);
    QVariantMap describe(const Solid::Device &device) const;
    QString labelOf(const QString &udi) const;
    void operationDone(const QString &udi, Solid::ErrorType error, const QVariant &errorData, const QString &what);

    QHash<QString, Solid::Device> m_devices;    // keeps the device interfaces alive
    QHash<QString, Solid::Device> m_ejecting;   // optical drives with an eject request in flight
    QStringList m_order;
    QSet<QString> m_watched;
    QSet<QString> m_busy;
    QSet<QString> m_mounting;       // mounts requested through mount()
    QVariantList m_list;
    QTimer m_spaceTimer;
};
