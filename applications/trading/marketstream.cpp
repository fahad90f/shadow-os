// SPDX-License-Identifier: GPL-3.0-or-later
#include "marketstream.h"

#include <QDateTime>
#include <QJsonArray>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QWebSocket>

#include <cmath>

namespace {

// A service that is alive sends something at least every few seconds (the
// candle of the market on the chart is pushed every second or two).
const int kSilenceMs = 30000;

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

} // namespace

MarketStream::MarketStream(QObject *parent)
    : QObject(parent)
    , m_retry(new QTimer(this))
    , m_silence(new QTimer(this))
{
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, &MarketStream::open);
    m_silence->setSingleShot(true);
    m_silence->setInterval(kSilenceMs);
    connect(m_silence, &QTimer::timeout, this, [this] {
        // Connected on paper, but nothing arrives: start over.
        closeSocket();
        retryLater();
    });
}

MarketStream::~MarketStream()
{
    closeSocket();
}

void MarketStream::setService(MarketData::Service service)
{
    m_service = service;
}

void MarketStream::setBaseUrl(const QString &url)
{
    QString base = url.trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    const QUrl parsed(base, QUrl::StrictMode);
    const bool usable = parsed.isValid() && !parsed.host().isEmpty()
        && (parsed.scheme() == QLatin1String("wss") || parsed.scheme() == QLatin1String("ws"));
    m_baseUrl = usable ? base : QString();
}

QUrl MarketStream::streamUrl(const QString &baseUrl, const QString &symbol, const QString &interval,
                             const QStringList &watchlist)
{
    if (baseUrl.isEmpty() || symbol.isEmpty())
        return {};
    // Symbols are letters and digits (the settings accept nothing else), so
    // the stream names need no escaping.
    const QString market = symbol.toLower();
    QStringList streams{market + QStringLiteral("@aggTrade"), market + QStringLiteral("@kline_") + interval};
    for (const QString &listed : watchlist)
        streams.append(listed.toLower() + QStringLiteral("@miniTicker"));
    return QUrl(baseUrl + QStringLiteral("/stream?streams=") + streams.join(QLatin1Char('/')), QUrl::StrictMode);
}

QStringList MarketStream::krakenSubscriptions(const QString &symbol, const QString &interval,
                                              const QStringList &watchlist)
{
    const auto text = [](const QJsonObject &params) {
        return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("method"), QStringLiteral("subscribe")},
                                                           {QStringLiteral("params"), params}})
                                     .toJson(QJsonDocument::Compact));
    };
    QStringList list;
    const QString pair = MarketData::krakenPair(symbol);
    if (pair.isEmpty())
        return list;
    list.append(text({{QStringLiteral("channel"), QStringLiteral("trade")},
                      {QStringLiteral("symbol"), QJsonArray{pair}}, {QStringLiteral("snapshot"), false}}));
    const int minutes = MarketData::krakenMinutes(interval);
    if (minutes > 0) {
        list.append(text({{QStringLiteral("channel"), QStringLiteral("ohlc")},
                          {QStringLiteral("symbol"), QJsonArray{pair}}, {QStringLiteral("interval"), minutes},
                          {QStringLiteral("snapshot"), false}}));
    }
    QJsonArray pairs;
    for (const QString &listed : watchlist) {
        const QString name = MarketData::krakenPair(listed);
        if (!name.isEmpty())
            pairs.append(name);
    }
    if (!pairs.isEmpty())
        list.append(text({{QStringLiteral("channel"), QStringLiteral("ticker")}, {QStringLiteral("symbol"), pairs}}));
    return list;
}

void MarketStream::subscribe(const QString &symbol, const QString &interval, const QStringList &watchlist)
{
    QUrl url;
    QStringList subscriptions;
    if (m_service == MarketData::Service::Kraken) {
        subscriptions = krakenSubscriptions(symbol, interval, watchlist);
        if (!m_baseUrl.isEmpty() && !subscriptions.isEmpty())
            url = QUrl(m_baseUrl + QStringLiteral("/v2"), QUrl::StrictMode);
    } else {
        url = streamUrl(m_baseUrl, symbol, interval, watchlist);
    }
    if (url == m_url && subscriptions == m_subscriptions && (m_socket || m_retry->isActive()))
        return;
    m_url = url;
    m_subscriptions = subscriptions;
    m_attempt = 0;
    m_retry->stop();
    closeSocket();
    if (m_url.isValid() && !m_url.isEmpty())
        open();
}

void MarketStream::stop()
{
    m_url.clear();
    m_retry->stop();
    closeSocket();
}

void MarketStream::closeSocket()
{
    m_silence->stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        Q_EMIT connectedChanged();
    }
}

void MarketStream::retryLater()
{
    if (m_url.isEmpty())
        return;
    // 1, 2, 5, 10, then every 30 seconds.
    static const int pauses[] = {1000, 2000, 5000, 10000, 30000};
    m_retry->start(pauses[qMin(m_attempt, 4)]);
    ++m_attempt;
}

void MarketStream::open()
{
    if (m_url.isEmpty())
        return;
    closeSocket();
    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    QWebSocket *socket = m_socket;
    connect(socket, &QWebSocket::connected, this, [this, socket] {
        if (socket != m_socket)
            return;
        m_silence->start();
        for (const QString &subscription : std::as_const(m_subscriptions))
            socket->sendTextMessage(subscription);
    });
    connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &text) {
        if (socket == m_socket)
            onText(text);
    });
    const auto lost = [this, socket] {
        if (socket != m_socket)
            return;
        closeSocket();
        retryLater();
    };
    connect(socket, &QWebSocket::disconnected, this, lost);
    connect(socket, &QWebSocket::errorOccurred, this, lost);
    socket->open(m_url);
}

void MarketStream::onText(const QString &text)
{
    m_silence->start(); // a heartbeat counts: the connection is alive
    QList<Message> messages;
    if (m_service == MarketData::Service::Kraken)
        messages = parseKraken(text.toUtf8());
    else
        messages.append(parse(text.toUtf8()));
    for (const Message &message : std::as_const(messages)) {
        if (message.kind == Message::None)
            continue;
        // "Connected" means data is arriving, not that a socket is open.
        if (!m_connected) {
            m_connected = true;
            m_attempt = 0;
            Q_EMIT connectedChanged();
        }
        switch (message.kind) {
        case Message::Trade: Q_EMIT trade(message.symbol, message.price, message.quantity); break;
        case Message::Kline: Q_EMIT kline(message.symbol, message.interval, message.candle); break;
        case Message::Ticker: Q_EMIT ticker(message.ticker); break;
        case Message::None: break;
        }
    }
}

QList<MarketStream::Message> MarketStream::parseKraken(const QByteArray &json)
{
    QList<Message> list;
    const QJsonObject envelope = QJsonDocument::fromJson(json).object();
    const QString channel = envelope.value(QLatin1String("channel")).toString();
    const QString type = envelope.value(QLatin1String("type")).toString();
    const QJsonArray rows = envelope.value(QLatin1String("data")).toArray();
    static const QHash<int, QString> intervals{{1, QStringLiteral("1m")}, {5, QStringLiteral("5m")},
                                               {15, QStringLiteral("15m")}, {30, QStringLiteral("30m")},
                                               {60, QStringLiteral("1h")}, {240, QStringLiteral("4h")},
                                               {1440, QStringLiteral("1d")}};
    for (const QJsonValue &value : rows) {
        const QJsonObject data = value.toObject();
        Message message;
        message.symbol = MarketData::krakenSymbol(data.value(QLatin1String("symbol")).toString());
        if (message.symbol.isEmpty())
            continue;
        if (channel == QLatin1String("trade")) {
            if (type == QLatin1String("snapshot"))
                continue; // trades from before the connection
            if (!number(data.value(QLatin1String("price")), &message.price) || !(message.price > 0))
                continue;
            number(data.value(QLatin1String("qty")), &message.quantity);
            message.kind = Message::Trade;
        } else if (channel == QLatin1String("ohlc")) {
            Trading::Candle c;
            if (!number(data.value(QLatin1String("open")), &c.open) || !number(data.value(QLatin1String("high")), &c.high)
                || !number(data.value(QLatin1String("low")), &c.low) || !number(data.value(QLatin1String("close")), &c.close)
                || !number(data.value(QLatin1String("volume")), &c.volume))
                continue;
            if (!(c.low > 0) || c.high < c.low || c.open < c.low || c.open > c.high || c.close < c.low
                || c.close > c.high || c.volume < 0)
                continue;
            // "2026-11-25T19:00:00.000000000Z": to the second is all a candle needs.
            const QDateTime begin = QDateTime::fromString(
                data.value(QLatin1String("interval_begin")).toString().left(19) + QLatin1Char('Z'), Qt::ISODate);
            message.interval = intervals.value(data.value(QLatin1String("interval")).toInt());
            if (!begin.isValid() || message.interval.isEmpty())
                continue;
            c.openTime = begin.toMSecsSinceEpoch();
            c.closed = false; // the service does not say; a newer candle closes this one
            message.candle = c;
            message.kind = Message::Kline;
        } else if (channel == QLatin1String("ticker")) {
            MarketData::Ticker t;
            double volume = 0, vwap = 0;
            if (!number(data.value(QLatin1String("last")), &t.price) || !(t.price > 0))
                continue;
            t.symbol = message.symbol;
            number(data.value(QLatin1String("high")), &t.high);
            number(data.value(QLatin1String("low")), &t.low);
            number(data.value(QLatin1String("change_pct")), &t.changePct);
            if (number(data.value(QLatin1String("volume")), &volume) && number(data.value(QLatin1String("vwap")), &vwap))
                t.quoteVolume = volume * vwap;
            message.ticker = t;
            message.kind = Message::Ticker;
        } else {
            continue;
        }
        list.append(message);
    }
    return list;
}

MarketStream::Message MarketStream::parse(const QByteArray &json)
{
    Message message;
    const QJsonObject envelope = QJsonDocument::fromJson(json).object();
    // {"stream": "btcusdt@aggTrade", "data": {...}}; a single stream sends the data alone.
    const QJsonObject data = envelope.contains(QLatin1String("data")) ? envelope.value(QLatin1String("data")).toObject()
                                                                      : envelope;
    const QString event = data.value(QLatin1String("e")).toString();
    const QString symbol = data.value(QLatin1String("s")).toString();
    if (symbol.isEmpty())
        return message;
    if (event == QLatin1String("aggTrade") || event == QLatin1String("trade")) {
        double price = 0, quantity = 0;
        if (!number(data.value(QLatin1String("p")), &price) || !(price > 0))
            return message;
        number(data.value(QLatin1String("q")), &quantity);
        message.kind = Message::Trade;
        message.symbol = symbol;
        message.price = price;
        message.quantity = quantity;
    } else if (event == QLatin1String("kline")) {
        const QJsonObject k = data.value(QLatin1String("k")).toObject();
        Trading::Candle c;
        double openTime = 0;
        if (!number(k.value(QLatin1String("t")), &openTime) || !number(k.value(QLatin1String("o")), &c.open)
            || !number(k.value(QLatin1String("h")), &c.high) || !number(k.value(QLatin1String("l")), &c.low)
            || !number(k.value(QLatin1String("c")), &c.close) || !number(k.value(QLatin1String("v")), &c.volume))
            return message;
        // The same sanity as for the history: prices that cannot be prices are left out.
        if (!(c.low > 0) || c.high < c.low || c.open < c.low || c.open > c.high || c.close < c.low || c.close > c.high
            || c.volume < 0)
            return message;
        c.openTime = static_cast<long long>(openTime);
        c.closed = k.value(QLatin1String("x")).toBool();
        message.kind = Message::Kline;
        message.symbol = symbol;
        message.interval = k.value(QLatin1String("i")).toString();
        message.candle = c;
    } else if (event == QLatin1String("24hrMiniTicker") || event == QLatin1String("24hrTicker")) {
        MarketData::Ticker t;
        double open = 0;
        if (!number(data.value(QLatin1String("c")), &t.price) || !(t.price > 0))
            return message;
        t.symbol = symbol;
        number(data.value(QLatin1String("h")), &t.high);
        number(data.value(QLatin1String("l")), &t.low);
        number(data.value(QLatin1String("q")), &t.quoteVolume);
        // The change over 24 hours, from the price 24 hours ago.
        if (number(data.value(QLatin1String("o")), &open) && open > 0)
            t.changePct = (t.price - open) / open * 100.0;
        message.kind = Message::Ticker;
        message.symbol = symbol;
        message.ticker = t;
    }
    return message;
}
