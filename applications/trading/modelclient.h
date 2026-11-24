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
        int timeoutMs = 120000;
    };
    // Ask; answered() or failed() follows, exactly once. A model that is still
    // busy with an earlier question fails the new one with "busy".
    void ask(const Question &question);
    void cancelAll();
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
    void modelsListed(const QStringList &models, const QString &error);

private:
    struct Pending
    {
        QPointer<QNetworkReply> reply;
        QNetworkAccessManager *net = nullptr;
        qint64 startedMs = 0;
    };
    void finish(const QString &model, QNetworkReply *reply);
    void fail(const QString &model, const QString &code, const QString &message);
    QNetworkAccessManager *takeNet();

    TradingConfig *m_config;
    QHash<QString, Pending> m_pending;
    // One connection pool per question in flight, reused afterwards. A single
    // pool opens at most six connections to a host, so the seventh model
    // would wait for the first to finish and could time out in the queue.
    QList<QNetworkAccessManager *> m_idleNets;
    QNetworkAccessManager *m_listNet;
    QPointer<QNetworkReply> m_listing;
    QStringList m_catalog;
    bool m_catalogKnown = false;
};
