// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QQmlEngine>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class NetState;
class QTcpSocket;
class QSslSocket;

// Network diagnostics: single-target tools with live output and an automated
// troubleshooting run.
//
//   ping, MTU probe      iputils `ping` (`-M do` for the don't-fragment probe)
//   route trace          `traceroute`, or `tracepath` if that is what is installed
//   DNS lookup           `dig`, else `resolvectl query`, else the C library resolver
//   port check           a TCP connection attempt made by this process (QTcpSocket)
//   troubleshoot         link → address → gateway → DNS → HTTPS, stopping at the
//                        first layer that fails
//
// Every target is validated as a host name or IP literal before use and
// passed to the tools as one argument; nothing goes through a shell. Each
// tool acts on the one host (and port) the user entered — there is no range
// or sweep mode.
class Diagnostics : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString tool READ tool NOTIFY toolChanged)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)
    Q_PROPERTY(QString result READ result NOTIFY resultChanged)
    Q_PROPERTY(QString resultKind READ resultKind NOTIFY resultChanged)
    Q_PROPERTY(QVariantMap tools READ tools NOTIFY toolsChanged)
    Q_PROPERTY(bool troubleshooting READ troubleshooting NOTIFY troubleshootingChanged)
    Q_PROPERTY(QVariantList steps READ steps NOTIFY stepsChanged)
    Q_PROPERTY(QString verdict READ verdict NOTIFY verdictChanged)
    Q_PROPERTY(QString verdictDetail READ verdictDetail NOTIFY verdictChanged)
    Q_PROPERTY(QString verdictKind READ verdictKind NOTIFY verdictChanged)
    Q_PROPERTY(QString defaultTestHost READ defaultTestHost CONSTANT)

public:
    explicit Diagnostics(QObject *parent = nullptr);
    ~Diagnostics() override;

    bool running() const { return m_running; }
    QString tool() const { return m_tool; }
    QString output() const { return m_output; }
    QString result() const { return m_result; }
    QString resultKind() const { return m_resultKind; }
    QVariantMap tools() const { return m_tools; }
    bool troubleshooting() const { return m_troubleshooting; }
    QVariantList steps() const { return m_steps; }
    QString verdict() const { return m_verdict; }
    QString verdictDetail() const { return m_verdictDetail; }
    QString verdictKind() const { return m_verdictKind; }
    // The Debian mirror this system installs its updates from: a host the
    // machine needs to reach anyway.
    QString defaultTestHost() const { return QStringLiteral("deb.debian.org"); }

    // family: "auto", "IPv4" or "IPv6".
    Q_INVOKABLE void ping(const QString &host, const QString &family, int count);
    Q_INVOKABLE void traceRoute(const QString &host, const QString &family);
    Q_INVOKABLE void dnsLookup(const QString &name, const QString &recordType, const QString &server);
    Q_INVOKABLE void portCheck(const QString &host, int port, const QString &family, int timeoutSeconds);
    Q_INVOKABLE void mtuProbe(const QString &host, const QString &family);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void clearOutput();
    Q_INVOKABLE void refreshTools();

    Q_INVOKABLE void troubleshoot(const QString &testHost);

Q_SIGNALS:
    void runningChanged();
    void toolChanged();
    void outputChanged();
    void resultChanged();
    void toolsChanged();
    void troubleshootingChanged();
    void stepsChanged();
    void verdictChanged();
    // The troubleshooting run ended (also when stopped).
    void troubleshootFinished();
    // A tool run ended.
    void finished();

private:
    using ProcessDone = std::function<void(int exitCode, const QString &output)>;

    bool begin(const QString &tool);
    void end(const QString &kind, const QString &result);
    void append(const QString &text);
    // Run one external program. With 'live' its output is appended as it
    // arrives; either way the complete output is handed to 'done'. Exit code
    // -1: could not be started, -2: timed out, -3: stopped by the user.
    void run(const QString &program, const QStringList &arguments, int timeoutMs, bool live, ProcessDone done);
    QString resolveFamilyOption(const QString &target, const QString &family, QString *error) const;
    void resolve(const QString &host, std::function<void(const QList<QHostAddress> &, const QString &error)> done);

    // port check
    void portCheckNext();
    // MTU probe
    void mtuStart(const QString &address);
    void mtuPing(int payload, std::function<void(bool ok, const QString &note)> done);
    void mtuSearch(int low, int high);
    void mtuFinish(int payload);

    // troubleshooting
    void setStep(const QString &key, const QString &state, const QString &detail);
    void failStep(const QString &key, const QString &detail, const QString &verdict, const QString &explanation);
    void finishTroubleshoot();
    void tsLink();
    void tsAddress(const QStringList &linked);
    void tsGateway();
    void tsGatewayProbe(QList<QVariantMap> candidates, QStringList failures);
    void tsDns();
    void tsHttps();
    void setVerdict(const QString &kind, const QString &title, const QString &detail);

    bool m_running = false;
    bool m_stopRequested = false;
    QString m_tool, m_output, m_result, m_resultKind;
    QVariantMap m_tools;
    QPointer<QProcess> m_process;
    QTimer m_processTimeout;
    QString m_processOutput;
    bool m_processLive = false;
    ProcessDone m_processDone;
    int m_generation = 0;           // invalidates callbacks of a stopped run

    // port check state
    QList<QHostAddress> m_portTargets;
    QString m_portHost;
    int m_port = 0;
    int m_portTimeoutMs = 5000;
    QStringList m_portOpen, m_portProblems;
    QPointer<QTcpSocket> m_socket;

    // MTU probe state
    QString m_mtuTarget, m_mtuFamilyOption;
    int m_mtuOverhead = 28;
    int m_mtuLocal = 1500;

    // troubleshooting state
    bool m_troubleshooting = false;
    QVariantList m_steps;
    QString m_verdict, m_verdictDetail, m_verdictKind;
    QString m_tsHost;
    bool m_tsWarned = false;
    NetState *m_snapshot = nullptr;
    QPointer<QSslSocket> m_tls;
    int m_lookupId = -1;
};
