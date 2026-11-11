// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusConnection>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <functional>

// NetworkManager access shared by the Wi-Fi, Wired & Proxy and VPN back-ends.
//
// Reading and simple changes go through nmcli in terse mode ("nmcli -t"),
// which is NetworkManager's stable scripting interface; secrets and change
// notifications go over the system bus (org.freedesktop.NetworkManager).
namespace NetHw::Nm {

inline QString service() { return QStringLiteral("org.freedesktop.NetworkManager"); }
inline QString rootPath() { return QStringLiteral("/org/freedesktop/NetworkManager"); }

// A "KEY:value" record of nmcli's multi-line output (device show,
// connection show <id>). Values are not escaped in that mode.
using Record = QMap<QString, QString>;

// One line of tabular terse output → fields. ':' separates fields; nmcli
// escapes ':' and '\' inside a value with a backslash.
QStringList splitTerse(const QString &line);

// Multi-line terse output → records (separated by blank lines).
QList<Record> parseRecords(const QString &output);

// All values of an indexed key: "IP4.DNS" → IP4.DNS[1], IP4.DNS[2], …
QStringList indexedValues(const Record &record, const QString &key);

// "100 (connected)" → 100; -1 if there is no leading number.
int leadingNumber(const QString &text);

// nmcli prints "--" for an unset value in some columns.
QString unset(const QString &value);

bool cliAvailable();

using CliCallback = std::function<void(bool ok, const QString &out, const QString &error)>;

// Run "nmcli <args>" asynchronously. 'error' is the first line of nmcli's
// error output without the "Error: " prefix.
void run(QObject *context, const QStringList &args, CliCallback done, int timeoutMs = 30000);

// Tracks whether NetworkManager owns its bus name and turns its D-Bus
// signals into one coalesced changed() notification.
class Watcher : public QObject
{
    Q_OBJECT

public:
    explicit Watcher(QObject *parent = nullptr);

    bool busConnected() const;
    // Known only after the first runningChanged(); false until then.
    bool running() const { return m_running; }
    bool known() const { return m_known; }

Q_SIGNALS:
    // NetworkManager appeared on or vanished from the system bus (also
    // emitted once after construction with the initial answer).
    void runningChanged();
    // Something in NetworkManager changed (devices, access points,
    // connections, properties) — coalesced to at most one per second.
    void changed();

private Q_SLOTS:
    void onNmSignal();

private:
    void setRunning(bool running);

    QDBusConnection m_bus;
    QTimer m_coalesce;
    bool m_running = false;
    bool m_known = false;
};

// ---- D-Bus calls nmcli has no safe equivalent for ----------------------------

// Connection settings as NetworkManager's D-Bus API takes them: a{sa{sv}}.
using Settings = QMap<QString, QVariantMap>;

using ActivateCallback = std::function<void(bool ok, const QString &connectionPath,
                                            const QString &activePath, const QString &error)>;

// org.freedesktop.NetworkManager.AddAndActivateConnection: create a profile
// from 'settings' and activate it on 'devicePath'. Secrets travel inside the
// D-Bus message only — never through a command line. 'specificObject' is an
// access point path, or "/" when there is none.
void addAndActivate(const Settings &settings, const QString &devicePath, const QString &specificObject,
                    QObject *context, ActivateCallback done);

// org.freedesktop.NetworkManager.Settings.Connection.Delete on a profile path.
void deleteConnection(const QString &connectionPath, QObject *context,
                      std::function<void(bool ok, const QString &error)> done);

// Why an activation failed, from NMDeviceStateReason.
QString stateReasonText(uint reason);

bool isUuid(const QString &text);
// An interface name as the kernel accepts it (and safe as a tool argument).
bool isInterfaceName(const QString &text);

// ---- validation (authoritative; the pages only mirror it) --------------------

bool isIpv4Address(const QString &text);
bool isIpv6Address(const QString &text);
// "address/prefix"; the prefix is mandatory (1–32 or 1–128).
bool isIpv4Cidr(const QString &text);
bool isIpv6Cidr(const QString &text);

} // namespace NetHw::Nm
