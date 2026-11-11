// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// List model over a set of records (QVariantMap), with filtering and sorting
// done here so that NxDataTable only renders.
//
//   RecordModel {
//       roles: ["destination", "gateway", "device"]
//       records: NetState.routes              // array of objects
//       filterText: search.text               // substring match over filterRoles (all roles if empty)
//       criteria: ({ family: "IPv4" })        // exact matches; a list value matches any of its items
//   }
//   NxDataTable { model: routes; onSortRequested: (role, asc) => routes.sortBy(role, asc) }
//
// Updating 'records' never resets the model: rows are updated in place and
// only the tail is inserted or removed, so views keep their scroll position
// and delegates while the data refreshes every few seconds.
class RecordModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QStringList roles READ roles WRITE setRoles NOTIFY rolesChanged)
    Q_PROPERTY(QVariantList records READ records WRITE setRecords NOTIFY recordsChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
    Q_PROPERTY(QStringList filterRoles READ filterRoles WRITE setFilterRoles NOTIFY filterChanged)
    Q_PROPERTY(QVariantMap criteria READ criteria WRITE setCriteria NOTIFY filterChanged)
    Q_PROPERTY(QString sortRole READ sortRole NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY sortChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY recordsChanged)

public:
    explicit RecordModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QStringList roles() const { return m_roles; }
    void setRoles(const QStringList &roles);
    QVariantList records() const { return m_records; }
    void setRecords(const QVariantList &records);
    QString filterText() const { return m_filterText; }
    void setFilterText(const QString &text);
    QStringList filterRoles() const { return m_filterRoles; }
    void setFilterRoles(const QStringList &roles);
    QVariantMap criteria() const { return m_criteria; }
    void setCriteria(const QVariantMap &criteria);
    QString sortRole() const { return m_sortRole; }
    bool sortAscending() const { return m_sortAscending; }
    int count() const { return int(m_rows.size()); }
    int totalCount() const { return int(m_records.size()); }

    Q_INVOKABLE void sortBy(const QString &role, bool ascending = true);
    // The record shown in 'row' (empty map if out of range).
    Q_INVOKABLE QVariantMap get(int row) const;
    // First visible row whose 'role' equals 'value', or -1.
    Q_INVOKABLE int indexOf(const QString &role, const QVariant &value) const;

    // Digit runs compare by value, everything else case-insensitively, so
    // "eth10" sorts after "eth9" and 10.0.0.9 before 10.0.0.10.
    static int naturalCompare(const QString &a, const QString &b);

Q_SIGNALS:
    void rolesChanged();
    void recordsChanged();
    void filterChanged();
    void sortChanged();
    void countChanged();

private:
    bool accepts(const QVariantMap &record) const;
    void rebuild();

    QStringList m_roles;
    QVariantList m_records;
    QList<QVariantMap> m_rows;      // filtered and sorted
    QString m_filterText;
    QStringList m_filterRoles;
    QVariantMap m_criteria;
    QString m_sortRole;
    bool m_sortAscending = true;
};
