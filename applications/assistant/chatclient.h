// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>

class AssistantConfig;
class QNetworkAccessManager;

// Streaming client for one request to a language model. One request at a
// time per client; the model team (Council) uses several clients at once.
//
// Two wire protocols are supported, chosen from the config at send() time:
//   * OpenAI-compatible /chat/completions — NVIDIA's hosted models (the
//     default service, https://integrate.api.nvidia.com/v1) and a local
//     server on this machine (config api/local=true, e.g. ollama). Bearer
//     auth, OpenAI tool schema, OpenAI SSE.
//   * The native Anthropic Messages API (https://api.anthropic.com/v1/messages)
//     when the key is an Anthropic one — x-api-key auth, a top-level system
//     prompt, Anthropic tool schema and the Messages SSE event stream.
//
// The caller always speaks one neutral format — the OpenAI-shaped 'messages'
// and 'tools' arrays produced by MessageModel/ToolRegistry. This class
// translates to the active protocol, so nothing above it needs to know which
// backend is in use. For the Anthropic path it also preserves 'thinking'
// blocks: an assistant turn that made a tool call carries its raw thinking
// blocks (in the neutral message under a "thinking" field) so they can be
// replayed unchanged on the next request, as the Messages API requires.
//
// Emits content tokens as they arrive over Server-Sent Events, assembles any
// streamed tool calls, and reports every failure mode as a typed error rather
// than hanging or inventing an answer.
class ChatClient : public QObject
{
    Q_OBJECT
public:
    // 'net' lets several clients share one connection pool; by default each
    // client has its own.
    explicit ChatClient(AssistantConfig *config, QObject *parent = nullptr, QNetworkAccessManager *net = nullptr);
    ~ChatClient() override;

    bool busy() const { return !m_reply.isNull(); }
    // The model of the request in flight (or the last one).
    QString model() const { return m_model; }

    struct Options
    {
        int maxTokens = 0;         // 0 = the client's default
        double temperature = -1.0; // < 0 = the client's default
        int idleTimeoutMs = 0;     // 0 = the client's default
    };

    // Start a streaming completion. 'messages' and 'tools' are OpenAI-format
    // arrays (the first message may be a system message); pass an empty 'tools'
    // to disable tool calling for this request.
    void send(const QString &model, const QJsonArray &messages, const QJsonArray &tools);
    void send(const QString &model, const QJsonArray &messages, const QJsonArray &tools, const Options &options);

public Q_SLOTS:
    void cancel();

Q_SIGNALS:
    void started();
    // A chunk of assistant text.
    void delta(const QString &text);
    // The model finished. 'toolCalls' is the assembled OpenAI tool_calls array
    // (empty if none). 'thinking' is the raw Anthropic thinking blocks for this
    // turn (empty in local/OpenAI mode, or when the model did not think), to be
    // stored and replayed with the turn. 'finishReason' is e.g. "stop" or
    // "tool_calls".
    void completed(const QString &content, const QJsonArray &toolCalls,
                   const QJsonArray &thinking, const QString &finishReason);
    // A typed failure. 'code' is one of: no_key, network, auth, forbidden, not_found,
    // rate_limit, timeout, bad_response, server, refusal, no_tools (the model
    // rejected a request because it carried tools), no_images (because it
    // carried an image). 'message' is human-readable.
    void failed(const QString &code, const QString &message);

private:
    void onReadyRead();
    void onFinished();
    void processEvent(const QByteArray &event);          // OpenAI SSE
    void processAnthropicEvent(const QByteArray &event); // Anthropic SSE
    void emitHttpError(QNetworkReply *reply);

    // OpenAI <-> Anthropic translation (used only on the cloud path).
    static QJsonArray toAnthropicMessages(const QJsonArray &messages, QString *systemOut);
    static QJsonArray toAnthropicTools(const QJsonArray &tools);

    AssistantConfig *m_config;
    QNetworkAccessManager *m_net = nullptr;
    // Guarded: with a shared connection pool ('net') the pool owns the reply
    // and may be destroyed before this client (at exit, in mid-request).
    QPointer<QNetworkReply> m_reply;
    QString m_model;
    bool m_sentTools = false;   // the request in flight carries tools
    bool m_sentImages = false;  // … or an image
    bool m_sentToolHistory = false; // … or earlier tool calls and their results
    QByteArray m_buffer;
    QString m_content;
    bool m_anthropic = false; // which parser onFinished()/onReadyRead() uses

    // --- OpenAI streaming: tool calls assembled by their streamed index ---
    struct PartialCall { QString id; QString name; QString arguments; };
    QHash<int, PartialCall> m_calls;
    int m_lastCall = -1; // the highest call number seen in this reply

    // --- Anthropic streaming: content blocks assembled by their index ---
    struct Block {
        QString type;      // "text" | "thinking" | "redacted_thinking" | "tool_use"
        QString text;      // text blocks
        QString thinking;  // thinking blocks
        QString signature; // thinking block signature (replayed verbatim)
        QString data;      // redacted_thinking payload
        QString toolId;    // tool_use id
        QString toolName;  // tool_use name
        QString toolJson;  // tool_use input, streamed as partial JSON
    };
    QMap<int, Block> m_blocks;

    QString m_finishReason;
    bool m_sawData = false;
    bool m_failed = false;
};
