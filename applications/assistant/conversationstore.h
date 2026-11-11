// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

class ToolRegistry;

// One tool call attached to an assistant message, tracked through its life:
// pending (awaiting confirmation) → running → done/error, or denied.
struct ToolCallEntry
{
    QString id;
    QString name;
    QString argumentsJson; // raw JSON string from the model
    QString summary;       // one-line human label
    QString preview;       // exact command/path/args shown for confirmation
    QString status = QStringLiteral("pending");
    QString result;
    bool risky = false;
    bool changesSystem = false;
};

struct Message
{
    QString kind;      // "user" | "assistant" | "tool" | "error"
    QString content;
    bool streaming = false;
    QList<ToolCallEntry> toolCalls; // assistant messages only
    QString toolCallId;             // tool messages: which call this answers
    QString toolName;               // tool messages
};

// The active conversation's messages, as a list model for the chat view and as
// the source of the OpenAI-format request array.
class MessageModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Role {
        KindRole = Qt::UserRole + 1,
        ContentRole,
        StreamingRole,
        ToolCallsRole,
        HasToolCallsRole,
        ToolNameRole,
    };
    explicit MessageModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int appendUser(const QString &text);
    int appendAssistantStreaming();
    void appendDelta(int row, const QString &text);
    void finishAssistant(int row, const QString &content, const QJsonArray &toolCalls, ToolRegistry *registry);
    int appendToolResult(const QString &toolCallId, const QString &toolName, const QString &result);
    int appendError(const QString &text);

    // Update a pending tool call within an assistant message.
    void setToolStatus(int row, const QString &callId, const QString &status, const QString &result = {});
    bool hasPendingTools() const;

    void loadFrom(const QJsonArray &json);
    QJsonArray toJson() const;
    // OpenAI request array including a leading system message.
    QJsonArray apiMessages(const QString &systemPrompt) const;

    void clearAll();

Q_SIGNALS:
    void countChanged();

private:
    QVariantList toolCallsVariant(const Message &m) const;
    QList<Message> m_messages;
};

// The list of saved conversations and their on-disk persistence under
// ~/.local/share/shadow/assistant/<id>.json.
class ConversationStore : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Role { IdRole = Qt::UserRole + 1, TitleRole, UpdatedRole, SubtitleRole };
    explicit ConversationStore(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString createConversation();
    void renameConversation(const QString &id, const QString &title);
    void removeConversation(const QString &id);
    void touch(const QString &id, const QString &derivedTitle);

    QJsonArray loadMessages(const QString &id) const;
    void saveMessages(const QString &id, const QJsonArray &messages);

    QString storageDir() const;
    Q_INVOKABLE int indexOfId(const QString &id) const;
    Q_INVOKABLE QString titleOf(const QString &id) const;

Q_SIGNALS:
    void countChanged();

private:
    struct Conversation { QString id; QString title; QDateTime updated; };
    QString pathFor(const QString &id) const;
    void load();
    void writeMeta(const Conversation &c, const QJsonArray &messages);
    void sortByUpdated();

    QList<Conversation> m_items;
};
