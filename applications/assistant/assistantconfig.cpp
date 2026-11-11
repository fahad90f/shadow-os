// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistantconfig.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QStandardPaths>

namespace {

// Default model mapping. These are the identifiers the user supplied; they may
// not match a given NVIDIA account exactly, so they are written into the conf
// template (with a comment) as editable defaults rather than compiled-in
// constants used behind the user's back.
const QString kDefaultBrain = QStringLiteral("nvidia/nemotron-3.5-super-vl-120b-a12b");
const QString kDefaultPlanner = QStringLiteral("nvidia/nemotron-3-ultra-550b");
const QString kDefaultFast = QStringLiteral("nvidia/nemotron-3.5-lightning-30b");
const QString kDefaultOmni = QStringLiteral("nvidia/nemotron-3-nano-omni-30b");
const QString kDefaultBaseUrl = QStringLiteral("https://integrate.api.nvidia.com/v1");

// Canonical set of tool names the registry exposes. Kept in step with
// ToolRegistry; config only needs the names to compute the enabled list.
const QStringList kAllTools = {
    QStringLiteral("run_shell_command"), QStringLiteral("read_file"),
    QStringLiteral("write_file"),        QStringLiteral("search_files"),
    QStringLiteral("open_application"),  QStringLiteral("get_system_info"),
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
    if (!envKey.trimmed().isEmpty()) {
        m_apiKey = QString::fromUtf8(envKey).trimmed();
        m_apiKeySource = QStringLiteral("SHADOW_ASSISTANT_API_KEY environment variable");
    } else {
        const QString k = ini.value(QStringLiteral("api/key")).toString().trimmed();
        if (!k.isEmpty()) {
            m_apiKey = k;
            m_apiKeySource = iniPath();
        }
    }

    // --- base URL: dev override, then conf, then NVIDIA default. ---
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
    m_autonomy = ini.value(QStringLiteral("behaviour/autonomy"), QStringLiteral("ask_every")).toString().trimmed();
    if (m_autonomy != QLatin1String("ask_risky"))
        m_autonomy = QStringLiteral("ask_every");

    // --- disabled tools ---
    m_disabledTools = ini.value(QStringLiteral("tools/disabled")).toStringList();
    m_disabledTools.removeAll(QString());
    m_enabledTools.clear();
    for (const QString &t : kAllTools)
        if (!m_disabledTools.contains(t))
            m_enabledTools << t;

    Q_EMIT changed();
}

void AssistantConfig::save()
{
    QSettings ini(iniPath(), QSettings::IniFormat);
    ini.setValue(QStringLiteral("behaviour/autonomy"), m_autonomy);
    ini.setValue(QStringLiteral("tools/disabled"), m_disabledTools);
    // We deliberately never write api/key.
    ini.sync();
}

QString AssistantConfig::modelForRole(const QString &role) const
{
    if (role == QLatin1String("planner")) return m_planner;
    if (role == QLatin1String("fast")) return m_fast;
    if (role == QLatin1String("omni")) return m_omni;
    return m_brain;
}

void AssistantConfig::setAutonomy(const QString &value)
{
    const QString v = (value == QLatin1String("ask_risky")) ? value : QStringLiteral("ask_every");
    if (v == m_autonomy)
        return;
    m_autonomy = v;
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

QString AssistantConfig::configTemplate() const
{
    // Shown verbatim in the setup state so the user can create the file.
    return QStringLiteral(
        "# ~/.config/shadow/assistant.conf  —  create with mode 0600 (chmod 600)\n"
        "# SHADOW Assistant reads your NVIDIA API key from here (or from the\n"
        "# SHADOW_ASSISTANT_API_KEY environment variable). The key is never\n"
        "# stored in the application or the repository.\n"
        "\n"
        "[api]\n"
        "key=nvapi-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n"
        "# base_url=https://integrate.api.nvidia.com/v1   # usually leave unset\n"
        "\n"
        "[models]\n"
        "# Change these to the exact model IDs available on your NVIDIA account.\n"
        "brain=") + kDefaultBrain + QStringLiteral("\n"
        "planner=") + kDefaultPlanner + QStringLiteral("\n"
        "fast=") + kDefaultFast + QStringLiteral("\n"
        "omni=") + kDefaultOmni + QStringLiteral("\n"
        "\n"
        "[behaviour]\n"
        "# autonomy = ask_every (confirm every tool) | ask_risky (auto-run read-only)\n"
        "autonomy=ask_every\n"
        "\n"
        "[tools]\n"
        "# disabled = comma-separated tool names to turn off, e.g. run_shell_command\n"
        "disabled=\n");
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
    };
}
