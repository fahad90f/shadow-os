// SPDX-License-Identifier: GPL-3.0-or-later
#include "council.h"

#include "assistantconfig.h"
#include "chatclient.h"
#include "modelcatalog.h"

#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

namespace {
// What one earlier message may take of an advisor's prompt, and how many
// earlier messages it sees: the latest request matters most.
constexpr int kMaxMessageChars = 6000;
constexpr int kMaxMessages = 16;
// What one advisor's contribution may take of the lead's prompt.
constexpr int kMaxNoteChars = 7000;
constexpr int kMaxToolsListed = 30;
// After a "too many requests" answer: how small the team is, and for how long.
constexpr int kThrottledAdvisors = 2;
constexpr qint64 kThrottleMs = 90 * 1000;

QString clip(const QString &text, int max)
{
    return text.size() <= max ? text : text.left(max) + QStringLiteral("\n[… cut]");
}

QString firstSentence(const QString &text)
{
    const qsizetype dot = text.indexOf(QLatin1String(". "));
    return (dot > 0 ? text.left(dot + 1) : text).simplified();
}
} // namespace

Council::Council(AssistantConfig *config, ModelCatalog *catalog, QObject *parent)
    : QObject(parent), m_config(config), m_catalog(catalog)
{
    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this]() {
        // The wait is over: whoever has not answered is left out.
        for (int i = 0; i < m_members.size(); ++i) {
            if (m_members.at(i).state == QLatin1String("working"))
                finishMember(i, m_generation, QStringLiteral("late"), {}, tr("too slow"));
        }
        maybeReady();
    });
}

Council::~Council()
{
    // Requests still in flight (the app is closed while the team works) are
    // ended here, while the clients and their connection pools all exist.
    cancel();
}

QNetworkAccessManager *Council::netFor(int index)
{
    while (m_nets.size() <= index) {
        auto *net = new QNetworkAccessManager(this);
        net->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        m_nets.append(net);
    }
    return m_nets.at(index);
}

// ---- which kind of request -------------------------------------------------

QString Council::classify(const QString &text)
{
    const QString t = text.toLower();

    // Code: a code block, a source file, a language, or programming vocabulary.
    // Words with an everyday meaning too ("class", "program", "rust", "java")
    // count only in a programming phrase ("write a program", "in Rust").
    static const QRegularExpression codeWords(QStringLiteral(
        "```|c\\+\\+|c#"
        "|\\b(code|coding|programming|function|debug|debugging|compile|compiler|refactor|algorithm|regex"
        "|regular expression|sql|json|yaml|html|css|javascript|typescript|python|kotlin|golang|cpp|bash|qml|cmake"
        "|makefile|dockerfile|unit tests?|stack trace|traceback|segfault|syntax error|recursion|pull request"
        "|source code|shell script|rest api|api endpoint|git (commit|rebase|merge|diff)|bug in"
        "|fix (the|this|that|a|my) bug)\\b"
        "|\\b(write|create|make|build|fix|debug|review|explain|refactor|optimi[sz]e|test)( me)?"
        "( a| an| the| this| that| my| some)?( \\w+)? (program|script|class|method|app|application|library|module"
        "|query|loop|snippet)\\b"
        "|\\bin (rust|swift|java|go|c|ruby|php|perl|lua|haskell|scala)\\b"
        "|\\.(py|js|ts|tsx|jsx|c|h|cc|cpp|hpp|rs|go|java|kt|sh|qml|html|css|json|ya?ml|sql|toml)\\b"));
    if (codeWords.match(t).hasMatch())
        return QStringLiteral("coding");

    // Actions: something to do on this computer (the lead will use tools).
    // A verb counts only where an instruction puts it — at the start, after
    // punctuation, or after "please" / "can you" / "then" — so that "How do I
    // make pancakes?" and "What type of dog…" stay conversation.
    static const QRegularExpression instruction(QStringLiteral(
        "(^|[.!?;:,]\\s*|\\b(please|can you|could you|would you|will you|i need you to|i want you to|i'd like you to"
        "|go ahead and|then|and)\\s+)"
        "(open|launch|start|run|execute|install|uninstall|remove|delete|create|make|move|copy|rename|download"
        "|save|close|kill|restart|reboot|shut ?down|scan|ping|mount|unmount|update|upgrade|type|play|pause|mute"
        "|unmute|connect|disconnect|enable|disable|show|list|find|check|look at|search)\\b"));
    // Phrases that mean an action wherever they stand.
    static const QRegularExpression actionPhrases(QStringLiteral(
        "\\b(take a (screenshot|picture)|screenshot|search (the web|online)|look (it |this |that )?up"
        "|find (the |a |my )?(file|folder)s?|(list|show)( me)? (the |my |all )?(files|folders|directories|processes)"
        "|(check|show)( me)? (the |my )?(disk|memory|cpu|battery|network|wi-?fi|internet|updates|ip)"
        "|what('s| is) (using|on|taking) (up )?(my |the )?(disk|screen|memory|cpu|space)"
        "|how much (disk|space|memory|ram|battery)|turn (it |this |that )?(on|off)|switch (on|off)"
        "|set (the |my )?(volume|brightness|wallpaper|timer|alarm)|clean up|empty the (trash|bin)"
        "|(on|at) (the |my )?screen|in (the |my )?(home|downloads|documents) (folder|directory)"
        "|my (system|computer|laptop|pc|disk|files|folders|battery|network|wi-?fi)"
        "|on (this|my) (computer|machine|system|laptop|pc))\\b"));
    if (instruction.match(t).hasMatch() || actionPhrases.match(t).hasMatch())
        return QStringLiteral("agents");

    return QStringLiteral("chat");
}

Council::Plan Council::plan(const QString &userText, bool thorough) const
{
    Plan p;
    // In pentest mode every request is work with tools.
    p.kind = m_config->role() == QLatin1String("pentest") ? QStringLiteral("agents") : classify(userText);
    p.lead = m_catalog->lead(p.kind);
    if (!m_config->teamMode())
        return p; // solo: the lead alone
    QStringList pool = m_catalog->members(p.kind);
    if (thorough) {
        for (const char *other : {"coding", "agents", "chat"})
            for (const QString &id : m_catalog->members(QLatin1String(other)))
                if (!pool.contains(id))
                    pool << id;
    }
    for (const QString &id : pool)
        if (id != p.lead)
            p.advisors << id;
    int cap = m_config->teamMaxModels() > 0 ? m_config->teamMaxModels() - 1 : -1;
    if (throttled())
        cap = cap < 0 ? kThrottledAdvisors : qMin(cap, kThrottledAdvisors);
    if (cap >= 0 && p.advisors.size() > cap)
        p.advisors = p.advisors.mid(0, cap);
    return p;
}

void Council::noteRateLimit()
{
    m_throttledUntil = QDateTime::currentMSecsSinceEpoch() + kThrottleMs;
}

bool Council::throttled() const
{
    return QDateTime::currentMSecsSinceEpoch() < m_throttledUntil;
}

// ---- the advisors' prompt ----------------------------------------------------

QJsonArray Council::advisorMessages(const QJsonArray &conversation, const QJsonArray &tools) const
{
    QString system = QStringLiteral(
        "You are one of several AI models working in parallel as a team for \"Shadow\", a personal assistant on "
        "a Linux desktop (Shadow OS, Debian-based). A lead model will read every member's contribution and "
        "write the final reply; the user never sees your text. Contribute your best work on the user's latest "
        "message: be specific, correct and compact, with no preamble and no questions back. ");
    if (m_plan.kind == QLatin1String("coding")) {
        system += QStringLiteral(
            "This is a programming request: give complete, working code (or the exact fix), then in a few lines "
            "the pitfalls and edge cases you see. If there is a clearly better approach than the obvious one, say why. ");
    } else if (m_plan.kind == QLatin1String("agents")) {
        system += QStringLiteral(
            "This request needs things done on the computer: give the exact steps in order, with concrete "
            "commands, paths and arguments, and name anything risky (data loss, missing information). ");
    } else {
        system += QStringLiteral(
            "Answer directly in at most about 200 words, and say plainly what you are unsure of. ");
    }
    if (!tools.isEmpty()) {
        QStringList lines;
        for (const QJsonValue &tv : tools) {
            const QJsonObject fn = tv.toObject().value(QStringLiteral("function")).toObject();
            const QString name = fn.value(QStringLiteral("name")).toString();
            if (name.isEmpty() || lines.size() >= kMaxToolsListed)
                continue;
            lines << QStringLiteral("- %1: %2").arg(name, firstSentence(fn.value(QStringLiteral("description")).toString()));
        }
        system += QStringLiteral(
            "You cannot run anything yourself. The lead can, with these tools; when the request needs an action, "
            "name the tool and its arguments:\n") + lines.join(u'\n');
    }

    // The conversation as plain text turns. Tool calls and their results are
    // folded into the assistant's turn, and neighbouring turns of one role
    // are joined: several models accept only strictly alternating roles, and
    // none accepts tool messages without the tools themselves.
    struct Turn { QString role; QString text; };
    QList<Turn> turns;
    auto add = [&turns](const QString &role, const QString &text) {
        if (text.trimmed().isEmpty())
            return;
        if (!turns.isEmpty() && turns.last().role == role)
            turns.last().text += QStringLiteral("\n\n") + text;
        else
            turns.append({role, text});
    };
    for (const QJsonValue &mv : conversation) {
        const QJsonObject m = mv.toObject();
        const QString role = m.value(QStringLiteral("role")).toString();
        const QString content = m.value(QStringLiteral("content")).toString();
        if (role == QLatin1String("user")) {
            add(QStringLiteral("user"), clip(content, kMaxMessageChars));
        } else if (role == QLatin1String("assistant")) {
            QString text = clip(content, kMaxMessageChars);
            for (const QJsonValue &cv : m.value(QStringLiteral("tool_calls")).toArray()) {
                const QJsonObject fn = cv.toObject().value(QStringLiteral("function")).toObject();
                text += QStringLiteral("\n[action: %1 %2]")
                            .arg(fn.value(QStringLiteral("name")).toString(),
                                 clip(fn.value(QStringLiteral("arguments")).toString(), 400));
            }
            add(QStringLiteral("assistant"), text);
        } else if (role == QLatin1String("tool")) {
            add(QStringLiteral("assistant"), QStringLiteral("[result: %1]").arg(clip(content, 1500)));
        }
    }
    while (turns.size() > kMaxMessages)
        turns.removeFirst();
    while (!turns.isEmpty() && turns.first().role != QLatin1String("user"))
        turns.removeFirst();

    QJsonArray out;
    out.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), system}});
    for (const Turn &t : std::as_const(turns))
        out.append(QJsonObject{{QStringLiteral("role"), t.role}, {QStringLiteral("content"), t.text}});
    return out;
}

// ---- a turn -------------------------------------------------------------------

void Council::begin(const Plan &plan, const QJsonArray &conversation, const QJsonArray &tools, bool voice, bool thorough)
{
    cancel();
    m_plan = plan;
    m_leadState = QStringLiteral("waiting");
    m_members.clear();
    m_working = true;
    m_clock.start();
    const int generation = m_generation;

    const QJsonArray messages = advisorMessages(conversation, tools);
    ChatClient::Options options;
    options.maxTokens = m_plan.kind == QLatin1String("coding") ? 2000 : 700;
    if (thorough)
        options.maxTokens = options.maxTokens * 3 / 2;
    options.temperature = 0.5;
    const int waitMs = m_config->advisorWaitMs(m_plan.kind, voice, thorough);
    options.idleTimeoutMs = waitMs + 5000;

    for (const QString &id : m_plan.advisors) {
        Member member;
        member.id = id;
        member.state = QStringLiteral("working");
        member.client = new ChatClient(m_config, this, netFor(int(m_members.size())));
        m_members.append(member);
    }
    Q_EMIT membersChanged();

    for (int i = 0; i < m_members.size(); ++i) {
        ChatClient *client = m_members.at(i).client;
        connect(client, &ChatClient::completed, this,
                [this, i, generation](const QString &content, const QJsonArray &, const QJsonArray &, const QString &) {
            const QString text = content.trimmed();
            finishMember(i, generation, text.isEmpty() ? QStringLiteral("failed") : QStringLiteral("done"), text,
                         text.isEmpty() ? tr("an empty answer") : QString());
            maybeReady();
        });
        connect(client, &ChatClient::failed, this, [this, i, generation](const QString &code, const QString &message) {
            if (code == QLatin1String("rate_limit"))
                noteRateLimit();
            finishMember(i, generation, QStringLiteral("failed"), {}, message.section(u'\n', 0, 0));
            maybeReady();
        });
        // No tools: an advisor only writes.
        client->send(m_members.at(i).id, messages, QJsonArray{}, options);
    }

    if (m_members.isEmpty()) {
        // Nobody to wait for; still answer from the event loop, like a real wait.
        QTimer::singleShot(0, this, [this, generation]() {
            if (generation == m_generation)
                maybeReady();
        });
        return;
    }
    m_deadline->start(waitMs);
}

void Council::finishMember(int index, int generation, const QString &state, const QString &text, const QString &error)
{
    if (generation != m_generation || index < 0 || index >= m_members.size())
        return;
    Member &m = m_members[index];
    if (m.state != QLatin1String("working"))
        return;
    m.state = state;
    m.text = text;
    m.error = error;
    m.ms = m_clock.elapsed();
    if (state == QLatin1String("late") && m.client)
        m.client->cancel();
    Q_EMIT membersChanged();
}

void Council::maybeReady()
{
    if (!m_working)
        return;
    for (const Member &m : std::as_const(m_members))
        if (m.state == QLatin1String("working"))
            return;
    m_working = false;
    m_deadline->stop();
    Q_EMIT ready();
}

void Council::cancel()
{
    ++m_generation;
    m_deadline->stop();
    m_working = false;
    for (Member &m : m_members) {
        if (m.client) {
            m.client->disconnect(this);
            m.client->cancel();
            m.client->deleteLater();
            m.client = nullptr;
        }
        if (m.state == QLatin1String("working"))
            m.state = QStringLiteral("late");
    }
}

void Council::clear()
{
    cancel();
    const bool had = !m_members.isEmpty() || !m_plan.lead.isEmpty();
    m_members.clear();
    m_plan = Plan{};
    m_leadState.clear();
    if (had)
        Q_EMIT membersChanged();
}

void Council::setLead(const QString &id)
{
    if (id == m_plan.lead)
        return;
    m_plan.lead = id;
    Q_EMIT membersChanged();
}

void Council::setLeadState(const QString &state)
{
    if (state == m_leadState)
        return;
    m_leadState = state;
    Q_EMIT membersChanged();
}

int Council::contributed() const
{
    int n = 0;
    for (const Member &m : m_members)
        if (m.state == QLatin1String("done"))
            ++n;
    return n;
}

QString Council::notes() const
{
    QStringList parts;
    for (const Member &m : m_members) {
        if (m.state != QLatin1String("done"))
            continue;
        parts << QStringLiteral("[%1]\n%2").arg(ModelCatalog::shortName(m.id), clip(m.text, kMaxNoteChars));
    }
    if (parts.isEmpty())
        return {};
    return QStringLiteral(
               "\n\n--- Team notes ---\n"
               "%1 other AI models worked on the user's latest message at the same time as you. Their "
               "contributions follow; the user cannot see them.\n\n")
               .arg(parts.size())
        + parts.join(QStringLiteral("\n\n"))
        + QStringLiteral(
              "\n--- End of team notes ---\n"
              "Use the notes: combine the strongest ideas, prefer what several members agree on, and correct "
              "anything in them that is wrong. Then reply to the user, or call your tools, as you normally would. "
              "The notes are suggestions: the members could not run anything, so check before you rely on a claim "
              "about this computer. Do not mention the team or the notes unless the user asks.");
}

QVariantList Council::members() const
{
    QVariantList out;
    if (!m_plan.lead.isEmpty()) {
        out.append(QVariantMap{
            {QStringLiteral("id"), m_plan.lead},
            {QStringLiteral("name"), ModelCatalog::shortName(m_plan.lead)},
            {QStringLiteral("publisher"), ModelCatalog::publisher(m_plan.lead)},
            {QStringLiteral("role"), QStringLiteral("lead")},
            {QStringLiteral("state"), m_leadState.isEmpty() ? QStringLiteral("waiting") : m_leadState},
            {QStringLiteral("seconds"), 0.0},
        });
    }
    for (const Member &m : m_members) {
        out.append(QVariantMap{
            {QStringLiteral("id"), m.id},
            {QStringLiteral("name"), ModelCatalog::shortName(m.id)},
            {QStringLiteral("publisher"), ModelCatalog::publisher(m.id)},
            {QStringLiteral("role"), QStringLiteral("advisor")},
            {QStringLiteral("state"), m.state},
            {QStringLiteral("seconds"), m.ms / 1000.0},
            {QStringLiteral("error"), m.error},
        });
    }
    return out;
}
