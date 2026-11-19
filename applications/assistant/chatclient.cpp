// SPDX-License-Identifier: GPL-3.0-or-later
#include "chatclient.h"

#include "assistantconfig.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
constexpr int kTimeoutMs = 120000; // first-token + full stream budget
// Streaming, so a generous cap is safe (no HTTP timeout concern); replies are
// usually short but tool-use turns need headroom.
constexpr int kMaxTokens = 8192;

// Models that support the server-side refusal fallback (a safety decline is
// automatically retried on another model instead of failing the request).
bool supportsRefusalFallback(const QString &model)
{
    return model.contains(QLatin1String("opus-5"))
        || model.contains(QLatin1String("sonnet-5-5"))
        || model.contains(QLatin1String("fable-5"))
        || model.contains(QLatin1String("mythos-5"));
}
} // namespace

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

// ---- OpenAI -> Anthropic translation --------------------------------------

QJsonArray ChatClient::toAnthropicTools(const QJsonArray &tools)
{
    QJsonArray out;
    for (const QJsonValue &tv : tools) {
        const QJsonObject fn = tv.toObject().value(QStringLiteral("function")).toObject();
        if (fn.isEmpty())
            continue;
        out.append(QJsonObject{
            {QStringLiteral("name"), fn.value(QStringLiteral("name")).toString()},
            {QStringLiteral("description"), fn.value(QStringLiteral("description")).toString()},
            {QStringLiteral("input_schema"), fn.value(QStringLiteral("parameters")).toObject()},
        });
    }
    return out;
}

QJsonArray ChatClient::toAnthropicMessages(const QJsonArray &messages, QString *systemOut)
{
    QJsonArray out;
    QJsonArray pendingToolResults; // consecutive tool results -> one user message

    auto flushToolResults = [&]() {
        if (!pendingToolResults.isEmpty()) {
            out.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                   {QStringLiteral("content"), pendingToolResults}});
            pendingToolResults = QJsonArray{};
        }
    };

    for (const QJsonValue &mv : messages) {
        const QJsonObject m = mv.toObject();
        const QString role = m.value(QStringLiteral("role")).toString();

        if (role == QLatin1String("system")) {
            if (systemOut)
                *systemOut = m.value(QStringLiteral("content")).toString();
            continue;
        }
        if (role == QLatin1String("user")) {
            flushToolResults();
            out.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                   {QStringLiteral("content"), m.value(QStringLiteral("content")).toString()}});
            continue;
        }
        if (role == QLatin1String("tool")) {
            // A tool result becomes a tool_result content block in a user turn.
            pendingToolResults.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("tool_result")},
                {QStringLiteral("tool_use_id"), m.value(QStringLiteral("tool_call_id")).toString()},
                {QStringLiteral("content"), m.value(QStringLiteral("content")).toString()},
            });
            continue;
        }
        if (role == QLatin1String("assistant")) {
            flushToolResults();
            QJsonArray blocks;
            // Preserved thinking: replay the raw thinking blocks first, unchanged.
            for (const QJsonValue &tv : m.value(QStringLiteral("thinking")).toArray())
                blocks.append(tv);
            const QString text = m.value(QStringLiteral("content")).toString();
            if (!text.isEmpty())
                blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                          {QStringLiteral("text"), text}});
            for (const QJsonValue &cv : m.value(QStringLiteral("tool_calls")).toArray()) {
                const QJsonObject tc = cv.toObject();
                const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
                const QByteArray args = fn.value(QStringLiteral("arguments")).toString().toUtf8();
                QJsonObject input = QJsonDocument::fromJson(args.isEmpty() ? QByteArray("{}") : args).object();
                blocks.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("tool_use")},
                    {QStringLiteral("id"), tc.value(QStringLiteral("id")).toString()},
                    {QStringLiteral("name"), fn.value(QStringLiteral("name")).toString()},
                    {QStringLiteral("input"), input},
                });
            }
            if (blocks.isEmpty()) // nothing to say and nothing to call: skip
                continue;
            out.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                   {QStringLiteral("content"), blocks}});
            continue;
        }
    }
    flushToolResults();
    return out;
}

// ---- request ---------------------------------------------------------------

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
    m_blocks.clear();
    m_finishReason.clear();
    m_sawData = false;
    m_failed = false;
    m_anthropic = !m_config->localMode();

    if (!m_config->apiKeyConfigured()) {
        Q_EMIT failed(QStringLiteral("no_key"), tr("No Claude API key is configured."));
        return;
    }

    const QString endpoint = m_anthropic ? QStringLiteral("/messages")
                                          : QStringLiteral("/chat/completions");
    const QUrl url(m_config->baseUrl() + endpoint);
    if (!url.isValid() || url.scheme().isEmpty()) {
        Q_EMIT failed(QStringLiteral("bad_response"), tr("Invalid endpoint URL: %1").arg(url.toString()));
        return;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "text/event-stream");

    QJsonObject body;
    if (m_anthropic) {
        // --- native Anthropic Messages API ---
        QString system;
        const QJsonArray anthropicMessages = toAnthropicMessages(messages, &system);
        body.insert(QStringLiteral("model"), model);
        body.insert(QStringLiteral("max_tokens"), kMaxTokens);
        if (!system.isEmpty())
            body.insert(QStringLiteral("system"), system);
        body.insert(QStringLiteral("messages"), anthropicMessages);
        body.insert(QStringLiteral("stream"), true);
        // No temperature / thinking parameter: the current Claude models reject
        // a sampling parameter, and run adaptive thinking by default (it cannot
        // be disabled on Opus), so tool_choice defaults to "auto" too.
        if (!tools.isEmpty())
            body.insert(QStringLiteral("tools"), toAnthropicTools(tools));

        req.setRawHeader("x-api-key", m_config->apiKey().toUtf8());
        req.setRawHeader("anthropic-version", m_config->anthropicVersion().toUtf8());
        // Server-side refusal fallback: if a safety classifier declines, the
        // request is retried on another model automatically rather than failing.
        if (supportsRefusalFallback(model)) {
            body.insert(QStringLiteral("fallbacks"), QStringLiteral("default"));
            req.setRawHeader("anthropic-beta", "server-side-fallback-2026-07-01");
        }
    } else {
        // --- OpenAI-compatible local server (offline) ---
        body.insert(QStringLiteral("model"), model);
        body.insert(QStringLiteral("messages"), messages);
        body.insert(QStringLiteral("stream"), true);
        body.insert(QStringLiteral("temperature"), 0.6);
        if (!tools.isEmpty()) {
            body.insert(QStringLiteral("tools"), tools);
            body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
        }
        // A local server needs no key; a remote OpenAI-compatible one may.
        if (!m_config->apiKey().isEmpty())
            req.setRawHeader("Authorization", "Bearer " + m_config->apiKey().toUtf8());
    }

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
        if (m_anthropic)
            processAnthropicEvent(event);
        else
            processEvent(event);
    }
}

// ---- Anthropic Messages SSE ------------------------------------------------

void ChatClient::processAnthropicEvent(const QByteArray &event)
{
    for (const QByteArray &rawLine : event.split('\n')) {
        QByteArray line = rawLine.trimmed();
        if (!line.startsWith("data:"))
            continue; // the "event:" line carries the same type inside the JSON
        const QByteArray payload = line.mid(5).trimmed();
        if (payload.isEmpty())
            continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject o = doc.object();
        const QString type = o.value(QStringLiteral("type")).toString();

        if (type == QLatin1String("message_start")) {
            m_sawData = true;
        } else if (type == QLatin1String("content_block_start")) {
            m_sawData = true;
            const int idx = o.value(QStringLiteral("index")).toInt();
            const QJsonObject cb = o.value(QStringLiteral("content_block")).toObject();
            Block b;
            b.type = cb.value(QStringLiteral("type")).toString();
            if (b.type == QLatin1String("tool_use")) {
                b.toolId = cb.value(QStringLiteral("id")).toString();
                b.toolName = cb.value(QStringLiteral("name")).toString();
            } else if (b.type == QLatin1String("text")) {
                b.text = cb.value(QStringLiteral("text")).toString();
            } else if (b.type == QLatin1String("thinking")) {
                b.thinking = cb.value(QStringLiteral("thinking")).toString();
                b.signature = cb.value(QStringLiteral("signature")).toString();
            } else if (b.type == QLatin1String("redacted_thinking")) {
                b.data = cb.value(QStringLiteral("data")).toString();
            }
            m_blocks.insert(idx, b);
            if (!b.text.isEmpty()) {
                m_content += b.text;
                Q_EMIT delta(b.text);
            }
        } else if (type == QLatin1String("content_block_delta")) {
            const int idx = o.value(QStringLiteral("index")).toInt();
            Block &b = m_blocks[idx];
            const QJsonObject d = o.value(QStringLiteral("delta")).toObject();
            const QString dt = d.value(QStringLiteral("type")).toString();
            if (dt == QLatin1String("text_delta")) {
                const QString t = d.value(QStringLiteral("text")).toString();
                b.text += t;
                if (!t.isEmpty()) {
                    m_content += t;
                    Q_EMIT delta(t);
                }
            } else if (dt == QLatin1String("thinking_delta")) {
                b.thinking += d.value(QStringLiteral("thinking")).toString();
            } else if (dt == QLatin1String("signature_delta")) {
                b.signature += d.value(QStringLiteral("signature")).toString();
            } else if (dt == QLatin1String("input_json_delta")) {
                b.toolJson += d.value(QStringLiteral("partial_json")).toString();
            }
        } else if (type == QLatin1String("message_delta")) {
            const QString reason = o.value(QStringLiteral("delta")).toObject()
                                       .value(QStringLiteral("stop_reason")).toString();
            if (!reason.isEmpty())
                m_finishReason = reason;
        } else if (type == QLatin1String("error")) {
            const QJsonObject e = o.value(QStringLiteral("error")).toObject();
            const QString msg = e.value(QStringLiteral("message")).toString(tr("The API reported a streaming error."));
            m_failed = true;
            Q_EMIT failed(QStringLiteral("server"), msg);
        }
        // "ping", "content_block_stop", "message_stop": nothing to do.
    }
}

// ---- OpenAI-compatible SSE (local offline server) --------------------------

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
    case 400:
        code = QStringLiteral("bad_response");
        message = tr("The Claude API rejected the request (HTTP 400). Check the model ID in Settings.");
        break;
    case 401:
        code = QStringLiteral("auth");
        message = tr("The Claude API rejected the key (HTTP 401). Check the key in your assistant.conf.");
        break;
    case 403:
        code = QStringLiteral("auth");
        message = tr("The Claude API refused this request (HTTP 403). The key may be invalid or lack access "
                     "to the selected model. Check the model IDs in Settings and the key in assistant.conf.");
        break;
    case 404:
        code = QStringLiteral("not_found");
        message = tr("The model or endpoint was not found (HTTP 404). Check the model IDs in Settings.");
        break;
    case 429:
        code = QStringLiteral("rate_limit");
        message = tr("Rate limited by the API (HTTP 429). Wait a moment and try again.");
        break;
    case 529:
        code = QStringLiteral("server");
        message = tr("The Claude API is temporarily overloaded (HTTP 529). Try again shortly.");
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

    if (m_failed) // user-cancelled, or a streaming error already reported
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
    if (!m_buffer.trimmed().isEmpty()) {
        if (m_anthropic)
            processAnthropicEvent(m_buffer);
        else
            processEvent(m_buffer);
    }
    m_buffer.clear();

    if (!m_sawData) {
        Q_EMIT failed(QStringLiteral("bad_response"), tr("The API returned no data."));
        return;
    }

    QJsonArray toolCalls;
    QJsonArray thinking;
    QString finishReason;

    if (m_anthropic) {
        // Assemble thinking blocks (for replay) and tool calls from the stream,
        // in index order.
        QList<int> keys = m_blocks.keys();
        std::sort(keys.begin(), keys.end());
        for (int k : keys) {
            const Block &b = m_blocks.value(k);
            if (b.type == QLatin1String("thinking")) {
                // Only a signed thinking block can be replayed and validated;
                // skip an unsigned/empty one (there is nothing to preserve).
                if (!b.signature.isEmpty())
                    thinking.append(QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("thinking")},
                        {QStringLiteral("thinking"), b.thinking},
                        {QStringLiteral("signature"), b.signature},
                    });
            } else if (b.type == QLatin1String("redacted_thinking")) {
                thinking.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("redacted_thinking")},
                    {QStringLiteral("data"), b.data},
                });
            } else if (b.type == QLatin1String("tool_use")) {
                toolCalls.append(QJsonObject{
                    {QStringLiteral("id"), b.toolId.isEmpty() ? QStringLiteral("call_%1").arg(k) : b.toolId},
                    {QStringLiteral("type"), QStringLiteral("function")},
                    {QStringLiteral("function"), QJsonObject{
                         {QStringLiteral("name"), b.toolName},
                         {QStringLiteral("arguments"), b.toolJson.isEmpty() ? QStringLiteral("{}") : b.toolJson},
                     }},
                });
            }
        }
        if (m_finishReason == QLatin1String("refusal")) {
            Q_EMIT failed(QStringLiteral("refusal"),
                          tr("Claude declined this request. Try rephrasing it, or if you are in "
                             "pentest mode, switch to general mode for everyday tasks."));
            return;
        }
        finishReason = m_finishReason == QLatin1String("tool_use") ? QStringLiteral("tool_calls")
                                                                   : QStringLiteral("stop");
    } else {
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
        finishReason = m_finishReason.isEmpty() ? QStringLiteral("stop") : m_finishReason;
    }

    Q_EMIT completed(m_content, toolCalls, thinking, finishReason);
}
