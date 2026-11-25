// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/paper.h"
#include "core/risk.h"
#include "core/signal.h"
#include "marketdata.h"
#include "marketstream.h"
#include "skills/candle.h"
#include "skills/indicators.h"
#include "skills/smc.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class ModelClient;
class QNetworkAccessManager;
class QTimer;
class TradingConfig;

// Shadow Trading's engine: live prices in, an analysis by every connected
// model plus the rule-based analyst, one consensus signal, the risk rules,
// and the paper account that the orders go to. Exposed to QML as "trading".
//
// Everything shown comes from here and is real: prices from the exchange's
// public API, verdicts from the models that actually answered, statistics
// from the trades the paper account actually made. Nothing is a sample.
class TradingEngine : public QObject
{
    Q_OBJECT
    // "paper": orders are simulated. There is no other mode yet.
    Q_PROPERTY(QString mode READ mode CONSTANT)

    Q_PROPERTY(QVariantList watchlist READ watchlist NOTIFY marketChanged)
    // "loading" | "live" | "error"
    Q_PROPERTY(QString dataState READ dataState NOTIFY marketChanged)
    Q_PROPERTY(QString dataError READ dataError NOTIFY marketChanged)
    Q_PROPERTY(QString dataSource READ dataSource CONSTANT)
    // Prices arrive as they change (the live stream is connected); otherwise
    // they are asked for every few seconds.
    Q_PROPERTY(bool streaming READ streaming NOTIFY marketChanged)
    Q_PROPERTY(int maxMarkets READ maxMarkets CONSTANT)
    // Without the stream, prices are asked for this often.
    Q_PROPERTY(int pollSeconds READ pollSeconds CONSTANT)
    // The markets the data service lists, for adding one: loaded by refreshMarkets().
    Q_PROPERTY(bool marketsLoaded READ marketsLoaded NOTIFY marketsChanged)
    Q_PROPERTY(bool marketsLoading READ marketsLoading NOTIFY marketsChanged)
    Q_PROPERTY(QString marketsError READ marketsError NOTIFY marketsChanged)

    Q_PROPERTY(QString symbol READ symbol WRITE setSymbol NOTIFY selectionChanged)
    Q_PROPERTY(QString symbolName READ symbolName NOTIFY selectionChanged)
    Q_PROPERTY(QString interval READ interval WRITE setInterval NOTIFY selectionChanged)
    Q_PROPERTY(QStringList intervals READ intervals CONSTANT)

    // Bumped whenever the candles of the selected market change.
    Q_PROPERTY(int chartRevision READ chartRevision NOTIFY chartChanged)
    // Older candles are being fetched for the chart / there are none before the oldest one.
    Q_PROPERTY(bool loadingOlder READ loadingOlder NOTIFY chartChanged)
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY chartChanged)
    // The last candle: {time, open, high, low, close, change, changePct, closed}.
    Q_PROPERTY(QVariantMap quote READ quote NOTIFY chartChanged)
    // The closes of the newest candles of the selected market, oldest first.
    Q_PROPERTY(QVariantList closes READ closes NOTIFY chartChanged)
    // The indicator and structure readings of the selected market.
    Q_PROPERTY(QVariantMap readings READ readings NOTIFY chartChanged)

    Q_PROPERTY(bool analyzing READ analyzing NOTIFY signalChanged)
    Q_PROPERTY(QVariantMap signal READ signal NOTIFY signalChanged)
    Q_PROPERTY(QVariantList analysts READ analysts NOTIFY analystsChanged)
    // The NVIDIA key: {hasKey, hint, source}. Never the key itself.
    Q_PROPERTY(QVariantMap key READ key NOTIFY analystsChanged)
    // Models NVIDIA offers that are not on the panel yet (after refreshCatalog()).
    Q_PROPERTY(QStringList catalog READ catalog NOTIFY catalogChanged)
    Q_PROPERTY(QString catalogError READ catalogError NOTIFY catalogChanged)

    Q_PROPERTY(bool autoTrade READ autoTrade WRITE setAutoTrade NOTIFY settingsChanged)
    Q_PROPERTY(bool autoAnalyze READ autoAnalyze WRITE setAutoAnalyze NOTIFY settingsChanged)
    // The analysis team reads each other's first calls and then makes its final ones.
    Q_PROPERTY(bool teamReview READ teamReview WRITE setTeamReview NOTIFY settingsChanged)
    // The seats of the analysis team, in order: {key, title, focus}.
    Q_PROPERTY(QVariantList teamSeats READ teamSeatList CONSTANT)
    Q_PROPERTY(QVariantMap riskSettings READ riskSettings NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap paperSettings READ paperSettings NOTIFY settingsChanged)

    Q_PROPERTY(QVariantMap account READ account NOTIFY accountChanged)
    Q_PROPERTY(QVariantList positions READ positions NOTIFY accountChanged)
    Q_PROPERTY(QVariantList trades READ trades NOTIFY accountChanged)       // newest first
    Q_PROPERTY(QVariantList equityCurve READ equityCurve NOTIFY accountChanged)
    Q_PROPERTY(QVariantList activity READ activity NOTIFY activityChanged)   // newest first

    Q_PROPERTY(bool webViewAvailable READ webViewAvailable CONSTANT)

public:
    explicit TradingEngine(TradingConfig *config, QObject *parent = nullptr);
    ~TradingEngine() override;

    // Begin fetching prices. Nothing is requested before this.
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

    QString mode() const { return QStringLiteral("paper"); }
    QVariantList watchlist() const;
    QString dataState() const { return m_dataState; }
    QString dataError() const { return m_dataError; }
    QString dataSource() const;
    bool streaming() const;
    int maxMarkets() const;
    int pollSeconds() const;
    bool marketsLoaded() const { return !m_marketCatalog.isEmpty(); }
    bool marketsLoading() const { return m_marketsLoading; }
    QString marketsError() const { return m_marketsError; }
    bool loadingOlder() const { return m_loadingOlder; }
    bool hasOlder() const { return !m_noOlder; }
    QString symbol() const;
    QString symbolName() const { return displayName(symbol()); }
    void setSymbol(const QString &symbol);
    QString interval() const;
    void setInterval(const QString &interval);
    QStringList intervals() const;
    int chartRevision() const { return m_chartRevision; }
    QVariantMap quote() const;
    QVariantMap readings() const;
    QVariantList closes() const;
    bool analyzing() const { return m_analyzing; }
    QVariantMap signal() const { return m_signal; }
    QVariantList analysts() const;
    QVariantMap key() const;
    QStringList catalog() const;
    QString catalogError() const { return m_catalogError; }
    bool autoTrade() const;
    void setAutoTrade(bool on);
    bool autoAnalyze() const;
    void setAutoAnalyze(bool on);
    bool teamReview() const;
    void setTeamReview(bool on);
    QVariantList teamSeatList() const;
    QVariantMap riskSettings() const;
    QVariantMap paperSettings() const;
    QVariantMap account() const;
    QVariantList positions() const;
    QVariantList trades() const;
    QVariantList equityCurve() const;
    QVariantList activity() const { return m_activity; }
    bool webViewAvailable() const { return m_webViewAvailable; }
    void setWebViewAvailable(bool available) { m_webViewAvailable = available; }

    // For the chart and the tests. candles() is what the analysts read (the
    // newest few hundred); chartCandles() is everything loaded for the chart,
    // which grows backwards as the chart is moved into the past.
    const Trading::Candles &candles() const { return m_candles; }
    const Trading::Candles &chartCandles() const { return m_chart; }
    // How many candles were added at the newest end since the market was
    // put on the chart: a chart that is looking at the past moves its view
    // by as much, so that the same candles stay in view.
    int tailAdded() const { return m_tailAdded; }
    // Fetch the candles before the oldest one the chart has.
    Q_INVOKABLE void loadOlder();

    // Markets: the service's list (for the "add a market" dialog), and the
    // watchlist. add/remove return "" on success, else what is wrong.
    Q_INVOKABLE void refreshMarkets();
    Q_INVOKABLE QVariantList findMarkets(const QString &text) const;
    Q_INVOKABLE QString addMarket(const QString &symbol);
    Q_INVOKABLE QString removeMarket(const QString &symbol);
    const Trading::Signal &lastSignal() const { return m_lastSignal; }
    Trading::PaperAccount &paperAccount() { return m_account; }

    // Run the analysts on the selected market now. False (with a reason in
    // the activity log) when there is not enough price history yet or an
    // analysis is already running.
    Q_INVOKABLE bool analyzeNow();
    // Place the trade of the current signal on the paper account, through the
    // risk rules. "" on success, else why not.
    Q_INVOKABLE QString placeSuggestedTrade();
    Q_INVOKABLE bool closePosition(qlonglong id);
    Q_INVOKABLE void resetPaperAccount();
    // Trade again after the drawdown limit halted trading.
    Q_INVOKABLE void resumeTrading();

    Q_INVOKABLE bool setRiskSetting(const QString &name, double value);
    Q_INVOKABLE bool setPaperSetting(const QString &name, double value);
    // The panel: NVIDIA-hosted models, each with a role.
    Q_INVOKABLE void setModelEnabled(const QString &model, bool enabled);
    Q_INVOKABLE QString addModel(const QString &model, const QString &role);
    Q_INVOKABLE void removeModel(const QString &model);
    Q_INVOKABLE void resetPanel();
    Q_INVOKABLE QString saveKey(const QString &key);
    Q_INVOKABLE void removeKey();
    // Send one tiny question to a model to check the key and the model;
    // modelTested() follows. testAll() asks every enabled model.
    Q_INVOKABLE void testModel(const QString &model);
    Q_INVOKABLE void testAll();
    // Fetch the list of models NVIDIA offers; catalogChanged() follows.
    Q_INVOKABLE void refreshCatalog();

    // The page shown in the TradingView tab: TradingView's own embeddable
    // chart for the selected market.
    // 'offlineText' is shown in the page when TradingView cannot be reached.
    Q_INVOKABLE QString tradingViewPage(bool dark, const QString &offlineText) const;

    static QString displayName(const QString &symbol);  // "BTCUSDT" -> "BTC/USDT"
    QJsonObject snapshot(const Trading::Indicators::Summary &ta, const Trading::Indicators::Summary *higher,
                         const Trading::Smc::Analysis &smc) const;
    // A model's answer as a verdict; !ok with a reason when it is unusable.
    static Trading::Verdict parseVerdict(const QString &source, const QString &text);
    // The instructions for one role: the task, the role's focus and its skill
    // briefs. A member of the analysis team is also given its seat.
    static QString systemPrompt(const QString &role, double minRewardRisk, const QString &seat = QString());
    // The analysis team. Each member reads the snapshot from one seat, so six
    // models are six different looks at the market and not one look six times.
    struct Seat { QString key; QString title; QString focus; };
    static const QList<Seat> &teamSeats();
    static QString seatTitle(const QString &key);
    // The seat of a model of the panel: by its place among the analysis
    // models, so it keeps its seat when another model is switched off.
    // Empty for a model that is not on the analysis team.
    QString seatOf(const QString &model) const;
    // What a team member is asked in the review: its own first call and its
    // teammates' calls, by seat.
    static QString reviewRequest(const Trading::Verdict &own, const std::vector<Trading::Verdict> &teammates);
    // The skills a role is briefed on: "ta", "smc", "risk", "chart".
    static QStringList skillsOf(const QString &role);

    QJsonObject dumpConfig() const;
    QJsonObject dumpAccount() const;
    QJsonObject dumpMarket() const;

Q_SIGNALS:
    void marketChanged();
    void selectionChanged();
    void chartChanged();
    // Another market or interval is on the chart: its view starts over.
    void chartReset();
    void marketsChanged();
    void signalChanged();
    void analystsChanged();
    void settingsChanged();
    void accountChanged();
    void activityChanged();
    // First candles of the selected market arrived (or failed to).
    void marketReady();
    void analysisFinished();
    void modelTested(const QString &model, bool ok, const QString &message);
    void catalogChanged();

private:
    void poll();
    void onCandles(const QString &symbol, const QString &interval, const Trading::Candles &series,
                   const QString &purpose);
    void onTickers(const QList<MarketData::Ticker> &list);
    void onDataFailed(const QString &what, const QString &message);
    void onUnlisted(const QString &symbol);
    void onStreamTrade(const QString &symbol, double price, double quantity);
    void onStreamKline(const QString &symbol, const QString &interval, const Trading::Candle &candle);
    void onStreamTicker(const MarketData::Ticker &ticker);
    void flushStream();
    void subscribeStream();
    void mergeChart();
    // Prices ("tickers") and candles fail and recover on their own; an empty
    // message means that source works. True when the state on screen changed.
    bool setDataError(const QString &what, const QString &message);
    void priceUpdate(const QString &symbol, double last, double high, double low);
    void onModelAnswered(const QString &id, const QString &text, qint64 ms);
    void onModelFailed(const QString &id, const QString &code, const QString &message, qint64 ms);
    void collect(const Trading::Verdict &verdict);
    void startTeamReview();
    void applyReview(const Trading::Verdict &final);
    void finishAnalysis();
    QString execute(const Trading::Signal &signal, const Trading::RiskDecision &decision);
    Trading::AccountView accountView(const QString &symbol, qint64 nowMs) const;
    void afterAccountChange(bool tradeClosed);
    void recordClosed(const std::vector<Trading::Trade> &closed);
    void log(const QString &kind, const QString &text);
    void load();
    void save();
    void resetMarket();
    QVariantMap signalMap(const Trading::Signal &signal, const Trading::RiskDecision &decision) const;

    TradingConfig *m_config;
    QNetworkAccessManager *m_net;
    MarketData *m_data;
    MarketStream *m_stream;
    ModelClient *m_models;
    QTimer *m_timer;
    QTimer *m_flush;                  // trades are applied to the chart a few times a second

    // market
    Trading::Candles m_candles;       // selected symbol and interval: what the analysts read
    Trading::Candles m_chart;         // the same plus the older candles loaded for the chart
    bool m_loadingOlder = false;
    bool m_noOlder = false;           // the market's history begins at m_chart.front()
    int m_tailAdded = 0;
    // Trades of the stream since the last flush.
    bool m_tradePending = false;
    double m_tradeLast = 0, m_tradeHigh = 0, m_tradeLow = 0, m_tradeVolume = 0;
    bool m_marketDirty = false;
    qint64 m_streamCandleAt = 0;      // when the stream last updated the forming candle
    qint64 m_lastIdleUpdate = 0;
    qint64 m_streamLoggedAt = 0;
    QList<QPair<QString, double>> m_marketCatalog;
    bool m_marketsLoading = false;
    QString m_marketsError;
    Trading::Candles m_higherCandles; // the same symbol one timeframe up
    QString m_higherFor;              // "SYMBOL:interval" m_higherCandles belongs to
    QHash<QString, MarketData::Ticker> m_tickers;
    QHash<QString, QVariantList> m_sparks; // per symbol: the closes of the last 24 hours
    QString m_dataState = QStringLiteral("loading");
    QString m_dataError;   // what is shown: the candles' error before the prices'
    QString m_tickerError;
    QString m_candleError;
    int m_chartRevision = 0;
    int m_pollCount = 0;
    bool m_running = false;
    bool m_marketReadySent = false;
    long long m_lastClosedTime = 0;   // open time of the newest closed candle seen
    long long m_seenTime = 0;         // extremes already applied to the account
    double m_seenHigh = 0;
    double m_seenLow = 0;

    // analysis
    bool m_analyzing = false;
    int m_generation = 0;             // answers of an abandoned analysis are ignored
    QSet<QString> m_waiting;          // models that have not answered yet
    QSet<QString> m_testing;          // models being tested from the Model Panel
    QSet<QString> m_teamAsked;        // team members whose first call is still out
    QSet<QString> m_reviewing;        // team members whose final call is still out
    QString m_analysisQuestion;       // the snapshot as the models were given it
    std::vector<Trading::Verdict> m_verdicts;
    Trading::Indicators::Summary m_ta;
    Trading::Smc::Analysis m_smc;
    QString m_analysisSymbol;
    QString m_analysisInterval;
    Trading::Signal m_lastSignal;
    Trading::RiskDecision m_lastDecision;
    QVariantMap m_signal;
    struct AnalystState { QString state; QString detail; qint64 ms = 0; };
    QHash<QString, AnalystState> m_analystState;

    // account
    Trading::PaperAccount m_account;
    bool m_halted = false;
    QVariantList m_equityCurve;       // [{t, equity}]
    qint64 m_lastCurvePoint = 0;
    QVariantList m_activity;
    bool m_webViewAvailable = false;
    QString m_catalogError;
};
