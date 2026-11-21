// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistant.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QTimer>

namespace {
// Backstop against a genuinely stuck model that calls tools forever. Set high
// enough that real multi-step agent work (scan → enumerate → read → write a
// report, etc.) never trips it; it exists only to cap a runaway loop.
constexpr int kMaxToolRounds = 100;
// Automatic retries for a transient API failure (server error, overload,
// timeout, rate limit) before the error is shown to the user. These are the
// "something went wrong, try again later" cases; retrying quietly clears most
// of them, especially on large high-effort requests.
constexpr int kMaxRetries = 3;
// What a lead model is asked for at most (NVIDIA), until it says it takes less.
constexpr int kDefaultReplyTokens = 8192;
}

Assistant::Assistant(QObject *parent)
    : QObject(parent)
{
    m_config = new AssistantConfig(this);
    m_store = new ConversationStore(this);
    m_messages = new MessageModel(this);
    m_registry = new ToolRegistry(m_config, this);
    m_client = new ChatClient(m_config, this);
    m_catalog = new ModelCatalog(m_config, this);
    m_council = new Council(m_config, m_catalog, this);
    m_vision = new VisionTeam(m_config, m_catalog, this);
    m_registry->setVision(m_vision);

    connect(m_config, &AssistantConfig::changed, this, [this]() {
        // A key saved (or removed) in Settings: the first-start page goes
        // away (or comes back) without a restart.
        if (!busy()) {
            const bool configured = m_config->apiKeyConfigured();
            if (configured && m_phase == QLatin1String("setup"))
                m_phase = hasPending() ? QStringLiteral("awaiting") : QStringLiteral("idle");
            else if (!configured && m_phase != QLatin1String("setup"))
                m_phase = QStringLiteral("setup");
        }
        Q_EMIT phaseChanged();
        Q_EMIT teamChanged(); // the model shown follows the service in use
    });
    connect(m_council, &Council::membersChanged, this, &Assistant::teamChanged);
    connect(m_council, &Council::ready, this, [this]() {
        // The other models are done (or the wait is over): the lead starts.
        Q_EMIT teamChanged();
        if (m_phase == QLatin1String("thinking") && !m_client->busy())
            startCompletion();
    });
    connect(m_registry->journal(), &UndoJournal::changed, this, &Assistant::actionLogChanged);

    connect(m_client, &ChatClient::delta, this, [this](const QString &text) {
        if (m_assistantRow >= 0) {
            setPhase(QStringLiteral("streaming"));
            m_messages->appendDelta(m_assistantRow, text);
            Q_EMIT replyDelta(text);
        }
    });
    connect(m_client, &ChatClient::completed, this,
            [this](const QString &content, const QJsonArray &toolCalls, const QJsonArray &thinking, const QString &) {
        if (m_assistantRow < 0)
            return;
        m_retries = 0; // a completion arrived: this request succeeded
        if (m_config->nvidia())
            m_catalog->noteKey(true);
        const int row = m_assistantRow;
        m_messages->finishAssistant(row, content, toolCalls, thinking, m_registry);
        m_assistantRow = -1;
        saveActive();
        Q_EMIT replySegmentFinished();

        if (toolCalls.isEmpty()) {
            m_council->setLeadState(QStringLiteral("done"));
            setPhase(QStringLiteral("idle"));
            if (!content.trimmed().isEmpty())
                Q_EMIT assistantReplyReady(content);
            return;
        }
        // Decide per call: auto-run (always under the default autopilot level,
        // read-only tools under ask_risky) or confirm in the UI. A call that
        // needs confirmation stays "pending" and the UI renders a card for it.
        const QVariantList vlist = m_messages->data(m_messages->index(row), MessageModel::ToolCallsRole).toList();
        bool anyPending = false;
        // Defer any continuation until the whole round is dispatched, so a
        // synchronously-completing tool does not trigger a second one.
        m_dispatching = true;
        for (const QVariant &cv : vlist) {
            const QVariantMap cm = cv.toMap();
            ToolCallEntry e;
            e.id = cm.value(QStringLiteral("id")).toString();
            e.name = cm.value(QStringLiteral("name")).toString();
            e.summary = cm.value(QStringLiteral("summary")).toString();
            e.preview = cm.value(QStringLiteral("preview")).toString();
            if (needsConfirmation(e.name)) {
                if (!anyPending) // announce the first pending call for spoken approval
                    Q_EMIT confirmationNeeded(e.summary, e.preview, e.id);
                anyPending = true; // stays pending; UI shows a confirmation card
            } else {
                executeCall(row, e);
            }
        }
        m_dispatching = false;
        if (anyPending)
            setPhase(QStringLiteral("awaiting"));
        else
            maybeContinue(row);
    });
    connect(m_client, &ChatClient::failed, this, [this](const QString &code, const QString &message) {
        // A failure reported from inside the stream (an SSE "error" event,
        // e.g. overloaded_error) arrives while the reply is still open, so the
        // client would stay busy until the server closes the connection — and
        // nothing notifies busy/canSend after that. The request is dead:
        // close it now, so the phase changes below carry busy=false.
        if (m_client->busy())
            m_client->cancel();
        if (code == QLatin1String("rate_limit"))
            m_council->noteRateLimit(); // smaller teams for a while
        if (code == QLatin1String("auth") && m_config->nvidia())
            m_catalog->noteKey(false); // Settings shows the key as not accepted
        // Auto-retry transient failures (server error, overload, timeout, rate
        // limit) — the "something went wrong, try again later" cases — but only
        // when nothing was streamed yet, so a retry can't duplicate partial
        // output. Permanent errors (bad request, auth, not found, refusal) are
        // shown immediately; retrying them would just fail again.
        const bool transient = code == QLatin1String("server") || code == QLatin1String("timeout")
                            || code == QLatin1String("rate_limit") || code == QLatin1String("network");
        const bool nothingStreamed = m_assistantRow < 0
            || m_messages->data(m_messages->index(m_assistantRow), MessageModel::ContentRole).toString().trimmed().isEmpty();
        if (transient && nothingStreamed && m_retries < kMaxRetries) {
            ++m_retries;
            m_messages->dropEmptyStreamingTail(); // discard the empty failed bubble
            m_assistantRow = -1;
            setPhase(QStringLiteral("thinking"));
            const int delayMs = 1500 * (1 << (m_retries - 1)); // 1.5s → 3s → 6s backoff
            restartLater(delayMs);
            return;
        }
        m_retries = 0;
        // NVIDIA: the model accepts only shorter replies than were asked
        // for: ask the same model again for half as much.
        if (nothingStreamed && m_config->nvidia() && code == QLatin1String("bad_response") && !m_turnLead.isEmpty()) {
            const QString lower = message.toLower();
            const int asked = m_replyLimit.value(m_turnLead, kDefaultReplyTokens);
            if (asked > 1024 && (lower.contains(QLatin1String("max_tokens")) || lower.contains(QLatin1String("max_completion_tokens"))
                                 || lower.contains(QLatin1String("maximum output")) || lower.contains(QLatin1String("output tokens")))) {
                m_replyLimit.insert(m_turnLead, asked / 2);
                m_messages->dropEmptyStreamingTail();
                m_assistantRow = -1;
                setPhase(QStringLiteral("thinking"));
                restartLater(0);
                return;
            }
        }
        // NVIDIA: this lead cannot do it (not offered to this key, cannot use
        // tools, refused, or still failing after the retries): the next
        // model of the group takes over the same request.
        if (nothingStreamed && tryNextLead(code))
            return;
        m_council->setLeadState(QStringLiteral("failed"));
        if (m_assistantRow >= 0) {
            // Drop the empty streaming placeholder's streaming flag.
            m_messages->finishAssistant(m_assistantRow, m_messages->data(m_messages->index(m_assistantRow), MessageModel::ContentRole).toString(), {}, {}, m_registry);
            m_assistantRow = -1;
        }
        m_messages->appendError(message);
        saveActive();
        setPhase(QStringLiteral("error"));
        Q_EMIT errorMessage(message);
        // Return to idle so the user can retry.
        setPhase(m_config->apiKeyConfigured() ? QStringLiteral("idle") : QStringLiteral("setup"));
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

void Assistant::settleLoadedConversation()
{
    const QString p = !m_config->apiKeyConfigured() ? QStringLiteral("setup")
        : hasPending()                              ? QStringLiteral("awaiting")
                                                    : QStringLiteral("idle");
    if (p != m_phase)
        setPhase(p);
    else
        Q_EMIT phaseChanged(); // hasPending/canSend follow the rows, not the phase string
}

QString Assistant::activeModel() const
{
    if (m_config->localMode())
        return m_config->localModel();
    if (m_config->nvidia()) {
        if (!m_turnLead.isEmpty())
            return m_turnLead;
        return m_catalog->lead(m_thinkHard ? QStringLiteral("agents") : QStringLiteral("chat"));
    }
    return m_config->modelForRole(m_thinkHard ? QStringLiteral("planner") : QStringLiteral("brain"));
}

void Assistant::newConversation()
{
    ++m_turn;
    m_council->clear();
    m_turnLead.clear();
    m_activeId = m_store->createConversation();
    m_messages->clearAll();
    m_assistantRow = -1;
    Q_EMIT activeChanged();
    settleLoadedConversation();
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
    ++m_turn;
    m_council->clear();
    m_turnLead.clear();
    m_activeId = id;
    m_messages->loadFrom(m_store->loadMessages(id));
    m_assistantRow = -1;
    Q_EMIT activeChanged();
    // A conversation left (or saved) while a tool call waited for a decision
    // comes back with that question open: it can still be answered.
    settleLoadedConversation();
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
        if (!busy())
            settleLoadedConversation();
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
    const bool pentestMode = m_config->role() == QLatin1String("pentest");

    QString base = QStringLiteral(
        "You are Shadow, a helpful, concise personal AI assistant running on "
        "Shadow OS (a Debian-based security/development Linux desktop). You are often used "
        "hands-free by voice: the user says \"Shadow\" to wake you, speaks a command, and "
        "hears your reply spoken aloud, so keep replies short, natural and speakable — avoid "
        "long code blocks or tables unless asked. You carry out real tasks on this computer by "
        "calling the provided tools: run commands, read/write/create/move/delete files and "
        "folders, write code (optionally opening it in the editor), open the browser and URLs, "
        "do a web search, open applications, run a command in the terminal, type text into the "
        "focused window (dictation), and take screenshots. Prefer calling a tool over guessing, "
        "and call one tool at a time. %1 Today is %2.");
    const QString approval = m_config->autonomy() == QLatin1String("autopilot")
        ? QStringLiteral(
              "Your tool calls run immediately, without the user approving them first, so act on "
              "the request directly, wait for each tool's result, and report what happened; never "
              "ask the user to approve or confirm a tool call.")
        : QStringLiteral(
              "Actions that change the system or reach the network are confirmed by the user "
              "before they run (a card, or a spoken yes/no), so call the tool and wait for its "
              "result rather than claiming a task is done.");

    if (pentestMode) {
        base += QStringLiteral(
            "\n\n--- PENETRATION TESTING MODE ---\n"
            "You are operating as a professional penetration testing AI. You have direct access to "
            "a Kali-class toolkit pre-installed on this Shadow OS system. Help the user conduct "
            "thorough, methodical security assessments using the pentest_scan, web_enum, "
            "search_exploits, run_sqlmap, crack_hash, brute_service and start_capture tools, "
            "as well as run_shell_command and run_in_terminal for any tool not covered by a "
            "dedicated wrapper. Follow the standard pentest methodology: reconnaissance → "
            "scanning → enumeration → exploitation → post-exploitation → reporting.\n"
            "KEY RULES:\n"
            "1. ALWAYS verify scope before running active/destructive tests — ask the user to "
            "   confirm the target IP/domain/scope in writing before any offensive step.\n"
            "2. Use only on systems the user owns or has a signed contract to test.\n"
            "3. For passive/read-only steps (searchsploit, nmap, nikto, hashcat) you can "
            "   proceed without scope confirmation. Active exploitation (sqlmap, hydra, msf) "
            "   always needs explicit confirmation.\n"
            "4. When an exploit succeeds, pivot methodically and document findings.\n"
            "5. Recommend remediation for every vulnerability found.\n"
            "Available specialized tools: pentest_scan (nmap profiles), web_enum (nikto+gobuster), "
            "search_exploits (searchsploit), run_sqlmap (SQL injection), crack_hash (hashcat/john), "
            "brute_service (hydra), start_capture (tcpdump).");
    }

    return base.arg(approval, QDate::currentDate().toString(Qt::ISODate));
}

bool Assistant::needsConfirmation(const QString &toolName) const
{
    // Autopilot: the user has chosen to run every tool without being asked.
    // (The hard safety rails in ToolRegistry still refuse the handful of
    // self-destructive paths, e.g. deleting the system root, so the assistant
    // cannot wipe its own OS by accident; everything else just runs.)
    if (m_config->autonomy() == QLatin1String("autopilot"))
        return false;
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
    m_retries = 0;
    beginTurn(trimmed, false);
}

void Assistant::sendSpokenMessage(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || !canSend())
        return;
    ensureConversation();
    m_messages->appendUser(trimmed);
    m_store->touch(m_activeId, trimmed);
    saveActive();
    m_toolRounds = 0;
    m_retries = 0;
    beginTurn(trimmed, true);
}

QString Assistant::lastUserText() const
{
    for (int r = m_messages->rowCount() - 1; r >= 0; --r) {
        const QModelIndex idx = m_messages->index(r);
        if (m_messages->data(idx, MessageModel::KindRole).toString() == QLatin1String("user"))
            return m_messages->data(idx, MessageModel::ContentRole).toString();
    }
    return {};
}

void Assistant::beginTurn(const QString &userText, bool spoken)
{
    if (!m_config->apiKeyConfigured()) {
        setPhase(QStringLiteral("setup"));
        return;
    }
    ++m_turn;
    m_restartPending = false;
    m_turnLead.clear();
    m_leadsTried.clear();
    if (!m_config->nvidia()) {
        // Claude, or a model on this computer: one model answers.
        m_council->clear();
        startCompletion();
        return;
    }
    const Council::Plan plan = m_council->plan(userText, m_thinkHard);
    if (plan.lead.isEmpty()) {
        const QString message = tr("NVIDIA offers none of the models configured for this kind of request. "
                                   "Open Settings and read the model list again.");
        m_messages->appendError(message);
        saveActive();
        setPhase(QStringLiteral("error"));
        Q_EMIT errorMessage(message);
        setPhase(QStringLiteral("idle"));
        return;
    }
    m_turnLead = plan.lead;
    m_leadsTried << plan.lead;
    const QJsonArray tools = m_registry->hasEnabledTools() ? m_registry->apiTools() : QJsonArray{};
    // busy()/canSend() change inside begin() (the team starts working), so
    // the phase is switched quietly first and notified after, as in
    // startCompletion().
    m_phase = QStringLiteral("thinking");
    m_council->begin(plan, m_messages->apiMessages(systemPrompt()), tools, spoken, m_thinkHard);
    Q_EMIT phaseChanged();
    Q_EMIT teamChanged();
    // Council::ready() starts the lead (at once when it works alone).
}

bool Assistant::tryNextLead(const QString &code)
{
    if (!m_config->nvidia() || m_turnLead.isEmpty())
        return false;
    // The key was rejected, the service cannot be reached, or the account is
    // over its request limit: no other model would fare better.
    if (code == QLatin1String("auth") || code == QLatin1String("no_key") || code == QLatin1String("network")
        || code == QLatin1String("rate_limit"))
        return false;
    if (code == QLatin1String("no_tools"))
        m_catalog->learnNoTools(m_turnLead);
    if (m_leadsTried.size() >= 4)
        return false;
    const QString next = m_catalog->lead(m_council->kind().isEmpty() ? QStringLiteral("chat") : m_council->kind(),
                                         m_leadsTried);
    if (next.isEmpty())
        return false;
    m_turnLead = next;
    m_leadsTried << next;
    m_council->setLead(next);
    m_messages->dropEmptyStreamingTail();
    m_assistantRow = -1;
    setPhase(QStringLiteral("thinking"));
    restartLater(0);
    return true;
}

// Start the request again after a pause (a retry, the next lead) — unless the
// turn it belongs to is over by then.
void Assistant::restartLater(int delayMs)
{
    const int turn = m_turn;
    m_restartPending = true;
    QTimer::singleShot(delayMs, this, [this, turn]() {
        if (turn != m_turn)
            return; // stopped, or the user moved on: that turn's flag was cleared with it
        m_restartPending = false;
        if (m_phase == QLatin1String("thinking")) {
            startCompletion();
        } else {
            Q_EMIT phaseChanged(); // busy() changed
        }
    });
}

void Assistant::startCompletion()
{
    if (!m_config->apiKeyConfigured()) {
        setPhase(QStringLiteral("setup"));
        return;
    }
    QString model;
    QString system = systemPrompt();
    if (m_config->nvidia()) {
        if (m_turnLead.isEmpty()) {
            // A turn taken up again (a retry, an answer to a question that was
            // still open after a restart): no team notes, the kind's lead.
            m_turnLead = m_catalog->lead(Council::classify(lastUserText()));
            if (m_turnLead.isEmpty())
                m_turnLead = m_catalog->lead(QStringLiteral("chat"));
            m_leadsTried = {m_turnLead};
        }
        model = m_turnLead;
        system += m_council->notes();
        m_council->setLeadState(QStringLiteral("working"));
    } else {
        model = m_thinkHard ? m_config->modelForRole(QStringLiteral("planner"))
                            : m_config->modelForRole(QStringLiteral("brain"));
    }
    const QJsonArray request = m_messages->apiMessages(system);
    const QJsonArray tools = m_registry->hasEnabledTools() ? m_registry->apiTools() : QJsonArray{};
    m_assistantRow = m_messages->appendAssistantStreaming();
    // busy()/canSend() only change inside send() (the client becomes busy),
    // so phaseChanged — their NOTIFY — must fire after send(), not before:
    // fired first, QML would read busy=false/canSend=true and keep those
    // stale values for the whole request. The phase is switched quietly
    // first, so a synchronous failure inside send() (no key, bad URL) still
    // moves it on (thinking → error → idle) and notifies by itself.
    const bool changed = m_phase != QLatin1String("thinking");
    m_phase = QStringLiteral("thinking");
    ChatClient::Options options;
    if (m_config->nvidia())
        options.maxTokens = m_replyLimit.value(model, kDefaultReplyTokens);
    m_client->send(model, request, tools, options);
    if (m_phase == QLatin1String("thinking") && (changed || m_client->busy()))
        Q_EMIT phaseChanged();
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
        logCall(callId, e.name, e.preview, QStringLiteral("denied"));
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

QVariantMap Assistant::firstPendingCall() const
{
    for (int r = 0; r < m_messages->rowCount(); ++r) {
        const QVariantList calls = m_messages->data(m_messages->index(r), MessageModel::ToolCallsRole).toList();
        for (const QVariant &cv : calls) {
            const QVariantMap cm = cv.toMap();
            if (cm.value(QStringLiteral("status")).toString() == QLatin1String("pending"))
                return QVariantMap{{QStringLiteral("id"), cm.value(QStringLiteral("id"))},
                                   {QStringLiteral("name"), cm.value(QStringLiteral("name"))},
                                   {QStringLiteral("summary"), cm.value(QStringLiteral("summary"))},
                                   {QStringLiteral("preview"), cm.value(QStringLiteral("preview"))}};
        }
    }
    return {};
}

bool Assistant::retry()
{
    if (!canSend())
        return false;
    QJsonArray rows = m_messages->toJson();
    bool failed = false;
    // The failed turn, from the end: its error row(s), then the reply that
    // failed — empty, or cut off mid-stream (a reply with tool calls is a
    // finished step of the turn and stays).
    while (!rows.isEmpty() && rows.last().toObject().value(QStringLiteral("kind")).toString() == QLatin1String("error")) {
        rows.removeLast();
        failed = true;
    }
    if (!failed)
        return false;
    if (!rows.isEmpty()) {
        const QJsonObject last = rows.last().toObject();
        if (last.value(QStringLiteral("kind")).toString() == QLatin1String("assistant")
            && last.value(QStringLiteral("toolCalls")).toArray().isEmpty())
            rows.removeLast();
    }
    // Something to answer: the user's message, or tool results to go on from.
    const QString kind = rows.isEmpty() ? QString() : rows.last().toObject().value(QStringLiteral("kind")).toString();
    if (kind != QLatin1String("user") && kind != QLatin1String("tool"))
        return false;
    m_messages->loadFrom(rows);
    m_assistantRow = -1;
    saveActive();
    m_toolRounds = 0;
    m_retries = 0;
    beginTurn(lastUserText(), false);
    return true;
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
    logCall(call.id, call.name, call.preview, QStringLiteral("running"));
    setPhase(QStringLiteral("thinking"));
    ++m_executing;
    Q_EMIT phaseChanged();

    const int turn = m_turn;
    m_registry->execute(inv, [this, row, call, turn](const ToolOutcome &out) {
        --m_executing;
        // busy() just changed (e.g. a tool that outlived a Stop): notify, or
        // QML keeps busy=true and canSend=false until something else changes.
        Q_EMIT phaseChanged();
        m_messages->setToolStatus(row, call.id, out.ok ? QStringLiteral("done") : QStringLiteral("error"), out.text);
        m_messages->appendToolResult(call.id, call.name, out.text);
        logCall(call.id, call.name, call.preview, out.ok ? QStringLiteral("done") : QStringLiteral("error"));
        saveActive();
        // A tool that outlived its turn (Stop, then a new message): its result
        // is recorded, but it does not start a request inside the new turn.
        if (turn == m_turn)
            maybeContinue(row);
    });
}

void Assistant::maybeContinue(int row)
{
    if (m_dispatching)
        return; // a tool finished mid-dispatch; the end-of-loop call continues
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
    // Start the next turn on a clean event-loop tick, never nested inside the
    // previous reply's finished() handler. That synchronous re-entrancy
    // (onFinished → completed → maybeContinue → startCompletion → send, which
    // opens a new QNetworkReply while the old one's slot is still on the stack)
    // is what crashed the assistant after a few tool rounds. A single-shot
    // guard stops two queued continuations from racing into two requests.
    if (m_continueScheduled)
        return;
    m_continueScheduled = true;
    const int turn = m_turn;
    QTimer::singleShot(0, this, [this, turn]() {
        m_continueScheduled = false;
        if (turn != m_turn)
            return;
        // If the user stopped (or an error landed) in the meantime, don't open
        // a new request. A live continuation is always mid-run (thinking /
        // awaiting / streaming); idle/error/setup means we should stand down.
        if (m_phase == QLatin1String("idle") || m_phase == QLatin1String("error")
            || m_phase == QLatin1String("setup"))
            return;
        startCompletion();
    });
}

void Assistant::stop()
{
    ++m_turn; // whatever was scheduled for this turn stands down
    m_restartPending = false;
    m_vision->cancel(); // a picture not yet sent stays here
    m_council->cancel();
    m_council->setLeadState(QStringLiteral("failed"));
    m_client->cancel();
    if (m_assistantRow >= 0) {
        const QString partial = m_messages->data(m_messages->index(m_assistantRow), MessageModel::ContentRole).toString();
        m_messages->finishAssistant(m_assistantRow, partial, {}, {}, m_registry);
        m_assistantRow = -1;
    }
    saveActive();
    setPhase(m_config->apiKeyConfigured() ? QStringLiteral("idle") : QStringLiteral("setup"));
    Q_EMIT phaseChanged();
    Q_EMIT stopped();
}

void Assistant::logCall(const QString &callId, const QString &name, const QString &preview, const QString &status)
{
    // The same call again (running → done/error): its one entry is updated
    // and moves to the top (the log reads newest change first).
    if (!callId.isEmpty()) {
        for (qsizetype i = 0; i < m_log.size(); ++i) {
            if (m_log.at(i).toMap().value(QStringLiteral("id")).toString() == callId) {
                m_log.removeAt(i);
                break;
            }
        }
    }
    m_log.prepend(QVariantMap{
        {QStringLiteral("id"), callId},
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

QVariantList Assistant::actionLog() const
{
    QVariantList out;
    const QJsonArray entries = m_registry->journal()->list();
    for (const QJsonValue &v : entries)
        out.append(v.toObject().toVariantMap());
    return out;
}

QString Assistant::undoAction(const QString &id)
{
    const QString err = m_registry->journal()->undo(id);
    if (!err.isEmpty())
        Q_EMIT errorMessage(err);
    return err;
}
