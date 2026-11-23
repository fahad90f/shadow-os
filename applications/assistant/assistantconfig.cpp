// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistantconfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

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

// NVIDIA's hosted models: one OpenAI-compatible endpoint, one key ("nvapi-…")
// for every model. The groups below are the models that work on a request
// together; each list is in the order the models are shown, and the "lead"
// lists name the models preferred to combine the others' work and run the
// tools, best first. Models the service does not list (GET /models) are
// skipped at run time, so a retired model never breaks the assistant. All of
// it can be changed in the [team] section of the conf file.
const QString kNvidiaBaseUrl = QStringLiteral("https://integrate.api.nvidia.com/v1");
const QStringList kTeamCoding = {
    QStringLiteral("moonshotai/kimi-k3.1-pro"),
    QStringLiteral("minimaxai/minimax-m3.1-flash"),
    QStringLiteral("nvidia/nemotron-3.5-super-vl-120b-a12b"),
    QStringLiteral("poolside/laguna-xs-2.1"),
    QStringLiteral("nvidia/nemotron-3-ultra-550b-a55b"),
    QStringLiteral("google/gemma-4-31b-it"),
};
const QStringList kTeamAgents = {
    QStringLiteral("moonshotai/kimi-k3.1-pro"),
    QStringLiteral("nvidia/nemotron-3.5-bolt-e4b"),
    QStringLiteral("minimaxai/minimax-m3.1-flash"),
    QStringLiteral("nvidia/nemotron-3.5-super-vl-120b-a12b"),
    QStringLiteral("nvidia/nemotron-3.5-lightning-30b-a3b"),
    QStringLiteral("poolside/laguna-xs-2.1"),
    QStringLiteral("nvidia/nemotron-3-ultra-550b-a55b"),
    QStringLiteral("google/gemma-4-31b-it"),
};
const QStringList kTeamChat = {
    QStringLiteral("mistralai/mistral-large-4-1t-a52b"),
    QStringLiteral("nvidia/nemotron-3.5-bolt-e4b"),
    QStringLiteral("nvidia/nemotron-3.5-super-vl-120b-a12b"),
    QStringLiteral("stepfun-ai/step-5-preview"),
    QStringLiteral("z-ai/glm-5.3-flash"),
    QStringLiteral("meta/muse-glimmer-30b"),
    QStringLiteral("poolside/laguna-xs-2.1"),
    QStringLiteral("nvidia/nemotron-3-ultra-550b-a55b"),
};
// Models that accept images (used by the look_at_screen / look_at_image tools).
const QStringList kTeamVision = {
    QStringLiteral("nvidia/nemotron-3.5-super-vl-120b-a12b"),
    QStringLiteral("moonshotai/kimi-k3.1-pro"),
    QStringLiteral("stepfun-ai/step-5-preview"),
    QStringLiteral("z-ai/glm-5.3-flash"),
    QStringLiteral("mistralai/mistral-large-4-1t-a52b"),
    QStringLiteral("meta/muse-glimmer-30b"),
    QStringLiteral("nvidia/nemotron-3-nano-omni-30b-a3b-reasoning"),
    QStringLiteral("nvidia/cosmos3-nano-reasoner"),
    QStringLiteral("nvidia/cosmos-reason2-8b"),
    QStringLiteral("google/paligemma"),
    QStringLiteral("nvidia/ising-calibration-1.5-31b"),
};
const QStringList kLeadCoding = {
    QStringLiteral("moonshotai/kimi-k3.1-pro"),
    QStringLiteral("nvidia/nemotron-3-ultra-550b-a55b"),
    QStringLiteral("minimaxai/minimax-m3.1-flash"),
    QStringLiteral("poolside/laguna-xs-2.1"),
    QStringLiteral("nvidia/nemotron-3.5-super-vl-120b-a12b"),
};
const QStringList kLeadAgents = {
    QStringLiteral("nvidia/nemotron-3-ultra-550b-a55b"),
    QStringLiteral("moonshotai/kimi-k3.1-pro"),
    QStringLiteral("minimaxai/minimax-m3.1-flash"),
    QStringLiteral("nvidia/nemotron-3.5-super-vl-120b-a12b"),
    QStringLiteral("poolside/laguna-xs-2.1"),
};
const QStringList kLeadChat = {
    QStringLiteral("mistralai/mistral-large-4-1t-a52b"),
    QStringLiteral("nvidia/nemotron-3-ultra-550b-a55b"),
    QStringLiteral("z-ai/glm-5.3-flash"),
    QStringLiteral("stepfun-ai/step-5-preview"),
    QStringLiteral("nvidia/nemotron-3.5-super-vl-120b-a12b"),
};

// A key somebody copied from a template without replacing it.
bool isPlaceholderKey(const QString &key)
{
    return key.contains(QLatin1String("XXXXXXXX"));
}

// The key saved from Settings: the first line of an owner-only file.
QString readKeyFile(const QString &path)
{
    QFile f(path);
    if (!QFileInfo(path).isFile() || !f.open(QIODevice::ReadOnly))
        return {};
    const QString key = QString::fromUtf8(f.readLine(1024)).trimmed();
    f.close();
    // Anyone else being able to read it is a mistake worth correcting quietly.
    const QFile::Permissions others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
                                    | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    if (f.permissions() & others)
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return key;
}

// A model list from the conf file ("a, b"), or the default when unset.
QStringList groupOr(const QSettings &ini, const QString &key, const QStringList &fallback);
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
    // Seeing: the vision models look at a picture or at the screen.
    QStringLiteral("look_at_image"),
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

namespace {
QStringList groupOr(const QSettings &ini, const QString &key, const QStringList &fallback)
{
    const QStringList configured = readList(ini, key);
    return configured.isEmpty() ? fallback : configured;
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

// ---- the API key saved from Settings ---------------------------------------

QString AssistantConfig::keyFilePath() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base + QStringLiteral("/shadow/assistant.key");
}

QString AssistantConfig::apiKeyHint() const
{
    // Enough to tell two keys apart, not enough to be of use to anyone.
    return m_apiKey.size() >= 12 ? m_apiKey.right(4) : QString();
}

QString AssistantConfig::saveApiKey(const QString &key)
{
    const QString k = key.trimmed();
    if (k.isEmpty())
        return tr("Paste the key first.");
    if (k.size() < 20 || k.size() > 512)
        return tr("That does not look like an API key (NVIDIA keys start with nvapi- and are about 70 characters).");
    for (const QChar c : k) {
        if (c.unicode() <= 0x20 || c.unicode() >= 0x7f)
            return tr("The key contains a space or a character that cannot be part of a key. Copy it again.");
    }
    if (isPlaceholderKey(k))
        return tr("That is the example from the template, not a real key.");
    const QString path = keyFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return tr("Could not create %1.").arg(QFileInfo(path).absolutePath());
    // Created readable by its owner only (the umask can only narrow 0600, so
    // there is no moment at which anyone else could open it), written in
    // full, then moved into place in one step.
    const QByteArray target = QFile::encodeName(path);
    const QByteArray tmp = target + ".new-" + QByteArray::number(qint64(QCoreApplication::applicationPid()));
    ::unlink(tmp.constData());
    const int fd = ::open(tmp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return tr("Could not write %1: %2").arg(path, QString::fromLocal8Bit(std::strerror(errno)));
    const QByteArray data = k.toUtf8() + '\n';
    int problem = 0;
    qsizetype written = 0;
    while (problem == 0 && written < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + written, size_t(data.size() - written));
        if (n < 0) {
            if (errno != EINTR)
                problem = errno;
        } else {
            written += n;
        }
    }
    if (problem == 0 && ::fsync(fd) != 0)
        problem = errno;
    if (::close(fd) != 0 && problem == 0)
        problem = errno;
    if (problem == 0 && ::rename(tmp.constData(), target.constData()) != 0)
        problem = errno;
    if (problem != 0) {
        ::unlink(tmp.constData());
        return tr("Could not write %1: %2").arg(path, QString::fromLocal8Bit(std::strerror(problem)));
    }
    reload();
    return {};
}

void AssistantConfig::removeSavedApiKey()
{
    QFile::remove(keyFilePath());
    reload();
}

QString AssistantConfig::providerName() const
{
    if (m_localMode)
        return tr("a model on this computer");
    return m_provider == QLatin1String("anthropic") ? QStringLiteral("Claude (Anthropic)") : QStringLiteral("NVIDIA");
}

QString AssistantConfig::keyHelpUrl() const
{
    return m_provider == QLatin1String("anthropic") ? QStringLiteral("https://console.anthropic.com/")
                                                    : QStringLiteral("https://build.nvidia.com/");
}

// ---- the model team ----------------------------------------------------------

void AssistantConfig::setTeamMode(bool on)
{
    if (on == m_teamMode)
        return;
    m_teamMode = on;
    QSettings(iniPath(), QSettings::IniFormat).setValue(QStringLiteral("team/mode"),
                                                        on ? QStringLiteral("team") : QStringLiteral("solo"));
    Q_EMIT changed();
}

QStringList AssistantConfig::teamGroup(const QString &kind) const
{
    if (kind == QLatin1String("coding")) return m_groupCoding;
    if (kind == QLatin1String("agents")) return m_groupAgents;
    if (kind == QLatin1String("vision")) return m_groupVision;
    return m_groupChat;
}

QStringList AssistantConfig::teamLeads(const QString &kind) const
{
    // The preferred leads first, then the rest of the group: any member can
    // lead when the preferred ones are unavailable.
    QStringList out = kind == QLatin1String("coding") ? m_leadCoding
                    : kind == QLatin1String("agents") ? m_leadAgents : m_leadChat;
    for (const QString &m : teamGroup(kind))
        if (!out.contains(m))
            out << m;
    return out;
}

int AssistantConfig::advisorWaitMs(const QString &kind, bool voice, bool thorough) const
{
    int secs = m_advisorSeconds;
    if (secs <= 0) {
        // Code is worth waiting for; a spoken question is not.
        secs = kind == QLatin1String("coding") ? 45 : kind == QLatin1String("vision") ? 30
             : kind == QLatin1String("agents") ? 20 : 15;
        if (voice)
            secs = kind == QLatin1String("coding") ? 20 : 10;
    }
    if (thorough)
        secs = qMin(secs * 2, 180);
    return secs * 1000;
}

void AssistantConfig::reload()
{
    QSettings ini(iniPath(), QSettings::IniFormat);

    // --- API key: the environment wins, then the key saved from Settings,
    // then the conf file. Never the repo. ---
    // A key belongs to one service and is only ever sent to that service:
    // NVIDIA_API_KEY is NVIDIA's and ANTHROPIC_API_KEY Anthropic's, whatever
    // they look like; any other key is NVIDIA's when it starts with nvapi-
    // and Anthropic's when it starts with sk-ant-. A key of another shape (a
    // gateway's token) goes where the configuration says.
    m_apiKey.clear();
    m_apiKeySource.clear();
    const QString savedKey = readKeyFile(keyFilePath());
    m_apiKeySaved = !savedKey.isEmpty();
    const QString nvidia = QStringLiteral("nvidia"), anthropic = QStringLiteral("anthropic");
    auto ownerOf = [&](const QString &key) {
        return key.startsWith(QLatin1String("nvapi-")) ? nvidia : key.startsWith(QLatin1String("sk-ant-")) ? anthropic : QString();
    };
    struct Candidate { QString key, source, owner; };
    QList<Candidate> candidates;
    const QString envKey = QString::fromUtf8(qgetenv("SHADOW_ASSISTANT_API_KEY")).trimmed();
    const QString envKeyNvidia = QString::fromUtf8(qgetenv("NVIDIA_API_KEY")).trimmed();
    const QString envKeyAnthropic = QString::fromUtf8(qgetenv("ANTHROPIC_API_KEY")).trimmed();
    const QString confKey = readString(ini, QStringLiteral("api/key"));
    if (!envKey.isEmpty())
        candidates.append({envKey, QStringLiteral("SHADOW_ASSISTANT_API_KEY environment variable"), ownerOf(envKey)});
    if (!envKeyNvidia.isEmpty())
        candidates.append({envKeyNvidia, QStringLiteral("NVIDIA_API_KEY environment variable"), nvidia});
    if (m_apiKeySaved)
        candidates.append({savedKey, QStringLiteral("Settings"), ownerOf(savedKey)});
    if (!confKey.isEmpty() && !isPlaceholderKey(confKey))
        candidates.append({confKey, iniPath(), ownerOf(confKey)});
    if (!envKeyAnthropic.isEmpty())
        candidates.append({envKeyAnthropic, QStringLiteral("ANTHROPIC_API_KEY environment variable"), anthropic});

    // --- the service. Named by a test or the conf; otherwise it follows the
    // key in effect; without a key it is NVIDIA's hosted models. ---
    const QString confUrl = readString(ini, QStringLiteral("api/base_url"));
    const QString confNvidiaUrl = readString(ini, QStringLiteral("api/nvidia_base_url"));
    QString forced = QString::fromUtf8(qgetenv("SHADOW_ASSISTANT_PROVIDER")).trimmed().toLower();
    if (forced != nvidia && forced != anthropic)
        forced = readString(ini, QStringLiteral("api/provider")).toLower();
    if (forced != nvidia && forced != anthropic)
        forced.clear();
    m_provider = forced.isEmpty() ? nvidia : forced;
    for (const Candidate &c : std::as_const(candidates)) {
        // With the service named, a key that is plainly the other service's
        // is passed over: it must never be sent there.
        if (!forced.isEmpty() && !c.owner.isEmpty() && c.owner != forced)
            continue;
        m_apiKey = c.key;
        m_apiKeySource = c.source;
        if (forced.isEmpty()) {
            if (!c.owner.isEmpty())
                m_provider = c.owner;
            else
                // A key of unknown shape: a conf with its own endpoint is the
                // Anthropic-protocol setup it was before NVIDIA was added.
                m_provider = !confUrl.isEmpty() && !confUrl.contains(QLatin1String("nvidia.com")) ? anthropic : nvidia;
        }
        break;
    }
    const QString providerUrl = m_provider == anthropic ? kDefaultBaseUrl : kNvidiaBaseUrl;

    // --- Anthropic API version header (cloud mode) ---
    m_anthropicVersion = readString(ini, QStringLiteral("api/anthropic_version"), kDefaultAnthropicVersion);
    if (m_anthropicVersion.isEmpty())
        m_anthropicVersion = kDefaultAnthropicVersion;

    // --- base URL: a development override, then the conf's endpoint for this
    // service, then the service's own. [api] base_url is Anthropic's endpoint
    // (it predates NVIDIA) and nvidia_base_url NVIDIA's, so a key never goes
    // to an endpoint that was written down for the other service. ---
    const QByteArray envUrl = qgetenv("SHADOW_ASSISTANT_BASE_URL");
    if (!envUrl.trimmed().isEmpty()) {
        m_baseUrl = QString::fromUtf8(envUrl).trimmed();
        m_baseUrlOverridden = true;
    } else {
        if (m_provider == anthropic)
            m_baseUrl = confUrl.contains(QLatin1String("nvidia.com")) ? QString() : confUrl;
        else
            m_baseUrl = !confNvidiaUrl.isEmpty() ? confNvidiaUrl
                      : confUrl.contains(QLatin1String("nvidia.com")) ? confUrl : QString();
        if (m_baseUrl.isEmpty())
            m_baseUrl = providerUrl;
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

    // --- the model team (NVIDIA) ---
    m_teamMode = readString(ini, QStringLiteral("team/mode"), QStringLiteral("team")).toLower() != QLatin1String("solo");
    m_groupCoding = groupOr(ini, QStringLiteral("team/coding"), kTeamCoding);
    m_groupAgents = groupOr(ini, QStringLiteral("team/agents"), kTeamAgents);
    m_groupChat = groupOr(ini, QStringLiteral("team/chat"), kTeamChat);
    m_groupVision = groupOr(ini, QStringLiteral("team/vision"), kTeamVision);
    m_leadCoding = groupOr(ini, QStringLiteral("team/lead_coding"), kLeadCoding);
    m_leadAgents = groupOr(ini, QStringLiteral("team/lead_agents"), kLeadAgents);
    m_leadChat = groupOr(ini, QStringLiteral("team/lead_chat"), kLeadChat);
    bool secsOk = false;
    const int secs = readString(ini, QStringLiteral("team/advisor_seconds")).toInt(&secsOk);
    m_advisorSeconds = secsOk ? qBound(3, secs, 180) : 0;
    bool maxOk = false;
    const int maxModels = readString(ini, QStringLiteral("team/max_models")).toInt(&maxOk);
    m_teamMaxModels = maxOk && maxModels > 0 ? qMin(maxModels, 64) : 0;

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

    // --- skills ---
    m_skillsEnabled = readBool(ini, QStringLiteral("skills/enabled"), true);
    m_disabledSkills = readList(ini, QStringLiteral("skills/disabled"));

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
    ini.setValue(QStringLiteral("skills/enabled"), m_skillsEnabled);
    ini.setValue(QStringLiteral("skills/disabled"), m_disabledSkills);
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
    // NVIDIA: the team decides per request (see Council); this is only the
    // name shown before a request has run.
    if (m_provider == QLatin1String("nvidia"))
        return teamLeads(role == QLatin1String("planner") ? QStringLiteral("agents") : QStringLiteral("chat")).value(0);
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

void AssistantConfig::setSkillsEnabled(bool on)
{
    if (on == m_skillsEnabled)
        return;
    m_skillsEnabled = on;
    save();
    Q_EMIT changed();
}

bool AssistantConfig::skillEnabled(const QString &name) const
{
    return !m_disabledSkills.contains(name);
}

void AssistantConfig::setSkillEnabled(const QString &name, bool on)
{
    const bool currently = !m_disabledSkills.contains(name);
    if (currently == on)
        return;
    if (on)
        m_disabledSkills.removeAll(name);
    else if (!name.isEmpty())
        m_disabledSkills << name;
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
    // Offered on the first-start page for those who prefer a file to the key
    // field in Settings.
    // Comments start with ';' and stay on lines of their own: QSettings
    // treats only ';' as a comment ('#' lines become keys) and would read a
    // comment after a value as part of the value.
    return QStringLiteral(
        "; ~/.config/shadow/assistant.conf  —  optional; create with mode 0600 (chmod 600)\n"
        "; The easy way to give SHADOW Assistant its API key is Settings in the\n"
        "; assistant. This file is the alternative, and holds the other options.\n"
        "; The assistant uses NVIDIA's hosted models: get a free key at\n"
        "; https://build.nvidia.com/ (keys start with nvapi-). The key is read from\n"
        "; Settings, from here, or from the SHADOW_ASSISTANT_API_KEY or NVIDIA_API_KEY\n"
        "; environment variable; it is never stored in the application or the repository.\n"
        "; Comments start with ';' on a line of their own, never after a value.\n"
        "\n"
        "[api]\n"
        "key=nvapi-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n"
        "; Endpoint (usually leave unset):\n"
        "; nvidia_base_url=https://integrate.api.nvidia.com/v1\n"
        "; An Anthropic key (sk-ant-...) makes the assistant use Claude instead.\n"
        "; Offline: run a model on this computer instead of the cloud (no key\n"
        "; needed). Easiest setup:  shadow-assistant-local setup\n"
        "; local=true\n"
        "; local_base_url=http://127.0.0.1:11434/v1\n"
        "\n"
        "[team]\n"
        "; team: every model of a group works on a request at the same time and a\n"
        "; lead model combines their work.  solo: one model answers.\n"
        "mode=team\n"
        "; The groups, as comma-separated model ids from https://build.nvidia.com/models\n"
        "; (unset = the built-in lists; models the service does not offer are skipped):\n"
        "; coding=moonshotai/kimi-k3.1-pro, minimaxai/minimax-m3.1-flash\n"
        "; agents=\n"
        "; chat=\n"
        "; vision=\n"
        "; The models preferred as lead, best first:\n"
        "; lead_coding=\n"
        "; lead_agents=\n"
        "; lead_chat=\n"
        "; How long the lead waits for the others, in seconds (default: 15 for chat,\n"
        "; 20 for actions, 45 for code; 10 when you asked by voice):\n"
        "; advisor_seconds=20\n"
        "; The most models that work on one request, the lead included (unset = the\n"
        "; whole group). A smaller team makes fewer requests per message:\n"
        "; max_models=4\n"
        "\n"
        "[models]\n"
        "; Only with a Claude key: the Claude model per role.\n"
        "; brain=") + kDefaultBrain + QStringLiteral("\n"
        "; planner=") + kDefaultPlanner + QStringLiteral("\n"
        "; fast=") + kDefaultFast + QStringLiteral("\n"
        "; omni=") + kDefaultOmni + QStringLiteral("\n"
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
        "[skills]\n"
        "; Skills are short instructions that help with coding, debugging,\n"
        "; testing and (in pentest mode) security testing. enabled=false turns\n"
        "; them all off; disabled= is a comma-separated list of skill names.\n"
        "; Your own skills (Markdown files) go in\n"
        "; ~/.local/share/shadow/assistant/skills/ and override a built-in of\n"
        "; the same name.\n"
        "enabled=true\n"
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
        {QStringLiteral("apiKeySaved"), m_apiKeySaved},
        {QStringLiteral("apiKeyHint"), apiKeyHint()},
        {QStringLiteral("provider"), m_localMode ? QStringLiteral("local") : m_provider},
        {QStringLiteral("team"), QJsonObject{
            {QStringLiteral("mode"), m_teamMode ? QStringLiteral("team") : QStringLiteral("solo")},
            {QStringLiteral("maxModels"), m_teamMaxModels},
            {QStringLiteral("coding"), QJsonArray::fromStringList(m_groupCoding)},
            {QStringLiteral("agents"), QJsonArray::fromStringList(m_groupAgents)},
            {QStringLiteral("chat"), QJsonArray::fromStringList(m_groupChat)},
            {QStringLiteral("vision"), QJsonArray::fromStringList(m_groupVision)},
        }},
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
        {QStringLiteral("skillsEnabled"), m_skillsEnabled},
        {QStringLiteral("disabledSkills"), QJsonArray::fromStringList(m_disabledSkills)},
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
