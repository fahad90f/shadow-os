// SPDX-License-Identifier: GPL-3.0-or-later
#include "sockets.h"

#include "netutil.h"

#include <command.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTime>

#include <unistd.h>

namespace {

const QStringList kSsArguments = {QStringLiteral("-H"), QStringLiteral("-O"), QStringLiteral("-t"), QStringLiteral("-u"),
                                  QStringLiteral("-n"), QStringLiteral("-a"), QStringLiteral("-p")};

// "addr:port" as printed by ss: 127.0.0.1:22, [::1]:22, *:22, 127.0.0.53%lo:53,
// [fe80::1]%eth0:546. The port is "*" for "any".
void splitEndpoint(const QString &endpoint, QString *address, QString *interface, int *port)
{
    const int colon = endpoint.lastIndexOf(QLatin1Char(':'));
    QString host = colon >= 0 ? endpoint.left(colon) : endpoint;
    const QString portText = colon >= 0 ? endpoint.mid(colon + 1) : QString();
    bool ok = false;
    const int number = portText.toInt(&ok);
    *port = ok ? number : -1;

    const int percent = host.indexOf(QLatin1Char('%'));
    if (percent >= 0) {
        *interface = host.mid(percent + 1);
        host = host.left(percent);
    } else {
        interface->clear();
    }
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2);
    *address = host;
}

bool isLoopback(const QString &address)
{
    return address.startsWith(QLatin1String("127.")) || address == QLatin1String("::1")
        || address.startsWith(QLatin1String("::ffff:127."));
}

} // namespace

SocketList::SocketList(QObject *parent)
    : QObject(parent)
{
}

bool SocketList::runningAsRoot() const
{
    return ::geteuid() == 0;
}

QVariantList SocketList::parse(const QStringList &lines)
{
    QVariantList sockets;
    // users:(("sshd",pid=612,fd=3),("systemd",pid=1,fd=44))
    static const QRegularExpression processRx(QStringLiteral("\\(\"((?:[^\"\\\\]|\\\\.)*)\",pid=(\\d+),fd=(\\d+)\\)"));
    int order = 0;
    for (const QString &raw : lines) {
        // Six whitespace-separated columns, then the process column, which may
        // itself contain spaces inside the quoted command names.
        QStringList columns;
        int pos = 0;
        const int n = int(raw.size());
        while (columns.size() < 6) {
            while (pos < n && raw.at(pos).isSpace())
                ++pos;
            if (pos >= n)
                break;
            const int start = pos;
            while (pos < n && !raw.at(pos).isSpace())
                ++pos;
            columns << raw.mid(start, pos - start);
        }
        if (columns.size() < 6)
            continue;
        const QString rest = raw.mid(pos).trimmed();
        const QString protocol = columns.at(0).toLower();
        if (protocol != QLatin1String("tcp") && protocol != QLatin1String("udp"))
            continue;

        QVariantMap m;
        m.insert(QStringLiteral("order"), order++);
        m.insert(QStringLiteral("protocol"), protocol);
        const QString state = columns.at(1);
        m.insert(QStringLiteral("state"), state);
        m.insert(QStringLiteral("recvQ"), columns.at(2).toInt());
        m.insert(QStringLiteral("sendQ"), columns.at(3).toInt());

        QString address, interface;
        int port = -1;
        splitEndpoint(columns.at(4), &address, &interface, &port);
        m.insert(QStringLiteral("localAddress"), address);
        m.insert(QStringLiteral("localInterface"), interface);
        m.insert(QStringLiteral("localPort"), port);
        const QString family = address == QLatin1String("*") ? QStringLiteral("IPv4+IPv6")
            : address.contains(QLatin1Char(':')) ? QStringLiteral("IPv6") : QStringLiteral("IPv4");
        m.insert(QStringLiteral("family"), family);
        const QString localAddress = address;
        splitEndpoint(columns.at(5), &address, &interface, &port);
        m.insert(QStringLiteral("peerAddress"), address);
        m.insert(QStringLiteral("peerPort"), port);

        // An unconnected UDP socket is the datagram equivalent of a listener.
        const bool listening = (protocol == QLatin1String("tcp") && state == QLatin1String("LISTEN"))
            || (protocol == QLatin1String("udp") && state == QLatin1String("UNCONN"));
        m.insert(QStringLiteral("listening"), listening);
        m.insert(QStringLiteral("exposed"), listening && !isLoopback(localAddress));
        m.insert(QStringLiteral("group"), listening ? QStringLiteral("listening")
                 : state == QLatin1String("ESTAB") ? QStringLiteral("connected") : QStringLiteral("other"));

        QStringList names, pids, described;
        auto it = processRx.globalMatch(rest);
        while (it.hasNext()) {
            const auto match = it.next();
            const QString name = match.captured(1);
            const QString pid = match.captured(2);
            if (!pids.contains(pid)) {
                pids << pid;
                described << QStringLiteral("%1 (%2)").arg(name, pid);
            }
            if (!names.contains(name))
                names << name;
        }
        m.insert(QStringLiteral("process"), names.join(QStringLiteral(", ")));
        m.insert(QStringLiteral("pid"), pids.isEmpty() ? 0 : pids.constFirst().toInt());
        m.insert(QStringLiteral("pids"), pids.join(QStringLiteral(", ")));
        m.insert(QStringLiteral("processes"), described.join(QStringLiteral(", ")));
        m.insert(QStringLiteral("hasProcess"), !pids.isEmpty());
        sockets << m;
    }
    return sockets;
}

void SocketList::setSockets(const QVariantList &sockets)
{
    int listening = 0, exposed = 0, established = 0, withoutProcess = 0;
    for (const QVariant &v : sockets) {
        const QVariantMap s = v.toMap();
        if (s.value(QStringLiteral("listening")).toBool())
            ++listening;
        if (s.value(QStringLiteral("exposed")).toBool())
            ++exposed;
        if (s.value(QStringLiteral("protocol")) == QLatin1String("tcp") && s.value(QStringLiteral("state")) == QLatin1String("ESTAB"))
            ++established;
        if (!s.value(QStringLiteral("hasProcess")).toBool())
            ++withoutProcess;
    }
    m_sockets = sockets;
    m_listening = listening;
    m_exposed = exposed;
    m_established = established;
    m_withoutProcess = withoutProcess;
    m_updatedAt = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    Q_EMIT socketsChanged();
}

void SocketList::setError(const QString &error)
{
    if (error == m_error)
        return;
    m_error = error;
    Q_EMIT errorChanged();
}

void SocketList::finish()
{
    m_loading = false;
    Q_EMIT loadingChanged();
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
    Q_EMIT refreshed();
}

void SocketList::refresh()
{
    if (m_loading)
        return;
    if (m_adminView)
        runElevated();
    else
        runOwn();
}

void SocketList::showAllProcesses()
{
    if (m_loading)
        return;
    runElevated();
}

void SocketList::showOwnView()
{
    if (m_adminView) {
        m_adminView = false;
        Q_EMIT viewChanged();
    }
    if (!m_loading)
        runOwn();
}

void SocketList::runOwn()
{
    m_loading = true;
    Q_EMIT loadingChanged();
    const QString ss = Net::findTool(QStringLiteral("ss"));
    Nexus::Command::run(this, ss.isEmpty() ? QStringLiteral("ss") : ss, kSsArguments, [this](const Nexus::CommandResult &r) {
        if (r.exitCode == -1) {
            setError(tr("The “ss” tool could not be started. Install the iproute2 package."));
            setSockets({});
        } else if (!r.ok()) {
            setError(tr("“ss” failed: %1").arg(Net::cleanMessage(r.errText())));
            setSockets({});
        } else {
            setError(QString());
            setSockets(parse(r.outText().split(QLatin1Char('\n'), Qt::SkipEmptyParts)));
        }
        // Completion is always delivered from the event loop.
        QMetaObject::invokeMethod(this, [this] { finish(); }, Qt::QueuedConnection);
    }, 15000);
}

void SocketList::runElevated()
{
    QString command, problem;
    QStringList arguments;
    if (!Net::elevatedCommand(Net::helperPath(), {QStringLiteral("sockets")}, &command, &arguments, &problem)) {
        QMetaObject::invokeMethod(this, [this, problem] { Q_EMIT elevationFailed(problem); }, Qt::QueuedConnection);
        return;
    }
    m_loading = true;
    Q_EMIT loadingChanged();
    // The timeout is generous: the user may need a while to authenticate.
    Nexus::Command::run(this, command, arguments, [this](const Nexus::CommandResult &r) {
        QString failure;
        QVariantList sockets;
        const QJsonObject reply = QJsonDocument::fromJson(r.out).object();
        if (r.exitCode == -2) {
            failure = tr("The request timed out while waiting for authentication.");
        } else if (r.exitCode == -1) {
            failure = tr("The administrator helper could not be started.");
        } else if (!Net::pkexecFailure(r.exitCode, r.errText()).isEmpty()) {
            failure = Net::pkexecFailure(r.exitCode, r.errText());
        } else if (!reply.value(QLatin1String("ok")).toBool()) {
            const QString detail = reply.value(QLatin1String("error")).toString();
            failure = detail.isEmpty() ? tr("The administrator helper failed (exit code %1).").arg(r.exitCode) : detail;
        } else {
            QStringList lines;
            const QJsonArray array = reply.value(QLatin1String("lines")).toArray();
            for (const QJsonValue &line : array)
                lines << line.toString();
            sockets = parse(lines);
        }

        if (failure.isEmpty()) {
            setError(QString());
            setSockets(sockets);
            if (!m_adminView) {
                m_adminView = true;
                Q_EMIT viewChanged();
            }
        } else {
            // Keep whatever is on screen; only report why the wider view failed.
            Q_EMIT elevationFailed(failure);
        }
        QMetaObject::invokeMethod(this, [this] { finish(); }, Qt::QueuedConnection);
    }, 180000);
}

QJsonObject SocketList::dump() const
{
    return QJsonObject{
        {QStringLiteral("command"), QStringLiteral("ss ") + kSsArguments.join(QLatin1Char(' '))},
        {QStringLiteral("error"), m_error},
        {QStringLiteral("runningAsRoot"), runningAsRoot()},
        {QStringLiteral("administratorView"), m_adminView},
        {QStringLiteral("total"), total()},
        {QStringLiteral("listening"), m_listening},
        {QStringLiteral("exposedListening"), m_exposed},
        {QStringLiteral("establishedTcp"), m_established},
        {QStringLiteral("withoutProcess"), m_withoutProcess},
        {QStringLiteral("sockets"), QJsonArray::fromVariantList(m_sockets)},
    };
}
