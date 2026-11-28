// SPDX-License-Identifier: GPL-3.0-or-later
#include "marketdata.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
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

// ---- OANDA --------------------------------------------------------------------------

QStringList MarketData::oandaMarkets()
{
    static const QStringList list{QStringLiteral("XAUUSD"), QStringLiteral("XAGUSD"), QStringLiteral("EURUSD"),
                                  QStringLiteral("GBPUSD"), QStringLiteral("AUDUSD"), QStringLiteral("NZDUSD"),
                                  QStringLiteral("USDJPY"), QStringLiteral("USDCHF"), QStringLiteral("USDCAD")};
    return list;
}

bool MarketData::oandaForex(const QString &symbol)
{
    return oandaMarkets().contains(symbol);
}

QString MarketData::oandaInstrument(const QString &symbol)
{
    if (!oandaForex(symbol))
        return {};
    return symbol.left(3) + QLatin1Char('_') + symbol.mid(3);
}

QString MarketData::oandaGranularity(const QString &interval)
{
    static const QHash<QString, QString> table{{QStringLiteral("1m"), QStringLiteral("M1")},
                                               {QStringLiteral("5m"), QStringLiteral("M5")},
                                               {QStringLiteral("15m"), QStringLiteral("M15")},
                                               {QStringLiteral("30m"), QStringLiteral("M30")},
                                               {QStringLiteral("1h"), QStringLiteral("H1")},
                                               {QStringLiteral("4h"), QStringLiteral("H4")},
                                               {QStringLiteral("1d"), QStringLiteral("D")}};
    return table.value(interval);
}

Trading::Candles MarketData::parseOandaCandles(const QByteArray &json, QString *error)
{
    Trading::Candles series;
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    const QJsonObject root = doc.object();
    if (!doc.isObject() || !root.value(QLatin1String("candles")).isArray()) {
        if (error) {
            *error = root.value(QLatin1String("errorMessage")).toString();
            if (error->isEmpty())
                *error = doc.isObject() ? QStringLiteral("unexpected answer") : parse.errorString();
        }
        return series;
    }
    const QJsonArray rows = root.value(QLatin1String("candles")).toArray();
    series.reserve(size_t(rows.size()));
    for (const QJsonValue &value : rows) {
        const QJsonObject o = value.toObject();
        const QJsonObject mid = o.value(QLatin1String("mid")).toObject();
        Trading::Candle c;
        double seconds = 0;
        if (!number(o.value(QLatin1String("time")), &seconds) || !number(mid.value(QLatin1String("o")), &c.open)
            || !number(mid.value(QLatin1String("h")), &c.high) || !number(mid.value(QLatin1String("l")), &c.low)
            || !number(mid.value(QLatin1String("c")), &c.close)) {
            continue; // a malformed row is left out rather than guessed at
        }
        number(o.value(QLatin1String("volume")), &c.volume); // ticks, not lots
        if (!(c.low > 0) || c.high < c.low || c.open < c.low || c.open > c.high || c.close < c.low
            || c.close > c.high || c.volume < 0) {
            continue;
        }
        c.openTime = static_cast<long long>(std::llround(seconds * 1000.0));
        c.closed = o.value(QLatin1String("complete")).toBool();
        if (!series.empty() && c.openTime <= series.back().openTime)
            continue;
        series.push_back(c);
    }
    if (series.empty() && error && rows.isEmpty())
        *error = QString();
    else if (series.empty() && error)
        *error = QStringLiteral("no candles in the answer");
    return series;
}

QNetworkRequest MarketData::oandaRequest(const QUrl &url) const
{
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    req.setRawHeader("Accept-Datetime-Format", "UNIX");
    return req;
}

QString MarketData::oandaError(QNetworkReply *reply, const QByteArray &body) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403)
        return tr("OANDA did not accept the token (error %1). Check it under Settings → Prices, and that it "
                  "belongs to the account type chosen there (practice or live).").arg(status);
    const QString said = QJsonDocument::fromJson(body).object().value(QLatin1String("errorMessage")).toString();
    if (status >= 400 && !said.isEmpty())
        return tr("OANDA answered with error %1: %2").arg(status).arg(said.left(160));
    return describe(reply);
}

void MarketData::fetchOandaCandles(const QString &symbol, const QString &interval, int limit, const QString &purpose,
                                   qint64 beforeMs)
{
    const bool essential = purpose == QLatin1String("chart");
    if (m_token.isEmpty()) {
        if (essential)
            Q_EMIT failed(QStringLiteral("candles"),
                          tr("Prices come from OANDA: add your OANDA token under Settings → Prices "
                             "(a free practice account is enough)."));
        else if (purpose == QLatin1String("older"))
            Q_EMIT failed(QStringLiteral("older"), tr("No OANDA token."));
        return;
    }
    const QString instrument = oandaInstrument(symbol);
    const QString granularity = oandaGranularity(interval);
    if (instrument.isEmpty() || granularity.isEmpty()) {
        markUnlisted(symbol);
        if (essential)
            Q_EMIT failed(QStringLiteral("candles"), unlistedText(symbol));
        return;
    }
    const QString slot = QStringLiteral("k:") + symbol + QLatin1Char(':') + interval + QLatin1Char(':') + purpose;
    if (m_inFlight.value(slot))
        return;
    QUrl url(m_baseUrl + QStringLiteral("/v3/instruments/") + instrument + QStringLiteral("/candles"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("granularity"), granularity);
    query.addQueryItem(QStringLiteral("price"), QStringLiteral("M"));
    query.addQueryItem(QStringLiteral("count"), QString::number(qBound(1, limit, 5000)));
    if (beforeMs > 0)
        query.addQueryItem(QStringLiteral("to"), QString::number(beforeMs / 1000));
    url.setQuery(query);
    QNetworkReply *reply = m_net->get(oandaRequest(url));
    m_inFlight.insert(slot, reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, slot, symbol, interval, purpose, essential] {
        reply->deleteLater();
        m_inFlight.remove(slot);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (status != 200) {
            const QString said = QJsonDocument::fromJson(body).object().value(QLatin1String("errorMessage")).toString();
            if ((status == 400 || status == 404) && said.contains(QLatin1String("instrument"), Qt::CaseInsensitive)) {
                markUnlisted(symbol);
                if (essential)
                    Q_EMIT failed(QStringLiteral("candles"), unlistedText(symbol));
            } else if (essential) {
                Q_EMIT failed(QStringLiteral("candles"), oandaError(reply, body));
            } else if (purpose == QLatin1String("older")) {
                Q_EMIT failed(QStringLiteral("older"), oandaError(reply, body));
            }
            return;
        }
        QString error;
        const Trading::Candles series = parseOandaCandles(body, &error);
        if (series.empty() && (purpose == QLatin1String("older") || error.isEmpty())) {
            // None before that time: the beginning of the history it keeps.
            if (purpose == QLatin1String("older"))
                Q_EMIT candles(symbol, interval, series, purpose);
            else if (essential)
                Q_EMIT failed(QStringLiteral("candles"), tr("OANDA has no candles of %1 yet.").arg(symbol));
            return;
        }
        if (series.empty()) {
            if (essential)
                Q_EMIT failed(QStringLiteral("candles"),
                              tr("The market data for %1 could not be read: %2").arg(symbol, error));
            return;
        }
        Q_EMIT candles(symbol, interval, series, purpose);
    });
}

void MarketData::fetchOandaTicker(const QString &symbol, bool forCatalog)
{
    const QString slot = (forCatalog ? QStringLiteral("c:") : QStringLiteral("t:")) + symbol;
    if (m_inFlight.value(slot))
        return;
    QUrl url(m_baseUrl + QStringLiteral("/v3/instruments/") + oandaInstrument(symbol) + QStringLiteral("/candles"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("granularity"), QStringLiteral("D"));
    query.addQueryItem(QStringLiteral("price"), QStringLiteral("M"));
    query.addQueryItem(QStringLiteral("count"), QStringLiteral("2"));
    url.setQuery(query);
    QNetworkReply *reply = m_net->get(oandaRequest(url));
    m_inFlight.insert(slot, reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, slot, symbol, forCatalog] {
        reply->deleteLater();
        m_inFlight.remove(slot);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        Trading::Candles days = status == 200 ? parseOandaCandles(body, nullptr) : Trading::Candles();
        Ticker t;
        if (!days.empty()) {
            const Trading::Candle &today = days.back();
            t.symbol = symbol;
            t.price = today.close;
            t.high = today.high;
            t.low = today.low;
            // Against yesterday's close (the day starts at 17:00 New York, as
            // the forex market's day does).
            const double before = days.size() > 1 ? days[days.size() - 2].close : today.open;
            t.changePct = before > 0 ? (today.close - before) / before * 100.0 : 0;
        }
        if (forCatalog) {
            if (t.price > 0)
                m_catalog.append({symbol, t.price});
            if (--m_catalogWaiting == 0) {
                QList<QPair<QString, double>> list;
                for (const QString &listed : oandaMarkets()) {
                    for (const auto &row : std::as_const(m_catalog)) {
                        if (row.first == listed)
                            list.append(row);
                    }
                }
                m_catalog.clear();
                if (list.isEmpty())
                    Q_EMIT allPricesFailed(status == 200 ? tr("The list of markets could not be read.")
                                                         : oandaError(reply, body));
                else
                    Q_EMIT allPrices(list);
            }
            return;
        }
        if (status != 200) {
            Q_EMIT failed(QStringLiteral("tickers"), oandaError(reply, body));
            return;
        }
        if (t.price > 0)
            Q_EMIT tickers({t});
    });
}

void MarketData::setService(Service service)
{
    if (service != m_service)
        m_unlisted.clear();
    m_service = service;
}

// ---- Kraken ------------------------------------------------------------------------

QString MarketData::krakenSymbol(const QString &name)
{
    QString symbol = name.toUpper();
    symbol.remove(QLatin1Char('/'));
    // The service's own keys carry a class letter before each currency of
    // its oldest pairs: ZEURZUSD, XXBTZUSD.
    if (symbol.size() == 8 && (symbol.at(0) == QLatin1Char('X') || symbol.at(0) == QLatin1Char('Z'))
        && (symbol.at(4) == QLatin1Char('X') || symbol.at(4) == QLatin1Char('Z')))
        symbol = symbol.mid(1, 3) + symbol.mid(5, 3);
    return symbol;
}

namespace {

// The currencies a pair can be made of, and the two gold tokens.
const QStringList &krakenFiat()
{
    static const QStringList list{QStringLiteral("USD"), QStringLiteral("EUR"), QStringLiteral("GBP"),
                                  QStringLiteral("JPY"), QStringLiteral("CHF"), QStringLiteral("CAD"),
                                  QStringLiteral("AUD"), QStringLiteral("NZD")};
    return list;
}

// "EURUSD" -> {"EUR", "USD"}; "PAXGUSD" -> {"PAXG", "USD"}. Empty when the
// name does not end in a currency.
QPair<QString, QString> krakenSplit(const QString &symbol)
{
    for (const QString &quote : krakenFiat()) {
        if (symbol.size() > quote.size() && symbol.endsWith(quote))
            return {symbol.left(symbol.size() - quote.size()), quote};
    }
    return {};
}

bool krakenUnknownPair(const QJsonObject &root)
{
    const QJsonArray errors = root.value(QLatin1String("error")).toArray();
    for (const QJsonValue &e : errors) {
        if (e.toString().contains(QLatin1String("Unknown asset pair")))
            return true;
    }
    return false;
}

QString krakenError(const QJsonObject &root)
{
    const QJsonArray errors = root.value(QLatin1String("error")).toArray();
    QStringList said;
    for (const QJsonValue &e : errors)
        said.append(e.toString());
    return said.join(QStringLiteral("; ")).left(200);
}

} // namespace

QString MarketData::krakenPair(const QString &symbol)
{
    const QPair<QString, QString> parts = krakenSplit(symbol);
    return parts.first.isEmpty() ? QString() : parts.first + QLatin1Char('/') + parts.second;
}

bool MarketData::krakenForex(const QString &symbol)
{
    const QPair<QString, QString> parts = krakenSplit(symbol);
    if (parts.first.isEmpty())
        return false;
    const bool gold = parts.first == QLatin1String("PAXG") || parts.first == QLatin1String("XAUT");
    if (gold)
        return parts.second == QLatin1String("USD");
    // The dollar on one side, another currency on the other.
    return krakenFiat().contains(parts.first) && parts.first != parts.second
        && (parts.first == QLatin1String("USD") || parts.second == QLatin1String("USD"));
}

int MarketData::krakenMinutes(const QString &interval)
{
    static const QHash<QString, int> minutes{{QStringLiteral("1m"), 1}, {QStringLiteral("5m"), 5},
                                             {QStringLiteral("15m"), 15}, {QStringLiteral("30m"), 30},
                                             {QStringLiteral("1h"), 60}, {QStringLiteral("4h"), 240},
                                             {QStringLiteral("1d"), 1440}};
    return minutes.value(interval, 0);
}

Trading::Candles MarketData::parseKrakenOhlc(const QByteArray &json, qint64 intervalMs, qint64 nowMs,
                                             QString *error, bool *unknown)
{
    Trading::Candles series;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (unknown)
        *unknown = krakenUnknownPair(root);
    const QJsonObject result = root.value(QLatin1String("result")).toObject();
    QJsonArray rows;
    for (auto it = result.constBegin(); it != result.constEnd(); ++it) {
        if (it.value().isArray()) { // the pair; "last" beside it is a number
            rows = it.value().toArray();
            break;
        }
    }
    if (rows.isEmpty()) {
        if (error) {
            *error = krakenError(root);
            if (error->isEmpty())
                *error = QStringLiteral("no candles in the answer");
        }
        return series;
    }
    series.reserve(size_t(rows.size()));
    for (const QJsonValue &rowValue : std::as_const(rows)) {
        const QJsonArray row = rowValue.toArray();
        Trading::Candle c;
        double seconds = 0;
        // [time, open, high, low, close, vwap, volume, count]
        if (row.size() < 7 || !number(row.at(0), &seconds) || !number(row.at(1), &c.open)
            || !number(row.at(2), &c.high) || !number(row.at(3), &c.low) || !number(row.at(4), &c.close)
            || !number(row.at(6), &c.volume)) {
            continue;
        }
        if (!(c.low > 0) || c.high < c.low || c.open < c.low || c.open > c.high || c.close < c.low
            || c.close > c.high || c.volume < 0) {
            continue;
        }
        c.openTime = static_cast<long long>(seconds) * 1000;
        c.closed = c.openTime + intervalMs <= nowMs;
        if (!series.empty() && c.openTime <= series.back().openTime)
            continue;
        series.push_back(c);
    }
    if (series.empty() && error)
        *error = QStringLiteral("no candles in the answer");
    return series;
}

QList<MarketData::KrakenTicker> MarketData::parseKrakenTickers(const QByteArray &json, QString *error, bool *unknown)
{
    QList<KrakenTicker> list;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (unknown)
        *unknown = krakenUnknownPair(root);
    const QJsonObject result = root.value(QLatin1String("result")).toObject();
    for (auto it = result.constBegin(); it != result.constEnd(); ++it) {
        const QJsonObject o = it.value().toObject();
        KrakenTicker row;
        Ticker &t = row.ticker;
        t.symbol = krakenSymbol(it.key());
        // c: [last price, lot]; o: today's opening price; h, l, v, t: [today, last 24 hours].
        double open = 0, volume = 0, vwap = 0;
        if (t.symbol.isEmpty() || !number(o.value(QLatin1String("c")).toArray().at(0), &t.price) || !(t.price > 0))
            continue;
        if (number(o.value(QLatin1String("o")), &open) && open > 0)
            t.changePct = (t.price - open) / open * 100.0;
        number(o.value(QLatin1String("h")).toArray().at(1), &t.high);
        number(o.value(QLatin1String("l")).toArray().at(1), &t.low);
        if (number(o.value(QLatin1String("v")).toArray().at(1), &volume)
            && number(o.value(QLatin1String("p")).toArray().at(1), &vwap))
            t.quoteVolume = volume * vwap;
        row.trades24h = o.value(QLatin1String("t")).toArray().at(1).toInt();
        list.append(row);
    }
    if (list.isEmpty() && error) {
        *error = krakenError(root);
        if (error->isEmpty())
            *error = QStringLiteral("no prices in the answer");
    }
    return list;
}

QList<MarketData::Ticker> MarketData::krakenTickers(const QByteArray &json, QString *error, bool *unknown)
{
    QList<Ticker> list;
    const QList<KrakenTicker> rows = parseKrakenTickers(json, error, unknown);
    for (const KrakenTicker &row : rows)
        list.append(row.ticker);
    return list;
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

void MarketData::fetchCandles(const QString &symbol, const QString &interval, int limit, const QString &purpose,
                              qint64 beforeMs)
{
    // Only the chart's own candles are worth an error on screen; a
    // sparkline that did not arrive is simply not drawn.
    const bool essential = purpose == QLatin1String("chart");
    if (m_unlisted.contains(symbol)) {
        if (essential)
            Q_EMIT failed(QStringLiteral("candles"), unlistedText(symbol));
        return;
    }
    if (m_service == Service::Oanda) {
        fetchOandaCandles(symbol, interval, limit, purpose, beforeMs);
        return;
    }
    const QString slot = QStringLiteral("k:") + symbol + QLatin1Char(':') + interval + QLatin1Char(':') + purpose;
    if (m_inFlight.value(slot))
        return;
    const bool kraken = m_service == Service::Kraken;
    QUrl url(m_baseUrl + (kraken ? QStringLiteral("/0/public/OHLC") : QStringLiteral("/api/v3/klines")));
    QUrlQuery query;
    if (kraken) {
        query.addQueryItem(QStringLiteral("pair"), symbol);
        query.addQueryItem(QStringLiteral("interval"), QString::number(krakenMinutes(interval)));
    } else {
        query.addQueryItem(QStringLiteral("symbol"), symbol);
        query.addQueryItem(QStringLiteral("interval"), interval);
        query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 1000)));
        if (beforeMs > 0)
            query.addQueryItem(QStringLiteral("endTime"), QString::number(beforeMs));
    }
    url.setQuery(query);
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_net->get(req);
    m_inFlight.insert(slot, reply);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, slot, symbol, interval, purpose, essential, kraken, limit] {
        reply->deleteLater();
        m_inFlight.remove(slot);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (kraken && status == 200) {
            QString error;
            bool unknown = false;
            Trading::Candles series = parseKrakenOhlc(reply->readAll(), qint64(krakenMinutes(interval)) * 60000,
                                                      QDateTime::currentMSecsSinceEpoch(), &error, &unknown);
            if (unknown) {
                markUnlisted(symbol);
                if (essential)
                    Q_EMIT failed(QStringLiteral("candles"), unlistedText(symbol));
                return;
            }
            if (series.empty()) {
                if (essential)
                    Q_EMIT failed(QStringLiteral("candles"),
                                  tr("The market data for %1 could not be read: %2").arg(symbol, error));
                return;
            }
            // The service always sends all it has: the newest 720 candles of
            // the interval, and nothing before them. The caller asked for the
            // newest 'limit'; asked for older ones, it gets them all and
            // keeps what it does not have yet.
            if (purpose != QLatin1String("older") && limit > 0 && series.size() > size_t(limit))
                series.erase(series.begin(), series.end() - limit);
            Q_EMIT candles(symbol, interval, series, purpose);
            return;
        }
        if (status != 200) {
            if (invalidSymbol(status, reply->readAll())) {
                markUnlisted(symbol);
                if (essential)
                    Q_EMIT failed(QStringLiteral("candles"), unlistedText(symbol));
            } else if (essential) {
                Q_EMIT failed(QStringLiteral("candles"), describe(reply));
            } else if (purpose == QLatin1String("older")) {
                Q_EMIT failed(QStringLiteral("older"), describe(reply));
            }
            return;
        }
        QString error;
        const Trading::Candles series = parseKlines(reply->readAll(), QDateTime::currentMSecsSinceEpoch(), &error);
        if (series.empty() && purpose == QLatin1String("older")) {
            // No candles before that time: the beginning of the market's history.
            Q_EMIT candles(symbol, interval, series, purpose);
            return;
        }
        if (series.empty()) {
            if (essential)
                Q_EMIT failed(QStringLiteral("candles"),
                              tr("The market data for %1 could not be read: %2").arg(symbol, error));
            return;
        }
        Q_EMIT candles(symbol, interval, series, purpose);
    });
}

QList<QPair<QString, double>> MarketData::parseAllPrices(const QByteArray &json)
{
    // [{"symbol": "BTCUSDT", "price": "59207.12"}, …]
    QList<QPair<QString, double>> list;
    const QJsonArray rows = QJsonDocument::fromJson(json).array();
    list.reserve(rows.size());
    for (const QJsonValue &value : rows) {
        const QJsonObject o = value.toObject();
        const QString symbol = o.value(QLatin1String("symbol")).toString();
        double price = 0;
        if (!symbol.isEmpty() && number(o.value(QLatin1String("price")), &price) && price > 0)
            list.append({symbol, price});
    }
    return list;
}

void MarketData::fetchAllPrices()
{
    if (m_service == Service::Oanda) {
        if (m_catalogWaiting > 0)
            return;
        if (m_token.isEmpty()) {
            Q_EMIT allPricesFailed(tr("Add your OANDA token under Settings → Prices first."));
            return;
        }
        m_catalog.clear();
        m_catalogWaiting = int(oandaMarkets().size());
        for (const QString &symbol : oandaMarkets())
            fetchOandaTicker(symbol, true);
        return;
    }
    if (m_inFlight.value(QStringLiteral("all")))
        return;
    const bool kraken = m_service == Service::Kraken;
    QNetworkRequest req(QUrl(m_baseUrl + (kraken ? QStringLiteral("/0/public/Ticker")
                                                 : QStringLiteral("/api/v3/ticker/price"))));
    req.setTransferTimeout(20000);
    QNetworkReply *reply = m_net->get(req);
    m_inFlight.insert(QStringLiteral("all"), reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, kraken] {
        reply->deleteLater();
        m_inFlight.remove(QStringLiteral("all"));
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            Q_EMIT allPricesFailed(describe(reply));
            return;
        }
        QList<QPair<QString, double>> list;
        if (kraken) {
            // Of everything the service trades: the currency pairs and gold
            // against the dollar that trade often enough to chart.
            const QList<KrakenTicker> all = parseKrakenTickers(reply->readAll(), nullptr);
            for (const KrakenTicker &row : all) {
                if (krakenForex(row.ticker.symbol) && row.trades24h >= kKrakenMinTrades)
                    list.append({row.ticker.symbol, row.ticker.price});
            }
            std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        } else {
            list = parseAllPrices(reply->readAll());
        }
        if (list.isEmpty())
            Q_EMIT allPricesFailed(tr("The list of markets could not be read."));
        else
            Q_EMIT allPrices(list);
    });
}

void MarketData::fetchTicker(const QString &symbol)
{
    const QString slot = QStringLiteral("t:") + symbol;
    if (m_inFlight.value(slot))
        return;
    const bool kraken = m_service == Service::Kraken;
    QUrl url(m_baseUrl + (kraken ? QStringLiteral("/0/public/Ticker") : QStringLiteral("/api/v3/ticker/24hr")));
    QUrlQuery query;
    query.addQueryItem(kraken ? QStringLiteral("pair") : QStringLiteral("symbol"), symbol);
    url.setQuery(query);
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_net->get(req);
    m_inFlight.insert(slot, reply);
    ++m_singleLookups;
    connect(reply, &QNetworkReply::finished, this, [this, reply, slot, symbol, kraken] {
        reply->deleteLater();
        m_inFlight.remove(slot);
        --m_singleLookups;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (kraken) {
            bool unknown = false;
            const QList<Ticker> found = krakenTickers(body, nullptr, &unknown);
            if (unknown)
                markUnlisted(symbol);
            else if (status == 200 && !found.isEmpty())
                Q_EMIT tickers(found);
            return;
        }
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
    if (m_service == Service::Oanda) {
        if (m_token.isEmpty()) {
            Q_EMIT failed(QStringLiteral("tickers"),
                          tr("Prices come from OANDA: add your OANDA token under Settings → Prices "
                             "(a free practice account is enough)."));
            return;
        }
        for (const QString &symbol : std::as_const(symbols))
            fetchOandaTicker(symbol, false);
        return;
    }
    // While single lookups are sorting out a refused list, the list would
    // only be refused again.
    if (symbols.isEmpty() || m_inFlight.value(QStringLiteral("t")) || m_singleLookups > 0)
        return;
    const bool kraken = m_service == Service::Kraken;
    QUrl url;
    if (kraken) {
        url = QUrl(m_baseUrl + QStringLiteral("/0/public/Ticker"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("pair"), symbols.join(QLatin1Char(',')));
        url.setQuery(query);
    } else {
        // symbols=["BTCUSDT","ETHUSDT"], percent-encoded once, by hand: the list
        // contains brackets and quotes, which must not be encoded a second time.
        const QByteArray wanted = QJsonDocument(QJsonArray::fromStringList(symbols)).toJson(QJsonDocument::Compact);
        url = QUrl(m_baseUrl + QStringLiteral("/api/v3/ticker/24hr?symbols=")
                       + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromUtf8(wanted))),
                   QUrl::StrictMode);
    }
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_net->get(req);
    m_inFlight.insert(QStringLiteral("t"), reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, symbols, kraken] {
        reply->deleteLater();
        m_inFlight.remove(QStringLiteral("t"));
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (kraken && status == 200) {
            QString error;
            bool unknown = false;
            const QList<Ticker> found = krakenTickers(reply->readAll(), &error, &unknown);
            if (unknown) {
                // One unknown name refuses the whole list here too.
                if (symbols.size() == 1) {
                    markUnlisted(symbols.first());
                } else {
                    for (const QString &symbol : symbols)
                        fetchTicker(symbol);
                }
            } else if (found.isEmpty()) {
                Q_EMIT failed(QStringLiteral("tickers"), tr("The prices could not be read: %1").arg(error));
            } else {
                Q_EMIT tickers(found);
            }
            return;
        }
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
