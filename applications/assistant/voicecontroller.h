// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QString>
#include <QStringList>

class AssistantConfig;

// The voice subsystem for the hands-free "Shadow" assistant.
//
// Everything here drives verified command-line engines through an argument
// vector (never a shell), so the application links no extra libraries:
//
//   * wake word   — pocketsphinx keyphrase spotting ("shadow"), opt-in, run as
//                   a long-lived listener that only reports a hit; nothing is
//                   recorded or sent anywhere until the phrase fires, and never
//                   while muted.
//   * capture     — parecord / pw-record / arecord into a 16 kHz mono WAV, with
//                   a maximum utterance length.
//   * command STT — pocketsphinx offline transcription of the captured WAV
//                   (works with no network). (Live NVIDIA ASR is the intended
//                   primary but needs an audio endpoint + microphone; see README.)
//   * speak back  — piper (neural) if a voice model is configured, else
//                   speech-dispatcher (spd-say), else espeak-ng.
//
// When an engine is absent the controller reports it honestly (an "unavailable"
// state and a reason) rather than pretending to work. The microphone privacy
// mute and the opt-in wake toggle live in AssistantConfig.
class VoiceController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(AssistantConfig *config READ config WRITE setConfig NOTIFY configChanged)

    // Mic-side presence state for the orb:
    // "unavailable" | "muted" | "idle" | "wake" | "listening" | "transcribing" | "speaking"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool speaking READ speaking NOTIFY stateChanged)
    Q_PROPERTY(bool capturing READ capturing NOTIFY stateChanged)
    Q_PROPERTY(bool voiceTurn READ voiceTurn NOTIFY voiceTurnChanged)

    // Capability summary (recomputed by refresh()).
    Q_PROPERTY(bool anyEngine READ anyEngine NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool ttsAvailable READ ttsAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool sttAvailable READ sttAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool wakeAvailable READ wakeAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool captureAvailable READ captureAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool typeAvailable READ typeAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(QString resolvedTtsEngine READ resolvedTtsEngine NOTIFY capabilitiesChanged)
    Q_PROPERTY(QString resolvedCaptureTool READ resolvedCaptureTool NOTIFY capabilitiesChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY capabilitiesChanged)

public:
    explicit VoiceController(QObject *parent = nullptr);
    ~VoiceController() override;

    AssistantConfig *config() const { return m_config; }
    void setConfig(AssistantConfig *c);

    QString state() const { return m_state; }
    bool speaking() const { return m_state == QLatin1String("speaking"); }
    bool capturing() const { return m_state == QLatin1String("listening") || m_state == QLatin1String("transcribing"); }
    bool voiceTurn() const { return m_voiceTurn; }

    bool anyEngine() const { return ttsAvailable() || sttAvailable(); }
    bool ttsAvailable() const { return m_piper || m_speechd || m_espeak; }
    bool sttAvailable() const { return m_sttPocketsphinx; }
    bool wakeAvailable() const { return m_wakePocketsphinx && captureAvailable(); }
    bool captureAvailable() const { return !m_captureTool.isEmpty(); }
    bool typeAvailable() const { return m_wtype || m_xdotool; }
    QString resolvedTtsEngine() const;
    QString resolvedCaptureTool() const { return m_captureTool; }
    QString statusText() const;

    // Diagnostics and test entry points (run the real engines synchronously).
    QJsonObject diagnostics() const;
    // Transcribe a 16 kHz mono WAV with pocketsphinx; empty on failure.
    QString transcribeWavSync(const QString &wavPath, QString *error = nullptr) const;
    // Does the wake phrase occur in this WAV? (pocketsphinx keyphrase spotting.)
    bool wakePhraseInWavSync(const QString &wavPath, const QString &phrase, double threshold, QString *error = nullptr) const;
    // Synthesize text to a WAV with the resolved/forced TTS engine; false on failure.
    bool synthesizeToWavSync(const QString &text, const QString &outWav, QString *error = nullptr) const;

public Q_SLOTS:
    void refresh();
    // Begin a push-to-talk / post-wake capture of one spoken command.
    void startCommandCapture();
    // Stop any capture, wake listener restart, and speaking (barge-in / mute).
    void cancel();
    // Speak a line aloud (honours speak_replies + mute). Interrupts prior speech.
    void speak(const QString &text);
    void stopSpeaking();
    // Speak a confirmation prompt, then capture a yes/no answer; emits
    // yesNoResult(tag, yes). Used for hands-free action confirmation.
    void confirmBySpeech(const QString &tag, const QString &prompt);
    // Mark the start/end of a turn that originated by voice (so replies are spoken).
    void beginVoiceTurn();
    void endVoiceTurn();
    // Re-evaluate the wake listener against the current opt-in/mute settings.
    void syncWakeListener();

Q_SIGNALS:
    void configChanged();
    void stateChanged();
    void voiceTurnChanged();
    void capabilitiesChanged();
    // A fully transcribed spoken command (routed to the model as a user turn).
    void transcriptReady(const QString &text);
    // The wake phrase was heard while always-listening.
    void wakeDetected();
    // Result of confirmBySpeech(): yes=true if the user said yes.
    void yesNoResult(const QString &tag, bool yes);
    // Something could not be done; 'reason' is human-readable.
    void unavailable(const QString &reason);

private:
    void setState(const QString &s);
    void onConfigChanged();
    QStringList captureArgv(const QString &outWav, int seconds) const;
    QStringList ttsSpeakArgv(const QString &engine, const QString &text) const;
    void transcribeCapturedFile(const QString &wavPath);
    void startWakeListener();
    void stopWakeListener();
    QString pocketsphinxBin() const { return m_pocketsphinxBin; }
    QString modelDir() const;
    QStringList pocketsphinxTranscribeArgv(const QString &wavPath) const;
    QStringList pocketsphinxKeyphraseLiveArgv() const;

    AssistantConfig *m_config = nullptr;
    QString m_state = QStringLiteral("idle");
    bool m_voiceTurn = false;
    QString m_pendingTag;           // confirmBySpeech tag awaiting a yes/no
    bool m_awaitingYesNo = false;

    // Detected engines/tools.
    bool m_piper = false, m_speechd = false, m_espeak = false;
    bool m_sttPocketsphinx = false, m_wakePocketsphinx = false;
    bool m_wtype = false, m_xdotool = false;
    QString m_captureTool;          // resolved capture program ("" if none)
    QString m_pocketsphinxBin;      // "pocketsphinx" or "pocketsphinx_continuous"

    // Live processes (null when idle).
    QProcess *m_capture = nullptr;
    QProcess *m_speak = nullptr;
    QProcess *m_wake = nullptr;
    QString m_captureWav;
};
