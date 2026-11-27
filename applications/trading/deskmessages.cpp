// SPDX-License-Identifier: GPL-3.0-or-later
#include "deskmessages.h"

#include <QJsonObject>

DeskMessages::DeskMessages(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DeskMessages::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant DeskMessages::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Message &m = m_rows.at(index.row());
    switch (role) {
    case IdRole: return double(m.id);
    case RoleRole: return m.role;
    case TextRole: return m.text;
    case StateRole: return m.state;
    case KindRole: return m.kind;
    case TimeRole: return double(m.time);
    case ReplyToRole: return double(m.replyTo);
    case ByRole: return m.by;
    case MarketRole: return m.market;
    case TagsRole: return m.tags;
    case MsRole: return double(m.ms);
    case DetailRole: return m.detail;
    default: return {};
    }
}

QHash<int, QByteArray> DeskMessages::roleNames() const
{
    return {{IdRole, "messageId"}, {RoleRole, "role"}, {TextRole, "text"}, {StateRole, "phase"},
            {KindRole, "kind"}, {TimeRole, "time"}, {ReplyToRole, "replyTo"}, {ByRole, "by"},
            {MarketRole, "market"}, {TagsRole, "tags"}, {MsRole, "ms"}, {DetailRole, "detail"}};
}

void DeskMessages::append(const Message &message)
{
    beginInsertRows({}, int(m_rows.size()), int(m_rows.size()));
    m_rows.append(message);
    endInsertRows();
    Q_EMIT countChanged();
}

int DeskMessages::rowOf(qint64 id) const
{
    // The rows that change are almost always the newest.
    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (m_rows.at(i).id == id)
            return i;
    }
    return -1;
}

const DeskMessages::Message *DeskMessages::find(qint64 id) const
{
    const int row = rowOf(id);
    return row < 0 ? nullptr : &m_rows.at(row);
}

void DeskMessages::reset(const QList<Message> &rows)
{
    beginResetModel();
    m_rows = rows;
    endResetModel();
    Q_EMIT countChanged();
}

void DeskMessages::trim(int max)
{
    const int extra = int(m_rows.size()) - max;
    if (extra <= 0)
        return;
    beginRemoveRows({}, 0, extra - 1);
    m_rows.erase(m_rows.begin(), m_rows.begin() + extra);
    endRemoveRows();
    Q_EMIT countChanged();
}

QJsonObject DeskMessages::toJson(const Message &m)
{
    QJsonObject o{{QStringLiteral("id"), m.id}, {QStringLiteral("role"), m.role}, {QStringLiteral("text"), m.text},
                  {QStringLiteral("state"), m.state}, {QStringLiteral("time"), m.time}};
    if (!m.kind.isEmpty())
        o.insert(QStringLiteral("kind"), m.kind);
    if (m.replyTo)
        o.insert(QStringLiteral("reply_to"), m.replyTo);
    if (!m.by.isEmpty())
        o.insert(QStringLiteral("by"), m.by);
    if (!m.market.isEmpty())
        o.insert(QStringLiteral("market"), m.market);
    if (!m.tags.isEmpty())
        o.insert(QStringLiteral("tags"), QJsonArray::fromStringList(m.tags));
    if (m.ms)
        o.insert(QStringLiteral("ms"), m.ms);
    if (!m.detail.isEmpty())
        o.insert(QStringLiteral("detail"), m.detail);
    return o;
}

DeskMessages::Message DeskMessages::fromJson(const QJsonObject &o)
{
    Message m;
    m.id = o.value(QLatin1String("id")).toInteger();
    m.role = o.value(QLatin1String("role")).toString();
    m.text = o.value(QLatin1String("text")).toString();
    m.state = o.value(QLatin1String("state")).toString();
    m.kind = o.value(QLatin1String("kind")).toString();
    m.time = o.value(QLatin1String("time")).toInteger();
    m.replyTo = o.value(QLatin1String("reply_to")).toInteger();
    m.by = o.value(QLatin1String("by")).toString();
    m.market = o.value(QLatin1String("market")).toString();
    const QJsonArray tags = o.value(QLatin1String("tags")).toArray();
    for (const QJsonValue &t : tags)
        m.tags.append(t.toString());
    m.ms = o.value(QLatin1String("ms")).toInteger();
    m.detail = o.value(QLatin1String("detail")).toString();
    return m;
}
