// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>

class AssistantConfig;
class QNetworkAccessManager;
class QNetworkReply;

// Streaming client for the assistant's language model. One request at a time.
//
// Two wire protocols are supported, chosen from the config at send() time:
//   * Cloud (default): the native Anthropic Messages API
//     (https://api.anthropic.com/v1/messages) — x-api-key auth, a top-level
//     system prompt, Anthropic tool schema and the Messages SSE event stream.
//   * Local (config api/local=true): an OpenAI-compatible /chat/completions
//     endpoint on this machine (e.g. ollama), so the assistant still runs
//     fully offline. Bearer auth, OpenAI tool schema, OpenAI SSE.
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
    explicit ChatClient(AssistantConfig *config, QObject *parent = nullptr);
    ~ChatClient() override;

    bool busy() const { return m_reply != nullptr; }

    // Start a streaming completion. 'messages' and 'tools' are OpenAI-format
    // arrays (the first message may be a system message); pass an empty 'tools'
    // to disable tool calling for this request.
    void send(const QString &model, const QJsonArray &messages, const QJsonArray &tools);

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
    // A typed failure. 'code' is one of: no_key, network, auth, not_found,
    // rate_limit, timeout, bad_response, server, refusal. 'message' is human-readable.
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
    QNetworkReply *m_reply = nullptr;
    QByteArray m_buffer;
    QString m_content;
    bool m_anthropic = false; // which parser onFinished()/onReadyRead() uses

    // --- OpenAI streaming: tool calls assembled by their streamed index ---
    struct PartialCall { QString id; QString name; QString arguments; };
    QHash<int, PartialCall> m_calls;

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
