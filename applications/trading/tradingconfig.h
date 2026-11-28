// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/paper.h"
#include "core/risk.h"
#include "core/sbt.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

// Settings and the API key of Shadow Trading.
//
//   ~/.config/shadow/trading.conf        settings (INI)
//   ~/.config/shadow/trading/nvidia.key  the NVIDIA API key, mode 600
//   ~/.config/shadow/trading/oanda.token the OANDA token (forex prices), mode 600
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
    // The name of a model of the panel: displayName(), with the model's size
    // added when another model of the panel would have the same name
    // ("Gemma 3 12B" and "Gemma 3 4B").
    QString memberName(const QString &model) const;
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

    // ---- forex prices: OANDA ----------------------------------------------
    // The token in effect ("" = none): SHADOW_TRADING_OANDA_TOKEN, else the
    // saved file. C++ only: never handed to QML, sent only to OANDA.
    QString oandaToken() const;
    QString oandaTokenSource() const; // "environment" | "saved" | ""
    QString oandaTokenHint() const;   // the last four characters
    QString saveOandaToken(const QString &token);
    void removeOandaToken();
    QString oandaTokenPath() const;
    // "practice" (api-fxpractice.oanda.com) or "live" (api-fxtrade.oanda.com):
    // which of OANDA's two services the token belongs to.
    QString oandaAccount() const { return m_oandaAccount; }
    void setOandaAccount(const QString &account);
    // Where the forex prices come from: "oanda" (gold, silver and the dollar
    // pairs; needs the token) or "kraken" (no key; no silver, gold as a token).
    QString forexSource() const { return m_forexSource; }

    // ---- market ------------------------------------------------------------
    // Forex (true, what the application offers) or crypto (false: only
    // market/kind=crypto in the settings file, or SHADOW_TRADING_MARKET_KIND
    // where the file says nothing; the test suite's stand-in exchange speaks
    // the crypto service's API): which data service, which list of markets and
    // which selection the rest of this section is about.
    bool forex() const { return m_forex; }
    void setForex(bool forex);
    QString dataUrl() const { return m_dataUrl; }
    // The live stream ("wss://…"); empty when there is none to use.
    QString streamUrl() const { return m_streamUrl; }
    QStringList watchlist() const { return m_watchlist; }
    static constexpr int kMaxMarkets = 12;
    static bool validSymbol(const QString &symbol);
    // Add a market at the end of the watchlist, or take one out. They return
    // "" on success, else what is wrong. The last market cannot be removed.
    QString addMarket(const QString &symbol);
    QString removeMarket(const QString &symbol);
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
    // How long a model is given to answer, in seconds (asked once more when
    // it does not), and how many questions are with the service at a time.
    static constexpr int kTimeoutMin = 30, kTimeoutMax = 600;
    int modelTimeoutSeconds() const { return m_modelTimeout; }
    QString setModelTimeoutSeconds(int seconds);
    int modelParallel() const { return m_modelParallel; }
    // The analysis team reads each other's first calls and then makes its final ones.
    bool teamReview() const { return m_teamReview; }
    void setTeamReview(bool on);

    // ---- desk ------------------------------------------------------------
    // The model that reads every message of the conversation (quick, small)
    // and the one that answers questions (deeper). Built-in choices unless
    // the settings name others.
    static constexpr const char *kDeskFast = "nvidia/nemotron-3.5-bolt-e4b";
    static constexpr const char *kDeskDeep = "moonshotai/kimi-k3.1-pro";
    QString deskFastModel() const;
    QString deskDeepModel() const;
    // which: "fast" | "deep"; "" puts the built-in choice back. "" on success.
    QString setDeskModel(const QString &which, const QString &model);
    // The chart shows the market the conversation is about.
    bool deskFollowChart() const { return m_deskFollow; }
    void setDeskFollowChart(bool on);

    // ---- SBT Model 1 -------------------------------------------------------
    // The trader's strategy: every trade comes from it ([sbt] in the file).
    Trading::Sbt::Settings sbt() const;
    // The AI team must confirm a setup before it is traded (when there is a key).
    bool sbtAiConfirm() const { return m_sbtAiConfirm; }
    // name: ai_confirm, require_pd_array (bool), zone ("wick" | "body"),
    // stop_buffer_atr, pd_distance_atr, max_wait_candles.
    bool setSbt(const QString &name, const QVariant &value);

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
    static QString writeSecret(const QString &path, const QString &value); // "" on success
    QString assistantKey() const;

    QList<Member> m_panel;
    QString m_baseUrl;
    bool m_forex = true;
    QString m_forexSource = QStringLiteral("oanda");
    QString m_oandaAccount = QStringLiteral("practice");
    QString marketKey(const QString &name) const; // "market/<name>" or "market/forex_<name>"
    QString m_dataUrl;
    QString m_streamUrl;
    QStringList m_watchlist;
    QString m_symbol;
    QString m_interval;
    bool m_autoTrade = false;
    bool m_autoAnalyze = true;
    bool m_teamReview = true;
    int m_modelTimeout = 180;
    int m_modelParallel = 6;
    Trading::Sbt::Settings m_sbt;
    bool m_sbtAiConfirm = true;
    QString m_deskFast;
    QString m_deskDeep;
    bool m_deskFollow = true;
    Trading::RiskSettings m_risk;
    Trading::PaperSettings m_paper;
};
