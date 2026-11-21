// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

class Assistant;
class QTimer;
class VoiceController;

// "shadow-assistant --voice-session WAV" (spec voice v2 §3): the whole
// app-side voice loop, headless. The voice engine helper hears WAV instead of
// the microphone and writes its speech to a directory instead of the speaker;
// a real Assistant (pointed at a test server with SHADOW_ASSISTANT_BASE_URL)
// answers; VoiceController wires the two exactly as in the app. Every step
// is printed on stdout as one JSON object per line ("t" = seconds since the
// session started):
//
//   session, engine, ready, state, wake, final, sent, heard, reply, speak,
//   speak_start, speak_end, listen, follow_up, timeout, confirm, yesno,
//   tool, unavailable, error, engine_restart, eof, action, done
//
// The session ends when the helper has consumed its input (its "eof" event,
// or — for a helper that sends none — the WAV's duration after "ready"; or
// the engine settled without a helper), every scripted action ran, and voice
// and agent have been quiet for a moment; or after --voice-timeout.
// Exit status: 0 ended normally, 1 timed out, 2 bad arguments.
//
// --voice-actions "T:ACTION[:ARG];…" (tests) runs controller actions T seconds
// after the engine settled (became ready, or found it cannot): start, finish,
// toggle, cancel, stop-agent, stop-speaking, mute, unmute, refresh,
// speak:TEXT, preview:VOICE, send:TEXT (a typed-in voice turn), install,
// cancel-install.
class VoiceSession : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        QString wav;
        QString outDir;        // empty: a new temporary directory
        double timeoutSecs = 120;
        QString actions;
    };

    explicit VoiceSession(const Options &options, QObject *parent = nullptr);
    // Runs the event loop until the session ends; returns the exit status.
    int run();

private:
    struct Action
    {
        double at = 0;
        QString name;
        QString arg;
    };

    void emitLine(const QString &event, QJsonObject fields = {});
    void wire();
    void onEngineEvent(const QJsonObject &ev);
    void onCommand(const QJsonObject &cmd);
    void onSettled();
    void runAction(const Action &a);
    void checkQuiet();
    bool quiet() const;
    void finish(const QString &reason, int exitCode);
    QString parseActions(const QString &spec);

    Options m_opt;
    Assistant *m_agent = nullptr;
    VoiceController *m_voice = nullptr;
    QElapsedTimer m_clock;
    QTimer *m_quietTimer = nullptr;
    QTimer *m_deadline = nullptr;
    QList<Action> m_actions;
    int m_actionsPending = 0;
    bool m_settled = false;
    bool m_eof = false;            // the helper has consumed all of its input
    bool m_inputTimerStarted = false;
    bool m_recheckQueued = false;  // a quiet check is queued for the next event-loop turn
    bool m_finished = false;
    int m_exitCode = 0;
    QString m_lastPhase;
    QString m_conversation;  // created by this session; removed at the end

    // counters for the summary
    int m_finals = 0, m_sent = 0, m_replies = 0, m_speakChunks = 0;
    int m_levelEmits = 0, m_levelBucket = -1, m_levelBucketCount = 0, m_levelMaxHz = 0;
};
