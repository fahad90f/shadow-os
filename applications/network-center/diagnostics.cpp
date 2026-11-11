// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"

#include "netstate.h"
#include "netutil.h"

#include <command.h>

#include <QDateTime>
#include <QFile>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslError>
#include <QSslSocket>
#include <QTcpSocket>

#include <csignal>
#include <memory>
#include <sys/prctl.h>

namespace {

enum { MaxOutput = 256 * 1024 };

QString quoteArgument(const QString &argument)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    return plain.match(argument).hasMatch() ? argument : QLatin1Char('\'') + argument + QLatin1Char('\'');
}

QString commandLine(const QString &program, const QStringList &arguments)
{
    QStringList parts{program.section(QLatin1Char('/'), -1)};
    for (const QString &a : arguments)
        parts << quoteArgument(a);
    return QStringLiteral("$ ") + parts.join(QLatin1Char(' ')) + QLatin1Char('\n');
}

QString protocolName(QSsl::SslProtocol protocol)
{
    switch (protocol) {
    case QSsl::TlsV1_2: return QStringLiteral("TLS 1.2");
    case QSsl::TlsV1_3: return QStringLiteral("TLS 1.3");
    default: return QStringLiteral("TLS");
    }
}

QString issuerOf(const QSslCertificate &certificate)
{
    QStringList parts = certificate.issuerInfo(QSslCertificate::Organization);
    parts += certificate.issuerInfo(QSslCertificate::CommonName);
    parts.removeDuplicates();
    return parts.join(QStringLiteral(", "));
}

} // namespace

Diagnostics::Diagnostics(QObject *parent)
    : QObject(parent)
{
    m_processTimeout.setSingleShot(true);
    connect(&m_processTimeout, &QTimer::timeout, this, [this] {
        if (m_process) {
            m_process->setProperty("nxEndReason", -2);
            m_process->kill();
        }
    });
    refreshTools();
    m_steps = {
        QVariantMap{{QStringLiteral("key"), QStringLiteral("link")}, {QStringLiteral("title"), tr("Network link")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("address")}, {QStringLiteral("title"), tr("IP address")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("gateway")}, {QStringLiteral("title"), tr("Default gateway")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("dns")}, {QStringLiteral("title"), tr("Name resolution (DNS)")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("https")}, {QStringLiteral("title"), tr("HTTPS reachability")}},
    };
    for (QVariant &v : m_steps) {
        QVariantMap m = v.toMap();
        m.insert(QStringLiteral("state"), QStringLiteral("pending"));
        m.insert(QStringLiteral("detail"), QString());
        v = m;
    }
}

Diagnostics::~Diagnostics()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

void Diagnostics::refreshTools()
{
    QVariantMap tools;
    tools.insert(QStringLiteral("ping"), !Net::findTool(QStringLiteral("ping")).isEmpty());
    const bool traceroute = !Net::findTool(QStringLiteral("traceroute")).isEmpty();
    const bool tracepath = !Net::findTool(QStringLiteral("tracepath")).isEmpty();
    tools.insert(QStringLiteral("trace"), traceroute ? QStringLiteral("traceroute") : tracepath ? QStringLiteral("tracepath") : QString());
    tools.insert(QStringLiteral("dig"), !Net::findTool(QStringLiteral("dig")).isEmpty());
    tools.insert(QStringLiteral("resolvectl"), !Net::findTool(QStringLiteral("resolvectl")).isEmpty());
    tools.insert(QStringLiteral("ip"), !Net::findTool(QStringLiteral("ip")).isEmpty());
    tools.insert(QStringLiteral("tls"), QSslSocket::supportsSsl());
    if (tools != m_tools) {
        m_tools = tools;
        Q_EMIT toolsChanged();
    }
}

// ---- common run machinery -------------------------------------------------------

bool Diagnostics::begin(const QString &tool)
{
    if (m_running || m_troubleshooting)
        return false;
    ++m_generation;
    m_stopRequested = false;
    m_running = true;
    m_tool = tool;
    m_output.clear();
    m_result.clear();
    m_resultKind.clear();
    Q_EMIT toolChanged();
    Q_EMIT outputChanged();
    Q_EMIT resultChanged();
    Q_EMIT runningChanged();
    return true;
}

void Diagnostics::end(const QString &kind, const QString &result)
{
    if (!m_running)
        return;
    m_running = false;
    m_result = result;
    m_resultKind = kind;
    Q_EMIT resultChanged();
    Q_EMIT runningChanged();
    Q_EMIT finished();
}

void Diagnostics::append(const QString &text)
{
    if (text.isEmpty())
        return;
    m_output += text;
    if (m_output.size() > MaxOutput)
        m_output = tr("[earlier output dropped]\n") + m_output.right(MaxOutput * 3 / 4);
    Q_EMIT outputChanged();
}

void Diagnostics::clearOutput()
{
    if (m_running)
        return;
    m_output.clear();
    m_result.clear();
    m_resultKind.clear();
    Q_EMIT outputChanged();
    Q_EMIT resultChanged();
}

void Diagnostics::run(const QString &program, const QStringList &arguments, int timeoutMs, bool live, ProcessDone done)
{
    m_processOutput.clear();
    m_processLive = live;
    m_processDone = std::move(done);

    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessEnvironment(Shadow::Command::parseEnvironment());
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setStandardInputFile(QProcess::nullDevice());
    process->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
    process->setProgram(program);
    process->setArguments(arguments);

    auto drain = [this, process] {
        const QString text = QString::fromUtf8(process->readAllStandardOutput());
        if (text.isEmpty())
            return;
        m_processOutput += text;
        if (m_processLive)
            append(text);
    };
    auto finish = [this, process, drain](int code) {
        if (process->property("nxFinished").toBool())
            return;
        process->setProperty("nxFinished", true);
        drain();
        const int reason = process->property("nxEndReason").toInt();
        if (reason != 0)
            code = reason;
        if (m_process == process) {
            m_processTimeout.stop();
            m_process = nullptr;
        }
        process->deleteLater();
        ProcessDone callback = std::move(m_processDone);
        m_processDone = nullptr;
        // A copy: the callback may start the next process, which resets the buffer.
        const QString output = m_processOutput;
        if (callback && reason != -3)
            callback(code, output);
    };
    connect(process, &QProcess::readyReadStandardOutput, this, drain);
    connect(process, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? code : -1);
    });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(-1);
    });
    m_processTimeout.start(timeoutMs);
    process->start();
}

void Diagnostics::stop()
{
    if (!m_running && !m_troubleshooting)
        return;
    ++m_generation;         // every pending callback of this run becomes a no-op
    m_stopRequested = true;
    if (m_process) {
        m_process->setProperty("nxEndReason", -3);
        m_process->kill();
    }
    if (m_lookupId >= 0) {
        QHostInfo::abortHostLookup(m_lookupId);
        m_lookupId = -1;
    }
    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_tls) {
        m_tls->abort();
        m_tls->deleteLater();
        m_tls = nullptr;
    }
    if (m_troubleshooting) {
        for (QVariant &v : m_steps) {
            QVariantMap m = v.toMap();
            const QString state = m.value(QStringLiteral("state")).toString();
            if (state == QLatin1String("running") || state == QLatin1String("pending")) {
                m.insert(QStringLiteral("state"), QStringLiteral("skipped"));
                m.insert(QStringLiteral("detail"), state == QLatin1String("running") ? tr("Stopped.") : QString());
                v = m;
            }
        }
        Q_EMIT stepsChanged();
        setVerdict(QStringLiteral("info"), tr("Troubleshooting was stopped"), QString());
        m_troubleshooting = false;
        Q_EMIT troubleshootingChanged();
        Q_EMIT troubleshootFinished();
    }
    if (m_running) {
        append(tr("\n[stopped]\n"));
        end(QStringLiteral("info"), tr("Stopped."));
    }
}

QString Diagnostics::resolveFamilyOption(const QString &target, const QString &family, QString *error) const
{
    const bool wants4 = family == QLatin1String("IPv4"), wants6 = family == QLatin1String("IPv6");
    if (Net::isIPv4(target)) {
        if (wants6 && error)
            *error = tr("%1 is an IPv4 address; choose IPv4 or Automatic.").arg(target);
        return QStringLiteral("-4");
    }
    if (Net::isIPv6(target, true)) {
        if (wants4 && error)
            *error = tr("%1 is an IPv6 address; choose IPv6 or Automatic.").arg(target);
        return QStringLiteral("-6");
    }
    return wants4 ? QStringLiteral("-4") : wants6 ? QStringLiteral("-6") : QString();
}

void Diagnostics::resolve(const QString &host, std::function<void(const QList<QHostAddress> &, const QString &)> done)
{
    if (Net::isIpLiteral(host, true)) {
        done({QHostAddress(host)}, QString());
        return;
    }
    const int generation = m_generation;
    const int id = QHostInfo::lookupHost(host, this, [this, generation, done](const QHostInfo &info) {
        if (generation != m_generation || m_lookupId != info.lookupId())
            return;
        m_lookupId = -1;
        if (info.error() != QHostInfo::NoError)
            done({}, info.errorString());
        else
            done(info.addresses(), QString());
    });
    m_lookupId = id;
    // The C library retries every server; do not wait for all of that.
    QTimer::singleShot(15000, this, [this, generation, id, done] {
        if (generation != m_generation || m_lookupId != id)
            return;
        QHostInfo::abortHostLookup(id);
        m_lookupId = -1;
        done({}, tr("no answer from the configured DNS servers within 15 seconds"));
    });
}

// ---- ping -------------------------------------------------------------------------

void Diagnostics::ping(const QString &host, const QString &family, int count)
{
    if (!begin(QStringLiteral("ping")))
        return;
    QString error;
    const QString target = Net::normalizeHost(host, &error);
    if (target.isEmpty()) {
        end(QStringLiteral("danger"), error);
        return;
    }
    const QString option = resolveFamilyOption(target, family, &error);
    if (!error.isEmpty()) {
        end(QStringLiteral("danger"), error);
        return;
    }
    const QString program = Net::findTool(QStringLiteral("ping"));
    if (program.isEmpty()) {
        end(QStringLiteral("danger"), tr("ping is not installed (package iputils-ping)."));
        return;
    }
    count = qBound(1, count, 100);
    QStringList args = {QStringLiteral("-n"), QStringLiteral("-c"), QString::number(count), QStringLiteral("-W"), QStringLiteral("2")};
    if (!option.isEmpty())
        args << option;
    args << QStringLiteral("--") << target;
    append(commandLine(program, args));
    run(program, args, (count * 3 + 10) * 1000, true, [this, target](int code, const QString &output) {
        // "4 packets transmitted, 4 received, 0% packet loss, time 3004ms"
        // "rtt min/avg/max/mdev = 0.212/0.261/0.311/0.049 ms"
        static const QRegularExpression counts(QStringLiteral("(\\d+) packets transmitted, (\\d+) received"));
        static const QRegularExpression rtt(QStringLiteral("= [0-9.]+/([0-9.]+)/"));
        const auto c = counts.match(output);
        const auto r = rtt.match(output);
        QString summary;
        if (c.hasMatch())
            summary = tr("%1 of %2 echo requests were answered").arg(c.captured(2), c.captured(1));
        if (r.hasMatch())
            summary += tr(", average round trip %1 ms").arg(r.captured(1));
        if (code == 0)
            end(QStringLiteral("success"), summary.isEmpty() ? tr("%1 answered.").arg(target) : summary + QLatin1Char('.'));
        else if (code == 1)
            end(QStringLiteral("danger"), summary.isEmpty() ? tr("No reply from %1.").arg(target)
                                                               : tr("%1 — %2 did not answer.").arg(summary, target));
        else if (code == -2)
            end(QStringLiteral("danger"), tr("ping did not finish in time."));
        else if (code == -1)
            end(QStringLiteral("danger"), tr("ping could not be started."));
        else
            end(QStringLiteral("danger"), tr("ping reported an error (exit code %1); see the output.").arg(code));
    });
}

// ---- route trace --------------------------------------------------------------------

void Diagnostics::traceRoute(const QString &host, const QString &family)
{
    if (!begin(QStringLiteral("trace")))
        return;
    QString error;
    const QString target = Net::normalizeHost(host, &error, false);
    if (target.isEmpty()) {
        end(QStringLiteral("danger"), error);
        return;
    }
    const QString option = resolveFamilyOption(target, family, &error);
    if (!error.isEmpty()) {
        end(QStringLiteral("danger"), error);
        return;
    }
    QString program = Net::findTool(QStringLiteral("traceroute"));
    QStringList args;
    if (!program.isEmpty()) {
        // Numeric output, one probe per hop, two seconds per probe, 30 hops.
        if (!option.isEmpty())
            args << option;
        args << QStringLiteral("-n") << QStringLiteral("-w") << QStringLiteral("2") << QStringLiteral("-q") << QStringLiteral("1")
             << QStringLiteral("-m") << QStringLiteral("30") << target;
    } else {
        program = Net::findTool(QStringLiteral("tracepath"));
        if (program.isEmpty()) {
            end(QStringLiteral("danger"), tr("Neither traceroute (package traceroute) nor tracepath (package iputils-tracepath) is installed."));
            return;
        }
        if (!option.isEmpty())
            args << option;
        args << QStringLiteral("-n") << QStringLiteral("-m") << QStringLiteral("30") << target;
    }
    append(commandLine(program, args));
    const bool tracepath = program.endsWith(QLatin1String("tracepath"));
    run(program, args, 120000, true, [this, target, tracepath](int code, const QString &output) {
        if (code == -2) {
            end(QStringLiteral("warning"), tr("The trace did not finish within two minutes."));
            return;
        }
        if (code == -1) {
            end(QStringLiteral("danger"), tr("The trace tool could not be started."));
            return;
        }
        if (code != 0) {
            end(QStringLiteral("danger"), tr("The trace failed (exit code %1); see the output.").arg(code));
            return;
        }
        // Hop lines start with the hop number. Whether the destination itself
        // answered: tracepath says "reached"; traceroute prints the resolved
        // destination in its first line and, if reached, again as the last hop.
        static const QRegularExpression hopLine(QStringLiteral("^\\s*(\\d+)\\??:?\\s+(\\S+)"));
        static const QRegularExpression destination(QStringLiteral("\\(([0-9A-Fa-f:.]+)\\)"));
        const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        int hops = 0;
        QString lastHop;
        for (const QString &line : lines) {
            const auto match = hopLine.match(line);
            if (!match.hasMatch())
                continue;
            hops = qMax(hops, match.captured(1).toInt());
            if (match.captured(2) != QLatin1String("*") && match.captured(2) != QLatin1String("no"))
                lastHop = match.captured(2);
        }
        bool reached = false;
        if (tracepath) {
            reached = output.contains(QLatin1String("reached"));
        } else if (!lines.isEmpty()) {
            const auto match = destination.match(lines.constFirst());
            reached = match.hasMatch() && match.captured(1) == lastHop;
        }
        if (reached)
            end(QStringLiteral("success"), tr("%1 was reached after %n hop(s).", nullptr, hops).arg(target));
        else if (lastHop.isEmpty())
            end(QStringLiteral("warning"), tr("No router on the way to %1 answered the probes.").arg(target));
        else
            end(QStringLiteral("warning"), tr("The trace ended after %n hop(s) without an answer from %1; the last reply came from %2.",
                                               nullptr, hops).arg(target, lastHop));
    });
}

// ---- DNS lookup ----------------------------------------------------------------------

void Diagnostics::dnsLookup(const QString &name, const QString &recordType, const QString &server)
{
    if (!begin(QStringLiteral("dns")))
        return;
    static const QStringList types = {QStringLiteral("A"), QStringLiteral("AAAA"), QStringLiteral("CNAME"), QStringLiteral("MX"),
                                      QStringLiteral("NS"), QStringLiteral("TXT"), QStringLiteral("SOA"), QStringLiteral("SRV"),
                                      QStringLiteral("CAA"), QStringLiteral("PTR")};
    QString type = recordType.trimmed().toUpper();
    if (!types.contains(type)) {
        end(QStringLiteral("danger"), tr("“%1” is not a supported record type.").arg(recordType));
        return;
    }
    QString query = name.trimmed();
    const bool reverse = Net::isIpLiteral(query);
    if (reverse) {
        type = QStringLiteral("PTR");       // an address can only be looked up in reverse
    } else if (!Net::isDnsName(query)) {
        QString error;
        query = Net::normalizeHost(query, &error, false);
        if (query.isEmpty()) {
            end(QStringLiteral("danger"), error);
            return;
        }
    }
    QString dnsServer = server.trimmed();
    if (!dnsServer.isEmpty()) {
        QString error;
        dnsServer = Net::normalizeHost(dnsServer, &error, false);
        if (dnsServer.isEmpty()) {
            end(QStringLiteral("danger"), tr("DNS server: %1").arg(error));
            return;
        }
    }

    const QString dig = Net::findTool(QStringLiteral("dig"));
    if (!dig.isEmpty()) {
        QStringList args = {QStringLiteral("+nocmd"), QStringLiteral("+noall"), QStringLiteral("+comments"), QStringLiteral("+answer"),
                            QStringLiteral("+stats"), QStringLiteral("+time=3"), QStringLiteral("+tries=1")};
        if (!dnsServer.isEmpty())
            args << QLatin1Char('@') + dnsServer;
        if (reverse)
            args << QStringLiteral("-x") << query;
        else
            args << query << type;
        append(commandLine(dig, args));
        run(dig, args, 20000, true, [this, query, type](int code, const QString &output) {
            static const QRegularExpression status(QStringLiteral("status: ([A-Z]+)"));
            const auto s = status.match(output);
            int answers = 0;
            const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                if (!line.startsWith(QLatin1Char(';')) && !line.startsWith(QLatin1Char('$')))
                    ++answers;
            }
            if (code == -1) {
                end(QStringLiteral("danger"), tr("dig could not be started."));
            } else if (code == -2) {
                end(QStringLiteral("danger"), tr("dig did not finish in time."));
            } else if (code != 0 || !s.hasMatch()) {
                end(QStringLiteral("danger"), tr("No DNS server answered the query (dig exit code %1).").arg(code));
            } else if (s.captured(1) == QLatin1String("NOERROR")) {
                if (answers > 0)
                    end(QStringLiteral("success"), tr("%1 %2: %n record(s) returned.", nullptr, answers).arg(query, type));
                else
                    end(QStringLiteral("warning"), tr("%1 exists, but has no %2 record.").arg(query, type));
            } else if (s.captured(1) == QLatin1String("NXDOMAIN")) {
                end(QStringLiteral("danger"), tr("NXDOMAIN: the name %1 does not exist according to the queried server.").arg(query));
            } else {
                end(QStringLiteral("danger"), tr("The server answered with status %1.").arg(s.captured(1)));
            }
        });
        return;
    }

    const QString resolvectl = Net::findTool(QStringLiteral("resolvectl"));
    if (!resolvectl.isEmpty() && dnsServer.isEmpty()) {
        const QStringList args = {QStringLiteral("query"), QStringLiteral("--type=") + type, QStringLiteral("--legend=yes"), query};
        append(commandLine(resolvectl, args));
        run(resolvectl, args, 20000, true, [this, query](int code, const QString &) {
            if (code == 0)
                end(QStringLiteral("success"), tr("systemd-resolved resolved %1.").arg(query));
            else
                end(QStringLiteral("danger"), tr("systemd-resolved could not resolve %1 (exit code %2).").arg(query).arg(code));
        });
        return;
    }

    // Last resort: the C library's resolver, which only does address lookups.
    if (reverse || (type != QLatin1String("A") && type != QLatin1String("AAAA")) || !dnsServer.isEmpty()) {
        end(QStringLiteral("danger"), tr("Record types other than A/AAAA and custom servers need dig (package bind9-dnsutils), "
                                           "which is not installed."));
        return;
    }
    append(tr("dig is not installed; using the system resolver (getaddrinfo) for %1.\n").arg(query));
    resolve(query, [this, query, type](const QList<QHostAddress> &addresses, const QString &error) {
        if (!error.isEmpty()) {
            end(QStringLiteral("danger"), tr("%1 could not be resolved: %2").arg(query, error));
            return;
        }
        int shown = 0;
        for (const QHostAddress &a : addresses) {
            const bool v6 = a.protocol() == QAbstractSocket::IPv6Protocol;
            if ((type == QLatin1String("AAAA")) == v6) {
                append(a.toString() + QLatin1Char('\n'));
                ++shown;
            }
        }
        if (shown > 0)
            end(QStringLiteral("success"), tr("%1 %2: %n address(es).", nullptr, shown).arg(query, type));
        else
            end(QStringLiteral("warning"), tr("%1 has no %2 record.").arg(query, type));
    });
}

// ---- TCP port check -------------------------------------------------------------------

void Diagnostics::portCheck(const QString &host, int port, const QString &family, int timeoutSeconds)
{
    if (!begin(QStringLiteral("port")))
        return;
    QString error;
    const QString target = Net::normalizeHost(host, &error);
    if (target.isEmpty()) {
        end(QStringLiteral("danger"), error);
        return;
    }
    if (port < 1 || port > 65535) {
        end(QStringLiteral("danger"), tr("The port must be a number from 1 to 65535."));
        return;
    }
    resolveFamilyOption(target, family, &error);
    if (!error.isEmpty()) {
        end(QStringLiteral("danger"), error);
        return;
    }
    m_portHost = target;
    m_port = port;
    m_portTimeoutMs = qBound(1, timeoutSeconds, 30) * 1000;
    m_portOpen.clear();
    m_portProblems.clear();
    const bool literal = Net::isIpLiteral(target, true);
    append(tr("TCP connection test to %1, port %2 (timeout %3 s)\n").arg(target).arg(port).arg(m_portTimeoutMs / 1000));
    if (!literal)
        append(tr("Resolving %1 …\n").arg(target));

    resolve(target, [this, target, family, literal](const QList<QHostAddress> &addresses, const QString &lookupError) {
        if (!lookupError.isEmpty()) {
            end(QStringLiteral("danger"), tr("%1 could not be resolved: %2").arg(target, lookupError));
            return;
        }
        // One address per requested family: this checks a host, it does not scan.
        QHostAddress first4, first6;
        QStringList all;
        for (const QHostAddress &a : addresses) {
            all << a.toString();
            if (a.protocol() == QAbstractSocket::IPv4Protocol && first4.isNull())
                first4 = a;
            else if (a.protocol() == QAbstractSocket::IPv6Protocol && first6.isNull())
                first6 = a;
        }
        if (!literal)
            append(tr("%1 resolves to %2\n").arg(target, all.join(QStringLiteral(", "))));
        m_portTargets.clear();
        if (family != QLatin1String("IPv6") && !first4.isNull())
            m_portTargets << first4;
        if (family != QLatin1String("IPv4") && !first6.isNull())
            m_portTargets << first6;
        if (m_portTargets.isEmpty()) {
            end(QStringLiteral("danger"), tr("%1 has no %2 address.").arg(target, family == QLatin1String("auto") ? QStringLiteral("IP") : family));
            return;
        }
        portCheckNext();
    });
}

void Diagnostics::portCheckNext()
{
    if (m_portTargets.isEmpty()) {
        if (!m_portOpen.isEmpty()) {
            QString text = tr("Port %1 on %2 is open (%3).").arg(m_port).arg(m_portHost, m_portOpen.join(QStringLiteral(", ")));
            if (!m_portProblems.isEmpty())
                text += QLatin1Char(' ') + tr("Not reachable: %1.").arg(m_portProblems.join(QStringLiteral("; ")));
            end(m_portProblems.isEmpty() ? QStringLiteral("success") : QStringLiteral("warning"), text);
        } else {
            end(QStringLiteral("danger"), tr("Port %1 on %2 is not reachable: %3.").arg(m_port).arg(m_portHost, m_portProblems.join(QStringLiteral("; "))));
        }
        return;
    }
    const QHostAddress address = m_portTargets.takeFirst();
    const int generation = m_generation;
    auto *socket = new QTcpSocket(this);
    m_socket = socket;
    socket->setProxy(QNetworkProxy::NoProxy);       // the question is direct reachability
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto *watchdog = new QTimer(socket);
    watchdog->setSingleShot(true);

    auto done = [this, generation, socket, address](bool open, const QString &outcome, const QString &problem) {
        if (socket->property("nxDone").toBool())
            return;
        socket->setProperty("nxDone", true);
        socket->abort();
        socket->deleteLater();
        if (generation != m_generation)
            return;
        m_socket = nullptr;
        append(QStringLiteral("  %1\n").arg(outcome));
        if (open)
            m_portOpen << address.toString();
        else
            m_portProblems << QStringLiteral("%1 %2").arg(address.toString(), problem);
        portCheckNext();
    };
    connect(socket, &QTcpSocket::connected, this, [done, clock] {
        done(true, tr("open — the connection was established in %1 ms").arg(clock->elapsed()), QString());
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [done, clock, socket](QAbstractSocket::SocketError error) {
        if (error == QAbstractSocket::ConnectionRefusedError)
            done(false, tr("closed — the host refused the connection after %1 ms (nothing listens on this port)").arg(clock->elapsed()),
                 tr("refused the connection"));
        else
            done(false, tr("failed — %1").arg(socket->errorString()), tr("failed (%1)").arg(socket->errorString()));
    });
    connect(watchdog, &QTimer::timeout, this, [this, done] {
        done(false, tr("no answer within %1 s — the port is filtered by a firewall or the host is down").arg(m_portTimeoutMs / 1000),
             tr("did not answer"));
    });
    append(tr("Connecting to %1 port %2 …\n").arg(address.toString()).arg(m_port));
    watchdog->start(m_portTimeoutMs);
    socket->connectToHost(address, quint16(m_port));
}

// ---- path MTU probe ---------------------------------------------------------------------

void Diagnostics::mtuProbe(const QString &host, const QString &family)
{
    if (!begin(QStringLiteral("mtu")))
        return;
    QString error;
    const QString target = Net::normalizeHost(host, &error);
    if (target.isEmpty()) {
        end(QStringLiteral("danger"), error);
        return;
    }
    QString option = resolveFamilyOption(target, family, &error);
    if (!error.isEmpty()) {
        end(QStringLiteral("danger"), error);
        return;
    }
    if (Net::findTool(QStringLiteral("ping")).isEmpty()) {
        end(QStringLiteral("danger"), tr("ping is not installed (package iputils-ping)."));
        return;
    }
    if (option.isEmpty())
        option = QStringLiteral("-4");      // a path has one MTU per address family; default to IPv4
    m_mtuFamilyOption = option;
    const bool v6 = option == QLatin1String("-6");
    // ICMP echo payload + ICMP header (8) + IP header (20 or 40) = packet size.
    m_mtuOverhead = v6 ? 48 : 28;
    append(tr("Path MTU probe to %1 over %2 (don't-fragment echo requests)\n").arg(target, v6 ? QStringLiteral("IPv6") : QStringLiteral("IPv4")));

    resolve(target, [this, target, v6](const QList<QHostAddress> &addresses, const QString &lookupError) {
        if (!lookupError.isEmpty()) {
            end(QStringLiteral("danger"), tr("%1 could not be resolved: %2").arg(target, lookupError));
            return;
        }
        QString address;
        for (const QHostAddress &a : addresses) {
            if ((a.protocol() == QAbstractSocket::IPv6Protocol) == v6) {
                address = Net::isIpLiteral(target, true) ? target : a.toString();
                break;
            }
        }
        if (address.isEmpty()) {
            end(QStringLiteral("danger"), tr("%1 has no %2 address.").arg(target, v6 ? QStringLiteral("IPv6") : QStringLiteral("IPv4")));
            return;
        }
        if (address != target)
            append(tr("%1 resolves to %2\n").arg(target, address));
        mtuStart(address);
    });
}

void Diagnostics::mtuStart(const QString &address)
{
    m_mtuTarget = address;
    m_mtuLocal = 1500;
    const int generation = m_generation;
    auto proceed = [this, generation](const QString &device) {
        if (generation != m_generation)
            return;
        if (!device.isEmpty()) {
            QFile f(QStringLiteral("/sys/class/net/%1/mtu").arg(device));
            if (f.open(QIODevice::ReadOnly)) {
                const int mtu = f.readAll().trimmed().toInt();
                if (mtu >= 576)
                    m_mtuLocal = mtu;
            }
            append(tr("Outgoing interface %1 has an MTU of %2 bytes.\n").arg(device).arg(m_mtuLocal));
        }
        // The smallest packet every path must carry: 576 bytes (IPv4), 1280 (IPv6).
        const int minimum = (m_mtuOverhead == 48 ? 1280 : 576) - m_mtuOverhead;
        mtuPing(minimum, [this, minimum](bool ok, const QString &note) {
            if (!ok) {
                end(QStringLiteral("danger"), tr("%1 does not answer even a minimal unfragmented echo request%2. It may be down "
                                                   "or block ICMP, so the path MTU cannot be measured.")
                        .arg(m_mtuTarget, note.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(note)));
                return;
            }
            const int top = m_mtuLocal - m_mtuOverhead;
            if (top <= minimum) {
                mtuFinish(minimum);
                return;
            }
            mtuPing(top, [this, minimum, top](bool fits, const QString &) {
                if (fits)
                    mtuFinish(top);
                else
                    mtuSearch(minimum, top);
            });
        });
    };

    const int percent = address.indexOf(QLatin1Char('%'));
    if (percent > 0) {
        proceed(address.mid(percent + 1));
        return;
    }
    const QString ip = Net::findTool(QStringLiteral("ip"));
    if (ip.isEmpty()) {
        proceed(QString());
        return;
    }
    Shadow::Command::run(this, ip, {QStringLiteral("-j"), QStringLiteral("route"), QStringLiteral("get"), address},
                        [proceed](const Shadow::CommandResult &r) {
        const QJsonArray routes = QJsonDocument::fromJson(r.out).array();
        proceed(routes.isEmpty() ? QString() : routes.at(0).toObject().value(QLatin1String("dev")).toString());
    }, 5000);
}

void Diagnostics::mtuPing(int payload, std::function<void(bool ok, const QString &note)> done)
{
    const QString program = Net::findTool(QStringLiteral("ping"));
    const QStringList args = {QStringLiteral("-n"), QStringLiteral("-c"), QStringLiteral("1"), QStringLiteral("-W"), QStringLiteral("1"),
                              QStringLiteral("-M"), QStringLiteral("do"), QStringLiteral("-s"), QString::number(payload),
                              m_mtuFamilyOption, QStringLiteral("--"), m_mtuTarget};
    run(program, args, 8000, false, [this, payload, done](int code, const QString &output) {
        // Keep ping's own explanation ("message too long, mtu=1400",
        // "Frag needed and DF set (mtu = 1400)") next to the verdict.
        QString note;
        const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            if (line.contains(QLatin1String("mtu"), Qt::CaseInsensitive) || line.startsWith(QLatin1String("ping:"))) {
                note = line.trimmed();
                break;
            }
        }
        const bool ok = code == 0;
        append(tr("  payload %1 bytes → packet %2 bytes: %3%4\n")
                   .arg(payload).arg(payload + m_mtuOverhead)
                   .arg(ok ? tr("passed") : tr("did not pass"), note.isEmpty() ? QString() : QStringLiteral("  [%1]").arg(note)));
        done(ok, note);
    });
}

void Diagnostics::mtuSearch(int low, int high)
{
    // Invariant: 'low' passes, 'high' does not.
    if (high - low <= 1) {
        mtuFinish(low);
        return;
    }
    const int middle = low + (high - low) / 2;
    mtuPing(middle, [this, low, high, middle](bool ok, const QString &) {
        if (ok)
            mtuSearch(middle, high);
        else
            mtuSearch(low, middle);
    });
}

void Diagnostics::mtuFinish(int payload)
{
    const int mtu = payload + m_mtuOverhead;
    if (mtu >= m_mtuLocal)
        end(QStringLiteral("success"), tr("Path MTU to %1 is %2 bytes — the full MTU of the local interface; no smaller link on the path.")
                .arg(m_mtuTarget).arg(mtu));
    else
        end(QStringLiteral("warning"), tr("Path MTU to %1 is %2 bytes (largest unfragmented echo payload: %3), smaller than the "
                                            "local interface MTU of %4.").arg(m_mtuTarget).arg(mtu).arg(payload).arg(m_mtuLocal));
}

// ---- troubleshooting ----------------------------------------------------------------------

void Diagnostics::setStep(const QString &key, const QString &state, const QString &detail)
{
    for (QVariant &v : m_steps) {
        QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("key")) == key) {
            m.insert(QStringLiteral("state"), state);
            m.insert(QStringLiteral("detail"), detail);
            v = m;
        }
    }
    Q_EMIT stepsChanged();
}

void Diagnostics::setVerdict(const QString &kind, const QString &title, const QString &detail)
{
    m_verdictKind = kind;
    m_verdict = title;
    m_verdictDetail = detail;
    Q_EMIT verdictChanged();
}

void Diagnostics::failStep(const QString &key, const QString &detail, const QString &verdict, const QString &explanation)
{
    bool after = false;
    for (QVariant &v : m_steps) {
        QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("key")) == key) {
            m.insert(QStringLiteral("state"), QStringLiteral("fail"));
            m.insert(QStringLiteral("detail"), detail);
            after = true;
        } else if (after) {
            m.insert(QStringLiteral("state"), QStringLiteral("skipped"));
            m.insert(QStringLiteral("detail"), tr("Not tested: an earlier layer failed."));
        }
        v = m;
    }
    Q_EMIT stepsChanged();
    setVerdict(QStringLiteral("danger"), verdict, explanation);
    m_troubleshooting = false;
    Q_EMIT troubleshootingChanged();
    Q_EMIT troubleshootFinished();
}

void Diagnostics::finishTroubleshoot()
{
    if (m_tsWarned)
        setVerdict(QStringLiteral("warning"), tr("No blocking problem found"),
                   tr("Every layer up to HTTPS works, but at least one check needs attention — see the remarks above."));
    else
        setVerdict(QStringLiteral("success"), tr("No problem found"),
                   tr("Link, address, gateway, name resolution and an HTTPS connection to %1 all work.").arg(m_tsHost));
    m_troubleshooting = false;
    Q_EMIT troubleshootingChanged();
    Q_EMIT troubleshootFinished();
}

void Diagnostics::troubleshoot(const QString &testHost)
{
    if (m_running || m_troubleshooting)
        return;
    ++m_generation;
    m_stopRequested = false;
    m_tsWarned = false;
    for (QVariant &v : m_steps) {
        QVariantMap m = v.toMap();
        m.insert(QStringLiteral("state"), QStringLiteral("pending"));
        m.insert(QStringLiteral("detail"), QString());
        v = m;
    }
    Q_EMIT stepsChanged();

    QString error;
    const QString wanted = testHost.trimmed().isEmpty() ? defaultTestHost() : testHost;
    m_tsHost = Net::normalizeHost(wanted, &error, false);
    if (m_tsHost.isEmpty()) {
        setVerdict(QStringLiteral("danger"), tr("The test host is not valid"), error);
        Q_EMIT troubleshootFinished();
        return;
    }
    setVerdict(QString(), QString(), QString());
    m_troubleshooting = true;
    Q_EMIT troubleshootingChanged();

    setStep(QStringLiteral("link"), QStringLiteral("running"), tr("Reading the interface state…"));
    // A fresh snapshot of the kernel state, independent of what the UI shows.
    if (m_snapshot)
        m_snapshot->deleteLater();
    m_snapshot = new NetState(this);
    const int generation = m_generation;
    connect(m_snapshot, &NetState::refreshed, this, [this, generation] {
        if (generation != m_generation)
            return;
        tsLink();
    });
    m_snapshot->refresh();
}

void Diagnostics::tsLink()
{
    if (!m_snapshot->error().isEmpty()) {
        failStep(QStringLiteral("link"), m_snapshot->error(), tr("The network state could not be read"), m_snapshot->error());
        return;
    }
    QStringList linked, others;
    const QVariantList interfaces = m_snapshot->interfaces();
    for (const QVariant &v : interfaces) {
        const QVariantMap i = v.toMap();
        if (i.value(QStringLiteral("category")) == QLatin1String("loopback"))
            continue;
        const QString name = i.value(QStringLiteral("name")).toString();
        const QString state = i.value(QStringLiteral("status")).toString();
        if (state == QLatin1String("up"))
            linked << name;
        else
            others << QStringLiteral("%1 (%2)").arg(name, state == QLatin1String("down") ? tr("disabled")
                         : state == QLatin1String("dormant") ? tr("dormant") : tr("no carrier"));
    }
    if (linked.isEmpty() && others.isEmpty()) {
        failStep(QStringLiteral("link"), tr("No network adapter besides loopback exists."),
                 tr("No network adapter found"),
                 tr("The kernel knows no network interface other than loopback. The adapter may be missing, disabled in the "
                    "firmware, or its driver or firmware is not loaded."));
        return;
    }
    if (linked.isEmpty()) {
        QString radio;
        const QVariantList switches = m_snapshot->rfkill();
        for (const QVariant &v : switches) {
            const QVariantMap s = v.toMap();
            if (s.value(QStringLiteral("type")) != QLatin1String("wlan"))
                continue;
            if (s.value(QStringLiteral("hardBlocked")).toBool())
                radio = tr(" The Wi-Fi radio is switched off by a hardware switch.");
            else if (s.value(QStringLiteral("softBlocked")).toBool())
                radio = tr(" The Wi-Fi radio is switched off (airplane mode or a software switch).");
        }
        failStep(QStringLiteral("link"), tr("No interface has a link: %1.").arg(others.join(QStringLiteral(", "))),
                 tr("No network link"),
                 tr("None of the network interfaces is connected: %1. Plug in the network cable or connect to a Wi-Fi "
                    "network.%2").arg(others.join(QStringLiteral(", ")), radio));
        return;
    }
    setStep(QStringLiteral("link"), QStringLiteral("ok"), tr("Link detected on %1.").arg(linked.join(QStringLiteral(", "))));
    tsAddress(linked);
}

void Diagnostics::tsAddress(const QStringList &linked)
{
    QStringList usable, linkLocalOnly;
    for (const QString &name : linked) {
        const QVariantList addresses = m_snapshot->interfaceByName(name).value(QStringLiteral("addresses")).toList();
        QStringList global;
        bool linkLocal = false;
        for (const QVariant &av : addresses) {
            const QVariantMap a = av.toMap();
            if (a.value(QStringLiteral("scope")) == QLatin1String("global"))
                global << a.value(QStringLiteral("cidr")).toString();
            else if (a.value(QStringLiteral("scope")) == QLatin1String("link"))
                linkLocal = true;
        }
        if (!global.isEmpty())
            usable << QStringLiteral("%1: %2").arg(name, global.join(QStringLiteral(", ")));
        else if (linkLocal)
            linkLocalOnly << name;
    }
    if (usable.isEmpty()) {
        const QString names = linked.join(QStringLiteral(", "));
        failStep(QStringLiteral("address"),
                 linkLocalOnly.isEmpty() ? tr("Link without an IP address: %1.").arg(names)
                                         : tr("Only a link-local address: %1.").arg(linkLocalOnly.join(QStringLiteral(", "))),
                 tr("No IP address"),
                 tr("A link is up (%1), but no interface has a routable IP address. With automatic configuration this "
                    "means no DHCP server (or IPv6 router) answered; with manual configuration the address is missing. "
                    "Check the connection's IP settings.").arg(names));
        return;
    }
    setStep(QStringLiteral("address"), QStringLiteral("ok"), usable.join(QStringLiteral("; ")));
    tsGateway();
}

void Diagnostics::tsGateway()
{
    QList<QVariantMap> candidates;
    if (!m_snapshot->defaultRoute4().isEmpty())
        candidates << m_snapshot->defaultRoute4();
    if (!m_snapshot->defaultRoute6().isEmpty())
        candidates << m_snapshot->defaultRoute6();
    if (candidates.isEmpty()) {
        failStep(QStringLiteral("gateway"), tr("The routing table has no default route."),
                 tr("No default route"),
                 tr("This computer has an address but no default route, so it only reaches hosts on its own network. "
                    "The DHCP server did not announce a gateway, or the manual configuration lacks one."));
        return;
    }
    setStep(QStringLiteral("gateway"), QStringLiteral("running"), tr("Contacting the default gateway…"));
    tsGatewayProbe(candidates, {});
}

void Diagnostics::tsGatewayProbe(QList<QVariantMap> candidates, QStringList failures)
{
    if (candidates.isEmpty()) {
        failStep(QStringLiteral("gateway"), failures.join(QLatin1Char(' ')),
                 tr("The default gateway is unreachable"),
                 tr("%1 The link is up and an address is configured, so the fault lies between this computer and its "
                    "router: a wrong network or VLAN, a wrong static address or gateway, client isolation on the Wi-Fi, "
                    "or the router itself is down.").arg(failures.join(QLatin1Char(' '))));
        return;
    }
    const QVariantMap route = candidates.takeFirst();
    const QString gateway = route.value(QStringLiteral("gateway")).toString().section(QLatin1Char(','), 0, 0).trimmed();
    const QString device = route.value(QStringLiteral("device")).toString().section(QLatin1Char(','), 0, 0).trimmed();
    if (gateway.isEmpty()) {
        // A device route (tunnels, point-to-point links) has no next hop to test.
        setStep(QStringLiteral("gateway"), QStringLiteral("ok"),
                tr("The default route points directly at %1; there is no next-hop address to test.").arg(device));
        tsDns();
        return;
    }
    const bool v6 = gateway.contains(QLatin1Char(':'));
    // A link-local next hop is only meaningful together with its interface.
    const QString target = v6 && gateway.startsWith(QLatin1String("fe80"), Qt::CaseInsensitive) && Net::isInterfaceName(device)
        ? gateway + QLatin1Char('%') + device : gateway;
    const QString program = Net::findTool(QStringLiteral("ping"));
    if (program.isEmpty() || !Net::isIpLiteral(target, true)) {
        m_tsWarned = true;
        setStep(QStringLiteral("gateway"), QStringLiteral("warn"),
                tr("Default route via %1 on %2; it could not be tested because ping is not installed.").arg(gateway, device));
        tsDns();
        return;
    }
    const int generation = m_generation;
    const QStringList args = {QStringLiteral("-n"), QStringLiteral("-c"), QStringLiteral("2"), QStringLiteral("-W"), QStringLiteral("1"),
                              v6 ? QStringLiteral("-6") : QStringLiteral("-4"), QStringLiteral("--"), target};
    run(program, args, 10000, false, [this, generation, candidates, failures, gateway, device](int code, const QString &output) {
        if (generation != m_generation)
            return;
        if (code == 0) {
            static const QRegularExpression rtt(QStringLiteral("= [0-9.]+/([0-9.]+)/"));
            const auto r = rtt.match(output);
            setStep(QStringLiteral("gateway"), QStringLiteral("ok"),
                    r.hasMatch() ? tr("Gateway %1 on %2 answers (average %3 ms).").arg(gateway, device, r.captured(1))
                                 : tr("Gateway %1 on %2 answers.").arg(gateway, device));
            tsDns();
            return;
        }
        // No echo reply. If its hardware address resolved, the gateway is on
        // the link and merely ignores ping.
        const QString ip = Net::findTool(QStringLiteral("ip"));
        Shadow::Command::run(this, ip.isEmpty() ? QStringLiteral("ip") : ip,
                            {QStringLiteral("-j"), QStringLiteral("neigh"), QStringLiteral("show"), QStringLiteral("to"), gateway,
                             QStringLiteral("dev"), device},
                            [this, generation, candidates, failures, gateway, device](const Shadow::CommandResult &r) {
            if (generation != m_generation)
                return;
            QString mac, state;
            const QJsonArray entries = QJsonDocument::fromJson(r.out).array();
            if (!entries.isEmpty()) {
                const QJsonObject e = entries.at(0).toObject();
                mac = e.value(QLatin1String("lladdr")).toString();
                const QJsonArray states = e.value(QLatin1String("state")).toArray();
                for (const QJsonValue &s : states)
                    state += s.toString() + QLatin1Char(' ');
            }
            static const QStringList alive = {QStringLiteral("REACHABLE"), QStringLiteral("STALE"), QStringLiteral("DELAY"),
                                              QStringLiteral("PROBE"), QStringLiteral("PERMANENT")};
            bool resolved = false;
            for (const QString &s : alive)
                resolved = resolved || state.contains(s);
            if (!mac.isEmpty() && resolved) {
                m_tsWarned = true;
                setStep(QStringLiteral("gateway"), QStringLiteral("warn"),
                        tr("Gateway %1 on %2 does not answer ping, but its hardware address %3 is known, so it is on the "
                           "link and most likely filters ICMP.").arg(gateway, device, mac));
                tsDns();
                return;
            }
            QStringList next = failures;
            next << tr("Gateway %1 on %2 answers neither ping nor address resolution.").arg(gateway, device);
            tsGatewayProbe(candidates, next);
        }, 5000);
    });
}

void Diagnostics::tsDns()
{
    if (Net::isIpLiteral(m_tsHost)) {
        setStep(QStringLiteral("dns"), QStringLiteral("skipped"),
                tr("Not tested: the test host %1 is an IP address, so no name needs resolving.").arg(m_tsHost));
        tsHttps();
        return;
    }
    setStep(QStringLiteral("dns"), QStringLiteral("running"), tr("Resolving %1 with the system resolver…").arg(m_tsHost));
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    resolve(m_tsHost, [this, clock](const QList<QHostAddress> &addresses, const QString &error) {
        const QVariantMap dns = m_snapshot ? m_snapshot->dns() : QVariantMap();
        const QStringList servers = dns.value(QStringLiteral("servers")).toStringList();
        const QString source = dns.value(QStringLiteral("source")).toString();
        if (!error.isEmpty() || addresses.isEmpty()) {
            const QString reason = error.isEmpty() ? tr("the answer contained no address") : error;
            failStep(QStringLiteral("dns"), tr("%1 could not be resolved: %2.").arg(m_tsHost, reason),
                     tr("Name resolution fails"),
                     servers.isEmpty()
                         ? tr("The gateway is reachable, but the name %1 could not be resolved (%2). No DNS server is "
                              "configured (%3 lists none) — set one on the DNS page.")
                               .arg(m_tsHost, reason, source.isEmpty() ? QStringLiteral("resolv.conf") : source)
                         : tr("The gateway is reachable, but the name %1 could not be resolved (%2). Configured DNS servers: %3 "
                              "(from %4). They are unreachable, blocked, or refuse to answer — try another DNS server on the DNS page.")
                               .arg(m_tsHost, reason, servers.join(QStringLiteral(", ")),
                                    source.isEmpty() ? QStringLiteral("resolv.conf") : source));
            return;
        }
        QStringList shown;
        for (const QHostAddress &a : addresses) {
            if (shown.size() < 4)
                shown << a.toString();
        }
        setStep(QStringLiteral("dns"), QStringLiteral("ok"),
                tr("%1 resolves to %2%3 (%4 ms).").arg(m_tsHost, shown.join(QStringLiteral(", ")),
                                                      addresses.size() > shown.size() ? tr(" and %n more", nullptr, int(addresses.size() - shown.size())) : QString())
                    .arg(clock->elapsed()));
        tsHttps();
    });
}

void Diagnostics::tsHttps()
{
    const int generation = m_generation;
    const bool literal = Net::isIpLiteral(m_tsHost);
    setStep(QStringLiteral("https"), QStringLiteral("running"), tr("Connecting to %1, port 443…").arg(m_tsHost));

    struct State
    {
        bool tcp = false;
        bool finished = false;
        QStringList tlsErrors;
        QString issuer;
        QElapsedTimer clock;
    };
    auto state = std::make_shared<State>();
    state->clock.start();

    auto *socket = new QSslSocket(this);
    m_tls = socket;
    socket->setProxy(QNetworkProxy::NoProxy);
    socket->setPeerVerifyMode(QSslSocket::VerifyPeer);
    auto *watchdog = new QTimer(socket);
    watchdog->setSingleShot(true);
    const bool tls = QSslSocket::supportsSsl() && !literal;

    // outcome: 0 = works, 1 = TCP failed, 2 = TLS failed
    auto finish = [this, generation, socket, state, tls, literal](int outcome, const QString &reason) {
        if (state->finished)
            return;
        state->finished = true;
        const QSslCertificate certificate = socket->peerCertificate();
        const QString protocol = protocolName(socket->sessionProtocol());
        socket->abort();
        socket->deleteLater();
        if (generation != m_generation)
            return;
        m_tls = nullptr;
        const qint64 ms = state->clock.elapsed();
        if (outcome == 0 && tls) {
            setStep(QStringLiteral("https"), QStringLiteral("ok"),
                    tr("%1 handshake with %2 succeeded in %3 ms; the certificate was issued by %4.")
                        .arg(protocol, m_tsHost).arg(ms).arg(issuerOf(certificate).isEmpty() ? tr("an unnamed authority") : issuerOf(certificate)));
            finishTroubleshoot();
        } else if (outcome == 0) {
            m_tsWarned = true;
            setStep(QStringLiteral("https"), QStringLiteral("warn"),
                    literal ? tr("TCP port 443 of %1 is reachable (%2 ms). The certificate was not checked because the test "
                                 "host is an IP address.").arg(m_tsHost).arg(ms)
                            : tr("TCP port 443 of %1 is reachable (%2 ms). The TLS handshake was not tested because Qt's TLS "
                                 "back-end is not available.").arg(m_tsHost).arg(ms));
            finishTroubleshoot();
        } else if (outcome == 1) {
            failStep(QStringLiteral("https"), tr("No TCP connection to %1, port 443: %2.").arg(m_tsHost, reason),
                     tr("HTTPS connections are blocked"),
                     tr("Names resolve and the gateway answers, but a direct TCP connection to %1 on port 443 failed (%2). "
                        "Outgoing HTTPS is being filtered by a firewall, or this network only allows web access through "
                        "a proxy.").arg(m_tsHost, reason));
        } else {
            const QString errors = state->tlsErrors.isEmpty() ? reason : state->tlsErrors.join(QStringLiteral("; "));
            failStep(QStringLiteral("https"), tr("TCP works, but the TLS handshake with %1 failed: %2.").arg(m_tsHost, errors),
                     tr("HTTPS connections are not trustworthy"),
                     tr("A TCP connection to %1 on port 443 works, but its certificate could not be verified: %2.%3 "
                        "This is what TLS interception looks like (an inspecting proxy or a captive portal answering in the "
                        "server's place). A wrong system clock (now: %4) or missing CA certificates (package ca-certificates) "
                        "cause the same error.")
                         .arg(m_tsHost, errors,
                              state->issuer.isEmpty() ? QString() : tr(" The presented certificate chain was issued by “%1”.").arg(state->issuer),
                              QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"))));
        }
    };

    connect(socket, &QSslSocket::connected, this, [state, finish, tls] {
        state->tcp = true;
        if (!tls)
            finish(0, QString());
    });
    connect(socket, &QSslSocket::sslErrors, this, [state](const QList<QSslError> &errors) {
        for (const QSslError &e : errors) {
            if (!state->tlsErrors.contains(e.errorString()))
                state->tlsErrors << e.errorString();
            // The top of the presented chain names who is really answering.
            if (!e.certificate().isNull())
                state->issuer = issuerOf(e.certificate());
        }
    });
    connect(socket, &QSslSocket::encrypted, this, [finish] { finish(0, QString()); });
    connect(socket, &QSslSocket::errorOccurred, this, [socket, state, finish](QAbstractSocket::SocketError error) {
        if (!state->tcp)
            finish(1, error == QAbstractSocket::ConnectionRefusedError ? tr("the connection was refused") : socket->errorString());
        else
            finish(2, socket->errorString());
    });
    connect(watchdog, &QTimer::timeout, this, [state, finish] {
        if (!state->tcp)
            finish(1, tr("no answer within 12 seconds"));
        else
            finish(2, tr("the TLS handshake did not complete within 12 seconds"));
    });
    watchdog->start(12000);
    if (tls)
        socket->connectToHostEncrypted(m_tsHost, 443);
    else
        socket->connectToHost(m_tsHost, 443);
}
