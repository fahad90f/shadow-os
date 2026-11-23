// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class AssistantConfig;
class ChatClient;
class ModelCatalog;
class QNetworkAccessManager;
class QTimer;

// The model team: several models work on one request at the same time, and
// one of them — the lead — reads what the others wrote, combines it and
// answers the user or runs the tools.
//
// A turn has two steps:
//   1. begin(): every other model of the request's group (the advisors) gets
//      the conversation and writes its own answer or plan, all in parallel.
//      None of them can run a tool: a tool that changes the computer must run
//      once, not once per model. They are told which tools the lead has, so a
//      plan can name the exact steps.
//   2. ready(): when all advisors are done — or the wait is over, whichever
//      comes first — the caller starts the lead with notes() added to its
//      system prompt. The lead streams the reply and makes the tool calls;
//      further rounds of the same turn (after tool results) are the lead's
//      alone, with the same notes.
//
// An advisor that fails or is too slow is simply left out; a turn never fails
// because of an advisor. Which models take part is decided per request from
// the configured groups and what the service offers (ModelCatalog).
class Council : public QObject
{
    Q_OBJECT
public:
    Council(AssistantConfig *config, ModelCatalog *catalog, QObject *parent = nullptr);
    ~Council() override;

    // What kind of request a text is: "coding" | "agents" | "chat".
    static QString classify(const QString &text);

    struct Plan
    {
        QString kind;          // "coding" | "agents" | "chat"
        QString lead;          // model id; "" when the service offers none of the group
        QStringList advisors;  // the other models, in group order
    };
    // Who works on a request. 'thorough' (Think hard) brings in every model of
    // every group, not only the request's own.
    Plan plan(const QString &userText, bool thorough) const;

    // Step 1. 'conversation' is the neutral (OpenAI-shaped) message list the
    // lead will get, system prompt first; 'tools' the lead's tools.
    // 'skills' is the skill-instruction block (SkillLibrary::render) for this
    // request, added to every advisor's prompt so the team works the same way;
    // the lead gets it through the system prompt.
    void begin(const Plan &plan, const QJsonArray &conversation, const QJsonArray &tools, const QString &skills,
               bool voice, bool thorough);
    // Stop the advisors (the user pressed Stop). No ready() follows.
    void cancel();
    // Forget the turn (a new conversation, or the turn is over).
    void clear();

    bool working() const { return m_working; }
    QString kind() const { return m_plan.kind; }
    QString lead() const { return m_plan.lead; }
    // The lead was replaced (the first choice could not be used).
    void setLead(const QString &id);
    // "working" | "done" | "failed": shown next to the lead's name.
    void setLeadState(const QString &state);

    // What the advisors wrote, as a block for the lead's system prompt; ""
    // when nobody contributed.
    QString notes() const;
    // For the UI: [{id, name, publisher, role: "lead"|"advisor", state:
    // "waiting"|"working"|"done"|"failed"|"late", seconds}].
    QVariantList members() const;
    // How many advisors contributed.
    int contributed() const;

    // The service answered "too many requests". For the next minute and a
    // half the team is small (the lead and two others), so the lead's own
    // request is not starved by its advisors'.
    void noteRateLimit();
    bool throttled() const;

Q_SIGNALS:
    void membersChanged();
    void ready();

private:
    struct Member
    {
        QString id;
        QString state = QStringLiteral("waiting");
        QString text;
        QString error;
        qint64 ms = 0;
        ChatClient *client = nullptr;
    };

    QJsonArray advisorMessages(const QJsonArray &conversation, const QJsonArray &tools) const;
    QString m_skills; // the skill block for the current turn
    void finishMember(int index, int generation, const QString &state, const QString &text, const QString &error);
    void maybeReady();

    // The connection pool of advisor number 'index'.
    QNetworkAccessManager *netFor(int index);

    AssistantConfig *m_config;
    ModelCatalog *m_catalog;
    // One connection pool per advisor, kept between turns. A single pool
    // opens at most six HTTP/1.1 connections to a host, so the seventh model
    // would wait for the first to finish; with a pool each, all of them start
    // at once whatever protocol the service speaks.
    QList<QNetworkAccessManager *> m_nets;
    QTimer *m_deadline;
    Plan m_plan;
    QList<Member> m_members; // the advisors
    QString m_leadState;
    QElapsedTimer m_clock;
    bool m_working = false;
    int m_generation = 0; // replies of an abandoned turn are ignored
    qint64 m_throttledUntil = 0; // ms since the epoch
};
