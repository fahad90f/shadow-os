// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class AssistantConfig;
class QNetworkAccessManager;
class QNetworkReply;

// Streaming client for an OpenAI-compatible chat/completions endpoint
// (NVIDIA's integrate.api.nvidia.com by default). One request at a time.
//
// Emits content tokens as they arrive over Server-Sent Events, assembles any
// streamed tool calls, and reports every failure mode as a typed error rather
// than hanging or inventing an answer.
class ChatClient : public QObject
{
    Q_OBJECT
public:
    explicit ChatClient(AssistantConfig *config, QObject *parent = nullptr);
    ~ChatClient() override;

    bool busy() const { return m_reply != nullptr; }

    // Start a streaming completion. 'messages' and 'tools' are OpenAI-format
    // arrays; pass an empty 'tools' to disable tool calling for this request.
    void send(const QString &model, const QJsonArray &messages, const QJsonArray &tools);

public Q_SLOTS:
    void cancel();

Q_SIGNALS:
    void started();
    // A chunk of assistant text.
    void delta(const QString &text);
    // The model finished. 'toolCalls' is the assembled OpenAI tool_calls array
    // (empty if none). 'finishReason' is e.g. "stop" or "tool_calls".
    void completed(const QString &content, const QJsonArray &toolCalls, const QString &finishReason);
    // A typed failure. 'code' is one of: no_key, network, auth, not_found,
    // rate_limit, timeout, bad_response, server. 'message' is human-readable.
    void failed(const QString &code, const QString &message);

private:
    void onReadyRead();
    void onFinished();
    void processEvent(const QByteArray &event);
    void emitHttpError(QNetworkReply *reply);

    AssistantConfig *m_config;
    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_reply = nullptr;
    QByteArray m_buffer;
    QString m_content;
    // tool calls assembled by their streamed index
    struct PartialCall { QString id; QString name; QString arguments; };
    QHash<int, PartialCall> m_calls;
    QString m_finishReason;
    bool m_sawData = false;
    bool m_failed = false;
};
