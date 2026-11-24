// SPDX-License-Identifier: GPL-3.0-or-later
#include "marketdata.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>

namespace {

// The API sends numbers as strings ("92641.32000000").
bool number(const QJsonValue &value, double *out)
{
    bool ok = false;
    const double v = value.isString() ? value.toString().toDouble(&ok) : value.toDouble(std::nan(""));
    if (!value.isString())
        ok = value.isDouble();
    if (!ok || !std::isfinite(v))
        return false;
    *out = v;
    return true;
}

// {"code":-1121,"msg":"Invalid symbol."} with status 400: no such market.
bool invalidSymbol(int status, const QByteArray &body)
{
    return status == 400
        && QJsonDocument::fromJson(body).object().value(QLatin1String("code")).toInt() == -1121;
}

} // namespace

MarketData::MarketData(QNetworkAccessManager *net, QObject *parent)
    : QObject(parent)
    , m_net(net)
{
}

void MarketData::setBaseUrl(const QString &url)
{
    if (url != m_baseUrl)
        m_unlisted.clear(); // another service lists other markets
    m_baseUrl = url;
}

QString MarketData::unlistedText(const QString &symbol) const
{
    return tr("%1 is not a market the data service lists. Check the watchlist in the settings file.").arg(symbol);
}

void MarketData::markUnlisted(const QString &symbol)
{
    if (m_unlisted.contains(symbol))
        return;
    m_unlisted.insert(symbol);
    Q_EMIT unlisted(symbol);
}

Trading::Candles MarketData::parseKlines(const QByteArray &json, qint64 nowMs, QString *error)
{
    Trading::Candles series;
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    if (!doc.isArray()) {
        if (error)
            *error = doc.isObject() ? doc.object().value(QLatin1String("msg")).toString()
                                    : parse.errorString();
        if (error && error->isEmpty())
            *error = QStringLiteral("unexpected answer");
        return series;
    }
    // [openTime, open, high, low, close, volume, closeTime, …] per candle.
    const QJsonArray rows = doc.array();
    series.reserve(size_t(rows.size()));
    for (const QJsonValue &rowValue : rows) {
        const QJsonArray row = rowValue.toArray();
        Trading::Candle c;
        double openTime = 0, closeTime = 0;
        if (row.size() < 7 || !number(row.at(0), &openTime) || !number(row.at(1), &c.open)
            || !number(row.at(2), &c.high) || !number(row.at(3), &c.low) || !number(row.at(4), &c.close)
            || !number(row.at(5), &c.volume) || !number(row.at(6), &closeTime)) {
            continue; // a malformed row is left out rather than guessed at
        }
        // Prices that cannot be prices would poison every indicator.
        if (!(c.low > 0) || c.high < c.low || c.open < c.low || c.open > c.high || c.close < c.low
            || c.close > c.high || c.volume < 0) {
            continue;
        }
        c.openTime = static_cast<long long>(openTime);
        c.closed = static_cast<qint64>(closeTime) < nowMs;
        if (!series.empty() && c.openTime <= series.back().openTime)
            continue; // out of order or repeated
        series.push_back(c);
    }
    if (series.empty() && error)
        *error = QStringLiteral("no candles in the answer");
    return series;
}

QList<MarketData::Ticker> MarketData::parseTickers(const QByteArray &json, QString *error)
{
    QList<Ticker> list;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonArray rows = doc.array();
    if (doc.isObject()) {
        if (doc.object().contains(QLatin1String("symbol"))) {
            rows = QJsonArray{doc.object()}; // a single ticker
        } else {
            if (error)
                *error = doc.object().value(QLatin1String("msg")).toString();
            if (error && error->isEmpty())
                *error = QStringLiteral("unexpected answer");
            return list;
        }
    }
    for (const QJsonValue &value : std::as_const(rows)) {
        const QJsonObject o = value.toObject();
        Ticker t;
        t.symbol = o.value(QLatin1String("symbol")).toString();
        if (t.symbol.isEmpty() || !number(o.value(QLatin1String("lastPrice")), &t.price) || !(t.price > 0))
            continue;
        number(o.value(QLatin1String("priceChangePercent")), &t.changePct);
        number(o.value(QLatin1String("highPrice")), &t.high);
        number(o.value(QLatin1String("lowPrice")), &t.low);
        number(o.value(QLatin1String("quoteVolume")), &t.quoteVolume);
        list.append(t);
    }
    if (list.isEmpty() && error)
        *error = QStringLiteral("no prices in the answer");
    return list;
}

QString MarketData::describe(QNetworkReply *reply) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 451 || status == 403)
        return tr("The market data service refuses requests from this location (error %1).").arg(status);
    if (status == 429 || status == 418)
        return tr("The market data service asked us to slow down.");
    if (status >= 400)
        return tr("The market data service answered with error %1.").arg(status);
    if (reply->error() == QNetworkReply::TimeoutError || reply->error() == QNetworkReply::OperationCanceledError)
        return tr("The market data service did not answer in time.");
    return tr("Could not reach the market data service. Check the network connection.");
}

void MarketData::fetchCandles(const QString &symbol, const QString &interval, int limit, const QString &purpose)
{
    // Only the chart's own candles are worth an error on screen; a
    // sparkline that did not arrive is simply not drawn.
    const bool essential = purpose == QLatin1String("chart");
    if (m_unlisted.contains(symbol)) {
        if (essential)
            Q_EMIT failed(QStringLiteral("candles"), unlistedText(symbol));
        return;
    }
    const QString slot = QStringLiteral("k:") + symbol + QLatin1Char(':') + interval + QLatin1Char(':') + purpose;
    if (m_inFlight.value(slot))
        return;
    QUrl url(m_baseUrl + QStringLiteral("/api/v3/klines"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("symbol"), symbol);
    query.addQueryItem(QStringLiteral("interval"), interval);
    query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 1000)));
    url.setQuery(query);
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_net->get(req);
    m_inFlight.insert(slot, reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, slot, symbol, interval, purpose, essential] {
        reply->deleteLater();
        m_inFlight.remove(slot);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            if (invalidSymbol(status, reply->readAll())) {
                markUnlisted(symbol);
                if (essential)
                    Q_EMIT failed(QStringLiteral("candles"), unlistedText(symbol));
            } else if (essential) {
                Q_EMIT failed(QStringLiteral("candles"), describe(reply));
            }
            return;
        }
        QString error;
        const Trading::Candles series = parseKlines(reply->readAll(), QDateTime::currentMSecsSinceEpoch(), &error);
        if (series.empty()) {
            if (essential)
                Q_EMIT failed(QStringLiteral("candles"),
                              tr("The market data for %1 could not be read: %2").arg(symbol, error));
            return;
        }
        Q_EMIT candles(symbol, interval, series, purpose);
    });
}

void MarketData::fetchTicker(const QString &symbol)
{
    const QString slot = QStringLiteral("t:") + symbol;
    if (m_inFlight.value(slot))
        return;
    QUrl url(m_baseUrl + QStringLiteral("/api/v3/ticker/24hr"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("symbol"), symbol);
    url.setQuery(query);
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_net->get(req);
    m_inFlight.insert(slot, reply);
    ++m_singleLookups;
    connect(reply, &QNetworkReply::finished, this, [this, reply, slot, symbol] {
        reply->deleteLater();
        m_inFlight.remove(slot);
        --m_singleLookups;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (invalidSymbol(status, body)) {
            markUnlisted(symbol);
            return;
        }
        // Any other failure is left for the next round, which asks again.
        const QList<Ticker> list = status == 200 ? parseTickers(body, nullptr) : QList<Ticker>();
        if (!list.isEmpty())
            Q_EMIT tickers(list);
    });
}

void MarketData::fetchTickers(const QStringList &listed)
{
    QStringList symbols;
    for (const QString &symbol : listed) {
        if (!m_unlisted.contains(symbol))
            symbols.append(symbol);
    }
    // While single lookups are sorting out a refused list, the list would
    // only be refused again.
    if (symbols.isEmpty() || m_inFlight.value(QStringLiteral("t")) || m_singleLookups > 0)
        return;
    // symbols=["BTCUSDT","ETHUSDT"], percent-encoded once, by hand: the list
    // contains brackets and quotes, which must not be encoded a second time.
    const QByteArray wanted = QJsonDocument(QJsonArray::fromStringList(symbols)).toJson(QJsonDocument::Compact);
    const QUrl url(m_baseUrl + QStringLiteral("/api/v3/ticker/24hr?symbols=")
                       + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromUtf8(wanted))),
                   QUrl::StrictMode);
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_net->get(req);
    m_inFlight.insert(QStringLiteral("t"), reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, symbols] {
        reply->deleteLater();
        m_inFlight.remove(QStringLiteral("t"));
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            // One unknown name refuses the whole list, and the answer does
            // not say which: ask for each market on its own to find it.
            if (invalidSymbol(status, reply->readAll())) {
                if (symbols.size() == 1) {
                    markUnlisted(symbols.first());
                } else {
                    for (const QString &symbol : symbols)
                        fetchTicker(symbol);
                }
                return;
            }
            Q_EMIT failed(QStringLiteral("tickers"), describe(reply));
            return;
        }
        QString error;
        const QList<Ticker> list = parseTickers(reply->readAll(), &error);
        if (list.isEmpty()) {
            Q_EMIT failed(QStringLiteral("tickers"), tr("The prices could not be read: %1").arg(error));
            return;
        }
        Q_EMIT tickers(list);
    });
}
