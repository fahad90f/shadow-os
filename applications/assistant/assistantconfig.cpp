// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistantconfig.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QStandardPaths>

namespace {

// Default model mapping — a single Claude model serves every role. Claude
// Sonnet 4.6 (1M context) run at high effort with adaptive thinking is a
// strong all-rounder for multitasking, tool-using work, and using one model
// everywhere keeps behaviour and the prompt cache consistent. These are
// written into the conf template as editable defaults rather than
// compiled-in constants used behind the user's back; change any role in the
// conf file to use a different Claude model ID.
const QString kDefaultBrain = QStringLiteral("claude-sonnet-4-6");
const QString kDefaultPlanner = QStringLiteral("claude-sonnet-4-6");
const QString kDefaultFast = QStringLiteral("claude-sonnet-4-6");
const QString kDefaultOmni = QStringLiteral("claude-sonnet-4-6");
const QString kDefaultBaseUrl = QStringLiteral("https://api.anthropic.com/v1");
// Anthropic Messages API version pinned in the "anthropic-version" header.
const QString kDefaultAnthropicVersion = QStringLiteral("2023-06-01");
// Offline default: an OpenAI-compatible server on this machine. ollama's
// default port is 11434; shadow-assistant-local sets it up and pulls a model.
const QString kDefaultLocalBaseUrl = QStringLiteral("http://127.0.0.1:11434/v1");
const QString kDefaultLocalModel   = QStringLiteral("llama3.1:8b");

// Canonical set of tool names the registry exposes. Kept in step with
// ToolRegistry; config only needs the names to compute the enabled list.
const QStringList kAllTools = {
    QStringLiteral("run_shell_command"), QStringLiteral("read_file"),
    QStringLiteral("write_file"),        QStringLiteral("search_files"),
    QStringLiteral("open_application"),  QStringLiteral("get_system_info"),
    // Expanded capabilities for the voice assistant.
    QStringLiteral("open_url"),          QStringLiteral("web_search"),
    QStringLiteral("make_directory"),    QStringLiteral("move_path"),
    QStringLiteral("copy_path"),         QStringLiteral("delete_path"),
    QStringLiteral("write_code"),        QStringLiteral("run_in_terminal"),
    QStringLiteral("type_text"),         QStringLiteral("take_screenshot"),
    // Pentest-specific tools (active only when role == "pentest").
    QStringLiteral("pentest_scan"),      QStringLiteral("web_enum"),
    QStringLiteral("search_exploits"),   QStringLiteral("run_sqlmap"),
    QStringLiteral("crack_hash"),        QStringLiteral("brute_service"),
    QStringLiteral("start_capture"),
};

} // namespace

AssistantConfig::AssistantConfig(QObject *parent)
    : QObject(parent)
{
    reload();
}

QString AssistantConfig::iniPath() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base + QStringLiteral("/shadow/assistant.conf");
}

QString AssistantConfig::configPath() const
{
    return iniPath();
}

void AssistantConfig::reload()
{
    QSettings ini(iniPath(), QSettings::IniFormat);

    // --- API key: environment wins, then the conf file. Never the repo. ---
    m_apiKey.clear();
    m_apiKeySource.clear();
    const QByteArray envKey = qgetenv("SHADOW_ASSISTANT_API_KEY");
    const QByteArray envKeyAnthropic = qgetenv("ANTHROPIC_API_KEY");
    if (!envKey.trimmed().isEmpty()) {
        m_apiKey = QString::fromUtf8(envKey).trimmed();
        m_apiKeySource = QStringLiteral("SHADOW_ASSISTANT_API_KEY environment variable");
    } else if (!envKeyAnthropic.trimmed().isEmpty()) {
        m_apiKey = QString::fromUtf8(envKeyAnthropic).trimmed();
        m_apiKeySource = QStringLiteral("ANTHROPIC_API_KEY environment variable");
    } else {
        const QString k = ini.value(QStringLiteral("api/key")).toString().trimmed();
        if (!k.isEmpty()) {
            m_apiKey = k;
            m_apiKeySource = iniPath();
        }
    }

    // --- Anthropic API version header (cloud mode) ---
    m_anthropicVersion = ini.value(QStringLiteral("api/anthropic_version"), kDefaultAnthropicVersion).toString().trimmed();
    if (m_anthropicVersion.isEmpty())
        m_anthropicVersion = kDefaultAnthropicVersion;

    // --- base URL: dev override, then conf, then the Anthropic default. ---
    const QByteArray envUrl = qgetenv("SHADOW_ASSISTANT_BASE_URL");
    if (!envUrl.trimmed().isEmpty()) {
        m_baseUrl = QString::fromUtf8(envUrl).trimmed();
        m_baseUrlOverridden = true;
    } else {
        m_baseUrl = ini.value(QStringLiteral("api/base_url"), kDefaultBaseUrl).toString().trimmed();
        if (m_baseUrl.isEmpty())
            m_baseUrl = kDefaultBaseUrl;
        m_baseUrlOverridden = false;
    }
    while (m_baseUrl.endsWith(QLatin1Char('/')))
        m_baseUrl.chop(1);

    // --- local (offline) model ---
    m_localMode = ini.value(QStringLiteral("api/local"), false).toBool();
    m_localBaseUrl = ini.value(QStringLiteral("api/local_base_url"), kDefaultLocalBaseUrl).toString().trimmed();
    if (m_localBaseUrl.isEmpty())
        m_localBaseUrl = kDefaultLocalBaseUrl;
    while (m_localBaseUrl.endsWith(QLatin1Char('/')))
        m_localBaseUrl.chop(1);
    m_localModel = ini.value(QStringLiteral("models/local"), kDefaultLocalModel).toString().trimmed();
    if (m_localModel.isEmpty())
        m_localModel = kDefaultLocalModel;

    // --- model mapping ---
    m_brain = ini.value(QStringLiteral("models/brain"), kDefaultBrain).toString().trimmed();
    m_planner = ini.value(QStringLiteral("models/planner"), kDefaultPlanner).toString().trimmed();
    m_fast = ini.value(QStringLiteral("models/fast"), kDefaultFast).toString().trimmed();
    m_omni = ini.value(QStringLiteral("models/omni"), kDefaultOmni).toString().trimmed();
    if (m_brain.isEmpty()) m_brain = kDefaultBrain;
    if (m_planner.isEmpty()) m_planner = kDefaultPlanner;
    if (m_fast.isEmpty()) m_fast = kDefaultFast;
    if (m_omni.isEmpty()) m_omni = kDefaultOmni;

    // --- autonomy ---
    m_autonomy = ini.value(QStringLiteral("behaviour/autonomy"), QStringLiteral("autopilot")).toString().trimmed();
    if (m_autonomy != QLatin1String("ask_every") && m_autonomy != QLatin1String("ask_risky"))
        m_autonomy = QStringLiteral("autopilot");

    // --- role ---
    m_role = ini.value(QStringLiteral("behaviour/role"), QStringLiteral("general")).toString().trimmed();
    if (m_role != QLatin1String("pentest"))
        m_role = QStringLiteral("general");

    // --- disabled tools ---
    m_disabledTools = ini.value(QStringLiteral("tools/disabled")).toStringList();
    m_disabledTools.removeAll(QString());
    m_enabledTools.clear();
    for (const QString &t : kAllTools)
        if (!m_disabledTools.contains(t))
            m_enabledTools << t;

    // --- voice ---
    // Saying "Shadow" works out of the box. The listener transcribes on this
    // computer only and keeps nothing; the toggle in the sidebar and Settings
    // turns it off. Earlier versions wrote wake_word_enabled=false into every
    // saved configuration without the user choosing it, so the stored value
    // counts only once the user has switched it themselves (wake_word_chosen).
    m_wakeEnabled = ini.value(QStringLiteral("voice/wake_word_chosen"), false).toBool()
        ? ini.value(QStringLiteral("voice/wake_word_enabled"), true).toBool()
        : true;
    m_wakePhrase = ini.value(QStringLiteral("voice/wake_phrase"), QStringLiteral("shadow")).toString().trimmed().toLower();
    if (m_wakePhrase.isEmpty())
        m_wakePhrase = QStringLiteral("shadow");
    bool okT = false;
    m_wakeThreshold = ini.value(QStringLiteral("voice/wake_threshold"), QStringLiteral("1e-20")).toString().toDouble(&okT);
    if (!okT || m_wakeThreshold <= 0.0)
        m_wakeThreshold = 1e-20;
    m_micMuted = ini.value(QStringLiteral("voice/muted"), false).toBool();
    m_speakReplies = ini.value(QStringLiteral("voice/speak_replies"), true).toBool();
    m_ttsEngine = ini.value(QStringLiteral("voice/tts_engine"), QStringLiteral("auto")).toString().trimmed().toLower();
    if (!QStringList({QStringLiteral("auto"), QStringLiteral("piper"), QStringLiteral("speechd"), QStringLiteral("espeak")}).contains(m_ttsEngine))
        m_ttsEngine = QStringLiteral("auto");
    m_ttsVoice = ini.value(QStringLiteral("voice/tts_voice")).toString().trimmed();
    m_piperBin = ini.value(QStringLiteral("voice/piper_bin"), QStringLiteral("piper")).toString().trimmed();
    if (m_piperBin.isEmpty())
        m_piperBin = QStringLiteral("piper");
    m_piperModel = ini.value(QStringLiteral("voice/piper_model")).toString().trimmed();
    m_sttMode = ini.value(QStringLiteral("voice/stt_mode"), QStringLiteral("auto")).toString().trimmed().toLower();
    if (!QStringList({QStringLiteral("auto"), QStringLiteral("nvidia"), QStringLiteral("pocketsphinx")}).contains(m_sttMode))
        m_sttMode = QStringLiteral("auto");
    m_captureTool = ini.value(QStringLiteral("voice/capture_tool"), QStringLiteral("auto")).toString().trimmed().toLower();
    m_inputDevice = ini.value(QStringLiteral("voice/input_device")).toString().trimmed();
    m_maxUtteranceSecs = ini.value(QStringLiteral("voice/max_utterance_secs"), 12).toInt();
    if (m_maxUtteranceSecs < 2 || m_maxUtteranceSecs > 60)
        m_maxUtteranceSecs = 12;
    m_pocketsphinxModel = ini.value(QStringLiteral("voice/pocketsphinx_model")).toString().trimmed();

    Q_EMIT changed();
}

void AssistantConfig::save()
{
    QSettings ini(iniPath(), QSettings::IniFormat);
    ini.setValue(QStringLiteral("behaviour/autonomy"), m_autonomy);
    ini.setValue(QStringLiteral("behaviour/role"), m_role);
    ini.setValue(QStringLiteral("tools/disabled"), m_disabledTools);
    // Voice toggles/engine the UI can change (paths/voices stay file-edited).
    ini.setValue(QStringLiteral("voice/wake_word_enabled"), m_wakeEnabled);
    ini.setValue(QStringLiteral("voice/muted"), m_micMuted);
    ini.setValue(QStringLiteral("voice/speak_replies"), m_speakReplies);
    ini.setValue(QStringLiteral("voice/tts_engine"), m_ttsEngine);
    ini.setValue(QStringLiteral("voice/stt_mode"), m_sttMode);
    // We deliberately never write api/key.
    ini.sync();
}

QString AssistantConfig::modelForRole(const QString &role) const
{
    // Offline: one local model serves every role.
    if (m_localMode)
        return m_localModel;
    if (role == QLatin1String("planner")) return m_planner;
    if (role == QLatin1String("fast")) return m_fast;
    if (role == QLatin1String("omni")) return m_omni;
    return m_brain;
}

void AssistantConfig::setLocalMode(bool on)
{
    if (on == m_localMode)
        return;
    m_localMode = on;
    {
        QSettings ini(iniPath(), QSettings::IniFormat);
        ini.setValue(QStringLiteral("api/local"), on);
    }
    Q_EMIT changed();
}

void AssistantConfig::setAutonomy(const QString &value)
{
    const QString v = (value == QLatin1String("ask_every") || value == QLatin1String("ask_risky"))
                          ? value : QStringLiteral("autopilot");
    if (v == m_autonomy)
        return;
    m_autonomy = v;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setRole(const QString &value)
{
    const QString v = (value == QLatin1String("pentest")) ? value : QStringLiteral("general");
    if (v == m_role)
        return;
    m_role = v;
    save();
    Q_EMIT changed();
}

bool AssistantConfig::toolEnabled(const QString &name) const
{
    return !m_disabledTools.contains(name);
}

void AssistantConfig::setToolEnabled(const QString &name, bool enabled)
{
    if (!kAllTools.contains(name))
        return;
    const bool currently = !m_disabledTools.contains(name);
    if (currently == enabled)
        return;
    if (enabled)
        m_disabledTools.removeAll(name);
    else
        m_disabledTools << name;
    m_enabledTools.clear();
    for (const QString &t : kAllTools)
        if (!m_disabledTools.contains(t))
            m_enabledTools << t;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setWakeEnabled(bool v)
{
    if (v == m_wakeEnabled) return;
    m_wakeEnabled = v;
    QSettings(iniPath(), QSettings::IniFormat).setValue(QStringLiteral("voice/wake_word_chosen"), true);
    save();
    Q_EMIT changed();
}

void AssistantConfig::setMicMuted(bool v)
{
    if (v == m_micMuted) return;
    m_micMuted = v;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setSpeakReplies(bool v)
{
    if (v == m_speakReplies) return;
    m_speakReplies = v;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setTtsEngine(const QString &v)
{
    const QString n = v.trimmed().toLower();
    if (!QStringList({QStringLiteral("auto"), QStringLiteral("piper"), QStringLiteral("speechd"), QStringLiteral("espeak")}).contains(n))
        return;
    if (n == m_ttsEngine) return;
    m_ttsEngine = n;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setSttMode(const QString &v)
{
    const QString n = v.trimmed().toLower();
    if (!QStringList({QStringLiteral("auto"), QStringLiteral("nvidia"), QStringLiteral("pocketsphinx")}).contains(n))
        return;
    if (n == m_sttMode) return;
    m_sttMode = n;
    save();
    Q_EMIT changed();
}

QString AssistantConfig::configTemplate() const
{
    // Shown verbatim in the setup state so the user can create the file.
    return QStringLiteral(
        "# ~/.config/shadow/assistant.conf  —  create with mode 0600 (chmod 600)\n"
        "# SHADOW Assistant reads your Claude (Anthropic) API key from here (or\n"
        "# from the SHADOW_ASSISTANT_API_KEY or ANTHROPIC_API_KEY environment\n"
        "# variable). The key is never stored in the application or the repository.\n"
        "# Get a key at https://console.anthropic.com/ (keys start with sk-ant-).\n"
        "\n"
        "[api]\n"
        "key=sk-ant-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n"
        "# base_url=https://api.anthropic.com/v1   # usually leave unset\n"
        "# anthropic_version=2023-06-01            # Messages API version header\n"
        "# Offline: run a model on this computer instead of the cloud (no key\n"
        "# needed). Easiest setup:  shadow-assistant-local setup\n"
        "# local=true\n"
        "# local_base_url=http://127.0.0.1:11434/v1\n"
        "\n"
        "[models]\n"
        "# One Claude model serves every role by default (claude-sonnet-4-6,\n"
        "# run at high effort with adaptive thinking). Change any role to a\n"
        "# different Claude model ID if you prefer (e.g. claude-opus-5-5 for the\n"
        "# brain, claude-haiku-5-5 for fast low-cost replies).\n"
        "brain=") + kDefaultBrain + QStringLiteral("\n"
        "planner=") + kDefaultPlanner + QStringLiteral("\n"
        "fast=") + kDefaultFast + QStringLiteral("\n"
        "omni=") + kDefaultOmni + QStringLiteral("\n"
        "# local=llama3.1:8b   # model used when [api] local=true\n"
        "\n"
        "[behaviour]\n"
        "# autonomy = autopilot (run everything without asking; the default) |\n"
        "#            ask_risky (ask before changing the system) | ask_every (ask every time)\n"
        "autonomy=autopilot\n"
        "\n"
        "[tools]\n"
        "# disabled = comma-separated tool names to turn off, e.g. run_shell_command\n"
        "disabled=\n"
        "\n"
        "[voice]\n"
        "# Hands-free voice assistant \"Shadow\". Always-listening is OPT-IN.\n"
        "wake_word_enabled=false       # listen continuously for the wake word\n"
        "wake_phrase=shadow            # keyphrase that wakes the assistant\n"
        "wake_threshold=1e-20          # pocketsphinx kws threshold (smaller = stricter)\n"
        "muted=false                   # privacy mic mute (nothing is captured when true)\n"
        "speak_replies=true            # speak answers aloud\n"
        "tts_engine=auto               # auto | piper | speechd | espeak\n"
        "tts_voice=                    # speechd/espeak voice name (e.g. en-us)\n"
        "piper_bin=piper               # piper executable (neural TTS, if installed)\n"
        "piper_model=                  # path to a piper .onnx voice model (required for piper)\n"
        "stt_mode=auto                 # voice is transcribed offline with pocketsphinx (Claude has no speech-to-text)\n"
        "capture_tool=auto             # auto | parecord | pw-record | arecord\n"
        "input_device=                 # optional capture device (PulseAudio/PipeWire source)\n"
        "max_utterance_secs=12         # stop capturing a command after this long\n"
        "pocketsphinx_model=           # optional path to an acoustic model directory\n");
}

QJsonObject AssistantConfig::toJson() const
{
    return QJsonObject{
        {QStringLiteral("apiKeyConfigured"), apiKeyConfigured()},
        {QStringLiteral("apiKeySource"), m_apiKeySource},
        {QStringLiteral("baseUrl"), m_baseUrl},
        {QStringLiteral("baseUrlOverridden"), m_baseUrlOverridden},
        {QStringLiteral("configPath"), iniPath()},
        {QStringLiteral("models"), QJsonObject{
            {QStringLiteral("brain"), m_brain},
            {QStringLiteral("planner"), m_planner},
            {QStringLiteral("fast"), m_fast},
            {QStringLiteral("omni"), m_omni},
        }},
        {QStringLiteral("autonomy"), m_autonomy},
        {QStringLiteral("enabledTools"), QJsonArray::fromStringList(m_enabledTools)},
        {QStringLiteral("disabledTools"), QJsonArray::fromStringList(m_disabledTools)},
        {QStringLiteral("voice"), QJsonObject{
            {QStringLiteral("wakeEnabled"), m_wakeEnabled},
            {QStringLiteral("wakePhrase"), m_wakePhrase},
            {QStringLiteral("wakeThreshold"), m_wakeThreshold},
            {QStringLiteral("micMuted"), m_micMuted},
            {QStringLiteral("speakReplies"), m_speakReplies},
            {QStringLiteral("ttsEngine"), m_ttsEngine},
            {QStringLiteral("ttsVoice"), m_ttsVoice},
            {QStringLiteral("piperBin"), m_piperBin},
            {QStringLiteral("piperModel"), m_piperModel},
            {QStringLiteral("sttMode"), m_sttMode},
            {QStringLiteral("captureTool"), m_captureTool},
            {QStringLiteral("inputDevice"), m_inputDevice},
            {QStringLiteral("maxUtteranceSecs"), m_maxUtteranceSecs},
            {QStringLiteral("pocketsphinxModel"), m_pocketsphinxModel},
        }},
    };
}
