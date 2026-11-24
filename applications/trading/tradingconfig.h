// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/paper.h"
#include "core/risk.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

// Settings and the API key of Shadow Trading.
//
//   ~/.config/shadow/trading.conf        settings (INI)
//   ~/.config/shadow/trading/nvidia.key  the NVIDIA API key, mode 600
//   ~/.local/share/shadow/trading/       the paper account and its history
//
// Every model on the panel is one of NVIDIA's hosted models, reached through
// one endpoint with one key. The key is never stored in the application, its
// logs, QML or this repository: it is read at run time, held in memory, and
// sent only to NVIDIA's endpoint.
class TradingConfig : public QObject
{
    Q_OBJECT
public:
    // One model on the panel. The model id is its identity.
    struct Member
    {
        QString model;      // e.g. "moonshotai/kimi-k3.1-pro"
        QString role;       // "analysis" | "reasoning" | "vision" | "compute": what it concentrates on
        bool vision = false; // gets the chart picture as well as the numbers
        bool enabled = true;
    };

    explicit TradingConfig(QObject *parent = nullptr);

    void reload();

    // ---- the panel -----------------------------------------------------------
    static QStringList roles();
    QList<Member> panel() const { return m_panel; }
    Member member(const QString &model) const;
    void setMemberEnabled(const QString &model, bool enabled);
    // Add a model to a role. "" on success, else what is wrong.
    QString addMember(const QString &model, const QString &role);
    void removeMember(const QString &model);
    // Put the built-in panel back.
    void resetPanel();
    // "moonshotai/kimi-k3.1-pro" -> "Kimi K3.1 Pro", "Kimi", "moonshotai".
    static QString displayName(const QString &model);
    static QString shortName(const QString &model);
    static QString publisher(const QString &model);

    // ---- NVIDIA's service -------------------------------------------------------
    QString baseUrl() const { return m_baseUrl; }
    // The key in effect ("" = none). C++ only: never handed to QML.
    QString apiKey() const;
    // Where it came from: "environment" | "saved" | "assistant" | "".
    QString keySource() const;
    // The last four characters, enough to tell two keys apart.
    QString keyHint() const;
    // Save the key. Returns "" on success, else what is wrong.
    QString saveKey(const QString &key);
    void removeKey();
    QString keyFilePath() const;

    // ---- market ------------------------------------------------------------
    QString dataUrl() const { return m_dataUrl; }
    QStringList watchlist() const { return m_watchlist; }
    QString symbol() const { return m_symbol; }
    void setSymbol(const QString &symbol);
    QString interval() const { return m_interval; }
    void setInterval(const QString &interval);
    static QStringList intervals(); // "1m" … "1d"
    // The timeframe read alongside, for context: 15m -> 1h, 1h -> 4h, …
    static QString higherInterval(const QString &interval);
    static qint64 intervalMs(const QString &interval);

    // ---- engine ------------------------------------------------------------
    bool autoTrade() const { return m_autoTrade; }
    void setAutoTrade(bool on);
    bool autoAnalyze() const { return m_autoAnalyze; }
    void setAutoAnalyze(bool on);

    Trading::RiskSettings risk() const { return m_risk; }
    // One risk setting by name; false when the name or value is not valid.
    bool setRisk(const QString &name, double value);
    Trading::PaperSettings paper() const { return m_paper; }
    bool setPaper(const QString &name, double value);

    QString dataDir() const;   // created on demand
    QString configPath() const;

Q_SIGNALS:
    void changed();
    void keysChanged();

private:
    void store(const QString &key, const QVariant &value);
    void storePanel(const QList<Member> &panel);
    QString savedKey() const;
    QString assistantKey() const;

    QList<Member> m_panel;
    QString m_baseUrl;
    QString m_dataUrl;
    QStringList m_watchlist;
    QString m_symbol;
    QString m_interval;
    bool m_autoTrade = false;
    bool m_autoAnalyze = true;
    Trading::RiskSettings m_risk;
    Trading::PaperSettings m_paper;
};
