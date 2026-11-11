// SPDX-License-Identifier: GPL-3.0-or-later
#include "chatclient.h"

#include "assistantconfig.h"

#include <QJsonDocument>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
constexpr int kTimeoutMs = 120000; // first-token + full stream budget
}

ChatClient::ChatClient(AssistantConfig *config, QObject *parent)
    : QObject(parent), m_config(config)
{
    m_net = new QNetworkAccessManager(this);
    m_net->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_net->setTransferTimeout(kTimeoutMs);
}

ChatClient::~ChatClient()
{
    if (m_reply)
        m_reply->abort();
}

void ChatClient::send(const QString &model, const QJsonArray &messages, const QJsonArray &tools)
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_buffer.clear();
    m_content.clear();
    m_calls.clear();
    m_finishReason.clear();
    m_sawData = false;
    m_failed = false;

    if (!m_config->apiKeyConfigured()) {
        Q_EMIT failed(QStringLiteral("no_key"), tr("No NVIDIA API key is configured."));
        return;
    }

    const QUrl url(m_config->baseUrl() + QStringLiteral("/chat/completions"));
    if (!url.isValid() || url.scheme().isEmpty()) {
        Q_EMIT failed(QStringLiteral("bad_response"), tr("Invalid endpoint URL: %1").arg(url.toString()));
        return;
    }

    QJsonObject body{
        {QStringLiteral("model"), model},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("stream"), true},
        {QStringLiteral("temperature"), 0.6},
    };
    if (!tools.isEmpty()) {
        body.insert(QStringLiteral("tools"), tools);
        body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "text/event-stream");
    req.setRawHeader("Authorization", "Bearer " + m_config->apiKey().toUtf8());

    m_reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &ChatClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &ChatClient::onFinished);
    Q_EMIT started();
}

void ChatClient::cancel()
{
    if (m_reply) {
        m_failed = true; // suppress the finished() error path for a user cancel
        m_reply->abort();
    }
}

void ChatClient::onReadyRead()
{
    if (!m_reply)
        return;
    // Only treat the body as an SSE stream on a 200 response. Errors carry a
    // JSON body handled in onFinished().
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 0 && status != 200)
        return;

    m_buffer += m_reply->readAll();
    int sep;
    while ((sep = m_buffer.indexOf("\n\n")) != -1) {
        const QByteArray event = m_buffer.left(sep);
        m_buffer.remove(0, sep + 2);
        processEvent(event);
    }
}

void ChatClient::processEvent(const QByteArray &event)
{
    for (const QByteArray &rawLine : event.split('\n')) {
        QByteArray line = rawLine.trimmed();
        if (!line.startsWith("data:"))
            continue;
        QByteArray payload = line.mid(5).trimmed();
        if (payload == "[DONE]") {
            m_sawData = true;
            continue;
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        m_sawData = true;
        const QJsonObject o = doc.object();
        const QJsonArray choices = o.value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty())
            continue;
        const QJsonObject choice = choices.first().toObject();
        const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();

        const QString text = delta.value(QStringLiteral("content")).toString();
        if (!text.isEmpty()) {
            m_content += text;
            Q_EMIT this->delta(text);
        }

        const QJsonArray toolCalls = delta.value(QStringLiteral("tool_calls")).toArray();
        for (const QJsonValue &tcv : toolCalls) {
            const QJsonObject tc = tcv.toObject();
            const int index = tc.value(QStringLiteral("index")).toInt();
            PartialCall &pc = m_calls[index];
            if (tc.contains(QStringLiteral("id")))
                pc.id = tc.value(QStringLiteral("id")).toString();
            const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
            if (fn.contains(QStringLiteral("name")))
                pc.name += fn.value(QStringLiteral("name")).toString();
            if (fn.contains(QStringLiteral("arguments")))
                pc.arguments += fn.value(QStringLiteral("arguments")).toString();
        }

        const QString reason = choice.value(QStringLiteral("finish_reason")).toString();
        if (!reason.isEmpty())
            m_finishReason = reason;
    }
}

void ChatClient::emitHttpError(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    QString apiMessage;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonValue e = doc.object().value(QStringLiteral("error"));
        if (e.isObject())
            apiMessage = e.toObject().value(QStringLiteral("message")).toString();
        else if (e.isString())
            apiMessage = e.toString();
        if (apiMessage.isEmpty())
            apiMessage = doc.object().value(QStringLiteral("message")).toString();
        if (apiMessage.isEmpty())
            apiMessage = doc.object().value(QStringLiteral("detail")).toString();
    }

    QString code, message;
    switch (status) {
    case 401:
    case 403:
        code = QStringLiteral("auth");
        message = tr("The NVIDIA API rejected the key (HTTP %1). Check the key in your assistant.conf.").arg(status);
        break;
    case 404:
        code = QStringLiteral("not_found");
        message = tr("The model or endpoint was not found (HTTP 404). Check the model IDs in Settings.");
        break;
    case 429:
        code = QStringLiteral("rate_limit");
        message = tr("Rate limited by the API (HTTP 429). Wait a moment and try again.");
        break;
    default:
        if (status >= 500) {
            code = QStringLiteral("server");
            message = tr("The API returned a server error (HTTP %1).").arg(status);
        } else {
            code = QStringLiteral("network");
            message = reply->error() == QNetworkReply::OperationCanceledError
                ? tr("The request timed out.")
                : reply->errorString();
            if (reply->error() == QNetworkReply::OperationCanceledError)
                code = QStringLiteral("timeout");
        }
        break;
    }
    if (!apiMessage.isEmpty())
        message += QStringLiteral("\n") + apiMessage;
    Q_EMIT failed(code, message);
}

void ChatClient::onFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    m_reply = nullptr;
    reply->deleteLater();

    if (m_failed) // user-cancelled
        return;

    const QNetworkReply::NetworkError netErr = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (netErr != QNetworkReply::NoError && (status == 0 || status == 200)) {
        // Transport-level failure (DNS, TLS, connection, timeout) with no HTTP body.
        if (netErr == QNetworkReply::OperationCanceledError)
            Q_EMIT failed(QStringLiteral("timeout"), tr("The request timed out."));
        else
            Q_EMIT failed(QStringLiteral("network"), tr("Network error: %1").arg(reply->errorString()));
        return;
    }
    if (status != 200 && status != 0) {
        emitHttpError(reply);
        return;
    }

    // Drain any trailing buffered event.
    if (!m_buffer.trimmed().isEmpty())
        processEvent(m_buffer);
    m_buffer.clear();

    if (!m_sawData) {
        Q_EMIT failed(QStringLiteral("bad_response"), tr("The API returned no data."));
        return;
    }

    QJsonArray toolCalls;
    QList<int> keys = m_calls.keys();
    std::sort(keys.begin(), keys.end());
    for (int k : keys) {
        const PartialCall &pc = m_calls.value(k);
        if (pc.name.isEmpty())
            continue;
        toolCalls.append(QJsonObject{
            {QStringLiteral("id"), pc.id.isEmpty() ? QStringLiteral("call_%1").arg(k) : pc.id},
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{
                 {QStringLiteral("name"), pc.name},
                 {QStringLiteral("arguments"), pc.arguments},
             }},
        });
    }
    Q_EMIT completed(m_content, toolCalls, m_finishReason.isEmpty() ? QStringLiteral("stop") : m_finishReason);
}
