// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "skills/candle.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

// Public market data over REST: candles and 24-hour tickers. No account and
// no key are involved; nothing here can place an order. Two services are
// understood, and the settings say which one is used and where it is:
//  - Binance's public market data (crypto, quoted in USDT), and any service
//    that speaks the same API;
//  - Kraken's public market data, for the currency pairs and gold it lists
//    (EUR/USD, GBP/USD, USD/CHF, ...), quoted in dollars.
class MarketData : public QObject
{
    Q_OBJECT
public:
    struct Ticker
    {
        QString symbol;
        double price = 0;
        double changePct = 0; // over the last 24 hours
        double high = 0;
        double low = 0;
        double quoteVolume = 0;
    };

    MarketData(QNetworkAccessManager *net, QObject *parent = nullptr);

    enum class Service { Binance, Kraken };
    void setService(Service service);
    Service service() const { return m_service; }
    void setBaseUrl(const QString &url);
    QString baseUrl() const { return m_baseUrl; }

    // A market the service said it does not have. One such name in a list of
    // tickers makes the service refuse the whole list, so it is left out of
    // every later request, for as long as the application runs.
    bool isUnlisted(const QString &symbol) const { return m_unlisted.contains(symbol); }

    // Ask for the latest 'limit' candles; candles() or failed() follows, with
    // the same 'purpose' (what the caller wants them for). A request for a
    // series and purpose that is already being fetched is dropped.
    // With 'beforeMs' the candles that end before that time (older history).
    void fetchCandles(const QString &symbol, const QString &interval, int limit,
                      const QString &purpose = QStringLiteral("chart"), qint64 beforeMs = 0);
    void fetchTickers(const QStringList &symbols);
    // The last price of every market the service lists: what can be added
    // to the watchlist. allPrices() or allPricesFailed() follows.
    void fetchAllPrices();

    // Parsers, public for the tests.
    static Trading::Candles parseKlines(const QByteArray &json, qint64 nowMs, QString *error);
    static QList<Ticker> parseTickers(const QByteArray &json, QString *error);
    static QList<QPair<QString, double>> parseAllPrices(const QByteArray &json);

    // ---- Kraken ----
    // "ZEURZUSD" -> "EURUSD", "EUR/USD" -> "EURUSD": the names the application
    // uses are the service's short names without the separators.
    static QString krakenSymbol(const QString &name);
    // "EURUSD" -> "EUR/USD" (the stream's spelling); empty when it is not a pair.
    static QString krakenPair(const QString &symbol);
    // Minutes of an interval the service offers ("15m" -> 15); 0 when it has none.
    static int krakenMinutes(const QString &interval);
    // {"error":[...],"result":{"<pair>":[[time,o,h,l,c,vwap,volume,count],...]}}.
    // 'unknown' is set when the service says it has no such pair.
    static Trading::Candles parseKrakenOhlc(const QByteArray &json, qint64 intervalMs, qint64 nowMs, QString *error,
                                            bool *unknown = nullptr);
    struct KrakenTicker { Ticker ticker; int trades24h = 0; };
    static QList<KrakenTicker> parseKrakenTickers(const QByteArray &json, QString *error, bool *unknown = nullptr);
    static QList<Ticker> krakenTickers(const QByteArray &json, QString *error, bool *unknown = nullptr);
    // A currency pair or gold against the dollar: what the forex list offers.
    // Crosses (EUR/GBP) are left out: their profit would need a second rate.
    static bool krakenForex(const QString &symbol);
    // Fewer trades than this in 24 hours and a pair's candles are a few
    // stray prices, not a market (USD/JPY there, at the time of writing).
    static constexpr int kKrakenMinTrades = 500;

Q_SIGNALS:
    void candles(const QString &symbol, const QString &interval, const Trading::Candles &series,
                 const QString &purpose);
    void tickers(const QList<MarketData::Ticker> &list);
    // what: "candles" | "tickers"
    void failed(const QString &what, const QString &message);
    // The service has no market of this name. Sent once per name.
    void unlisted(const QString &symbol);
    void allPrices(const QList<QPair<QString, double>> &prices);
    void allPricesFailed(const QString &message);

private:
    QString describe(QNetworkReply *reply) const;
    QString unlistedText(const QString &symbol) const;
    void markUnlisted(const QString &symbol);
    void fetchTicker(const QString &symbol); // one market, after a list was refused

    QNetworkAccessManager *m_net;
    Service m_service = Service::Binance;
    QString m_baseUrl;
    QHash<QString, QPointer<QNetworkReply>> m_inFlight;
    QSet<QString> m_unlisted;
    int m_singleLookups = 0; // fetchTicker() requests on their way
};
