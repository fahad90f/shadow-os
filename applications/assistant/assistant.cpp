// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistant.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QJsonDocument>

namespace {
constexpr int kMaxToolRounds = 8;
}

Assistant::Assistant(QObject *parent)
    : QObject(parent)
{
    m_config = new AssistantConfig(this);
    m_store = new ConversationStore(this);
    m_messages = new MessageModel(this);
    m_registry = new ToolRegistry(m_config, this);
    m_client = new ChatClient(m_config, this);

    connect(m_config, &AssistantConfig::changed, this, &Assistant::phaseChanged);

    connect(m_client, &ChatClient::delta, this, [this](const QString &text) {
        if (m_assistantRow >= 0) {
            setPhase(QStringLiteral("streaming"));
            m_messages->appendDelta(m_assistantRow, text);
        }
    });
    connect(m_client, &ChatClient::completed, this,
            [this](const QString &content, const QJsonArray &toolCalls, const QString &) {
        if (m_assistantRow < 0)
            return;
        const int row = m_assistantRow;
        m_messages->finishAssistant(row, content, toolCalls, m_registry);
        m_assistantRow = -1;
        saveActive();

        if (toolCalls.isEmpty()) {
            setPhase(QStringLiteral("idle"));
            return;
        }
        // Decide per call: confirm in the UI, or auto-run (read-only tools
        // under the ask_risky autonomy level). A confirmed-needed call stays
        // "pending" and the UI renders a confirmation card for it.
        const QVariantList vlist = m_messages->data(m_messages->index(row), MessageModel::ToolCallsRole).toList();
        bool anyPending = false;
        for (const QVariant &cv : vlist) {
            const QVariantMap cm = cv.toMap();
            ToolCallEntry e;
            e.id = cm.value(QStringLiteral("id")).toString();
            e.name = cm.value(QStringLiteral("name")).toString();
            e.summary = cm.value(QStringLiteral("summary")).toString();
            e.preview = cm.value(QStringLiteral("preview")).toString();
            if (needsConfirmation(e.name)) {
                anyPending = true; // stays pending; UI shows a confirmation card
            } else {
                executeCall(row, e);
            }
        }
        if (anyPending)
            setPhase(QStringLiteral("awaiting"));
        else if (m_executing == 0)
            maybeContinue(row);
    });
    connect(m_client, &ChatClient::failed, this, [this](const QString &code, const QString &message) {
        if (m_assistantRow >= 0) {
            // Drop the empty streaming placeholder's streaming flag.
            m_messages->finishAssistant(m_assistantRow, m_messages->data(m_messages->index(m_assistantRow), MessageModel::ContentRole).toString(), {}, m_registry);
            m_assistantRow = -1;
        }
        m_messages->appendError(message);
        saveActive();
        setPhase(QStringLiteral("error"));
        Q_EMIT errorMessage(message);
        // Return to idle so the user can retry.
        setPhase(m_config->apiKeyConfigured() ? QStringLiteral("idle") : QStringLiteral("setup"));
        Q_UNUSED(code);
    });

    setPhase(m_config->apiKeyConfigured() ? QStringLiteral("idle") : QStringLiteral("setup"));
}

void Assistant::setPhase(const QString &p)
{
    if (p == m_phase)
        return;
    m_phase = p;
    Q_EMIT phaseChanged();
}

QString Assistant::orbState() const
{
    if (m_phase == QLatin1String("thinking") || m_phase == QLatin1String("awaiting"))
        return QStringLiteral("thinking");
    if (m_phase == QLatin1String("streaming"))
        return QStringLiteral("speaking");
    return QStringLiteral("idle");
}

void Assistant::setThinkHard(bool v)
{
    if (v == m_thinkHard)
        return;
    m_thinkHard = v;
    Q_EMIT thinkHardChanged();
}

void Assistant::reloadConfig()
{
    m_config->reload();
    setPhase(m_config->apiKeyConfigured() ? (busy() ? m_phase : QStringLiteral("idle")) : QStringLiteral("setup"));
}

void Assistant::newConversation()
{
    m_activeId = m_store->createConversation();
    m_messages->clearAll();
    m_assistantRow = -1;
    Q_EMIT activeChanged();
    setPhase(m_config->apiKeyConfigured() ? QStringLiteral("idle") : QStringLiteral("setup"));
}

void Assistant::ensureConversation()
{
    if (m_activeId.isEmpty())
        newConversation();
}

void Assistant::selectConversation(const QString &id)
{
    if (id == m_activeId || busy())
        return;
    m_activeId = id;
    m_messages->loadFrom(m_store->loadMessages(id));
    m_assistantRow = -1;
    Q_EMIT activeChanged();
    setPhase(m_config->apiKeyConfigured() ? QStringLiteral("idle") : QStringLiteral("setup"));
}

void Assistant::renameConversation(const QString &id, const QString &title)
{
    m_store->renameConversation(id, title);
}

void Assistant::deleteConversation(const QString &id)
{
    m_store->removeConversation(id);
    if (id == m_activeId) {
        m_activeId.clear();
        m_messages->clearAll();
        Q_EMIT activeChanged();
    }
}

void Assistant::saveActive()
{
    if (m_activeId.isEmpty())
        return;
    m_store->saveMessages(m_activeId, m_messages->toJson());
}

QString Assistant::systemPrompt() const
{
    return QStringLiteral(
        "You are SHADOW Assistant, a helpful, concise personal AI agent running on "
        "Shadow OS (a Debian-based security/development Linux desktop). You can hold a "
        "conversation and carry out tasks on this computer by calling the provided tools. "
        "Prefer calling a tool over guessing. Use short GitHub-flavoured Markdown in replies. "
        "Actions that change the system or reach the network are confirmed by the user in the "
        "UI before they run, so call the tool and wait for its result rather than claiming a "
        "task is done. Today is %1.")
        .arg(QDate::currentDate().toString(Qt::ISODate));
}

bool Assistant::needsConfirmation(const QString &toolName) const
{
    // Anything that changes the system or reaches the network always confirms.
    if (m_registry->toolChangesSystem(toolName))
        return true;
    // Read-only tools: confirm every time unless autonomy allows auto-run.
    return m_config->autonomy() != QLatin1String("ask_risky");
}

void Assistant::sendMessage(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || !canSend())
        return;
    ensureConversation();
    m_messages->appendUser(trimmed);
    m_store->touch(m_activeId, trimmed);
    saveActive();
    m_toolRounds = 0;
    startCompletion();
}

void Assistant::startCompletion()
{
    if (!m_config->apiKeyConfigured()) {
        setPhase(QStringLiteral("setup"));
        return;
    }
    const QString model = m_thinkHard ? m_config->modelForRole(QStringLiteral("planner"))
                                       : m_config->modelForRole(QStringLiteral("brain"));
    const QJsonArray request = m_messages->apiMessages(systemPrompt());
    const QJsonArray tools = m_registry->hasEnabledTools() ? m_registry->apiTools() : QJsonArray{};
    m_assistantRow = m_messages->appendAssistantStreaming();
    setPhase(QStringLiteral("thinking"));
    m_client->send(model, request, tools);
}

int Assistant::rowOfCall(const QString &callId) const
{
    for (int r = 0; r < m_messages->rowCount(); ++r) {
        const QVariantList calls = m_messages->data(m_messages->index(r), MessageModel::ToolCallsRole).toList();
        for (const QVariant &cv : calls)
            if (cv.toMap().value(QStringLiteral("id")).toString() == callId)
                return r;
    }
    return -1;
}

void Assistant::confirmTool(int row, const QString &callId, bool run)
{
    if (row < 0)
        row = rowOfCall(callId);
    if (row < 0)
        return;
    const QVariantList calls = m_messages->data(m_messages->index(row), MessageModel::ToolCallsRole).toList();
    ToolCallEntry e;
    bool found = false;
    for (const QVariant &cv : calls) {
        const QVariantMap cm = cv.toMap();
        if (cm.value(QStringLiteral("id")).toString() == callId) {
            e.id = callId;
            e.name = cm.value(QStringLiteral("name")).toString();
            e.summary = cm.value(QStringLiteral("summary")).toString();
            e.preview = cm.value(QStringLiteral("preview")).toString();
            if (cm.value(QStringLiteral("status")).toString() != QLatin1String("pending"))
                return; // already resolved
            found = true;
            break;
        }
    }
    if (!found)
        return;

    if (run) {
        executeCall(row, e);
    } else {
        const QString msg = tr("The user denied permission to run this action.");
        m_messages->setToolStatus(row, callId, QStringLiteral("denied"), msg);
        m_messages->appendToolResult(callId, e.name, msg);
        logCall(e.name, e.preview, QStringLiteral("denied"));
        saveActive();
        maybeContinue(row);
    }
}

void Assistant::confirmAll(bool run)
{
    // Snapshot pending call ids first; resolving mutates the model.
    QList<QPair<int, QString>> pending;
    for (int r = 0; r < m_messages->rowCount(); ++r) {
        const QVariantList calls = m_messages->data(m_messages->index(r), MessageModel::ToolCallsRole).toList();
        for (const QVariant &cv : calls) {
            const QVariantMap cm = cv.toMap();
            if (cm.value(QStringLiteral("status")).toString() == QLatin1String("pending"))
                pending.append({r, cm.value(QStringLiteral("id")).toString()});
        }
    }
    for (const auto &p : pending)
        confirmTool(p.first, p.second, run);
}

void Assistant::executeCall(int row, const ToolCallEntry &call)
{
    // Rebuild the invocation from the raw arguments stored on the model
    // (argumentsJson is kept in the model's JSON, not in the variant map).
    QString argsJson;
    const QJsonArray all = m_messages->toJson();
    if (row < all.size()) {
        const QJsonArray tc = all.at(row).toObject().value(QStringLiteral("toolCalls")).toArray();
        for (const QJsonValue &v : tc)
            if (v.toObject().value(QStringLiteral("id")).toString() == call.id)
                argsJson = v.toObject().value(QStringLiteral("argumentsJson")).toString();
    }

    ToolInvocation inv{call.id, call.name, QJsonDocument::fromJson(argsJson.toUtf8()).object()};
    m_messages->setToolStatus(row, call.id, QStringLiteral("running"));
    logCall(call.name, call.preview, QStringLiteral("running"));
    setPhase(QStringLiteral("thinking"));
    ++m_executing;
    Q_EMIT phaseChanged();

    m_registry->execute(inv, [this, row, call](const ToolOutcome &out) {
        --m_executing;
        m_messages->setToolStatus(row, call.id, out.ok ? QStringLiteral("done") : QStringLiteral("error"), out.text);
        m_messages->appendToolResult(call.id, call.name, out.text);
        logCall(call.name, call.preview, out.ok ? QStringLiteral("done") : QStringLiteral("error"));
        saveActive();
        maybeContinue(row);
    });
}

void Assistant::maybeContinue(int row)
{
    if (m_executing > 0)
        return; // wait for the rest of this round
    // If any call in this row is still pending, we are waiting on the user.
    const QVariantList calls = m_messages->data(m_messages->index(row), MessageModel::ToolCallsRole).toList();
    for (const QVariant &cv : calls) {
        if (cv.toMap().value(QStringLiteral("status")).toString() == QLatin1String("pending")) {
            setPhase(QStringLiteral("awaiting"));
            return;
        }
    }
    // All calls in this round are resolved: feed results back to the model.
    if (++m_toolRounds > kMaxToolRounds) {
        m_messages->appendError(tr("Stopped after %1 rounds of tool use to avoid a loop.").arg(kMaxToolRounds));
        saveActive();
        setPhase(QStringLiteral("idle"));
        return;
    }
    startCompletion();
}

void Assistant::stop()
{
    m_client->cancel();
    if (m_assistantRow >= 0) {
        const QString partial = m_messages->data(m_messages->index(m_assistantRow), MessageModel::ContentRole).toString();
        m_messages->finishAssistant(m_assistantRow, partial, {}, m_registry);
        m_assistantRow = -1;
    }
    saveActive();
    setPhase(m_config->apiKeyConfigured() ? QStringLiteral("idle") : QStringLiteral("setup"));
    Q_EMIT phaseChanged();
}

void Assistant::logCall(const QString &name, const QString &preview, const QString &status)
{
    m_log.prepend(QVariantMap{
        {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))},
        {QStringLiteral("name"), name},
        {QStringLiteral("preview"), preview},
        {QStringLiteral("status"), status},
    });
    while (m_log.size() > 200)
        m_log.removeLast();
    Q_EMIT toolLogChanged();
}

void Assistant::copyToClipboard(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}
