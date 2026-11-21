// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

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
    // Env-gated screenshot/test hooks (empty in normal use), used to open a
    // view or inject a first message deterministically for headless captures.
    Q_PROPERTY(QString testView READ testView CONSTANT)
    Q_PROPERTY(QString testPrompt READ testPrompt CONSTANT)
    Q_PROPERTY(QString testOrb READ testOrb CONSTANT)
    // A screenshot is being taken (SHADOW_SCREENSHOT): the UI draws one fixed
    // instant of its animations so captures are deterministic.
    Q_PROPERTY(bool testFrozen READ testFrozen CONSTANT)

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
    QString testView() const { return QString::fromUtf8(qgetenv("SHADOW_ASSISTANT_TEST_VIEW")); }
    QString testPrompt() const { return QString::fromUtf8(qgetenv("SHADOW_ASSISTANT_TEST_PROMPT")); }
    QString testOrb() const { return QString::fromUtf8(qgetenv("SHADOW_ASSISTANT_TEST_ORB")); }
    bool testFrozen() const { return !qEnvironmentVariableIsEmpty("SHADOW_SCREENSHOT"); }

    Q_INVOKABLE void newConversation();
    Q_INVOKABLE void selectConversation(const QString &id);
    Q_INVOKABLE void renameConversation(const QString &id, const QString &title);
    Q_INVOKABLE void deleteConversation(const QString &id);
    Q_INVOKABLE void sendMessage(const QString &text);
    Q_INVOKABLE void stop();
    // Resolve a pending tool call: run=true executes it, run=false denies it.
    Q_INVOKABLE void confirmTool(int row, const QString &callId, bool run);
    Q_INVOKABLE void confirmAll(bool run);
    // The first tool call still waiting for a decision, in conversation order
    // (the one the question card shows): {id, name, summary, preview}, or an
    // empty map when nothing waits.
    Q_INVOKABLE QVariantMap firstPendingCall() const;
    // "Try again" after an error: the failed turn (its error row and the
    // empty or cut-off reply before it) is removed and the request is sent
    // again, without a second copy of the user's message. False when there
    // is nothing to retry or the agent cannot send now.
    Q_INVOKABLE bool retry();
    Q_INVOKABLE void copyToClipboard(const QString &text) const;
    Q_INVOKABLE void reloadConfig();

    // Action history (file-changing tool calls) and undo.
    Q_INVOKABLE QVariantList actionLog() const;
    Q_INVOKABLE QString undoAction(const QString &id);

Q_SIGNALS:
    void phaseChanged();
    void activeChanged();
    void thinkHardChanged();
    void toolLogChanged();
    void actionLogChanged();
    void errorMessage(const QString &text);
    // Voice integration: a final assistant answer is ready to be spoken.
    void assistantReplyReady(const QString &text);
    // Voice integration: one streamed chunk of the assistant's text, as it
    // arrives (spoken sentence by sentence during a voice turn).
    void replyDelta(const QString &text);
    // Voice integration: an assistant row finished streaming; tool calls may
    // follow, then another row.
    void replySegmentFinished();
    // stop() was called (the Stop button): the reply is abandoned, so is
    // whatever was being said for it.
    void stopped();
    // A tool call is awaiting confirmation (for hands-free spoken approval); the
    // clickable Run/Deny card is shown regardless.
    void confirmationNeeded(const QString &summary, const QString &preview, const QString &callId);

private:
    void setPhase(const QString &p);
    // The conversation in the model was replaced (selected, new, deleted):
    // its phase follows what it holds — "awaiting" when a tool call still
    // waits for a decision — and busy/canSend/hasPending are notified even
    // when the phase string stays the same.
    void settleLoadedConversation();
    void ensureConversation();
    void startCompletion();
    void saveActive();
    QString systemPrompt() const;
    bool needsConfirmation(const QString &toolName) const;
    void executeCall(int row, const ToolCallEntry &call);
    void maybeContinue(int row);
    // One entry per call in the activity log: a call that changes status
    // (running → done) is updated and moved to the top, not logged twice.
    void logCall(const QString &callId, const QString &name, const QString &preview, const QString &status);
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
    int m_retries = 0;     // transient-failure retries for the current request
    // True while dispatching a round of tool calls. A tool that completes
    // synchronously would otherwise call maybeContinue() mid-loop AND again at
    // the end, firing two overlapping continuation requests; this defers the
    // continuation until the whole round has been dispatched.
    bool m_dispatching = false;
    // True while a continuation request is queued on the event loop, so a
    // second maybeContinue() in the same tick cannot start a second request.
    bool m_continueScheduled = false;
    QVariantList m_log;
};

// agent.messages and agent.conversations, made known to QML by name (not
// creatable), so the UI can type them and the QML tooling knows their members.
struct MessageModelForeign
{
    Q_GADGET
    QML_FOREIGN(MessageModel)
    QML_NAMED_ELEMENT(MessageModel)
    QML_UNCREATABLE("agent.messages")
};
struct ConversationStoreForeign
{
    Q_GADGET
    QML_FOREIGN(ConversationStore)
    QML_NAMED_ELEMENT(ConversationStore)
    QML_UNCREATABLE("agent.conversations")
};
