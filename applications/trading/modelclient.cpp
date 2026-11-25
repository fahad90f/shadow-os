// SPDX-License-Identifier: GPL-3.0-or-later
#include "modelclient.h"

#include "tradingconfig.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace {

// Room for a model that reasons before it answers; the answer itself is a few
// hundred tokens.
const int kMaxTokens = 8192;

// What the service said went wrong, shortened.
QString serviceMessage(const QByteArray &body)
{
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QJsonValue error = root.value(QLatin1String("error"));
    QString text = error.isObject() ? error.toObject().value(QLatin1String("message")).toString()
                                    : error.toString();
    if (text.isEmpty())
        text = root.value(QLatin1String("detail")).toString();
    if (text.isEmpty())
        text = root.value(QLatin1String("message")).toString();
    if (text.isEmpty())
        text = root.value(QLatin1String("title")).toString();
    text = text.simplified();
    if (text.size() > 200)
        text = text.left(200) + QStringLiteral("…");
    return text;
}

} // namespace

ModelClient::ModelClient(TradingConfig *config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_listNet(new QNetworkAccessManager(this))
{
}

ModelClient::~ModelClient()
{
    cancelAll();
}

QNetworkAccessManager *ModelClient::takeNet()
{
    return m_idleNets.isEmpty() ? new QNetworkAccessManager(this) : m_idleNets.takeLast();
}

void ModelClient::listModels()
{
    if (m_listing)
        return; // its result answers this call too
    QNetworkRequest req(QUrl(m_config->baseUrl() + QStringLiteral("/models")));
    req.setTransferTimeout(20000);
    // NVIDIA's list is public; the key is sent when there is one.
    const QString key = m_config->apiKey();
    if (!key.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    QNetworkReply *reply = m_listNet->get(req);
    m_listing = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QStringList ids;
        QString error;
        if (status == 0) {
            error = tr("Could not reach NVIDIA's service.");
        } else if (status == 401 || status == 403) {
            error = tr("NVIDIA did not accept the API key.");
        } else if (status != 200) {
            error = tr("NVIDIA's service answered with error %1.").arg(status);
        } else {
            const QJsonArray data = QJsonDocument::fromJson(reply->readAll()).object()
                                        .value(QLatin1String("data")).toArray();
            for (const QJsonValue &v : data) {
                const QString id = v.toObject().value(QLatin1String("id")).toString();
                if (!id.isEmpty() && id.size() <= 160)
                    ids.append(id);
            }
            ids.sort();
            if (ids.isEmpty())
                error = tr("NVIDIA's service listed no models.");
        }
        if (error.isEmpty()) {
            m_catalog = ids;
            m_catalogKnown = true;
        }
        Q_EMIT modelsListed(ids, error);
    });
}

void ModelClient::ask(const Question &q)
{
    if (q.model.isEmpty()) {
        Q_EMIT failed(q.model, QStringLiteral("bad_response"), tr("No model named."), 0);
        return;
    }
    if (m_pending.contains(q.model)) {
        Q_EMIT failed(q.model, QStringLiteral("busy"), tr("Still working on the previous question."), 0);
        return;
    }
    if (m_config->apiKey().isEmpty()) {
        Q_EMIT failed(q.model, QStringLiteral("no_key"), tr("No API key."), 0);
        return;
    }
    Pending pending;
    pending.question = q;
    pending.startedMs = QDateTime::currentMSecsSinceEpoch();
    pending.token = m_nextToken++;
    m_pending.insert(q.model, pending);
    m_queue.append(q.model);
    pump();
}

// Send the waiting questions, as many as may be with the service at a time.
void ModelClient::pump()
{
    const int limit = m_config->modelParallel();
    while (m_inFlight < limit && !m_queue.isEmpty()) {
        const QString model = m_queue.takeFirst();
        const auto it = m_pending.constFind(model);
        if (it != m_pending.constEnd() && !it->reply)
            send(model);
    }
}

void ModelClient::send(const QString &model)
{
    Pending &pending = m_pending[model];
    const Question &q = pending.question;
    const QString key = m_config->apiKey();
    if (key.isEmpty()) { // removed while the question waited
        fail(model, QStringLiteral("no_key"), tr("No API key."));
        return;
    }

    QJsonValue userContent = q.user;
    if (!q.imagePng.isEmpty()) {
        userContent = QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), q.user}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                        {QStringLiteral("image_url"),
                         QJsonObject{{QStringLiteral("url"), QStringLiteral("data:image/png;base64,")
                                                                 + QString::fromLatin1(q.imagePng.toBase64())}}}}};
    }
    const QJsonObject body{
        {QStringLiteral("model"), q.model},
        {QStringLiteral("messages"), QJsonArray{
            QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), q.system}},
            QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), userContent}}}},
        {QStringLiteral("max_tokens"), kMaxTokens},
        // Low, so the same snapshot gets much the same verdict twice.
        {QStringLiteral("temperature"), 0.2},
        {QStringLiteral("stream"), false},
    };

    QNetworkRequest req(QUrl(m_config->baseUrl() + QStringLiteral("/chat/completions")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    // Nothing arrives before the whole answer does, so this is how long the
    // model is given.
    pending.waitMs = q.timeoutMs > 0 ? q.timeoutMs : m_config->modelTimeoutSeconds() * 1000;
    req.setTransferTimeout(pending.waitMs);

    pending.net = takeNet();
    ++pending.attempt;
    ++m_inFlight;
    QNetworkReply *reply = pending.net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    pending.reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, model] {
        reply->deleteLater();
        const auto current = m_pending.find(model);
        if (current == m_pending.end() || current->reply != reply)
            return; // cancelled
        --m_inFlight;
        m_idleNets.append(current->net);
        current->net = nullptr;
        current->reply = nullptr;
        finish(model, reply);
        pump();
    });
}

bool ModelClient::retry(const QString &model, const QString &why, int delayMs)
{
    const auto it = m_pending.constFind(model);
    if (it == m_pending.constEnd() || it->attempt >= it->question.attempts)
        return false;
    const quint64 token = it->token;
    Q_EMIT retrying(model, why);
    QTimer::singleShot(delayMs, this, [this, model, token] {
        const auto now = m_pending.constFind(model);
        if (now == m_pending.constEnd() || now->token != token || now->reply)
            return; // cancelled meanwhile
        m_queue.append(model);
        pump();
    });
    return true;
}

void ModelClient::fail(const QString &model, const QString &code, const QString &message)
{
    const Pending pending = m_pending.take(model);
    m_queue.removeAll(model);
    if (pending.net)
        m_idleNets.append(pending.net);
    Q_EMIT failed(model, code, message, QDateTime::currentMSecsSinceEpoch() - pending.startedMs);
}

void ModelClient::finish(const QString &model, QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();

    const Pending &asked = m_pending[model];
    const int seconds = std::max(1, asked.waitMs / 1000);
    if (reply->error() == QNetworkReply::TimeoutError || reply->error() == QNetworkReply::OperationCanceledError) {
        if (retry(model, tr("No answer in %1 s. Asking once more.").arg(seconds), 1000))
            return;
        fail(model, QStringLiteral("timeout"),
             asked.attempt > 1 ? tr("No answer in %1 s, asked twice.").arg(seconds)
                               : tr("No answer in %1 s.").arg(seconds));
        return;
    }
    if (status == 0) {
        if (retry(model, tr("Could not reach NVIDIA's service. Trying once more."), 2000))
            return;
        fail(model, QStringLiteral("network"), tr("Could not reach NVIDIA's service."));
        return;
    }
    if (status == 429 || status >= 500) {
        // "Too many requests" says when to come back; not later than half a minute.
        bool ok = false;
        const int after = reply->rawHeader("Retry-After").trimmed().toInt(&ok);
        const int delayMs = status == 429 ? std::clamp(ok ? after : 5, 1, 30) * 1000 : 2000;
        if (retry(model, status == 429 ? tr("NVIDIA says too many requests. Asking again shortly.")
                                       : tr("NVIDIA's service has a problem (error %1). Asking once more.").arg(status),
                  delayMs))
            return;
    }
    if (status != 200) {
        // For a rejected key the service's own words are not repeated: a
        // service may quote part of the key back.
        if (status == 401)
            fail(model, QStringLiteral("auth"), tr("NVIDIA did not accept the API key."));
        else if (status == 403)
            fail(model, QStringLiteral("forbidden"), tr("NVIDIA refused this key for this model."));
        else if (status == 404)
            fail(model, QStringLiteral("not_found"), tr("NVIDIA does not offer this model (any more)."));
        else if (status == 429)
            fail(model, QStringLiteral("rate_limit"), tr("Too many requests, or the account is out of credit."));
        else if (status >= 500)
            fail(model, QStringLiteral("server"), tr("NVIDIA's service has a problem (error %1).").arg(status));
        else {
            const QString said = serviceMessage(raw);
            fail(model, QStringLiteral("bad_response"),
                 said.isEmpty() ? tr("NVIDIA's service answered with error %1.").arg(status)
                                : tr("NVIDIA's service answered with error %1: %2").arg(status).arg(said));
        }
        return;
    }

    const QJsonObject message = QJsonDocument::fromJson(raw).object().value(QLatin1String("choices")).toArray()
                                    .at(0).toObject().value(QLatin1String("message")).toObject();
    if (!message.value(QLatin1String("refusal")).toString().isEmpty()) {
        fail(model, QStringLiteral("refusal"), tr("The model declined to answer."));
        return;
    }
    // The answer is "content". A reasoning model's thinking arrives in a
    // field of its own ("reasoning_content") and is not the answer.
    QString text;
    const QJsonValue content = message.value(QLatin1String("content"));
    if (content.isArray()) { // some models answer in parts
        const QJsonArray parts = content.toArray();
        for (const QJsonValue &part : parts)
            text += part.toObject().value(QLatin1String("text")).toString();
    } else {
        text = content.toString();
    }
    if (text.trimmed().isEmpty()) {
        fail(model, QStringLiteral("bad_response"), tr("The model sent an empty answer."));
        return;
    }
    const Pending done = m_pending.take(model);
    Q_EMIT answered(model, text, QDateTime::currentMSecsSinceEpoch() - done.startedMs);
}

void ModelClient::cancelAll()
{
    const QHash<QString, Pending> pending = m_pending;
    m_pending.clear();
    m_queue.clear();
    m_inFlight = 0;
    for (const Pending &p : pending) {
        if (p.reply)
            p.reply->abort();
        if (p.net)
            m_idleNets.append(p.net);
    }
}
