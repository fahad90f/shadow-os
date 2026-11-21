// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class QTimer;

// Client of the offline voice engine helper, shadow-voiced (spec §2.1/§2.2).
//
// The helper is one long-running Python process that owns the microphone and
// the speaker and runs VAD, the wake word, speech-to-text and the neural
// voice. This class only manages that process and its line protocol:
//
//   * finds it ($SHADOW_VOICED, next to the app, /usr, /usr/local);
//   * asks "shadow-voiced --probe" first (fast, loads nothing) whether its
//     data, Python modules and a capture tool are there, and reports exactly
//     what is missing instead of starting something that cannot work;
//   * runs "shadow-voiced --serve", writes commands and parses the
//     newline-delimited JSON events on stdout;
//   * keeps the last stderr lines, so a crash can be explained to the user;
//   * restarts a helper that died, with backoff (1, 2, 4, 8, 15, 30 s), and
//     gives up with status Failed after 3 failures within 2 minutes — a
//     helper that cannot even load its models would otherwise loop forever.
//
// Nothing here ever blocks the GUI thread: every process is asynchronous and
// stop() escalates quit → SIGTERM → SIGKILL on timers. (Only the destructor,
// at application exit, waits a few hundred milliseconds so no helper is left
// behind; and probeSync() is for the command-line diagnostics.)
class VoiceEngine : public QObject
{
    Q_OBJECT
public:
    enum class Status {
        Stopped,     // not started, or stopped on purpose
        Probing,     // "--probe" is running
        Starting,    // "--serve" is running, models are loading (no "ready" yet)
        Ready,       // "ready" received: commands are served
        Restarting,  // died unexpectedly; a restart is scheduled (backoff)
        Unavailable, // the helper, its data or its Python modules are missing
        Failed,      // gave up: crashed repeatedly, or never became ready
    };
    Q_ENUM(Status)

    explicit VoiceEngine(QObject *parent = nullptr);
    ~VoiceEngine() override;

    // The helper executable: $SHADOW_VOICED (exclusively, when set), else
    // <applicationDirPath>/../lib/shadow-assistant/voice/shadow-voiced, else
    // /usr/lib/…, else /usr/local/lib/…. Empty when there is none; 'why' then
    // says what was looked for.
    static QString locateHelper(QString *why = nullptr);
    // Runs "<program> --probe" and waits (command-line diagnostics only).
    static QJsonObject probeSync(const QString &program, int *exitCode, QString *error,
                                 int timeoutMs = 15000);

    Status status() const { return m_status; }
    // Human-readable explanation of the status (empty when Ready).
    QString message() const { return m_message; }
    // For Unavailable: "helper" | "data" | "deps" | "probe".
    QString unavailableKind() const { return m_unavailableKind; }
    QString program() const { return m_program; }
    QJsonObject probeResult() const { return m_probeJson; }
    int probeExitCode() const { return m_probeExit; }
    QJsonObject readyInfo() const { return m_ready; }
    bool isReady() const { return m_status == Status::Ready; }
    bool isRunning() const;
    QStringList stderrTail(int maxLines = 12) const;
    int recentFailures() const;
    // The helper ran with --input-wav and reported the end of that input.
    bool inputFinished() const { return m_sawEof; }

    // Extra "--serve" arguments (tests: --input-wav FILE, --output-dir DIR).
    void setServeArguments(const QStringList &args) { m_serveArgs = args; }

    // Probe, then serve when usable. No-op while probing/starting/ready.
    void start();
    // Ask the helper to quit; escalate to SIGTERM/SIGKILL if it does not.
    void stop();
    // stop(), then start() again once the old process is gone; forgets the
    // crash history (user-initiated "re-check").
    void restart();
    // Write one command line; false when no helper process is running.
    bool send(const QJsonObject &command);

Q_SIGNALS:
    void statusChanged();
    // The --serve process is running (before "ready"): commands may be queued.
    void processStarted();
    // Every event line from the helper (already parsed).
    void event(const QJsonObject &event);
    // The same, emitted just before event(): a trace connected here shows
    // each helper event before the effects its handlers cause.
    void eventReceived(const QJsonObject &event);
    // Every command line written to the helper (diagnostics / headless tests).
    void commandSent(const QJsonObject &command);
    // The helper died; a restart follows after 'delayMs'.
    void restartScheduled(int delayMs, int failures);

private:
    void setStatus(Status s, const QString &message = QString());
    void runProbe();
    void onProbeFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void launchServe();
    void onStdout();
    void onStderr();
    void onServeError(QProcess::ProcessError error);
    void onServeFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleFailure(const QString &what);
    void discardServe();
    void appendStderr(const QByteArray &data);

    Status m_status = Status::Stopped;
    QString m_message;
    QString m_unavailableKind;
    QString m_program;
    QStringList m_serveArgs;

    QProcess *m_probe = nullptr;
    QTimer *m_probeTimer = nullptr;
    QJsonObject m_probeJson;
    int m_probeExit = -1;

    QProcess *m_proc = nullptr;
    QByteArray m_stdoutBuf;
    QByteArray m_stderrBuf;     // incomplete last stderr line
    QStringList m_stderrLines;  // ring of the last lines
    QJsonObject m_ready;
    QString m_fatal;            // message of a fatal "error" event
    bool m_sawEof = false;

    bool m_stopping = false;        // the current process is being stopped on purpose
    bool m_restartAfterStop = false;
    QTimer *m_restartTimer = nullptr;
    QTimer *m_readyTimer = nullptr; // a helper that never says "ready" counts as failed
    QTimer *m_termTimer = nullptr;  // stop(): SIGTERM, then SIGKILL
    QList<qint64> m_failures;       // monotonic ms of recent failures
    int m_backoffIndex = 0;
    bool m_forwardStderr = false;   // SHADOW_VOICE_DEBUG=1: copy helper stderr to ours
};
