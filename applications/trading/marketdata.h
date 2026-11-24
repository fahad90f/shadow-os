// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "skills/candle.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
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

    void setBaseUrl(const QString &url);
    QString baseUrl() const { return m_baseUrl; }

    // A market the service said it does not have. One such name in a list of
    // tickers makes the service refuse the whole list, so it is left out of
    // every later request, for as long as the application runs.
    bool isUnlisted(const QString &symbol) const { return m_unlisted.contains(symbol); }

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
    // The service has no market of this name. Sent once per name.
    void unlisted(const QString &symbol);

private:
    QString describe(QNetworkReply *reply) const;
    QString unlistedText(const QString &symbol) const;
    void markUnlisted(const QString &symbol);
    void fetchTicker(const QString &symbol); // one market, after a list was refused

    QNetworkAccessManager *m_net;
    QString m_baseUrl;
    QHash<QString, QPointer<QNetworkReply>> m_inFlight;
    QSet<QString> m_unlisted;
    int m_singleLookups = 0; // fetchTicker() requests on their way
};
