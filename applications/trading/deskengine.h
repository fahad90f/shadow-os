// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/desk.h"
#include "deskmessages.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class ChatStream;
class QTimer;
class TradingConfig;
class TradingEngine;

// The desk: an analyst that follows the conversation and the market for as
// long as the application runs.
//
// A message is accepted at once (post() returns before anything is analysed)
// and then handled in layers, none of which blocks the next message:
//
// 1. Read, at once and without a model (core/desk.h): the market it names,
//    price levels, indicator and chart observations, the trade being weighed,
//    whether it asks something. The analysis state is updated field by field,
//    and levels are checked against the live price straight away. A question
//    that only asks for live facts (price, RSI, the team's signal …) is
//    answered here from the real data.
// 2. Understood, in the background, by a quick model that gets only the new
//    messages plus the current state and summary (never the whole
//    conversation again), and returns what to change. Messages that arrive
//    while it works are batched into its next request.
// 3. Answered, when a message asks something: a model writes the answer word
//    by word (streamed), with the accumulated state, the live market and the
//    recent conversation as context. Several answers can be under way; each
//    stays below the message it answers, and each can be stopped.
//
// Next to the conversation, the market side runs on its own: the live price
// against every level the conversation named (a note when it comes close or
// crosses), indicator readings compared as candles close, and the analysis
// team, whose result (and its forming call) feeds the state. A question about
// whether to trade starts the team in the background when its last call on
// that market is missing or old; the answer does not wait for it.
//
// Everything is kept in ~/.local/share/shadow/trading/desk.json and comes back
// when the application starts again; an answer that was cut off by closing
// the application says so and can be asked again.
class DeskEngine : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(DeskMessages *messages READ messages CONSTANT)
    Q_PROPERTY(bool running READ running NOTIFY statusChanged)
    Q_PROPERTY(QVariantMap status READ status NOTIFY statusChanged)
    Q_PROPERTY(QVariantList tasks READ tasks NOTIFY tasksChanged)
    Q_PROPERTY(QVariantMap analysis READ analysis NOTIFY analysisChanged)
    Q_PROPERTY(QVariantList changes READ changes NOTIFY analysisChanged)
    Q_PROPERTY(QVariantList signalChanges READ signalChanges NOTIFY analysisChanged)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
public:
    DeskEngine(TradingConfig *config, TradingEngine *engine, QObject *parent = nullptr);
    ~DeskEngine() override;

    // Load the saved conversation and start following the market.
    void start();

    DeskMessages *messages() const { return m_messages; }
    bool running() const { return m_running; }
    QVariantMap status() const;
    QVariantList tasks() const;
    QVariantMap analysis() const;
    QVariantList changes() const;
    QVariantList signalChanges() const;
    QVariantMap settings() const;

    // Accept a message. Returns {accepted, id, duplicate, error} at once; the
    // message is read on the next turn of the event loop. 'token' names one
    // draft: the same token twice is one message (a double press).
    Q_INVOKABLE QVariantMap post(const QString &text, const QString &token = QString());
    Q_INVOKABLE void stopAnswer(double id);
    Q_INVOKABLE void retryAnswer(double id);
    Q_INVOKABLE void clearConversation();
    // which: "fast" | "deep"
    Q_INVOKABLE QString setModel(const QString &which, const QString &model);
    Q_INVOKABLE void setFollowChart(bool on);

    // Nothing under way and nothing waiting (for the tests).
    bool idle() const;
    QJsonObject dump() const;

Q_SIGNALS:
    void statusChanged();
    void tasksChanged();
    void analysisChanged();
    void settingsChanged();
    void accepted(qint64 id);
    void answerFinished(qint64 id);
    void becameIdle();

private:
    struct Task
    {
        QString kind;           // "read" | "answer"
        QString model;
        qint64 since = 0;
        QString stage;          // "waiting" | "sent" | "thinking" | "writing"
        QList<qint64> batch;    // read: the messages
        qint64 message = 0;     // answer: the desk message being written
        qint64 question = 0;    // answer: the message it answers
        QString market;
        qint64 focusAt = 0;     // read: the message that last moved the focus when it was sent
    };
    struct Resolved
    {
        QString symbol;         // "" when the message named no market
        QString name;
        bool listed = false;    // on the watchlist: live data exists
        QString base;
    };

    void readMessage(qint64 id);
    Resolved resolve(const Trading::Desk::Reading &reading) const;
    // Live price of a market: the chart's when it is the selected one, else the watchlist's.
    bool livePrice(const QString &symbol, double *price) const;
    QStringList describe(const Trading::Desk::Reading &reading) const;
    QString liveFacts(const Trading::Desk::Reading &reading) const;
    // 'after': the message the answer goes below (0: at the end).
    void startAnswer(qint64 question, bool deep, const QString &market, qint64 after = 0);
    double nearPctFor(const QString &symbol) const;
    bool listed(const QString &symbol) const;
    void invalidateStatus();
    QVariantMap buildStatus() const;
    static QString withoutState(const QString &text);
    void maybeStartTeam(const QString &market);
    void scheduleRead();
    void flushRead();
    void onDelta(quint64 request, const QString &text);
    void onFinished(quint64 request, const QString &text, qint64 ms);
    void onFailed(quint64 request, const QString &code, const QString &message, qint64 ms);
    void applyReading(const Task &task, const QString &text);
    void applyAnswerState(const Task &task, const QJsonObject &state);
    void onMarketTick();
    void onSignal();
    void note(const QString &kind, const QString &text, const QString &market = QString());
    void notes(const std::vector<Trading::Desk::Change> &changes);
    void touched(const QString &source, const QString &what);
    void taskChanged();
    void invalidate();
    QVariantMap buildAnalysis() const;
    QJsonObject bookJson(const Trading::Desk::Book &book, bool compact) const;
    QJsonObject liveJson(const QString &symbol) const;
    QJsonArray recentTurns(qint64 before, int max) const;
    QString answerSystem() const;
    QString readSystem() const;
    static QString visiblePart(const QString &text);
    void scheduleSave();
    void load();
    void save();
    void logEvent(const QString &event, qint64 id, const QString &detail = QString());

    TradingConfig *m_config;
    TradingEngine *m_engine;
    DeskMessages *m_messages;
    ChatStream *m_stream;
    Trading::Desk::DeskState m_state;
    QHash<quint64, Task> m_tasks;
    QList<qint64> m_unread;          // read at once, not yet by the model
    quint64 m_reading = 0;           // the model's read under way
    QTimer *m_readTimer;
    QTimer *m_saveTimer;
    QTimer *m_tickTimer;
    QHash<QString, qint64> m_tokens; // draft token -> message id
    QStringList m_tokenOrder;
    QSet<qint64> m_answered;         // questions that got (or are getting) an answer
    QSet<QString> m_warnedUnlisted;
    qint64 m_nextId = 0;
    bool m_running = false;
    bool m_loaded = false;
    // Status for the window, each from a real event.
    qint64 m_lastRead = 0;
    qint64 m_lastUnderstood = 0;
    qint64 m_lastUpdate = 0;
    QString m_lastUpdateSource;
    QString m_lastUpdateText;
    QString m_readError;
    qint64 m_readErrorAt = 0;
    // The market side.
    double m_lastSignalTime = 0;
    bool m_teamAsked = false;        // the desk started the current team analysis
    QString m_teamMarket;
    qint64 m_teamSince = 0;
    struct Snapshot { QVariantMap readings; qint64 time = 0; QString zoneRsi; QString macd; QString trend; QString event; };
    QHash<QString, Snapshot> m_snapshots;
    QHash<QString, QString> m_teamCalls; // market -> the team's last action there
    QVariantList m_signalChanges;    // newest first
    qint64 m_lastTick = 0;
    QJsonArray m_log;                // events with times, for the tests
    mutable QVariantMap m_analysisCache;
    mutable QVariantMap m_statusCache;
    mutable bool m_statusDirty = true;
    qint64 m_focusMessage = 0;       // the message that last moved the conversation to a market
    bool m_hasKey = false;
    QString m_path;
    mutable bool m_analysisDirty = true;
    QElapsedTimer m_clock;
};
