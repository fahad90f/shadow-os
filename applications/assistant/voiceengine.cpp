// SPDX-License-Identifier: GPL-3.0-or-later
#include "voiceengine.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

#include <cstdio>

namespace {

// Restart delays after a crash (spec §2.5), in milliseconds.
constexpr int kBackoffMs[] = {1000, 2000, 4000, 8000, 15000, 30000};
constexpr int kBackoffSteps = int(sizeof(kBackoffMs) / sizeof(kBackoffMs[0]));
// Give up after this many failures within kFailureWindowMs.
constexpr int kMaxFailures = 3;
constexpr qint64 kFailureWindowMs = 2 * 60 * 1000;
// "--probe" loads nothing heavy (spec: < 1 s); anything this slow is broken.
constexpr int kProbeTimeoutMs = 15000;
// Loading the models takes a few seconds; a helper that has not said
// "ready" after this long is treated as failed (and restarted).
constexpr int kReadyTimeoutMs = 180000;
constexpr int kStderrLines = 40;

qint64 monotonicMs()
{
    return QDeadlineTimer::current().deadline();
}

bool isExecutableFile(const QString &path)
{
    const QFileInfo fi(path);
    return fi.isFile() && fi.isExecutable();
}

// The last line of 'out' that parses as a JSON object.
QJsonObject lastJsonObject(const QByteArray &out)
{
    const QList<QByteArray> lines = out.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray line = it->trimmed();
        if (!line.startsWith('{'))
            continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            return doc.object();
    }
    return {};
}

QString firstLine(const QString &s)
{
    const QString t = s.trimmed();
    const qsizetype nl = t.indexOf(u'\n');
    return nl < 0 ? t : t.left(nl);
}

} // namespace

VoiceEngine::VoiceEngine(QObject *parent)
    : QObject(parent)
{
    m_forwardStderr = qEnvironmentVariableIntValue("SHADOW_VOICE_DEBUG") == 1;

    m_restartTimer = new QTimer(this);
    m_restartTimer->setSingleShot(true);
    connect(m_restartTimer, &QTimer::timeout, this, [this]() {
        if (m_status == Status::Restarting)
            launchServe();
    });

    m_readyTimer = new QTimer(this);
    m_readyTimer->setSingleShot(true);
    connect(m_readyTimer, &QTimer::timeout, this, [this]() {
        if (m_proc && m_status == Status::Starting) {
            m_fatal = tr("The voice engine did not finish loading within %1 seconds.")
                          .arg(kReadyTimeoutMs / 1000);
            m_proc->kill(); // finished() → handleFailure()
        }
    });

    m_termTimer = new QTimer(this);
    m_termTimer->setSingleShot(true);
    connect(m_termTimer, &QTimer::timeout, this, [this]() {
        if (!m_proc || m_proc->state() == QProcess::NotRunning)
            return;
        if (m_termTimer->property("killNext").toBool()) {
            m_proc->kill();
            return;
        }
        m_proc->terminate();
        m_termTimer->setProperty("killNext", true);
        m_termTimer->start(2000);
    });

    m_probeTimer = new QTimer(this);
    m_probeTimer->setSingleShot(true);
    connect(m_probeTimer, &QTimer::timeout, this, [this]() {
        if (m_probe)
            m_probe->kill(); // finished() reports the timeout
    });
}

VoiceEngine::~VoiceEngine()
{
    // Application exit: never leave a helper (and the microphone it holds)
    // behind. A bounded wait is acceptable here — nothing is drawn any more.
    if (m_probe) {
        m_probe->disconnect(this);
        m_probe->kill();
        m_probe->waitForFinished(300);
    }
    if (m_proc) {
        m_proc->disconnect(this);
        if (m_proc->state() == QProcess::Running) {
            m_proc->write(QByteArrayLiteral("{\"cmd\":\"quit\"}\n"));
            m_proc->closeWriteChannel();
            if (!m_proc->waitForFinished(500)) {
                m_proc->terminate();
                if (!m_proc->waitForFinished(500)) {
                    m_proc->kill();
                    m_proc->waitForFinished(300);
                }
            }
        } else if (m_proc->state() == QProcess::Starting) {
            m_proc->kill();
            m_proc->waitForFinished(300);
        }
    }
}

QString VoiceEngine::locateHelper(QString *why)
{
    const QString env = qEnvironmentVariable("SHADOW_VOICED");
    if (!env.isEmpty()) {
        // An explicit override is used exclusively: a typo must not silently
        // fall back to another installation.
        if (isExecutableFile(env))
            return env;
        if (why)
            *why = QFileInfo::exists(env)
                ? tr("SHADOW_VOICED points to %1, which is not executable.").arg(env)
                : tr("SHADOW_VOICED points to %1, which does not exist.").arg(env);
        return {};
    }
    const QString rel = QStringLiteral("/lib/shadow-assistant/voice/shadow-voiced");
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty())
        candidates << QFileInfo(appDir + QStringLiteral("/..") + rel).absoluteFilePath();
    candidates << QStringLiteral("/usr") + rel << QStringLiteral("/usr/local") + rel;
    candidates.removeDuplicates();
    for (const QString &c : std::as_const(candidates))
        if (isExecutableFile(c))
            return c;
    if (why)
        *why = tr("The voice helper (shadow-voiced) is not installed — looked in %1.")
                   .arg(candidates.join(QStringLiteral(", ")));
    return {};
}

QJsonObject VoiceEngine::probeSync(const QString &program, int *exitCode, QString *error, int timeoutMs)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, {QStringLiteral("--probe")});
    if (!p.waitForStarted(5000)) {
        if (exitCode) *exitCode = -1;
        if (error) *error = p.errorString();
        return {};
    }
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        if (exitCode) *exitCode = -2;
        if (error) *error = tr("timed out after %1 ms").arg(timeoutMs);
        return {};
    }
    if (exitCode)
        *exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -3;
    const QJsonObject o = lastJsonObject(p.readAllStandardOutput());
    if (error && o.isEmpty())
        *error = firstLine(QString::fromUtf8(p.readAllStandardError()));
    return o;
}

bool VoiceEngine::isRunning() const
{
    return m_proc && m_proc->state() != QProcess::NotRunning;
}

QStringList VoiceEngine::stderrTail(int maxLines) const
{
    QStringList lines = m_stderrLines;
    if (!m_stderrBuf.isEmpty())
        lines << QString::fromUtf8(m_stderrBuf);
    if (lines.size() > maxLines)
        lines = lines.mid(lines.size() - maxLines);
    return lines;
}

int VoiceEngine::recentFailures() const
{
    const qint64 now = monotonicMs();
    int n = 0;
    for (const qint64 t : m_failures)
        if (now - t <= kFailureWindowMs)
            ++n;
    return n;
}

void VoiceEngine::setStatus(Status s, const QString &message)
{
    if (s == m_status && message == m_message)
        return;
    m_status = s;
    m_message = message;
    Q_EMIT statusChanged();
}

void VoiceEngine::start()
{
    if (m_status == Status::Probing || m_status == Status::Starting || m_status == Status::Ready
        || m_status == Status::Restarting)
        return;
    if (isRunning()) {
        // Still shutting down from a stop(): start again once it is gone.
        m_restartAfterStop = true;
        return;
    }
    m_stopping = false;
    QString why;
    m_program = locateHelper(&why);
    if (m_program.isEmpty()) {
        m_unavailableKind = QStringLiteral("helper");
        setStatus(Status::Unavailable, why);
        return;
    }
    runProbe();
}

void VoiceEngine::runProbe()
{
    m_probeJson = {};
    m_probeExit = -1;
    m_probe = new QProcess(this);
    m_probe->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_probe, &QProcess::finished, this, &VoiceEngine::onProbeFinished);
    connect(m_probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || !m_probe)
            return;
        // No finished() follows a failed start.
        const QString err = m_probe->errorString();
        m_probe->deleteLater();
        m_probe = nullptr;
        m_probeTimer->stop();
        m_unavailableKind = QStringLiteral("helper");
        setStatus(Status::Unavailable, tr("The voice helper %1 could not be run: %2").arg(m_program, err));
    });
    setStatus(Status::Probing, tr("Checking the voice engine…"));
    m_probeTimer->start(kProbeTimeoutMs);
    m_probe->start(m_program, {QStringLiteral("--probe")});
    m_probe->closeWriteChannel();
}

void VoiceEngine::onProbeFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *p = m_probe;
    if (!p)
        return;
    m_probe = nullptr;
    p->deleteLater();
    const bool timedOut = !m_probeTimer->isActive();
    m_probeTimer->stop();
    if (m_stopping) {
        setStatus(Status::Stopped);
        if (m_restartAfterStop) {
            m_restartAfterStop = false;
            start();
        }
        return;
    }
    const QByteArray out = p->readAllStandardOutput();
    const QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
    m_probeJson = lastJsonObject(out);
    m_probeExit = exitStatus == QProcess::NormalExit ? exitCode : -1;
    const QString reason = m_probeJson.value(QStringLiteral("reason")).toString();
    QStringList missing;
    const QJsonArray miss = m_probeJson.value(QStringLiteral("missing")).toArray();
    for (const QJsonValue &v : miss)
        missing << v.toString();
    const QString missingText = missing.isEmpty() ? QString()
        : tr(" (missing: %1)").arg(missing.join(QStringLiteral(", ")));

    if (timedOut) {
        m_unavailableKind = QStringLiteral("probe");
        setStatus(Status::Unavailable, tr("The voice helper did not answer its self-check within %1 s.")
                                           .arg(kProbeTimeoutMs / 1000));
        return;
    }
    switch (m_probeExit) {
    case 0:
    case 5: // no capture tool: the voice can still speak; listening reports why it cannot
        if (m_probeJson.isEmpty() && m_probeExit == 0) {
            m_unavailableKind = QStringLiteral("probe");
            setStatus(Status::Unavailable, tr("The voice helper gave no answer to its self-check. %1")
                                               .arg(firstLine(err)).trimmed());
            return;
        }
        launchServe();
        return;
    case 3:
        m_unavailableKind = QStringLiteral("data");
        setStatus(Status::Unavailable,
                  (reason.isEmpty() ? tr("The voice models are not installed.") : reason) + missingText);
        return;
    case 4:
        m_unavailableKind = QStringLiteral("deps");
        setStatus(Status::Unavailable,
                  (reason.isEmpty() ? tr("Python modules the voice engine needs are missing.") : reason) + missingText);
        return;
    default:
        m_unavailableKind = QStringLiteral("probe");
        setStatus(Status::Unavailable,
                  tr("The voice helper's self-check failed (exit %1). %2")
                      .arg(m_probeExit)
                      .arg(reason.isEmpty() ? firstLine(err) : reason).trimmed());
        return;
    }
}

void VoiceEngine::launchServe()
{
    discardServe();
    m_stopping = false;
    m_sawEof = false;
    m_fatal.clear();
    m_ready = {};
    m_stdoutBuf.clear();
    m_stderrBuf.clear();
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &VoiceEngine::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError, this, &VoiceEngine::onStderr);
    connect(m_proc, &QProcess::errorOccurred, this, &VoiceEngine::onServeError);
    connect(m_proc, &QProcess::finished, this, &VoiceEngine::onServeFinished);
    connect(m_proc, &QProcess::started, this, &VoiceEngine::processStarted);
    setStatus(Status::Starting, tr("Loading the speech models…"));
    m_readyTimer->start(kReadyTimeoutMs);
    m_proc->start(m_program, QStringList{QStringLiteral("--serve")} + m_serveArgs);
}

void VoiceEngine::discardServe()
{
    if (!m_proc)
        return;
    m_proc->disconnect(this);
    if (m_proc->state() != QProcess::NotRunning)
        m_proc->kill();
    m_proc->deleteLater();
    m_proc = nullptr;
}

bool VoiceEngine::send(const QJsonObject &command)
{
    if (!m_proc || m_proc->state() != QProcess::Running)
        return false;
    m_proc->write(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
    Q_EMIT commandSent(command);
    return true;
}

void VoiceEngine::onStdout()
{
    if (!m_proc)
        return;
    m_stdoutBuf += m_proc->readAllStandardOutput();
    qsizetype nl;
    while (m_proc && (nl = m_stdoutBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuf.left(nl).trimmed();
        m_stdoutBuf.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            // Not protocol: keep it with the diagnostics rather than drop it.
            appendStderr("stdout: " + line + '\n');
            continue;
        }
        const QJsonObject ev = doc.object();
        const QString name = ev.value(QStringLiteral("event")).toString();
        if (name == QLatin1String("ready")) {
            m_ready = ev;
            m_readyTimer->stop();
            m_backoffIndex = 0;
            setStatus(Status::Ready);
        } else if (name == QLatin1String("error") && ev.value(QStringLiteral("fatal")).toBool()) {
            m_fatal = ev.value(QStringLiteral("message")).toString();
        } else if (name == QLatin1String("eof")) {
            m_sawEof = true;
        }
        Q_EMIT eventReceived(ev);
        Q_EMIT event(ev);
    }
}

void VoiceEngine::appendStderr(const QByteArray &data)
{
    m_stderrBuf += data;
    qsizetype nl;
    while ((nl = m_stderrBuf.indexOf('\n')) >= 0) {
        QString line = QString::fromUtf8(m_stderrBuf.left(nl)).trimmed();
        m_stderrBuf.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        if (line.size() > 400)
            line = line.left(400) + QStringLiteral("…");
        m_stderrLines << line;
        if (m_stderrLines.size() > kStderrLines)
            m_stderrLines.removeFirst();
    }
    if (m_stderrBuf.size() > 4096)
        m_stderrBuf = m_stderrBuf.right(4096);
}

void VoiceEngine::onStderr()
{
    if (!m_proc)
        return;
    const QByteArray data = m_proc->readAllStandardError();
    if (m_forwardStderr) {
        std::fwrite(data.constData(), 1, size_t(data.size()), stderr);
        std::fflush(stderr);
    }
    appendStderr(data);
}

void VoiceEngine::onServeError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart || !m_proc)
        return; // a crash also ends in finished(), which handles it
    // exec() failed (no finished() follows): missing interpreter, permissions…
    const QString why = m_proc->errorString();
    m_proc->disconnect(this);
    m_proc->deleteLater();
    m_proc = nullptr;
    m_readyTimer->stop();
    if (m_stopping) {
        setStatus(Status::Stopped);
        return;
    }
    handleFailure(tr("The voice helper could not be started: %1").arg(why));
}

void VoiceEngine::onServeFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_proc)
        return;
    // Drain what is left so the last events and error lines are not lost.
    m_stdoutBuf += m_proc->readAllStandardOutput();
    if (!m_stdoutBuf.endsWith('\n') && !m_stdoutBuf.isEmpty())
        m_stdoutBuf += '\n';
    onStdout();
    if (!m_proc)
        return; // an event handler stopped or restarted the engine
    appendStderr(m_proc->readAllStandardError() + '\n');
    m_proc->disconnect(this);
    m_proc->deleteLater();
    m_proc = nullptr;
    m_readyTimer->stop();
    m_termTimer->stop();
    m_termTimer->setProperty("killNext", false);

    if (m_stopping) {
        m_stopping = false;
        setStatus(Status::Stopped);
        if (m_restartAfterStop) {
            m_restartAfterStop = false;
            start();
        }
        return;
    }
    if (m_sawEof && exitStatus == QProcess::NormalExit && exitCode == 0) {
        // --input-wav: the helper finished its input and quit; nothing failed.
        setStatus(Status::Stopped, tr("The voice input ended."));
        return;
    }
    QString what = exitStatus == QProcess::CrashExit
        ? tr("The voice engine crashed.")
        : tr("The voice engine exited with code %1.").arg(exitCode);
    if (!m_fatal.isEmpty())
        what = m_fatal;
    handleFailure(what);
}

void VoiceEngine::handleFailure(const QString &what)
{
    const qint64 now = monotonicMs();
    m_failures.append(now);
    while (!m_failures.isEmpty() && now - m_failures.first() > kFailureWindowMs)
        m_failures.removeFirst();
    const QStringList tail = stderrTail(8);
    const QString details = tail.isEmpty() ? QString() : QStringLiteral("\n") + tail.join(u'\n');
    if (m_failures.size() >= kMaxFailures) {
        // A helper that dies again and again (models it cannot load, a broken
        // Python module) would burn CPU and toggle the microphone forever.
        setStatus(Status::Failed,
                  tr("The voice engine stopped %1 times within 2 minutes and was not restarted again. %2")
                          .arg(m_failures.size())
                          .arg(what)
                      + details);
        return;
    }
    const int delay = kBackoffMs[qMin(m_backoffIndex, kBackoffSteps - 1)];
    ++m_backoffIndex;
    setStatus(Status::Restarting,
              tr("%1 Restarting in %2 s.").arg(what).arg(delay / 1000) + details);
    Q_EMIT restartScheduled(delay, int(m_failures.size()));
    m_restartTimer->start(delay);
}

void VoiceEngine::stop()
{
    m_restartTimer->stop();
    m_readyTimer->stop();
    m_restartAfterStop = false;
    if (m_probe) {
        m_stopping = true;
        m_probe->kill(); // the probe is cheap to kill; finished() completes the stop
        return;
    }
    if (!m_proc) {
        m_stopping = false;
        if (m_status != Status::Unavailable && m_status != Status::Failed)
            setStatus(Status::Stopped);
        return;
    }
    m_stopping = true;
    if (m_proc->state() == QProcess::Running) {
        m_proc->write(QByteArrayLiteral("{\"cmd\":\"quit\"}\n"));
        m_proc->closeWriteChannel();
    }
    m_termTimer->setProperty("killNext", false);
    m_termTimer->start(1500);
}

void VoiceEngine::restart()
{
    m_restartTimer->stop();
    m_failures.clear();
    m_backoffIndex = 0;
    if (m_proc || m_probe) {
        stop();
        m_restartAfterStop = true;
        return;
    }
    m_status = Status::Stopped; // allow start() from Unavailable/Failed/Restarting
    start();
}
