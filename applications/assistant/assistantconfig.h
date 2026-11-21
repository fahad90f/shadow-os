// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>

// Runtime configuration for SHADOW Assistant.
//
// The Claude (Anthropic) API key is NEVER stored in the repository or written
// by this application. It is read at run time from, in order of precedence:
//   1. the environment variable  SHADOW_ASSISTANT_API_KEY
//   2. the environment variable  ANTHROPIC_API_KEY
//   3. ~/.config/shadow/assistant.conf   [api] key=...
//
// Everything else (model mapping, autonomy, which tools are enabled) lives in
// the same INI file and may be changed from the Settings page. The base URL
// defaults to the Anthropic Messages API endpoint and can be overridden for
// testing with the environment variable SHADOW_ASSISTANT_BASE_URL.
class AssistantConfig : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool apiKeyConfigured READ apiKeyConfigured NOTIFY changed)
    Q_PROPERTY(QString apiKeySource READ apiKeySource NOTIFY changed)
    Q_PROPERTY(QString baseUrl READ baseUrl NOTIFY changed)
    Q_PROPERTY(bool baseUrlOverridden READ baseUrlOverridden NOTIFY changed)
    // Local, offline model (an OpenAI-compatible server on this machine, e.g.
    // ollama) instead of the Claude cloud. Nothing leaves the computer.
    Q_PROPERTY(bool localMode READ localMode WRITE setLocalMode NOTIFY changed)
    Q_PROPERTY(QString localModel READ localModel NOTIFY changed)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)
    Q_PROPERTY(QString configTemplate READ configTemplate CONSTANT)
    Q_PROPERTY(QString brainModel READ brainModel NOTIFY changed)
    Q_PROPERTY(QString plannerModel READ plannerModel NOTIFY changed)
    Q_PROPERTY(QString fastModel READ fastModel NOTIFY changed)
    Q_PROPERTY(QString omniModel READ omniModel NOTIFY changed)
    // "ask_every" | "ask_risky" | "autopilot"
    Q_PROPERTY(QString autonomy READ autonomy WRITE setAutonomy NOTIFY changed)
    // "general" | "pentest"  — changes the system prompt and available tools
    Q_PROPERTY(QString role READ role WRITE setRole NOTIFY changed)
    Q_PROPERTY(QStringList enabledTools READ enabledTools NOTIFY changed)

    // --- Voice (hands-free "Shadow") -------------------------------------
    // Persisted in the [voice] section of the same conf file (spec voice v2
    // §2.3). The wake word itself is fixed: "Shadow".
    Q_PROPERTY(bool wakeEnabled READ wakeEnabled WRITE setWakeEnabled NOTIFY changed)
    Q_PROPERTY(QString wakePhrase READ wakePhrase CONSTANT)
    // Privacy mute: the microphone is never opened while true.
    Q_PROPERTY(bool micMuted READ micMuted WRITE setMicMuted NOTIFY changed)
    Q_PROPERTY(bool speakReplies READ speakReplies WRITE setSpeakReplies NOTIFY changed)
    // Neural voice id (Kokoro), default "af_heart".
    Q_PROPERTY(QString voice READ voice WRITE setVoice NOTIFY changed)
    // 0.7 … 1.4, default 1.0.
    Q_PROPERTY(double speechRate READ speechRate WRITE setSpeechRate NOTIFY changed)
    // "auto" | "moonshine" | "whisper"
    Q_PROPERTY(QString sttModel READ sttModel WRITE setSttModel NOTIFY changed)
    // "low" | "normal" | "high"
    Q_PROPERTY(QString wakeSensitivity READ wakeSensitivity WRITE setWakeSensitivity NOTIFY changed)
    // After a spoken reply, listen a few seconds for a follow-up without the wake word.
    Q_PROPERTY(bool followUp READ followUp WRITE setFollowUp NOTIFY changed)
    // Capture source (PipeWire/PulseAudio name); "" = the default input.
    Q_PROPERTY(QString inputDevice READ inputDevice WRITE setInputDevice NOTIFY changed)
    // "auto" | "neural" | "espeak"
    Q_PROPERTY(QString ttsEngine READ ttsEngine WRITE setTtsEngine NOTIFY changed)

public:
    explicit AssistantConfig(QObject *parent = nullptr);

    // In local mode no key is needed: the model runs on this machine.
    bool apiKeyConfigured() const { return m_localMode || !m_apiKey.isEmpty(); }
    QString apiKeySource() const { return m_apiKeySource; }
    // The key itself is never exposed to QML; only the network layer reads it.
    QString apiKey() const { return m_apiKey; }
    // The endpoint actually used: the local server in local mode, else the cloud.
    QString baseUrl() const { return m_localMode ? m_localBaseUrl : m_baseUrl; }
    // Value for the Anthropic "anthropic-version" request header (cloud mode).
    QString anthropicVersion() const { return m_anthropicVersion; }
    bool baseUrlOverridden() const { return m_baseUrlOverridden; }
    bool localMode() const { return m_localMode; }
    void setLocalMode(bool on);
    QString localModel() const { return m_localModel; }
    QString configPath() const;
    QString configTemplate() const;

    QString brainModel() const { return m_brain; }
    QString plannerModel() const { return m_planner; }
    QString fastModel() const { return m_fast; }
    QString omniModel() const { return m_omni; }
    // role: "brain" | "planner" | "fast" | "omni"
    QString modelForRole(const QString &role) const;

    QString autonomy() const { return m_autonomy; }
    void setAutonomy(const QString &value);
    QString role() const { return m_role; }
    void setRole(const QString &value);

    QStringList enabledTools() const { return m_enabledTools; }
    Q_INVOKABLE bool toolEnabled(const QString &name) const;
    Q_INVOKABLE void setToolEnabled(const QString &name, bool enabled);

    // --- Voice accessors ---
    bool wakeEnabled() const { return m_wakeEnabled; }
    void setWakeEnabled(bool v);
    QString wakePhrase() const { return QStringLiteral("shadow"); }
    bool micMuted() const { return m_micMuted; }
    void setMicMuted(bool v);
    bool speakReplies() const { return m_speakReplies; }
    void setSpeakReplies(bool v);
    QString voice() const { return m_voice; }
    void setVoice(const QString &v);
    double speechRate() const { return m_speechRate; }
    void setSpeechRate(double v);
    QString sttModel() const { return m_sttModel; }
    void setSttModel(const QString &v);
    QString wakeSensitivity() const { return m_wakeSensitivity; }
    void setWakeSensitivity(const QString &v);
    bool followUp() const { return m_followUp; }
    void setFollowUp(bool v);
    QString inputDevice() const { return m_inputDevice; }
    void setInputDevice(const QString &v);
    QString ttsEngine() const { return m_ttsEngine; }
    void setTtsEngine(const QString &v);

    // How a value is read from the conf file: an inline "  # comment" or
    // "  ; comment" is not part of it (earlier templates put comments on
    // value lines, and QSettings keeps them: "muted=false  # …" read as
    // true), surrounding blanks are not either. Public for the tests.
    static QString sanitizeValue(const QString &raw);

    // Re-read everything from disk / environment.
    Q_INVOKABLE void reload();
    // Diagnostic snapshot (never contains the key itself).
    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();

private:
    QString iniPath() const;
    void save();

    QString m_apiKey, m_apiKeySource;
    QString m_baseUrl;
    QString m_anthropicVersion;
    bool m_baseUrlOverridden = false;
    bool m_localMode = false;
    QString m_localBaseUrl;
    QString m_localModel;
    QString m_brain, m_planner, m_fast, m_omni;
    QString m_autonomy;
    QString m_role;
    QStringList m_enabledTools;
    bool m_toolsKeySet = false; // whether [tools] disabled=... was present
    QStringList m_disabledTools;

    // Voice
    bool m_wakeEnabled = true;
    bool m_micMuted = false;
    bool m_speakReplies = true;
    QString m_voice;
    double m_speechRate = 1.0;
    QString m_sttModel;
    QString m_wakeSensitivity;
    bool m_followUp = true;
    QString m_inputDevice;
    QString m_ttsEngine;
};
