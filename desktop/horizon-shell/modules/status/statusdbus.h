// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small QtDBus helpers shared by the status back-ends. Everything here is
// asynchronous: no call in this file blocks the shell's GUI thread.
#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class QDBusServiceWatcher;

namespace StatusModule {

// ---- values that arrive inside D-Bus variants -------------------------------
// "o": the path, or an empty string for "/" (D-Bus APIs use "/" for "none").
QString dbusObjectPath(const QVariant &value);
// "ao"
QStringList dbusObjectPaths(const QVariant &value);
// "a{sv}"
QVariantMap dbusMap(const QVariant &value);
// "aa{sv}"
QList<QVariantMap> dbusMapList(const QVariant &value);

// Sends 'message' and runs 'done' with the reply (or the error message) when
// it arrives. Nothing is called if 'context' is destroyed first.
void dbusCall(const QDBusConnection &bus, const QDBusMessage &message, QObject *context,
              const std::function<void(const QDBusMessage &reply)> &done, int timeoutMs = 10000);

// A method call that never starts (D-Bus-activates) the service: asking for
// the state of a service the administrator has switched off must not turn it
// on again.
QDBusMessage dbusMethod(const QString &service, const QString &path, const QString &interface,
                        const QString &method, bool autoStart = false);

// Calls 'up' when 'service' gets an owner on 'bus' and 'down' when it loses it.
QDBusServiceWatcher *dbusWatchService(const QDBusConnection &bus, const QString &service, QObject *context,
                                      const std::function<void()> &up, const std::function<void()> &down);

// Cache of the properties of one interface of one remote object, kept current
// through org.freedesktop.DBus.Properties (GetAll once, PropertiesChanged
// afterwards).
class DBusProperties : public QObject
{
    Q_OBJECT

public:
    DBusProperties(const QDBusConnection &bus, const QString &service, const QString &path,
                   const QString &interface, QObject *parent = nullptr);

    QString service() const { return m_service; }
    QString path() const { return m_path; }
    QString interface() const { return m_interface; }

    // Whether calls may start the service through D-Bus activation (default: no).
    void setAutoStart(bool autoStart) { m_autoStart = autoStart; }

    // True while the cache holds the result of a successful GetAll.
    bool isValid() const { return m_valid; }
    // True once a GetAll has finished, whatever its result.
    bool isLoaded() const { return m_loaded; }
    QString errorMessage() const { return m_error; }

    const QVariantMap &values() const { return m_values; }
    bool contains(const QString &name) const { return m_values.contains(name); }
    QVariant value(const QString &name) const { return m_values.value(name); }
    QString string(const QString &name) const { return m_values.value(name).toString(); }
    bool boolean(const QString &name, bool fallback = false) const;
    qlonglong integer(const QString &name, qlonglong fallback = 0) const;
    double real(const QString &name, double fallback = 0) const;

    // (Re)reads all properties.
    void load();
    // Forgets everything, e.g. when the service has gone.
    void clear();
    // Writes one property; 'done' gets an empty string on success, else the error.
    void set(const QString &name, const QVariant &value,
             const std::function<void(const QString &error)> &done = {});

Q_SIGNALS:
    // A GetAll finished; isValid() tells whether it succeeded.
    void loaded();
    // The cached values changed (also emitted together with loaded()).
    void changed();

private Q_SLOTS:
    void propertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);

private:
    QDBusConnection m_bus;
    QString m_service, m_path, m_interface;
    QVariantMap m_values;
    QString m_error;
    quint64 m_serial = 0;
    bool m_autoStart = false;
    bool m_valid = false;
    bool m_loaded = false;
};

} // namespace StatusModule
