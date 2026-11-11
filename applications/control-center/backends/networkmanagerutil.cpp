// SPDX-License-Identifier: GPL-3.0-or-later
#include "networkmanagerutil.h"

#include "networkcommon.h"

#include <QCoreApplication>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QRegularExpression>

#include <command.h>

#include <arpa/inet.h>

namespace NetHw::Nm {

QStringList splitTerse(const QString &line)
{
    QStringList fields;
    QString current;
    bool escaped = false;
    for (const QChar c : line) {
        if (escaped) {
            current.append(c);
            escaped = false;
        } else if (c == QLatin1Char('\\')) {
            escaped = true;
        } else if (c == QLatin1Char(':')) {
            fields.append(current);
            current.clear();
        } else {
            current.append(c);
        }
    }
    fields.append(current);
    return fields;
}

QList<Record> parseRecords(const QString &output)
{
    QList<Record> records;
    Record current;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) {
            if (!current.isEmpty()) {
                records.append(current);
                current.clear();
            }
            continue;
        }
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        current.insert(line.left(colon), line.mid(colon + 1));
    }
    if (!current.isEmpty())
        records.append(current);
    return records;
}

QStringList indexedValues(const Record &record, const QString &key)
{
    QStringList values;
    for (int i = 1; ; ++i) {
        const auto it = record.constFind(QStringLiteral("%1[%2]").arg(key).arg(i));
        if (it == record.constEnd())
            break;
        values.append(it.value());
    }
    return values;
}

int leadingNumber(const QString &text)
{
    qsizetype end = 0;
    while (end < text.size() && text.at(end).isDigit())
        ++end;
    if (end == 0)
        return -1;
    bool ok = false;
    const int value = text.left(end).toInt(&ok);
    return ok ? value : -1;
}

QString unset(const QString &value)
{
    return value == QLatin1String("--") ? QString() : value;
}

bool cliAvailable()
{
    return Shadow::Command::available(QStringLiteral("nmcli"));
}

void run(QObject *context, const QStringList &args, CliCallback done, int timeoutMs)
{
    Shadow::Command::run(context, QStringLiteral("nmcli"), args, [done](const Shadow::CommandResult &r) {
        if (!done)
            return;
        QString error;
        if (!r.ok()) {
            error = firstErrorLine(r.errText());
            if (error.isEmpty())
                error = firstErrorLine(r.outText());
            if (error.isEmpty()) {
                error = r.exitCode == -2 ? QStringLiteral("nmcli did not finish in time")
                      : r.exitCode == -1 ? QStringLiteral("nmcli could not be started")
                      : QStringLiteral("nmcli failed with exit code %1").arg(r.exitCode);
            }
        }
        done(r.ok(), r.outText(), error);
    }, timeoutMs);
}

// ---- Watcher ------------------------------------------------------------------

Watcher::Watcher(QObject *parent)
    : QObject(parent), m_bus(QDBusConnection::systemBus())
{
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(1000);
    connect(&m_coalesce, &QTimer::timeout, this, &Watcher::changed);

    if (!m_bus.isConnected()) {
        // No system bus at all (build chroot, broken system): answer on the
        // next event-loop pass so users can connect to runningChanged() first.
        QTimer::singleShot(0, this, [this] { setRunning(false); });
        return;
    }

    auto *watcher = new QDBusServiceWatcher(service(), m_bus,
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { setRunning(true); });
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { setRunning(false); });
    dbusNameHasOwner(m_bus, service(), this, [this](bool owned) { setRunning(owned); });

    // Any property change or object appearing/disappearing below
    // NetworkManager's tree. An empty path matches every object.
    m_bus.connect(service(), QString(), QStringLiteral("org.freedesktop.DBus.Properties"),
                  QStringLiteral("PropertiesChanged"), this, SLOT(onNmSignal()));
    m_bus.connect(service(), QStringLiteral("/org/freedesktop"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                  QStringLiteral("InterfacesAdded"), this, SLOT(onNmSignal()));
    m_bus.connect(service(), QStringLiteral("/org/freedesktop"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                  QStringLiteral("InterfacesRemoved"), this, SLOT(onNmSignal()));
    m_bus.connect(service(), QString(), QStringLiteral("org.freedesktop.NetworkManager.Settings.Connection"),
                  QStringLiteral("Updated"), this, SLOT(onNmSignal()));
}

bool Watcher::busConnected() const
{
    return m_bus.isConnected();
}

void Watcher::onNmSignal()
{
    if (!m_coalesce.isActive())
        m_coalesce.start();
}

void Watcher::setRunning(bool running)
{
    if (m_known && running == m_running)
        return;
    m_known = true;
    m_running = running;
    Q_EMIT runningChanged();
}

// ---- D-Bus ----------------------------------------------------------------------------

void addAndActivate(const Settings &settings, const QString &devicePath, const QString &specificObject,
                    QObject *context, ActivateCallback done)
{
    static const QMetaType registered = qDBusRegisterMetaType<Settings>();
    Q_UNUSED(registered)
    const QDBusMessage call = dbusMethod(service(), rootPath(), service(), QStringLiteral("AddAndActivateConnection"),
                                         {QVariant::fromValue(settings),
                                          QVariant::fromValue(QDBusObjectPath(devicePath)),
                                          QVariant::fromValue(QDBusObjectPath(specificObject.isEmpty() ? QStringLiteral("/") : specificObject))});
    dbusCall(QDBusConnection::systemBus(), call, context, [done](const QDBusMessage &reply) {
        if (!done)
            return;
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
            done(false, QString(), QString(), dbusErrorText(reply));
            return;
        }
        done(true, dbusPlain(reply.arguments().at(0)).toString(), dbusPlain(reply.arguments().at(1)).toString(), QString());
    }, 30000);
}

void deleteConnection(const QString &connectionPath, QObject *context,
                      std::function<void(bool ok, const QString &error)> done)
{
    const QDBusMessage call = dbusMethod(service(), connectionPath,
                                         QStringLiteral("org.freedesktop.NetworkManager.Settings.Connection"),
                                         QStringLiteral("Delete"));
    dbusCall(QDBusConnection::systemBus(), call, context, [done](const QDBusMessage &reply) {
        if (done)
            done(reply.type() == QDBusMessage::ReplyMessage, dbusErrorText(reply));
    });
}

QString stateReasonText(uint reason)
{
    // NMDeviceStateReason (NetworkManager D-Bus API)
    switch (reason) {
    case 7: return QCoreApplication::translate("NetworkManager", "The password is missing or was not accepted.");
    case 8: return QCoreApplication::translate("NetworkManager", "The access point ended the connection — usually a wrong password.");
    case 9: case 10: return QCoreApplication::translate("NetworkManager", "The Wi-Fi security settings were not accepted.");
    case 11: return QCoreApplication::translate("NetworkManager", "The access point did not answer in time — check the password and the distance.");
    case 15: case 16: case 17: return QCoreApplication::translate("NetworkManager", "No address was received from the network (DHCP failed).");
    case 5: return QCoreApplication::translate("NetworkManager", "The network did not hand out an IP configuration.");
    case 39: return QCoreApplication::translate("NetworkManager", "The connection was cancelled.");
    case 40: return QCoreApplication::translate("NetworkManager", "The cable is not plugged in.");
    case 53: return QCoreApplication::translate("NetworkManager", "The network was not found.");
    case 36: return QCoreApplication::translate("NetworkManager", "The device was removed.");
    default: return QCoreApplication::translate("NetworkManager", "NetworkManager reported error code %1.").arg(reason);
    }
}

bool isUuid(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return pattern.match(text).hasMatch();
}

bool isInterfaceName(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.@-]{1,15}$"));
    return pattern.match(text).hasMatch() && text != QLatin1String(".") && text != QLatin1String("..")
           && !text.startsWith(QLatin1Char('-'));
}

// ---- validation ------------------------------------------------------------------

namespace {
bool parsesAs(int family, const QString &text)
{
    if (text.isEmpty() || text.size() > 64)
        return false;
    for (const QChar c : text) {
        // inet_pton stops at nothing, but be explicit about what an address may contain.
        const bool ok = c.isDigit() || c == QLatin1Char('.') || c == QLatin1Char(':')
                        || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
                        || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
        if (!ok)
            return false;
    }
    unsigned char buffer[sizeof(struct in6_addr)];
    return inet_pton(family, text.toLatin1().constData(), buffer) == 1;
}

bool isCidr(int family, const QString &text, int maxPrefix)
{
    const qsizetype slash = text.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash != text.lastIndexOf(QLatin1Char('/')))
        return false;
    const QString prefix = text.mid(slash + 1);
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,3}$"));
    if (!digits.match(prefix).hasMatch())
        return false;
    const int bits = prefix.toInt();
    return bits >= 1 && bits <= maxPrefix && parsesAs(family, text.left(slash));
}
} // namespace

bool isIpv4Address(const QString &text) { return parsesAs(AF_INET, text); }
bool isIpv6Address(const QString &text) { return parsesAs(AF_INET6, text); }
bool isIpv4Cidr(const QString &text) { return isCidr(AF_INET, text, 32); }
bool isIpv6Cidr(const QString &text) { return isCidr(AF_INET6, text, 128); }

} // namespace NetHw::Nm
