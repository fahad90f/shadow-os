// SPDX-License-Identifier: GPL-3.0-or-later
#include "chatstream.h"

#include "tradingconfig.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace {

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString serviceMessage(const QByteArray &body)
{
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QJsonValue error = root.value(QLatin1String("error"));
    QString text = error.isObject() ? error.toObject().value(QLatin1String("message")).toString() : error.toString();
    if (text.isEmpty())
        text = root.value(QLatin1String("detail")).toString();
    if (text.isEmpty())
        text = root.value(QLatin1String("message")).toString();
    text = text.simplified();
    if (text.size() > 200)
        text = text.left(200) + QStringLiteral("…");
    return text;
}

// The text of a message's "content": a string, or a list of parts.
QString contentText(const QJsonValue &content)
{
    if (!content.isArray())
        return content.toString();
    QString text;
    const QJsonArray parts = content.toArray();
    for (const QJsonValue &part : parts)
        text += part.toObject().value(QLatin1String("text")).toString();
    return text;
}

} // namespace

ChatStream::ChatStream(TradingConfig *config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
}

ChatStream::~ChatStream()
{
    // No signals from a dying object.
    blockSignals(true);
    cancelAll();
}

void ChatStream::setLimit(int limit)
{
    m_limit = std::max(1, limit);
    pump();
}

QNetworkAccessManager *ChatStream::takeNet()
{
    return m_idleNets.isEmpty() ? new QNetworkAccessManager(this) : m_idleNets.takeLast();
}

quint64 ChatStream::start(const Request &request)
{
    const quint64 id = m_next++;
    Pending p;
    p.request = request;
    p.askedMs = nowMs();
    m_requests.insert(id, p);
    if (m_config->apiKey().isEmpty() || request.model.isEmpty()) {
        // Answered on the next turn of the event loop, like every other result.
        const bool noKey = m_config->apiKey().isEmpty();
        QTimer::singleShot(0, this, [this, id, noKey] {
            if (m_requests.contains(id))
                fail(id, noKey ? QStringLiteral("no_key") : QStringLiteral("bad_response"),
                     noKey ? tr("No API key.") : tr("No model named."));
        });
        return id;
    }
    // Waiting requests in order of priority, first come first served within one.
    int at = m_queue.size();
    while (at > 0 && m_requests.value(m_queue.at(at - 1)).request.priority < request.priority)
        --at;
    m_queue.insert(at, id);
    pump();
    return id;
}

void ChatStream::pump()
{
    while (m_active < m_limit && !m_queue.isEmpty()) {
        const quint64 id = m_queue.takeFirst();
        if (m_requests.contains(id) && !m_requests.value(id).reply)
            send(id);
    }
}

void ChatStream::send(quint64 id)
{
    Pending &p = m_requests[id];
    const QString key = m_config->apiKey();
    if (key.isEmpty()) {
        fail(id, QStringLiteral("no_key"), tr("No API key."));
        return;
    }
    const Request &r = p.request;
    const QJsonObject body{
        {QStringLiteral("model"), r.model},
        {QStringLiteral("messages"), r.messages},
        {QStringLiteral("max_tokens"), r.maxTokens},
        {QStringLiteral("temperature"), r.temperature},
        {QStringLiteral("stream"), r.stream},
    };
    QNetworkRequest req(QUrl(m_config->baseUrl() + QStringLiteral("/chat/completions")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", r.stream ? "text/event-stream" : "application/json");
    req.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    // Qt's transfer timeout counts time without any data: the silence a
    // streamed answer may have, which is what is wanted.
    req.setTransferTimeout(r.idleTimeoutMs);

    p.net = takeNet();
    ++p.attempt;
    p.buffer.clear();
    p.text.clear();
    p.sawData = p.sawWords = p.done = p.streamed = p.refusal = false;
    ++m_active;
    QNetworkReply *reply = p.net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    p.reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, id, reply] {
        const auto it = m_requests.constFind(id);
        if (it != m_requests.constEnd() && it->reply == reply)
            readMore(id);
    });
    connect(reply, &QNetworkReply::finished, this, [this, id, reply] {
        reply->deleteLater();
        const auto it = m_requests.find(id);
        if (it == m_requests.end() || it->reply != reply)
            return; // cancelled
        --m_active;
        finish(id);
        pump();
    });
    Q_EMIT started(id);
}

// The event stream so far: "data: {json}" lines, "data: [DONE]" at the end.
void ChatStream::readMore(quint64 id)
{
    Pending &p = m_requests[id];
    QNetworkReply *reply = p.reply;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray type = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    if (status != 200 || !type.contains("text/event-stream"))
        return; // read whole at the end
    p.streamed = true;
    p.buffer += reply->readAll();
    p.sawData = true;
    QString added;
    bool thought = false;
    int nl;
    while ((nl = p.buffer.indexOf('\n')) >= 0) {
        QByteArray line = p.buffer.left(nl).trimmed();
        p.buffer.remove(0, nl + 1);
        if (!line.startsWith("data:"))
            continue; // comments (": keep-alive"), event names, blank lines
        line = line.mid(5).trimmed();
        if (line == "[DONE]") {
            p.done = true;
            continue;
        }
        const QJsonObject chunk = QJsonDocument::fromJson(line).object();
        const QJsonObject choice = chunk.value(QLatin1String("choices")).toArray().at(0).toObject();
        const QJsonObject delta = choice.value(QLatin1String("delta")).toObject();
        if (!delta.value(QLatin1String("refusal")).toString().isEmpty())
            p.refusal = true;
        const QString piece = contentText(delta.value(QLatin1String("content")));
        if (!piece.isEmpty())
            added += piece;
        // A reasoning model's thinking arrives in a field of its own; it is not the answer.
        if (!delta.value(QLatin1String("reasoning_content")).toString().isEmpty()
            || !delta.value(QLatin1String("reasoning")).toString().isEmpty())
            thought = true;
    }
    if (thought && added.isEmpty() && !p.sawWords)
        Q_EMIT thinking(id);
    if (!added.isEmpty()) {
        p.text += added;
        p.sawWords = true;
        Q_EMIT delta(id, added);
    }
}

bool ChatStream::retry(quint64 id, const QString &why, int delayMs)
{
    const auto it = m_requests.find(id);
    if (it == m_requests.end() || it->sawWords || it->attempt >= it->request.attempts)
        return false;
    release(*it);
    Q_EMIT retrying(id, why);
    QTimer::singleShot(delayMs, this, [this, id] {
        const auto now = m_requests.constFind(id);
        if (now == m_requests.constEnd() || now->reply)
            return; // cancelled meanwhile
        m_queue.prepend(id);
        pump();
    });
    return true;
}

void ChatStream::release(Pending &p)
{
    if (p.net)
        m_idleNets.append(p.net);
    p.net = nullptr;
    p.reply = nullptr;
}

void ChatStream::fail(quint64 id, const QString &code, const QString &message)
{
    Pending p = m_requests.take(id);
    m_queue.removeAll(id);
    release(p);
    Q_EMIT failed(id, code, message, nowMs() - p.askedMs);
}

void ChatStream::finish(quint64 id)
{
    Pending &p = m_requests[id];
    QNetworkReply *reply = p.reply;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const int seconds = std::max(1, p.request.idleTimeoutMs / 1000);

    if (reply->error() == QNetworkReply::TimeoutError || reply->error() == QNetworkReply::OperationCanceledError) {
        if (retry(id, tr("No answer in %1 s. Asking once more.").arg(seconds), 1000))
            return;
        if (p.sawWords) { // what arrived stands, cut short
            const Pending done = m_requests.take(id);
            m_queue.removeAll(id);
            Pending copy = done;
            release(copy);
            Q_EMIT finished(id, done.text, nowMs() - done.askedMs);
            return;
        }
        fail(id, QStringLiteral("timeout"), tr("No answer in %1 s.").arg(seconds));
        return;
    }
    if (status == 0) {
        if (retry(id, tr("Could not reach NVIDIA's service. Trying once more."), 2000))
            return;
        fail(id, QStringLiteral("network"), tr("Could not reach NVIDIA's service."));
        return;
    }
    const QByteArray rest = reply->readAll();
    if (status == 429 || status >= 500) {
        bool ok = false;
        const int after = reply->rawHeader("Retry-After").trimmed().toInt(&ok);
        const int delayMs = status == 429 ? std::clamp(ok ? after : 3, 1, 20) * 1000 : 1500;
        if (retry(id, status == 429 ? tr("NVIDIA says too many requests. Asking again shortly.")
                                    : tr("NVIDIA's service has a problem (error %1). Asking once more.").arg(status),
                  delayMs))
            return;
    }
    if (status != 200) {
        if (status == 401)
            fail(id, QStringLiteral("auth"), tr("NVIDIA did not accept the API key."));
        else if (status == 403)
            fail(id, QStringLiteral("forbidden"), tr("NVIDIA refused this key for this model."));
        else if (status == 404)
            fail(id, QStringLiteral("not_found"), tr("NVIDIA does not offer this model (any more)."));
        else if (status == 429)
            fail(id, QStringLiteral("rate_limit"), tr("Too many requests, or the account is out of credit."));
        else if (status >= 500)
            fail(id, QStringLiteral("server"), tr("NVIDIA's service has a problem (error %1).").arg(status));
        else {
            const QString said = serviceMessage(rest);
            fail(id, QStringLiteral("bad_response"),
                 said.isEmpty() ? tr("NVIDIA's service answered with error %1.").arg(status)
                                : tr("NVIDIA's service answered with error %1: %2").arg(status).arg(said));
        }
        return;
    }

    if (p.streamed) {
        // The last lines may have come with the end of the reply.
        if (!rest.isEmpty()) {
            p.buffer += rest;
            if (!p.buffer.endsWith('\n'))
                p.buffer += '\n';
        }
        if (!p.buffer.isEmpty()) {
            // readMore() reads from the reply; hand it the rest directly.
            QByteArray lines = p.buffer;
            p.buffer.clear();
            QString added;
            for (const QByteArray &raw : lines.split('\n')) {
                QByteArray line = raw.trimmed();
                if (!line.startsWith("data:"))
                    continue;
                line = line.mid(5).trimmed();
                if (line == "[DONE]")
                    continue;
                const QJsonObject delta = QJsonDocument::fromJson(line).object().value(QLatin1String("choices"))
                                              .toArray().at(0).toObject().value(QLatin1String("delta")).toObject();
                added += contentText(delta.value(QLatin1String("content")));
                if (!delta.value(QLatin1String("refusal")).toString().isEmpty())
                    p.refusal = true;
            }
            if (!added.isEmpty()) {
                p.text += added;
                p.sawWords = true;
                Q_EMIT delta(id, added);
            }
        }
    } else {
        // The service answered in one piece (no stream).
        const QJsonObject message = QJsonDocument::fromJson(rest).object().value(QLatin1String("choices")).toArray()
                                        .at(0).toObject().value(QLatin1String("message")).toObject();
        if (!message.value(QLatin1String("refusal")).toString().isEmpty())
            p.refusal = true;
        p.text = contentText(message.value(QLatin1String("content")));
        if (!p.text.isEmpty() && p.request.stream) {
            p.sawWords = true;
            Q_EMIT delta(id, p.text);
        }
    }
    if (p.refusal && p.text.trimmed().isEmpty()) {
        fail(id, QStringLiteral("refusal"), tr("The model declined to answer."));
        return;
    }
    if (p.text.trimmed().isEmpty()) {
        if (retry(id, tr("The model sent an empty answer. Asking once more."), 500))
            return;
        fail(id, QStringLiteral("bad_response"), tr("The model sent an empty answer."));
        return;
    }
    Pending done = m_requests.take(id);
    m_queue.removeAll(id);
    release(done);
    Q_EMIT finished(id, done.text, nowMs() - done.askedMs);
}

void ChatStream::cancel(quint64 id)
{
    const auto it = m_requests.find(id);
    if (it == m_requests.end())
        return;
    QPointer<QNetworkReply> reply = it->reply;
    if (reply) {
        --m_active;
        it->reply = nullptr; // its finished() is ignored from now on
        reply->abort();
    }
    fail(id, QStringLiteral("cancelled"), tr("Stopped."));
    pump();
}

void ChatStream::cancelAll()
{
    const QList<quint64> ids = m_requests.keys();
    for (quint64 id : ids)
        cancel(id);
}
