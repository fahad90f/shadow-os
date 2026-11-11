// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QEventLoop>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <functional>

// Private helpers shared by the Network and Hardware settings back-ends
// (Wi-Fi, Wired & Proxy, Bluetooth, VPN, Displays, Sound, Power, Keyboard,
// Mouse). Nothing in here is exposed to QML as a type of its own.
namespace NetHw {

// A list of rows (QVariantMap) with a fixed set of roles, exposed to QML as
// an item model. setRows() replaces the content but keeps the rows whose key
// is unchanged: views only see the inserts, removals, moves and value changes
// that really happened, so delegates (and what the user is doing with them —
// hovering, dragging a slider) survive a refresh.
//
//   Repeater {
//       model: backend.networks
//       NxRow { required property string ssid; required property int signal; ... }
//   }
//
// Every row must carry a value for every role; 'keyRole' identifies a row.
class RowModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    RowModel(const QStringList &roles, const QString &keyRole, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_rows.size()); }
    void setRows(const QVariantList &rows);
    QVariantList rows() const;
    Q_INVOKABLE QVariantMap get(int row) const;
    // The row with the given key, or an empty map.
    Q_INVOKABLE QVariantMap find(const QString &key) const;

Q_SIGNALS:
    void countChanged();

private:
    QString keyOf(const QVariantMap &row) const { return row.value(m_keyRole).toString(); }

    QStringList m_roles;
    QString m_keyRole;
    QList<QVariantMap> m_rows;
};

// --dump support: spin a local event loop until backend->ready() is true or
// the timeout elapses. Back-ends gather their data asynchronously.
template<typename Backend>
void waitUntilReady(Backend *backend, int timeoutMs = 8000)
{
    if (backend->ready())
        return;
    QEventLoop loop;
    QObject::connect(backend, &Backend::readyChanged, &loop, [backend, &loop] {
        if (backend->ready())
            loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
}

// ---- D-Bus (always asynchronous) -------------------------------------------

using DBusReplyCallback = std::function<void(const QDBusMessage &reply)>;
using DBusPropertiesCallback = std::function<void(bool ok, const QVariantMap &properties, const QString &error)>;
using DBusResultCallback = std::function<void(bool ok, const QString &error)>;

QDBusMessage dbusMethod(const QString &service, const QString &path, const QString &interface,
                        const QString &method, const QVariantList &arguments = {});

// 'done' receives the reply (reply.type() == QDBusMessage::ErrorMessage on
// failure). Nothing is called if 'context' is destroyed first.
void dbusCall(const QDBusConnection &bus, const QDBusMessage &message, QObject *context,
              DBusReplyCallback done, int timeoutMs = 5000);

// org.freedesktop.DBus.Properties.GetAll / Set.
void dbusGetAll(const QDBusConnection &bus, const QString &service, const QString &path,
                const QString &interface, QObject *context, DBusPropertiesCallback done, int timeoutMs = 5000);
void dbusSet(const QDBusConnection &bus, const QString &service, const QString &path,
             const QString &interface, const QString &property, const QVariant &value,
             QObject *context, DBusResultCallback done, int timeoutMs = 5000);

// Whether 'service' currently has an owner on 'bus'.
void dbusNameHasOwner(const QDBusConnection &bus, const QString &service, QObject *context,
                      std::function<void(bool owned)> done);

// "org.freedesktop.DBus.Error.ServiceUnknown: The name … was not provided" →
// a message fit for the user: the text after the error name, or the name.
QString dbusErrorText(const QDBusMessage &reply);

// Unwrap nested D-Bus containers (QDBusArgument, QDBusVariant, object paths)
// into plain QVariant lists, maps and strings.
QVariant dbusPlain(const QVariant &value);

// ---- rfkill -----------------------------------------------------------------

struct RfkillState
{
    bool present = false;   // at least one switch of that type exists
    bool hard = false;      // blocked by a hardware switch / firmware
    bool soft = false;      // blocked in software
};

// State of the kernel rfkill switches of 'type' ("wlan", "bluetooth"), read
// from /sys/class/rfkill.
RfkillState rfkillState(const QString &type);

// ---- misc ---------------------------------------------------------------------

// Environment for child tools that must talk to the session's compositor
// rather than inherit this process's test/dump platform settings.
QProcessEnvironment toolEnvironment();

// First line of a tool's error output, without a leading "Error: ".
QString firstErrorLine(const QString &text);

struct ToolResult
{
    int exitCode = -1;      // -1: could not be started or crashed, -2: timed out
    QByteArray out;
    QByteArray err;
    bool ok() const { return exitCode == 0; }
};

// Run a tool with an argument vector (never a shell) in toolEnvironment().
// Like Nexus::Command::run(), but for tools that must reach the compositor.
void runTool(QObject *context, const QString &program, const QStringList &arguments,
             std::function<void(const ToolResult &)> done, int timeoutMs = 15000);

} // namespace NetHw
