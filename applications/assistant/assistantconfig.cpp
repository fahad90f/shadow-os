// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistantconfig.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
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

// Voice defaults (spec voice v2 §2.3).
const QString kDefaultVoice = QStringLiteral("af_heart");
const QStringList kSttModels = {QStringLiteral("auto"), QStringLiteral("moonshine"), QStringLiteral("whisper")};
const QStringList kWakeSensitivities = {QStringLiteral("low"), QStringLiteral("normal"), QStringLiteral("high")};
const QStringList kTtsEngines = {QStringLiteral("auto"), QStringLiteral("neural"), QStringLiteral("espeak")};
constexpr double kMinSpeechRate = 0.7;
constexpr double kMaxSpeechRate = 1.4;

// QSettings hands back a QStringList when an INI value contains a comma
// ("brain=x  # chat, tools, vision" from an old template); the value as
// written in the file is that list joined again.
QString rawString(const QVariant &v)
{
    if (v.metaType().id() == QMetaType::QStringList)
        return v.toStringList().join(QStringLiteral(", "));
    return v.toString();
}

// One sanitized value of the conf file; 'fallback' when the key is absent.
QString readString(const QSettings &ini, const QString &key, const QString &fallback = QString())
{
    if (!ini.contains(key))
        return fallback;
    return AssistantConfig::sanitizeValue(rawString(ini.value(key)));
}

bool readBool(const QSettings &ini, const QString &key, bool fallback)
{
    const QString v = readString(ini, key).toLower();
    if (v == QLatin1String("true") || v == QLatin1String("1") || v == QLatin1String("yes") || v == QLatin1String("on"))
        return true;
    if (v == QLatin1String("false") || v == QLatin1String("0") || v == QLatin1String("no") || v == QLatin1String("off"))
        return false;
    return fallback;
}

// A comma-separated list value ([tools] disabled=a, b).
QStringList readList(const QSettings &ini, const QString &key)
{
    QStringList out;
    const QStringList parts = readString(ini, key).split(u',');
    for (const QString &p : parts) {
        const QString t = p.trimmed();
        if (!t.isEmpty() && !out.contains(t))
            out << t;
    }
    return out;
}

QString oneOf(const QString &value, const QStringList &allowed, const QString &fallback)
{
    const QString v = value.trimmed().toLower();
    return allowed.contains(v) ? v : fallback;
}

// A Kokoro voice id such as "af_heart" or "bf_emma".
bool isVoiceId(const QString &v)
{
    static const QRegularExpression re(QStringLiteral("^[a-z]{1,3}_[a-z0-9]{1,32}$"));
    return re.match(v).hasMatch();
}

double clampRate(double v)
{
    if (!(v > 0.0)) // also NaN
        return 1.0;
    return qBound(kMinSpeechRate, v, kMaxSpeechRate);
}

} // namespace

QString AssistantConfig::sanitizeValue(const QString &raw)
{
    // A comment starts at '#' or ';' at the start of the value or after a
    // blank. No value this file holds (model ids, tool names, voice ids,
    // device names, URLs, keys) contains " #" or " ;".
    static const QRegularExpression inlineComment(QStringLiteral("(?:^|\\s)[#;].*$"),
                                                  QRegularExpression::DotMatchesEverythingOption);
    QString v = raw;
    v.remove(inlineComment);
    return v.trimmed();
}

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
        const QString k = readString(ini, QStringLiteral("api/key"));
        if (!k.isEmpty()) {
            m_apiKey = k;
            m_apiKeySource = iniPath();
        }
    }

    // --- Anthropic API version header (cloud mode) ---
    m_anthropicVersion = readString(ini, QStringLiteral("api/anthropic_version"), kDefaultAnthropicVersion);
    if (m_anthropicVersion.isEmpty())
        m_anthropicVersion = kDefaultAnthropicVersion;

    // --- base URL: dev override, then conf, then the Anthropic default. ---
    const QByteArray envUrl = qgetenv("SHADOW_ASSISTANT_BASE_URL");
    if (!envUrl.trimmed().isEmpty()) {
        m_baseUrl = QString::fromUtf8(envUrl).trimmed();
        m_baseUrlOverridden = true;
    } else {
        m_baseUrl = readString(ini, QStringLiteral("api/base_url"), kDefaultBaseUrl);
        if (m_baseUrl.isEmpty())
            m_baseUrl = kDefaultBaseUrl;
        m_baseUrlOverridden = false;
    }
    while (m_baseUrl.endsWith(QLatin1Char('/')))
        m_baseUrl.chop(1);

    // --- local (offline) model ---
    m_localMode = readBool(ini, QStringLiteral("api/local"), false);
    m_localBaseUrl = readString(ini, QStringLiteral("api/local_base_url"), kDefaultLocalBaseUrl);
    if (m_localBaseUrl.isEmpty())
        m_localBaseUrl = kDefaultLocalBaseUrl;
    while (m_localBaseUrl.endsWith(QLatin1Char('/')))
        m_localBaseUrl.chop(1);
    m_localModel = readString(ini, QStringLiteral("models/local"), kDefaultLocalModel);
    if (m_localModel.isEmpty())
        m_localModel = kDefaultLocalModel;

    // --- model mapping ---
    m_brain = readString(ini, QStringLiteral("models/brain"), kDefaultBrain);
    m_planner = readString(ini, QStringLiteral("models/planner"), kDefaultPlanner);
    m_fast = readString(ini, QStringLiteral("models/fast"), kDefaultFast);
    m_omni = readString(ini, QStringLiteral("models/omni"), kDefaultOmni);
    if (m_brain.isEmpty()) m_brain = kDefaultBrain;
    if (m_planner.isEmpty()) m_planner = kDefaultPlanner;
    if (m_fast.isEmpty()) m_fast = kDefaultFast;
    if (m_omni.isEmpty()) m_omni = kDefaultOmni;

    // --- autonomy ---
    m_autonomy = readString(ini, QStringLiteral("behaviour/autonomy"), QStringLiteral("autopilot"));
    if (m_autonomy != QLatin1String("ask_every") && m_autonomy != QLatin1String("ask_risky"))
        m_autonomy = QStringLiteral("autopilot");

    // --- role ---
    m_role = readString(ini, QStringLiteral("behaviour/role"), QStringLiteral("general"));
    if (m_role != QLatin1String("pentest"))
        m_role = QStringLiteral("general");

    // --- disabled tools ---
    m_disabledTools = readList(ini, QStringLiteral("tools/disabled"));
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
    // Every value is sanitized (see sanitizeValue()): a conf written from the
    // old template ("muted=false   # privacy …") reads correctly again.
    // Keys of the old pocketsphinx/piper voice (wake_phrase, wake_threshold,
    // tts_voice, piper_bin, piper_model, stt_mode, capture_tool,
    // max_utterance_secs, pocketsphinx_model) are ignored if present.
    m_wakeEnabled = readBool(ini, QStringLiteral("voice/wake_word_chosen"), false)
        ? readBool(ini, QStringLiteral("voice/wake_word_enabled"), true)
        : true;
    m_micMuted = readBool(ini, QStringLiteral("voice/muted"), false);
    m_speakReplies = readBool(ini, QStringLiteral("voice/speak_replies"), true);
    m_voice = readString(ini, QStringLiteral("voice/voice"), kDefaultVoice).toLower();
    if (!isVoiceId(m_voice))
        m_voice = kDefaultVoice;
    bool rateOk = false;
    const double rate = readString(ini, QStringLiteral("voice/speech_rate"), QStringLiteral("1.0")).toDouble(&rateOk);
    m_speechRate = rateOk ? clampRate(rate) : 1.0;
    m_sttModel = oneOf(readString(ini, QStringLiteral("voice/stt_model")), kSttModels, QStringLiteral("auto"));
    m_wakeSensitivity = oneOf(readString(ini, QStringLiteral("voice/wake_sensitivity")), kWakeSensitivities,
                              QStringLiteral("normal"));
    m_followUp = readBool(ini, QStringLiteral("voice/follow_up"), true);
    m_inputDevice = readString(ini, QStringLiteral("voice/input_device"));
    // "piper" / "speechd" from the old voice → auto.
    m_ttsEngine = oneOf(readString(ini, QStringLiteral("voice/tts_engine")), kTtsEngines, QStringLiteral("auto"));

    Q_EMIT changed();
}

void AssistantConfig::save()
{
    QSettings ini(iniPath(), QSettings::IniFormat);
    ini.setValue(QStringLiteral("behaviour/autonomy"), m_autonomy);
    ini.setValue(QStringLiteral("behaviour/role"), m_role);
    ini.setValue(QStringLiteral("tools/disabled"), m_disabledTools);
    // Every voice setting the UI can change, clean (this also replaces values
    // an old template left with an inline comment).
    ini.setValue(QStringLiteral("voice/wake_word_enabled"), m_wakeEnabled);
    ini.setValue(QStringLiteral("voice/muted"), m_micMuted);
    ini.setValue(QStringLiteral("voice/speak_replies"), m_speakReplies);
    ini.setValue(QStringLiteral("voice/voice"), m_voice);
    ini.setValue(QStringLiteral("voice/speech_rate"), m_speechRate);
    ini.setValue(QStringLiteral("voice/stt_model"), m_sttModel);
    ini.setValue(QStringLiteral("voice/wake_sensitivity"), m_wakeSensitivity);
    ini.setValue(QStringLiteral("voice/follow_up"), m_followUp);
    ini.setValue(QStringLiteral("voice/input_device"), m_inputDevice);
    ini.setValue(QStringLiteral("voice/tts_engine"), m_ttsEngine);
    // Heal files written from older templates. Their "# …" comment lines
    // are not comments to QSettings ('#' is an ordinary character in its INI
    // format; only ';' starts a comment): a "# base_url=…" line was read as a
    // key named "# base_url" and is written back as "%23%20base_url=…".
    // Drop those, and the keys of the old pocketsphinx/piper voice.
    for (const QString &group : ini.childGroups()) {
        ini.beginGroup(group);
        for (const QString &key : ini.childKeys())
            if (key.startsWith(u'#'))
                ini.remove(key);
        ini.endGroup();
    }
    for (const char *dead : {"wake_phrase", "wake_threshold", "tts_voice", "piper_bin", "piper_model", "stt_mode",
                             "capture_tool", "max_utterance_secs", "pocketsphinx_model"})
        ini.remove(QStringLiteral("voice/") + QLatin1String(dead));
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

void AssistantConfig::setVoice(const QString &v)
{
    const QString n = v.trimmed().toLower();
    if (!isVoiceId(n) || n == m_voice)
        return;
    m_voice = n;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setSpeechRate(double v)
{
    const double n = clampRate(v);
    if (qFuzzyCompare(n, m_speechRate))
        return;
    m_speechRate = n;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setSttModel(const QString &v)
{
    const QString n = oneOf(v, kSttModels, QString());
    if (n.isEmpty() || n == m_sttModel)
        return;
    m_sttModel = n;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setWakeSensitivity(const QString &v)
{
    const QString n = oneOf(v, kWakeSensitivities, QString());
    if (n.isEmpty() || n == m_wakeSensitivity)
        return;
    m_wakeSensitivity = n;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setFollowUp(bool v)
{
    if (v == m_followUp)
        return;
    m_followUp = v;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setInputDevice(const QString &v)
{
    const QString n = sanitizeValue(v);
    if (n == m_inputDevice)
        return;
    m_inputDevice = n;
    save();
    Q_EMIT changed();
}

void AssistantConfig::setTtsEngine(const QString &v)
{
    const QString n = oneOf(v, kTtsEngines, QString());
    if (n.isEmpty() || n == m_ttsEngine)
        return;
    m_ttsEngine = n;
    save();
    Q_EMIT changed();
}

QString AssistantConfig::configTemplate() const
{
    // Shown verbatim in the setup state so the user can create the file.
    // Comments start with ';' and stay on lines of their own: QSettings
    // treats only ';' as a comment ('#' lines become keys) and would read a
    // comment after a value as part of the value.
    return QStringLiteral(
        "; ~/.config/shadow/assistant.conf  —  create with mode 0600 (chmod 600)\n"
        "; SHADOW Assistant reads your Claude (Anthropic) API key from here (or\n"
        "; from the SHADOW_ASSISTANT_API_KEY or ANTHROPIC_API_KEY environment\n"
        "; variable). The key is never stored in the application or the repository.\n"
        "; Get a key at https://console.anthropic.com/ (keys start with sk-ant-).\n"
        "; Comments start with ';' on a line of their own, never after a value.\n"
        "\n"
        "[api]\n"
        "key=sk-ant-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n"
        "; Endpoint (usually leave unset):\n"
        "; base_url=https://api.anthropic.com/v1\n"
        "; Messages API version header:\n"
        "; anthropic_version=2023-06-01\n"
        "; Offline: run a model on this computer instead of the cloud (no key\n"
        "; needed). Easiest setup:  shadow-assistant-local setup\n"
        "; local=true\n"
        "; local_base_url=http://127.0.0.1:11434/v1\n"
        "\n"
        "[models]\n"
        "; One Claude model serves every role by default (claude-sonnet-4-6,\n"
        "; run at high effort with adaptive thinking). Change any role to a\n"
        "; different Claude model ID if you prefer (e.g. claude-opus-5-5 for the\n"
        "; brain, claude-haiku-5-5 for fast low-cost replies).\n"
        "brain=") + kDefaultBrain + QStringLiteral("\n"
        "planner=") + kDefaultPlanner + QStringLiteral("\n"
        "fast=") + kDefaultFast + QStringLiteral("\n"
        "omni=") + kDefaultOmni + QStringLiteral("\n"
        "; Model used when [api] local=true:\n"
        "; local=llama3.1:8b\n"
        "\n"
        "[behaviour]\n"
        "; autopilot: run everything without asking (the default)\n"
        "; ask_risky: ask before changing the system   ask_every: ask every time\n"
        "autonomy=autopilot\n"
        "\n"
        "[tools]\n"
        "; Comma-separated tool names to turn off, e.g. run_shell_command\n"
        "disabled=\n"
        "\n"
        "[voice]\n"
        "; Hands-free voice: say \"Shadow\", speak, hear the reply. Speech is\n"
        "; recognised on this computer. Install the voice with: shadow-voice-setup\n"
        "; Listen for the wake word \"Shadow\" (true or false):\n"
        "wake_word_enabled=true\n"
        "; Privacy mute: the microphone is never opened while true:\n"
        "muted=false\n"
        "; Speak answers to spoken requests aloud:\n"
        "speak_replies=true\n"
        "; Voice: af_heart, af_bella, af_sarah, bf_emma, af_nicole or af_sky:\n"
        "voice=af_heart\n"
        "; Speaking rate, 0.7 (slower) to 1.4 (faster):\n"
        "speech_rate=1.0\n"
        "; Speech recognition: auto, moonshine or whisper:\n"
        "stt_model=auto\n"
        "; Wake word sensitivity: low, normal or high:\n"
        "wake_sensitivity=normal\n"
        "; After a spoken reply, listen 5 s for a follow-up without the wake word:\n"
        "follow_up=true\n"
        "; Microphone (PipeWire/PulseAudio source name); empty = the default input:\n"
        "input_device=\n"
        "; Speech output: auto, neural or espeak:\n"
        "tts_engine=auto\n");
}

QJsonObject AssistantConfig::toJson() const
{
    return QJsonObject{
        {QStringLiteral("apiKeyConfigured"), apiKeyConfigured()},
        {QStringLiteral("apiKeySource"), m_apiKeySource},
        {QStringLiteral("baseUrl"), m_baseUrl},
        {QStringLiteral("baseUrlOverridden"), m_baseUrlOverridden},
        {QStringLiteral("configPath"), iniPath()},
        // What the setup page offers to paste (a placeholder key, never the real one).
        {QStringLiteral("configTemplate"), configTemplate()},
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
            {QStringLiteral("wakePhrase"), wakePhrase()},
            {QStringLiteral("micMuted"), m_micMuted},
            {QStringLiteral("speakReplies"), m_speakReplies},
            {QStringLiteral("voice"), m_voice},
            {QStringLiteral("speechRate"), m_speechRate},
            {QStringLiteral("sttModel"), m_sttModel},
            {QStringLiteral("wakeSensitivity"), m_wakeSensitivity},
            {QStringLiteral("followUp"), m_followUp},
            {QStringLiteral("inputDevice"), m_inputDevice},
            {QStringLiteral("ttsEngine"), m_ttsEngine},
        }},
    };
}
