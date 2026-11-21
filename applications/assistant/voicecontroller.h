// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "speechtext.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQmlParserStatus>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>

class Assistant;
class AssistantConfig;
class QProcess;
class QTimer;
class VoiceEngine;

Q_MOC_INCLUDE("assistant.h")
Q_MOC_INCLUDE("assistantconfig.h")

// The voice of the hands-free "Shadow" assistant (spec voice v2 §2.3–§2.5).
//
// Speech work happens in the shadow-voiced helper process (VoiceEngine): it
// owns the microphone and the speaker and runs VAD, the wake word, offline
// speech-to-text (Moonshine/Whisper) and the neural voice (Kokoro). This
// controller is the app-side half:
//
//   * presents the helper's state honestly to QML (state, engine,
//     engineMessage, capability flags, live levels and partial transcript);
//   * when 'agent' is set, wires voice and agent in C++: a transcript becomes
//     a message, the streamed reply is spoken sentence by sentence while it
//     arrives, tool confirmations are asked and answered by voice, errors and
//     Stop end the voice turn, and a short follow-up listen runs after a
//     spoken reply. QML only presents.
//   * falls back to eSpeak NG ("basic" engine: speaks, cannot listen) when
//     the neural engine is not installed, and can install it in the app
//     (shadow-voice-setup --user --progress-json).
//
// No method blocks the GUI thread: the helper, eSpeak and the installer are
// asynchronous processes, and every wait has a watchdog so no state can stick.
class VoiceController : public QObject, public QQmlParserStatus
{
    Q_OBJECT
    QML_ELEMENT
    Q_INTERFACES(QQmlParserStatus)

    // Must be agent.config. Adopted from 'agent' when not set explicitly.
    Q_PROPERTY(AssistantConfig *config READ config WRITE setConfig NOTIFY configChanged)
    // Setting it enables the C++ voice ⇄ agent wiring (spec §2.4).
    Q_PROPERTY(Assistant *agent READ agent WRITE setAgent NOTIFY agentChanged)

    // "unavailable" | "starting" | "muted" | "idle" | "listening" | "transcribing" | "speaking"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // "neural" | "starting" | "basic" | "missing" | "installing" | "error"
    Q_PROPERTY(QString engine READ engine NOTIFY engineChanged)
    // Why the engine is not "neural", and the fix (never empty unless neural).
    Q_PROPERTY(QString engineMessage READ engineMessage NOTIFY engineChanged)
    // The wake word is actively being listened for right now.
    Q_PROPERTY(bool armed READ armed NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool speaking READ speaking NOTIFY stateChanged)
    Q_PROPERTY(bool capturing READ capturing NOTIFY stateChanged)
    // The current turn came from voice: its reply is spoken.
    Q_PROPERTY(bool voiceTurn READ voiceTurn NOTIFY voiceTurnChanged)

    // Live presentation values, updated at most ~30 times a second.
    Q_PROPERTY(qreal micLevel READ micLevel NOTIFY levelsChanged)
    Q_PROPERTY(qreal speechLevel READ speechLevel NOTIFY levelsChanged)
    Q_PROPERTY(QString partialText READ partialText NOTIFY levelsChanged)
    Q_PROPERTY(QString lastHeard READ lastHeard NOTIFY levelsChanged)
    // The sentence being spoken right now, as this controller sent it (the
    // reply's sentence with its Markdown stripped, numbers as digits) — not
    // the helper's normalised form ("38.2 GB", not "thirty eight point two
    // gigabytes"). "" when nothing is spoken, and when the helper's sentence
    // cannot be matched to a sent one with confidence (never a wrong one).
    Q_PROPERTY(QString spokenSentence READ spokenSentence NOTIFY spokenSentenceChanged)
    // What the microphone is listening for right now:
    // "" (no listen) | "command" (push-to-talk) | "followup" (after a spoken
    // reply, no wake word needed) | "yesno" (a spoken confirmation) |
    // "wake" (the helper's own listen after a bare "Shadow", or a wake
    // utterance being transcribed).
    Q_PROPERTY(QString listenMode READ listenMode NOTIFY listenModeChanged)

    // Honest capability flags.
    Q_PROPERTY(bool ttsAvailable READ ttsAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool sttAvailable READ sttAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool wakeAvailable READ wakeAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool captureAvailable READ captureAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool typeAvailable READ typeAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool anyEngine READ anyEngine NOTIFY capabilitiesChanged)
    // Curated female voices: {id, name, accent, description, available}.
    Q_PROPERTY(QVariantList voices READ voices NOTIFY capabilitiesChanged)
    // Installed speech-to-text models ("moonshine", "whisper").
    Q_PROPERTY(QStringList sttModels READ sttModels NOTIFY capabilitiesChanged)

    // In-app installation of the neural engine.
    Q_PROPERTY(bool installing READ installing NOTIFY installChanged)
    Q_PROPERTY(qreal installProgress READ installProgress NOTIFY installChanged)
    Q_PROPERTY(QString installStatus READ installStatus NOTIFY installChanged)

    // One line for Settings, e.g. "Natural voice ready · Heart · listening for “Shadow”".
    Q_PROPERTY(QString statusText READ statusText NOTIFY capabilitiesChanged)
    // "kokoro" | "espeak" | ""
    Q_PROPERTY(QString resolvedTtsEngine READ resolvedTtsEngine NOTIFY capabilitiesChanged)
    // The capture tool the helper uses (pw-record / parec / arecord), "" if none.
    Q_PROPERTY(QString resolvedCaptureTool READ resolvedCaptureTool NOTIFY capabilitiesChanged)

public:
    explicit VoiceController(QObject *parent = nullptr);
    ~VoiceController() override;

    // QQmlParserStatus: the engine starts once QML has set every property.
    void classBegin() override {}
    void componentComplete() override;

    // Start the engine (QML does this in componentComplete). C++ users set
    // the serve arguments on voiceEngine() first, then call start().
    void start();
    // The helper client (headless session driver and diagnostics only).
    VoiceEngine *voiceEngine() const { return m_engine; }
    // A listen was sent and has not ended yet (C++ only, for the session driver).
    bool listenPending() const { return m_listenActive; }
    // --dump voice: what would be used, from "shadow-voiced --probe" only.
    // Never starts the helper, so the microphone is never opened.
    static QJsonObject probeDiagnostics(AssistantConfig *config);
    // Where the setup tool is ($SHADOW_VOICE_SETUP, next to the app, /usr/bin, /usr/local/bin).
    static QString locateSetupTool(QString *why = nullptr);

    AssistantConfig *config() const { return m_config; }
    void setConfig(AssistantConfig *c);
    Assistant *agent() const { return m_agent; }
    void setAgent(Assistant *a);

    QString state() const { return m_state; }
    QString engine() const { return m_engineName; }
    QString engineMessage() const { return m_engineMessage; }
    bool armed() const;
    bool speaking() const { return m_state == QLatin1String("speaking"); }
    bool capturing() const { return m_state == QLatin1String("listening") || m_state == QLatin1String("transcribing"); }
    bool voiceTurn() const { return m_voiceTurn; }
    qreal micLevel() const { return m_micLevel; }
    qreal speechLevel() const { return m_speechLevel; }
    QString partialText() const { return m_partial; }
    QString lastHeard() const { return m_lastHeard; }
    QString spokenSentence() const { return m_spokenSentence; }
    QString listenMode() const { return m_listenModeShown; }
    bool ttsAvailable() const { return !resolvedTtsEngine().isEmpty(); }
    bool sttAvailable() const;
    bool wakeAvailable() const;
    bool captureAvailable() const;
    bool typeAvailable() const { return m_wtype || m_xdotool; }
    bool anyEngine() const { return ttsAvailable() || sttAvailable(); }
    QVariantList voices() const;
    QStringList sttModels() const;
    bool installing() const { return m_installing; }
    qreal installProgress() const { return m_installProgress; }
    QString installStatus() const { return m_installStatus; }
    QString statusText() const;
    QString resolvedTtsEngine() const;
    QString resolvedCaptureTool() const { return m_captureTool; }

    Q_INVOKABLE QJsonObject diagnostics() const;

public Q_SLOTS:
    // Push-to-talk: begin a voice turn and listen for one command. Refuses
    // with unavailable(reason) — and leaves voiceTurn off — when it cannot.
    void startListening();
    // Second click: stop listening and transcribe what was said.
    void finishListening();
    // startListening(), or finishListening() while capturing.
    void toggleListening();
    // Discard: stop listening (no transcription), stop speaking, end the turn.
    void cancel();
    // Speak a text (Markdown is stripped). Honours speakReplies; interrupts
    // what is being said. The microphone mute never silences the speaker.
    void speak(const QString &text);
    void stopSpeaking();
    // A sample sentence in the given voice, without changing the config.
    void previewVoice(const QString &id);
    void beginVoiceTurn();
    void endVoiceTurn();
    // Speak 'prompt', then listen for yes/no → yesNoResult(tag, yes).
    void confirmBySpeech(const QString &tag, const QString &prompt);
    // Re-detect tools, re-probe and restart the helper.
    void refresh();
    // Install the neural engine for this user (shadow-voice-setup).
    void installEngine();
    void cancelInstall();

Q_SIGNALS:
    // A finished spoken command (also emitted when the agent wiring sends it).
    void transcriptReady(const QString &text);
    // A finished spoken command that could not be sent (the agent is busy).
    void heard(const QString &text);
    // "Shadow" was heard (confirmed by the transcript).
    void wakeDetected();
    void yesNoResult(const QString &tag, bool yes);
    // Something the user asked for cannot be done; 'reason' is for a toast.
    void unavailable(const QString &reason);
    void engineChanged();
    void levelsChanged();
    void stateChanged();
    void voiceTurnChanged();
    void capabilitiesChanged();
    void configChanged();
    void agentChanged();
    void installChanged();
    void spokenSentenceChanged();
    void listenModeChanged();

private:
    // engine + state bookkeeping
    void onEngineStatus();
    void onEngineEvent(const QJsonObject &ev);
    void onConfigChanged();
    void updateEngine();
    void updateState();
    void syncHelperConfig(bool force);
    void detectTools();

    // listening
    QString listenBlocker() const;
    void startListen(const QString &mode, double timeoutSecs);
    void clearListen();
    void handleFinal(const QJsonObject &ev);
    void handleTimeout(const QJsonObject &ev);
    void routeTranscript(const QString &text);
    void handleYesNoAnswer(const QString &text);
    // After one call was answered by voice: the next waiting call (if any)
    // is asked aloud during a voice turn.
    void askNextPending();
    void handleMic(const QJsonObject &ev);

    // speaking
    // 'sentences' are the sentences 'text' consists of (for spokenSentence);
    // empty = 'text' is one sentence.
    void queueSpeech(const QString &id, const QString &text, bool more, const QStringList &sentences = {});
    void afterSpeech(const QString &id, std::function<void(bool interrupted)> then);
    void onSpeechEnded(const QString &id, bool interrupted);
    void stopSpeech(bool suppressReply);
    void pumpEspeak();
    QString newId(const QString &prefix);

    // agent wiring
    void onReplyDelta(const QString &chunk);
    void onReplySegmentFinished();
    void onAssistantReplyReady(const QString &text);
    void onConfirmationNeeded(const QString &summary, const QString &preview, const QString &callId);
    void onAgentError(const QString &text);
    void onAgentStopped();
    void onAgentPhaseChanged();
    void finishVoiceReply(bool followUp);
    void maybeFollowUp();
    bool replySpeechWanted() const;

    // levels
    void noteLevel(const QString &source, double rms);
    void tickLevels();
    void setPartial(const QString &text);
    void notifyLevels();

    // spokenSentence / listenMode
    void updateListenMode();
    void setSpokenSentence(const QString &sentence);
    void onHelperSentence(const QString &id, const QString &text);
    void forgetSpeechTracks();

    // install
    void onInstallOutput();
    void onInstallFinished(int exitCode, const QString &failure);

    AssistantConfig *m_config = nullptr;
    QPointer<Assistant> m_agent;
    VoiceEngine *m_engine = nullptr;
    bool m_started = false;

    QString m_state = QStringLiteral("starting");
    QString m_engineName = QStringLiteral("starting");
    QString m_engineMessage;
    QString m_helperState;       // the helper's own "state" event value ("" when not running)
    QString m_captureTool;       // ready.capture
    QString m_micStatus;         // last "mic" event status: "ok" | "error" | "stopped"
    QString m_micMessage;
    QJsonObject m_lastConfigSent;
    int m_lastMuteSent = -1;     // -1 unknown, else 0/1

    // tools found on PATH
    QString m_espeakBin;
    bool m_wtype = false, m_xdotool = false;

    // voice turn + listening
    bool m_voiceTurn = false;
    bool m_turnByListen = false;  // the voice turn was begun by startListening() itself
    bool m_listenActive = false;
    QString m_listenId;
    QString m_listenMode;
    QTimer *m_listenWatchdog = nullptr;
    QString m_yesNoTag;          // tool call awaiting a spoken yes/no
    int m_seq = 0;

    // reply streaming
    SentenceStream m_stream;
    QString m_replyId;           // open speech id of the reply being streamed
    bool m_replySuppressed = false; // the user stopped this reply: speak no more of it
    QHash<QString, std::function<void(bool)>> m_afterSpeech;
    QSet<QString> m_helperSpeech; // ids sent to the helper whose speak_end has not come yet

    // What was sent to be spoken, per speech id, sentence by sentence: the
    // helper's "sentence" events carry its normalised text ("thirty eight
    // point two gigabytes"); they are matched against these to find the
    // sentence as the reply shows it. 'cur'/'off' is where the last match
    // ended (sentence index, word offset): the helper speaks in order.
    struct SpeechTrack
    {
        QStringList texts;
        QList<QStringList> words;
        int cur = 0;
        int off = 0;
    };
    QHash<QString, SpeechTrack> m_tracks;
    QString m_spokenSentence;
    QString m_listenModeShown;
    bool m_wakeListen = false;   // the helper heard "Shadow" and listens for the command itself

    // eSpeak NG fallback ("basic voice")
    // 'sentence': the one sentence this item is, "" when it is several.
    struct EspeakItem { QString id; QString text; bool more; QString sentence; };
    QList<EspeakItem> m_espeakQueue;
    QProcess *m_espeak = nullptr;
    EspeakItem m_espeakCurrent;
    // Ids eSpeak has begun and whose closing chunk has not played yet: the
    // state stays "speaking" between the sentences of a streamed reply.
    QSet<QString> m_espeakOpen;

    // levels and live transcript
    qreal m_micLevel = 0, m_speechLevel = 0;
    double m_micRaw = 0, m_ttsRaw = 0;
    qint64 m_micAt = 0, m_ttsAt = 0;  // when the last raw level arrived (ms, m_clock)
    QElapsedTimer m_clock;
    QTimer *m_levelTimer = nullptr;
    QString m_partial, m_lastHeard;
    bool m_testLevel = false, m_testPartial = false;
    bool m_levelsDirty = false;  // a text change waits for the next level tick

    // installer
    QProcess *m_install = nullptr;
    QByteArray m_installBuf;
    bool m_installing = false;
    bool m_installCancelled = false;
    qreal m_installProgress = 0;
    QString m_installStatus;
    QString m_installError;

    // repeated helper errors are reported once per 10 s
    QString m_lastErrorText;
    qint64 m_lastErrorAt = -100000;
};
