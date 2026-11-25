// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class TradingConfig;

// One question to one of NVIDIA's hosted models, answered in full (no
// streaming: the answer is a small JSON object). The wire protocol is the
// OpenAI-compatible /chat/completions of https://integrate.api.nvidia.com/v1;
// every model shares that endpoint and the one key.
//
// A large model can take minutes, and a hosted service sometimes drops or
// refuses a request that would succeed a moment later. So that a slow model
// still gets its say: a question waits as long as the settings allow
// (nvidia/timeout_seconds), a question that got no answer, or a "too many
// requests" or server error, is asked once more, and only so many questions
// are with the service at a time (nvidia/parallel); the rest wait their turn
// here, where waiting does not count against their time.
//
// The key is read from TradingConfig at send time and goes only into the
// request's Authorization header. It is never logged and never part of an
// error message.
class ModelClient : public QObject
{
    Q_OBJECT
public:
    explicit ModelClient(TradingConfig *config, QObject *parent = nullptr);
    ~ModelClient() override;

    struct Question
    {
        QString model;       // the model id, which is also how the answer is addressed
        QString system;
        QString user;
        QByteArray imagePng; // a picture for a model that reads pictures; empty for the others
        int timeoutMs = 0;   // how long to wait for the answer; 0: what the settings say
        int attempts = 2;    // how often it is asked at most
    };
    // Ask; answered() or failed() follows, exactly once. A model that is still
    // busy with an earlier question fails the new one with "busy".
    void ask(const Question &question);
    void cancelAll();
    // Questions with the service right now (the others wait their turn).
    int inFlight() const { return m_inFlight; }
    bool busy(const QString &model) const { return m_pending.contains(model); }

    // The models the service offers; modelsListed() follows.
    void listModels();
    bool catalogKnown() const { return m_catalogKnown; }
    QStringList catalog() const { return m_catalog; }
    // False only when the service's list is known and does not have the model.
    bool offered(const QString &model) const { return !m_catalogKnown || m_catalog.contains(model); }

Q_SIGNALS:
    void answered(const QString &model, const QString &text, qint64 ms);
    // code: no_key | busy | network | timeout | auth | forbidden | not_found |
    // rate_limit | server | refusal | bad_response
    void failed(const QString &model, const QString &code, const QString &message, qint64 ms);
    // The question got no usable answer and is being asked once more.
    void retrying(const QString &model, const QString &why);
    void modelsListed(const QStringList &models, const QString &error);

private:
    struct Pending
    {
        Question question;
        QPointer<QNetworkReply> reply; // null while it waits for its turn or for its second try
        QNetworkAccessManager *net = nullptr;
        qint64 startedMs = 0;          // when it was asked for
        int attempt = 0;               // times sent
        int waitMs = 0;                // how long the last attempt was given
        quint64 token = 0;             // tells this question from a later one to the same model
    };
    void pump();
    void send(const QString &model);
    void finish(const QString &model, QNetworkReply *reply);
    void fail(const QString &model, const QString &code, const QString &message);
    // Ask again after 'delayMs' if the question may be asked once more; false if not.
    bool retry(const QString &model, const QString &why, int delayMs);
    QNetworkAccessManager *takeNet();

    TradingConfig *m_config;
    QHash<QString, Pending> m_pending;
    QStringList m_queue;      // models whose question waits for its turn
    int m_inFlight = 0;
    quint64 m_nextToken = 1;
    // One connection pool per question in flight, reused afterwards. A single
    // pool opens at most six connections to a host, so the seventh model
    // would wait for the first to finish and could time out in the queue.
    QList<QNetworkAccessManager *> m_idleNets;
    QNetworkAccessManager *m_listNet;
    QPointer<QNetworkReply> m_listing;
    QStringList m_catalog;
    bool m_catalogKnown = false;
};
