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
// The NVIDIA API key is NEVER stored in the repository or written by this
// application. It is read at run time from, in order of precedence:
//   1. the environment variable  SHADOW_ASSISTANT_API_KEY
//   2. ~/.config/shadow/assistant.conf   [api] key=...
//
// Everything else (model mapping, autonomy, which tools are enabled) lives in
// the same INI file and may be changed from the Settings page. The base URL
// defaults to NVIDIA's OpenAI-compatible endpoint and can be overridden for
// testing with the environment variable SHADOW_ASSISTANT_BASE_URL.
class AssistantConfig : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool apiKeyConfigured READ apiKeyConfigured NOTIFY changed)
    Q_PROPERTY(QString apiKeySource READ apiKeySource NOTIFY changed)
    Q_PROPERTY(QString baseUrl READ baseUrl NOTIFY changed)
    Q_PROPERTY(bool baseUrlOverridden READ baseUrlOverridden NOTIFY changed)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)
    Q_PROPERTY(QString configTemplate READ configTemplate CONSTANT)
    Q_PROPERTY(QString brainModel READ brainModel NOTIFY changed)
    Q_PROPERTY(QString plannerModel READ plannerModel NOTIFY changed)
    Q_PROPERTY(QString fastModel READ fastModel NOTIFY changed)
    Q_PROPERTY(QString omniModel READ omniModel NOTIFY changed)
    // "ask_every" | "ask_risky"
    Q_PROPERTY(QString autonomy READ autonomy WRITE setAutonomy NOTIFY changed)
    Q_PROPERTY(QStringList enabledTools READ enabledTools NOTIFY changed)

public:
    explicit AssistantConfig(QObject *parent = nullptr);

    bool apiKeyConfigured() const { return !m_apiKey.isEmpty(); }
    QString apiKeySource() const { return m_apiKeySource; }
    // The key itself is never exposed to QML; only the network layer reads it.
    QString apiKey() const { return m_apiKey; }
    QString baseUrl() const { return m_baseUrl; }
    bool baseUrlOverridden() const { return m_baseUrlOverridden; }
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

    QStringList enabledTools() const { return m_enabledTools; }
    Q_INVOKABLE bool toolEnabled(const QString &name) const;
    Q_INVOKABLE void setToolEnabled(const QString &name, bool enabled);

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
    bool m_baseUrlOverridden = false;
    QString m_brain, m_planner, m_fast, m_omni;
    QString m_autonomy;
    QStringList m_enabledTools;
    bool m_toolsKeySet = false; // whether [tools] disabled=... was present
    QStringList m_disabledTools;
};
