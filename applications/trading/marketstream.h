// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "marketdata.h"
#include "skills/candle.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QTimer;
class QWebSocket;

// Live market data pushed over a WebSocket: every trade of the market on the
// chart, its candle as it forms, and the last price of each market of the
// watchlist. Public data only: no account, no key, and nothing is ever sent
// but the subscription. Two services are understood, as in MarketData: a
// Binance-compatible one (the subscription is in the address) and Kraken's
// (the subscription is sent once the connection is made).
//
// It is an addition to MarketData, never a replacement: the history comes
// from there, and when the stream is down the engine falls back to asking
// MarketData every few seconds. The connection is made again by itself, with
// growing pauses, and when nothing has arrived for a while.
class MarketStream : public QObject
{
    Q_OBJECT
public:
    explicit MarketStream(QObject *parent = nullptr);
    ~MarketStream() override;

    void setService(MarketData::Service service);
    // "wss://host[:port]"; empty: no stream at all.
    void setBaseUrl(const QString &url);
    QString baseUrl() const { return m_baseUrl; }

    // What to listen to. Connects, or connects again when it changed.
    void subscribe(const QString &symbol, const QString &interval, const QStringList &watchlist);
    void stop();
    bool isConnected() const { return m_connected; }

    // The address the subscription is made with (public for the tests).
    static QUrl streamUrl(const QString &baseUrl, const QString &symbol, const QString &interval,
                          const QStringList &watchlist);

    struct Message
    {
        enum Kind { None, Trade, Kline, Ticker } kind = None;
        QString symbol;
        QString interval;         // Kline
        double price = 0;         // Trade
        double quantity = 0;      // Trade
        Trading::Candle candle;   // Kline
        MarketData::Ticker ticker; // Ticker
    };
    // One message of the combined stream. Anything that is not one of the
    // three kinds, or has a number that is not a number, gives kind None.
    static Message parse(const QByteArray &json);
    // One message of Kraken's stream, which can carry several trades or
    // tickers; nothing for a heartbeat, a status or an acknowledgement.
    static QList<Message> parseKraken(const QByteArray &json);
    // What is sent to Kraken's stream once connected (public for the tests).
    static QStringList krakenSubscriptions(const QString &symbol, const QString &interval,
                                           const QStringList &watchlist);

Q_SIGNALS:
    void trade(const QString &symbol, double price, double quantity);
    void kline(const QString &symbol, const QString &interval, const Trading::Candle &candle);
    void ticker(const MarketData::Ticker &ticker);
    void connectedChanged();

private:
    void open();
    void closeSocket();
    void retryLater();
    void onText(const QString &text);

    MarketData::Service m_service = MarketData::Service::Binance;
    QString m_baseUrl;
    QUrl m_url;               // what is (to be) connected; empty: nothing wanted
    QStringList m_subscriptions; // sent once connected (Kraken)
    QWebSocket *m_socket = nullptr;
    QTimer *m_retry;
    QTimer *m_silence;        // fires when nothing has arrived for too long
    int m_attempt = 0;
    bool m_connected = false;
};
