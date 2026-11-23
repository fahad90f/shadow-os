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
// The API key is NEVER stored in the repository. It is read at run time from,
// in order of precedence:
//   1. the environment variable  SHADOW_ASSISTANT_API_KEY
//   2. the environment variable  NVIDIA_API_KEY
//   3. the key saved from Settings:  ~/.config/shadow/assistant.key  (0600)
//   4. ~/.config/shadow/assistant.conf   [api] key=...   (optional)
//   5. the environment variable  ANTHROPIC_API_KEY
// The key is write-only for the UI: saveApiKey() stores it, nothing hands it
// back to QML (only whether one is set, where it comes from and its last
// four characters).
//
// The service is NVIDIA's hosted models (OpenAI-compatible API at
// integrate.api.nvidia.com) unless the key is an Anthropic one ("sk-ant-…")
// or [api] provider says otherwise; [api] local=true runs a model on this
// computer instead. Everything else (the model team, autonomy, which tools
// are enabled, the voice) lives in the same INI file and may be changed from
// the Settings page. The base URL can be overridden for testing with the
// environment variable SHADOW_ASSISTANT_BASE_URL.
class AssistantConfig : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool apiKeyConfigured READ apiKeyConfigured NOTIFY changed)
    Q_PROPERTY(QString apiKeySource READ apiKeySource NOTIFY changed)
    // The key in effect ends with these characters ("" when none is set).
    Q_PROPERTY(QString apiKeyHint READ apiKeyHint NOTIFY changed)
    // A key saved from Settings exists (it can be removed there).
    Q_PROPERTY(bool apiKeySaved READ apiKeySaved NOTIFY changed)
    // "nvidia" | "anthropic": the cloud service used when not in local mode.
    Q_PROPERTY(QString provider READ provider NOTIFY changed)
    Q_PROPERTY(QString providerName READ providerName NOTIFY changed)
    // Where a key for the service is created.
    Q_PROPERTY(QString keyHelpUrl READ keyHelpUrl NOTIFY changed)
    // true: every model of the request's group works on it at the same time
    // and a lead model combines their work. false: one model answers.
    Q_PROPERTY(bool teamMode READ teamMode WRITE setTeamMode NOTIFY changed)
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
    // Skills on at all; a skill is used only when the request matches it.
    Q_PROPERTY(bool skillsEnabled READ skillsEnabled WRITE setSkillsEnabled NOTIFY changed)

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
    QString apiKeyHint() const;
    bool apiKeySaved() const { return m_apiKeySaved; }
    // Store a key entered in Settings (owner-only file next to the conf).
    // Returns "" on success, otherwise why it was not saved.
    Q_INVOKABLE QString saveApiKey(const QString &key);
    Q_INVOKABLE void removeSavedApiKey();
    QString keyFilePath() const;
    QString provider() const { return m_provider; }
    QString providerName() const;
    QString keyHelpUrl() const;
    bool nvidia() const { return !m_localMode && m_provider == QLatin1String("nvidia"); }
    bool anthropic() const { return !m_localMode && m_provider == QLatin1String("anthropic"); }

    // --- the model team (NVIDIA) ---
    bool teamMode() const { return m_teamMode; }
    void setTeamMode(bool on);
    // The models of a group, in order: "coding" | "agents" | "chat" | "vision".
    QStringList teamGroup(const QString &kind) const;
    // The most models that work on one request, the lead included; 0 = every
    // model of the group ([team] max_models).
    int teamMaxModels() const { return m_teamMaxModels; }
    // The models preferred as lead for a kind, best first.
    QStringList teamLeads(const QString &kind) const;
    // How long the lead waits for the other models' work, in milliseconds.
    int advisorWaitMs(const QString &kind, bool voice, bool thorough) const;
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
    bool skillsEnabled() const { return m_skillsEnabled; }
    void setSkillsEnabled(bool on);
    bool skillEnabled(const QString &name) const;
    void setSkillEnabled(const QString &name, bool on);
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
    bool m_apiKeySaved = false;
    QString m_provider;
    bool m_teamMode = true;
    QStringList m_groupCoding, m_groupAgents, m_groupChat, m_groupVision;
    QStringList m_leadCoding, m_leadAgents, m_leadChat;
    int m_advisorSeconds = 0;
    int m_teamMaxModels = 0;
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
    bool m_skillsEnabled = true;
    QStringList m_disabledSkills;
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
