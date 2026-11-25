// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class TradingEngine;

// The trades of the paper account as a list for the window: the open
// positions first (with their running profit), then the closed trades, newest
// first.
//
// Prices arrive many times a second, and every one of them changes a running
// profit. This model changes only those numbers in the rows that are there;
// rows come and go only when a position opens or closes. A list rebuilt on
// every price would be torn down and built again under the pointer, and a
// button in it (Close) could not be clicked.
class TradesModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *source READ source WRITE setSource NOTIFY sourceChanged)
    // At most this many rows; 0 = all.
    Q_PROPERTY(int limit READ limit WRITE setLimit NOTIFY limitChanged)
    // Leave the closed trades out (the Portfolio's list of open positions).
    Q_PROPERTY(bool openOnly READ openOnly WRITE setOpenOnly NOTIFY limitChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles { OpenRole = Qt::UserRole + 1, TradeRole };
    explicit TradesModel(QObject *parent = nullptr);

    QObject *source() const;
    void setSource(QObject *source);
    int limit() const { return m_limit; }
    void setLimit(int limit);
    bool openOnly() const { return m_openOnly; }
    void setOpenOnly(bool on);
    int count() const { return int(m_rows.size()); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    void sourceChanged();
    void limitChanged();
    void countChanged();

private:
    struct Row { bool open = false; QVariantMap trade; };
    void refresh();

    QPointer<TradingEngine> m_engine;
    QList<Row> m_rows;
    QList<Row> m_closed;      // the closed trades, fetched again only when their number changes
    int m_closedFor = -1;
    int m_limit = 0;
    bool m_openOnly = false;
};
