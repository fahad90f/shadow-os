// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

// The desk's conversation for the window: one row per message, oldest first.
// Rows change in place (an answer that grows word by word, a message that
// has been read), so a view keeps its scroll position and only the row that
// changed is laid out again.
class DeskMessages : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    struct Message
    {
        qint64 id = 0;
        QString role;     // "you" | "desk" | "note"
        QString text;
        // you:  "accepted" -> "read" -> "understood"
        // desk: "waiting" -> "thinking" -> "writing" -> "done" | "failed" | "stopped" | "interrupted"
        // note: "done"
        QString state;
        QString kind;     // note: "market" | "team" | "info" | "warning"; desk: "model" | "live"
        qint64 time = 0;
        qint64 replyTo = 0;   // desk: the message it answers
        QString by;           // desk: the model's name, or "live data"
        QString market;       // the market it is about, if any
        QStringList tags;     // you: what was understood, short
        qint64 ms = 0;        // desk: how long the answer took
        QString detail;       // failed: why; you: "read by …"
        QString raw;          // desk: the answer as received, the state block included; not shown, not saved
    };

    enum Role {
        IdRole = Qt::UserRole + 1, RoleRole, TextRole, StateRole, KindRole, TimeRole, ReplyToRole, ByRole,
        MarketRole, TagsRole, MsRole, DetailRole,
    };

    explicit DeskMessages(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return int(m_rows.size()); }

    void append(const Message &message);
    // Right below the message 'after' (at the end when there is none).
    void insertAfter(qint64 after, const Message &message);
    // The row of a message, -1 when there is none.
    int rowOf(qint64 id) const;
    const Message *find(qint64 id) const;
    // Change a message; 'roles' says which roles changed (empty: all).
    template<typename F>
    bool update(qint64 id, F change, const QList<int> &roles = {})
    {
        const int row = rowOf(id);
        if (row < 0)
            return false;
        change(m_rows[row]);
        const QModelIndex at = index(row);
        Q_EMIT dataChanged(at, at, roles);
        return true;
    }
    const QList<Message> &all() const { return m_rows; }
    void reset(const QList<Message> &rows);
    // Keep the newest 'max' rows.
    void trim(int max);

    static QJsonObject toJson(const Message &m);
    static Message fromJson(const QJsonObject &o);

Q_SIGNALS:
    void countChanged();

private:
    QList<Message> m_rows;
};
