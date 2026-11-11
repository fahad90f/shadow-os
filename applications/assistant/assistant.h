// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

#include "assistantconfig.h"
#include "chatclient.h"
#include "conversationstore.h"
#include "toolregistry.h"

// The agent that holds a conversation and carries out tasks. It owns the
// configuration, the streaming API client, the conversation store and the tool
// registry, and drives the chat ⇄ tool-call loop:
//
//   user turn → model (brain/planner) → streamed reply (+ optional tool calls)
//   → each tool call is confirmed in the UI (unless auto-allowed) → executed →
//   results fed back to the model → final reply.
//
// Nothing that changes the system or reaches the network runs without an
// explicit in-UI confirmation click.
class Assistant : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(AssistantConfig *config READ config CONSTANT)
    Q_PROPERTY(MessageModel *messages READ messages CONSTANT)
    Q_PROPERTY(ConversationStore *conversations READ conversations CONSTANT)
    // "setup" (no key) | "idle" | "thinking" | "streaming" | "awaiting" | "error"
    Q_PROPERTY(QString phase READ phase NOTIFY phaseChanged)
    // Orb/presence state: "idle" | "thinking" | "speaking" | "listening"
    Q_PROPERTY(QString orbState READ orbState NOTIFY phaseChanged)
    Q_PROPERTY(QString activeId READ activeId NOTIFY activeChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY phaseChanged)
    Q_PROPERTY(bool canSend READ canSend NOTIFY phaseChanged)
    Q_PROPERTY(bool hasPending READ hasPending NOTIFY phaseChanged)
    Q_PROPERTY(bool thinkHard READ thinkHard WRITE setThinkHard NOTIFY thinkHardChanged)
    Q_PROPERTY(QVariantList toolLog READ toolLog NOTIFY toolLogChanged)
    // Voice is scaffolded but disabled (no tested backend in this build).
    Q_PROPERTY(bool voiceAvailable READ voiceAvailable CONSTANT)

public:
    explicit Assistant(QObject *parent = nullptr);

    AssistantConfig *config() { return m_config; }
    MessageModel *messages() { return m_messages; }
    ConversationStore *conversations() { return m_store; }
    QString phase() const { return m_phase; }
    QString orbState() const;
    QString activeId() const { return m_activeId; }
    bool busy() const { return m_client->busy() || m_executing > 0; }
    bool canSend() const { return m_config->apiKeyConfigured() && !busy() && !hasPending(); }
    bool hasPending() const { return m_messages->hasPendingTools(); }
    bool thinkHard() const { return m_thinkHard; }
    void setThinkHard(bool v);
    QVariantList toolLog() const { return m_log; }
    bool voiceAvailable() const { return false; }

    Q_INVOKABLE void newConversation();
    Q_INVOKABLE void selectConversation(const QString &id);
    Q_INVOKABLE void renameConversation(const QString &id, const QString &title);
    Q_INVOKABLE void deleteConversation(const QString &id);
    Q_INVOKABLE void sendMessage(const QString &text);
    Q_INVOKABLE void stop();
    // Resolve a pending tool call: run=true executes it, run=false denies it.
    Q_INVOKABLE void confirmTool(int row, const QString &callId, bool run);
    Q_INVOKABLE void confirmAll(bool run);
    Q_INVOKABLE void copyToClipboard(const QString &text) const;
    Q_INVOKABLE void reloadConfig();

Q_SIGNALS:
    void phaseChanged();
    void activeChanged();
    void thinkHardChanged();
    void toolLogChanged();
    void errorMessage(const QString &text);
    // Voice integration: a final assistant answer is ready to be spoken.
    void assistantReplyReady(const QString &text);
    // A tool call is awaiting confirmation (for hands-free spoken approval); the
    // clickable Run/Deny card is shown regardless.
    void confirmationNeeded(const QString &summary, const QString &preview, const QString &callId);

private:
    void setPhase(const QString &p);
    void ensureConversation();
    void startCompletion();
    void saveActive();
    QString systemPrompt() const;
    bool needsConfirmation(const QString &toolName) const;
    void executeCall(int row, const ToolCallEntry &call);
    void maybeContinue(int row);
    void logCall(const QString &name, const QString &preview, const QString &status);
    int rowOfCall(const QString &callId) const;

    AssistantConfig *m_config;
    ChatClient *m_client;
    ConversationStore *m_store;
    MessageModel *m_messages;
    ToolRegistry *m_registry;

    QString m_phase = QStringLiteral("idle");
    QString m_activeId;
    int m_assistantRow = -1;
    bool m_thinkHard = false;
    int m_executing = 0;   // tool calls currently running
    int m_toolRounds = 0;  // guard against runaway tool loops
    QVariantList m_log;
};
