// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "skills/candle.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

// Public market data from a Binance-compatible REST API: candles and 24-hour
// tickers. No account and no key are involved; nothing here can place an
// order. The base URL comes from the settings (default: Binance's public
// market-data host).
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

    void setBaseUrl(const QString &url) { m_baseUrl = url; }
    QString baseUrl() const { return m_baseUrl; }

    // Ask for the latest 'limit' candles; candles() or failed() follows, with
    // the same 'purpose' (what the caller wants them for). A request for a
    // series and purpose that is already being fetched is dropped.
    void fetchCandles(const QString &symbol, const QString &interval, int limit,
                      const QString &purpose = QStringLiteral("chart"));
    void fetchTickers(const QStringList &symbols);

    // Parsers, public for the tests.
    static Trading::Candles parseKlines(const QByteArray &json, qint64 nowMs, QString *error);
    static QList<Ticker> parseTickers(const QByteArray &json, QString *error);

Q_SIGNALS:
    void candles(const QString &symbol, const QString &interval, const Trading::Candles &series,
                 const QString &purpose);
    void tickers(const QList<MarketData::Ticker> &list);
    // what: "candles" | "tickers"
    void failed(const QString &what, const QString &message);

private:
    QString describe(QNetworkReply *reply) const;

    QNetworkAccessManager *m_net;
    QString m_baseUrl;
    QHash<QString, QPointer<QNetworkReply>> m_inFlight;
};
