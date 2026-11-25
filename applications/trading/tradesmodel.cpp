// SPDX-License-Identifier: GPL-3.0-or-later
#include "tradesmodel.h"

#include "tradingengine.h"

TradesModel::TradesModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

QObject *TradesModel::source() const
{
    return m_engine;
}

void TradesModel::setSource(QObject *source)
{
    auto *engine = qobject_cast<TradingEngine *>(source);
    if (engine == m_engine)
        return;
    if (m_engine)
        disconnect(m_engine, nullptr, this, nullptr);
    m_engine = engine;
    m_closedFor = -1;
    if (m_engine)
        connect(m_engine, &TradingEngine::accountChanged, this, &TradesModel::refresh);
    Q_EMIT sourceChanged();
    refresh();
}

void TradesModel::setLimit(int limit)
{
    if (limit == m_limit)
        return;
    m_limit = limit;
    Q_EMIT limitChanged();
    refresh();
}

void TradesModel::setOpenOnly(bool on)
{
    if (on == m_openOnly)
        return;
    m_openOnly = on;
    Q_EMIT limitChanged();
    refresh();
}

int TradesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant TradesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    return role == OpenRole ? QVariant(row.open) : role == TradeRole ? QVariant(row.trade) : QVariant();
}

QHash<int, QByteArray> TradesModel::roleNames() const
{
    return {{OpenRole, "open"}, {TradeRole, "trade"}};
}

void TradesModel::refresh()
{
    QList<Row> rows;
    if (m_engine) {
        const QVariantList open = m_engine->positions();
        for (const QVariant &v : open)
            rows.append({true, v.toMap()});
        if (!m_openOnly) {
            // The history changes only when a trade closes or the account is reset.
            const int closed = m_engine->closedTradeCount();
            if (closed != m_closedFor) {
                m_closed.clear();
                const QVariantList trades = m_engine->trades();
                for (const QVariant &v : trades)
                    m_closed.append({false, v.toMap()});
                m_closedFor = closed;
            }
            rows += m_closed;
        }
    }
    if (m_limit > 0 && rows.size() > m_limit)
        rows.resize(m_limit);

    // The same trades in the same order: only their numbers may have changed.
    const auto key = [](const Row &row) { return row.trade.value(QStringLiteral("id")).toLongLong(); };
    bool same = rows.size() == m_rows.size();
    for (qsizetype i = 0; same && i < rows.size(); ++i)
        same = rows.at(i).open == m_rows.at(i).open && key(rows.at(i)) == key(m_rows.at(i));
    if (!same) {
        const bool counted = rows.size() != m_rows.size();
        beginResetModel();
        m_rows = rows;
        endResetModel();
        if (counted)
            Q_EMIT countChanged();
        return;
    }
    for (qsizetype i = 0; i < rows.size(); ++i) {
        if (rows.at(i).trade == m_rows.at(i).trade)
            continue;
        m_rows[i].trade = rows.at(i).trade;
        const QModelIndex at = index(int(i));
        Q_EMIT dataChanged(at, at, {TradeRole});
    }
}
