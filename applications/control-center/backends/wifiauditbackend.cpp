// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifiauditbackend.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>

#include <unistd.h>

static const int kMaxLogLines = 200;
static const QString kHelperPath = QStringLiteral("/usr/libexec/shadow/wifi-audit-helper");
static const QString kWordlist   = QStringLiteral("/usr/share/shadow-arsenal/wordlists/wifi-common.txt");

WifiAuditBackend::WifiAuditBackend(QObject *parent) : QObject(parent) {}

WifiAuditBackend::~WifiAuditBackend()
{
    killHelper();
    if (m_scan) { m_scan->kill(); m_scan->waitForFinished(2000); }
}

QString WifiAuditBackend::builtinWordlist() const { return kWordlist; }

void WifiAuditBackend::setPhase(const QString &p)
{
    if (m_phase == p) return;
    m_phase = p;
    m_running = (p != QLatin1String("idle") && p != QLatin1String("found")
                 && p != QLatin1String("not_found") && p != QLatin1String("error"));
    Q_EMIT changed();
}

void WifiAuditBackend::appendLog(const QString &line)
{
    if (m_log.size() >= kMaxLogLines)
        m_log.removeFirst();
    m_log.append(line);
    Q_EMIT logChanged();
}

void WifiAuditBackend::parseLine(const QString &line)
{
    if (line.startsWith(QLatin1String("PHASE:"))) {
        setPhase(line.mid(6).trimmed());
    } else if (line.startsWith(QLatin1String("LOG:"))) {
        appendLog(line.mid(4).trimmed());
    } else if (line.startsWith(QLatin1String("PROGRESS:"))) {
        const QStringList parts = line.mid(9).split(QLatin1Char(':'));
        if (parts.size() >= 2) {
            m_progress = parts[0].toInt();
            m_total    = parts[1].toInt();
            Q_EMIT changed();
        }
    } else if (line.startsWith(QLatin1String("RESULT:"))) {
        m_result = line.mid(7).trimmed();
        setPhase(QStringLiteral("found"));
    } else if (line.startsWith(QLatin1String("NOT_FOUND"))) {
        setPhase(QStringLiteral("not_found"));
    } else if (line.startsWith(QLatin1String("ERROR:"))) {
        m_error = line.mid(6).trimmed();
        appendLog(QStringLiteral("Error: ") + m_error);
        setPhase(QStringLiteral("error"));
    }
}

void WifiAuditBackend::start(const QString &bssid, const QString &ssid,
                              const QString &channel, const QString &wordlist)
{
    if (m_running) return;

    m_result   = QString();
    m_error    = QString();
    m_progress = 0;
    m_total    = 0;
    m_log.clear();
    m_buf.clear();
    Q_EMIT logChanged();

    const QString wl = wordlist.isEmpty() ? kWordlist : wordlist;

    QStringList args;
    if (geteuid() != 0)
        args << kHelperPath;
    args << QStringLiteral("bssid=") + bssid
         << QStringLiteral("ssid=")  + ssid
         << QStringLiteral("channel=") + channel
         << QStringLiteral("wordlist=") + wl;

    m_helper = new QProcess(this);
    m_helper->setProcessEnvironment(QProcessEnvironment::systemEnvironment());

    connect(m_helper, &QProcess::readyReadStandardOutput, this, [this] {
        m_buf += m_helper->readAllStandardOutput();
        int nl;
        while ((nl = m_buf.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_buf.left(nl)).trimmed();
            m_buf.remove(0, nl + 1);
            if (!line.isEmpty())
                parseLine(line);
        }
    });
    connect(m_helper, &QProcess::readyReadStandardError, this, [this] {
        const QString err = QString::fromUtf8(m_helper->readAllStandardError()).trimmed();
        if (!err.isEmpty())
            appendLog(err);
    });
    connect(m_helper, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &WifiAuditBackend::onHelperFinished);

    setPhase(QStringLiteral("monitor"));
    if (geteuid() == 0)
        m_helper->start(kHelperPath, args);
    else
        m_helper->start(QStringLiteral("pkexec"), args);
}

void WifiAuditBackend::stop()
{
    if (!m_running && !m_helper) return;
    appendLog(tr("Stopping…"));
    killHelper();
    setPhase(QStringLiteral("idle"));
}

void WifiAuditBackend::killHelper()
{
    if (!m_helper) return;
    m_helper->disconnect(this);
    m_helper->terminate();
    if (!m_helper->waitForFinished(3000))
        m_helper->kill();
    m_helper->deleteLater();
    m_helper = nullptr;
    m_buf.clear();
}

void WifiAuditBackend::onHelperFinished(int exitCode, QProcess::ExitStatus)
{
    // Any remaining buffered output.
    if (m_helper) {
        m_buf += m_helper->readAllStandardOutput();
        int nl;
        while ((nl = m_buf.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_buf.left(nl)).trimmed();
            m_buf.remove(0, nl + 1);
            if (!line.isEmpty()) parseLine(line);
        }
    }

    if (m_running) {
        if (exitCode == 126) {
            m_error = tr("Not authorised — authentication was cancelled or failed.");
        } else if (exitCode == 127) {
            m_error = tr("polkit is not running in this session.");
        } else if (exitCode != 0 && m_phase != QLatin1String("found")
                   && m_phase != QLatin1String("not_found")) {
            m_error = tr("The helper exited with code %1.").arg(exitCode);
            setPhase(QStringLiteral("error"));
        }
    }

    if (m_helper) {
        m_helper->deleteLater();
        m_helper = nullptr;
    }

    if (m_running)
        setPhase(QStringLiteral("idle"));
}

void WifiAuditBackend::scanNetworks()
{
    if (m_scanBusy) return;
    m_scanBusy = true;
    Q_EMIT scanBusyChanged();

    m_scan = new QProcess(this);
    m_scan->setProcessEnvironment(QProcessEnvironment::systemEnvironment());

    connect(m_scan, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
        const QByteArray out = m_scan->readAllStandardOutput();
        m_scan->deleteLater();
        m_scan = nullptr;
        m_scanBusy = false;
        Q_EMIT scanBusyChanged();

        if (exitCode != 0) { Q_EMIT networksChanged(); return; }

        // Parse nmcli -t -f SSID,BSSID,SIGNAL,CHAN,SECURITY output.
        // Fields are colon-separated; nmcli escapes literal colons as \:.
        m_networks.clear();
        for (const QByteArray &rawLine : out.split('\n')) {
            const QString line = QString::fromUtf8(rawLine).trimmed();
            if (line.isEmpty()) continue;

            // Split on un-escaped colons.
            QStringList cols;
            QString cur;
            for (int i = 0; i < line.size(); ++i) {
                if (line[i] == QLatin1Char('\\') && i + 1 < line.size()) {
                    cur += line[++i]; // escaped character
                } else if (line[i] == QLatin1Char(':')) {
                    cols << cur; cur.clear();
                } else {
                    cur += line[i];
                }
            }
            cols << cur;
            if (cols.size() < 5) continue;

            const QString ssid  = cols[0];
            const QString bssid = cols[1];
            const int signal    = cols[2].toInt();
            const QString chan   = cols[3];
            const QString sec    = cols[4];

            if (bssid.isEmpty() || bssid == QLatin1String("--")) continue;

            // Skip open networks (no handshake to capture).
            if (sec.contains(QLatin1String("WPA"), Qt::CaseInsensitive) == false
                && sec.contains(QLatin1String("WEP"), Qt::CaseInsensitive) == false)
                continue;

            m_networks.append(QVariantMap{
                {QStringLiteral("ssid"),    ssid},
                {QStringLiteral("bssid"),   bssid},
                {QStringLiteral("signal"),  signal},
                {QStringLiteral("channel"), chan},
                {QStringLiteral("security"), sec},
            });
        }
        Q_EMIT networksChanged();
    });

    m_scan->start(QStringLiteral("nmcli"),
                  {QStringLiteral("-t"), QStringLiteral("-f"),
                   QStringLiteral("SSID,BSSID,SIGNAL,CHAN,SECURITY"),
                   QStringLiteral("device"), QStringLiteral("wifi"),
                   QStringLiteral("list"), QStringLiteral("--rescan"),
                   QStringLiteral("yes")});
}
