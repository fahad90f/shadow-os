// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class TradingConfig;

// Conversations with NVIDIA's hosted models for the desk: any number of
// requests, several to the same model at once, each addressed by its own id,
// answered word by word as the model writes ("stream": server-sent events of
// the OpenAI-compatible /chat/completions), and each one can be stopped.
//
// It is separate from ModelClient, which serves the analysis team, so that a
// question in the conversation never waits behind a team analysis.
//
// A request that fails before any word arrived (no connection, "too many
// requests", a server error, no data in time) is sent once more. Once words
// have been shown, it is not: the answer would start again from the top.
//
// The key is read from TradingConfig at send time and goes only into the
// request's Authorization header; it is never logged or part of a message.
class ChatStream : public QObject
{
    Q_OBJECT
public:
    explicit ChatStream(TradingConfig *config, QObject *parent = nullptr);
    ~ChatStream() override;

    struct Request
    {
        QString model;
        QJsonArray messages;   // [{role, content}, …]
        int maxTokens = 2048;
        double temperature = 0.3;
        bool stream = true;
        // How long the service may be silent: before the first data, and
        // between two pieces of a streamed answer.
        int idleTimeoutMs = 60000;
        int attempts = 2;
        int priority = 0;      // higher first, among the waiting ones
    };

    // Ask. Returns the request's id (> 0). started(), then delta()s, then
    // finished() or failed() follow, the last exactly once (also for a
    // request that waits its turn and is then cancelled: failed "cancelled").
    quint64 start(const Request &request);
    // Stop a request, waiting or under way. failed(id, "cancelled") follows.
    void cancel(quint64 id);
    void cancelAll();
    bool contains(quint64 id) const { return m_requests.contains(id); }
    // Requests with the service right now, and waiting for their turn.
    int active() const { return m_active; }
    int waiting() const { return m_queue.size(); }
    // At most this many with the service at a time.
    void setLimit(int limit);

Q_SIGNALS:
    void started(quint64 id);       // sent to the service
    void thinking(quint64 id);      // a reasoning model is working: data arrives, no answer yet
    void delta(quint64 id, const QString &text); // the next piece of the answer
    void finished(quint64 id, const QString &text, qint64 ms);
    // code: no_key | cancelled | network | timeout | auth | forbidden | not_found |
    // rate_limit | server | refusal | bad_response
    void failed(quint64 id, const QString &code, const QString &message, qint64 ms);
    void retrying(quint64 id, const QString &why);

private:
    struct Pending
    {
        Request request;
        QPointer<QNetworkReply> reply;
        QNetworkAccessManager *net = nullptr;
        qint64 askedMs = 0;   // when start() was called
        int attempt = 0;
        QByteArray buffer;    // unread bytes of the event stream
        QString text;         // the answer so far
        bool sawData = false; // anything at all arrived
        bool sawWords = false;// a piece of the answer was handed out
        bool done = false;    // [DONE] seen
        bool streamed = false;// the service answered with an event stream
        bool refusal = false;
    };
    void pump();
    void send(quint64 id);
    void readMore(quint64 id);
    void finish(quint64 id);
    void fail(quint64 id, const QString &code, const QString &message);
    bool retry(quint64 id, const QString &why, int delayMs);
    void release(Pending &pending);
    QNetworkAccessManager *takeNet();

    TradingConfig *m_config;
    QHash<quint64, Pending> m_requests;
    QList<quint64> m_queue;
    int m_active = 0;
    int m_limit = 3;
    quint64 m_next = 1;
    QList<QNetworkAccessManager *> m_idleNets;
};
