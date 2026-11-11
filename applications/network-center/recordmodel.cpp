// SPDX-License-Identifier: GPL-3.0-or-later
#include "recordmodel.h"

#include <algorithm>

namespace {

bool isNumeric(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Bool:
        return true;
    default:
        return false;
    }
}

QString textOf(const QVariant &v)
{
    if (v.typeId() == QMetaType::QStringList || v.typeId() == QMetaType::QVariantList)
        return v.toStringList().join(QLatin1Char(' '));
    return v.toString();
}

} // namespace

RecordModel::RecordModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int RecordModel::naturalCompare(const QString &a, const QString &b)
{
    int i = 0, j = 0;
    const int na = int(a.size()), nb = int(b.size());
    while (i < na && j < nb) {
        const QChar ca = a.at(i), cb = b.at(j);
        if (ca.isDigit() && cb.isDigit()) {
            int si = i, sj = j;
            while (si < na && a.at(si) == QLatin1Char('0')) ++si;
            while (sj < nb && b.at(sj) == QLatin1Char('0')) ++sj;
            int ei = si, ej = sj;
            while (ei < na && a.at(ei).isDigit()) ++ei;
            while (ej < nb && b.at(ej).isDigit()) ++ej;
            if (ei - si != ej - sj)
                return (ei - si) < (ej - sj) ? -1 : 1;      // more digits → larger number
            for (int k = 0; k < ei - si; ++k) {
                if (a.at(si + k) != b.at(sj + k))
                    return a.at(si + k) < b.at(sj + k) ? -1 : 1;
            }
            i = ei;
            j = ej;
            continue;
        }
        const QChar la = ca.toLower(), lb = cb.toLower();
        if (la != lb)
            return la < lb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < na) return 1;
    if (j < nb) return -1;
    return 0;
}

int RecordModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant RecordModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const int r = role - Qt::UserRole - 1;
    if (r < 0 || r >= m_roles.size())
        return {};
    return m_rows.at(index.row()).value(m_roles.at(r));
}

QHash<int, QByteArray> RecordModel::roleNames() const
{
    QHash<int, QByteArray> names;
    for (int i = 0; i < m_roles.size(); ++i)
        names.insert(Qt::UserRole + 1 + i, m_roles.at(i).toUtf8());
    return names;
}

void RecordModel::setRoles(const QStringList &roles)
{
    if (roles == m_roles)
        return;
    beginResetModel();
    m_roles = roles;
    endResetModel();
    Q_EMIT rolesChanged();
}

void RecordModel::setRecords(const QVariantList &records)
{
    if (records == m_records)
        return;
    m_records = records;
    rebuild();
    Q_EMIT recordsChanged();
}

void RecordModel::setFilterText(const QString &text)
{
    if (text == m_filterText)
        return;
    m_filterText = text;
    rebuild();
    Q_EMIT filterChanged();
}

void RecordModel::setFilterRoles(const QStringList &roles)
{
    if (roles == m_filterRoles)
        return;
    m_filterRoles = roles;
    rebuild();
    Q_EMIT filterChanged();
}

void RecordModel::setCriteria(const QVariantMap &criteria)
{
    if (criteria == m_criteria)
        return;
    m_criteria = criteria;
    rebuild();
    Q_EMIT filterChanged();
}

void RecordModel::sortBy(const QString &role, bool ascending)
{
    if (role == m_sortRole && ascending == m_sortAscending)
        return;
    m_sortRole = role;
    m_sortAscending = ascending;
    rebuild();
    Q_EMIT sortChanged();
}

QVariantMap RecordModel::get(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row) : QVariantMap();
}

int RecordModel::indexOf(const QString &role, const QVariant &value) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).value(role) == value)
            return i;
    }
    return -1;
}

bool RecordModel::accepts(const QVariantMap &record) const
{
    for (auto it = m_criteria.constBegin(); it != m_criteria.constEnd(); ++it) {
        const QVariant actual = record.value(it.key());
        const QVariant &wanted = it.value();
        if (wanted.typeId() == QMetaType::QVariantList || wanted.typeId() == QMetaType::QStringList) {
            const QStringList options = wanted.toStringList();
            if (!options.contains(actual.toString()))
                return false;
        } else if (wanted.typeId() == QMetaType::Bool) {
            if (actual.toBool() != wanted.toBool())
                return false;
        } else if (actual.toString() != wanted.toString()) {
            return false;
        }
    }

    const QString needle = m_filterText.trimmed();
    if (needle.isEmpty())
        return true;
    // Every whitespace-separated word must occur somewhere in the row.
    const QStringList words = needle.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QStringList &searchRoles = m_filterRoles.isEmpty() ? m_roles : m_filterRoles;
    QString haystack;
    for (const QString &role : searchRoles) {
        haystack += textOf(record.value(role));
        haystack += QLatin1Char('\n');
    }
    for (const QString &word : words) {
        if (!haystack.contains(word, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

void RecordModel::rebuild()
{
    QList<QVariantMap> rows;
    rows.reserve(m_records.size());
    for (const QVariant &v : std::as_const(m_records)) {
        const QVariantMap record = v.toMap();
        if (accepts(record))
            rows.append(record);
    }

    if (!m_sortRole.isEmpty()) {
        const QString role = m_sortRole;
        const bool ascending = m_sortAscending;
        std::stable_sort(rows.begin(), rows.end(), [&role, ascending](const QVariantMap &a, const QVariantMap &b) {
            const QVariant x = a.value(role), y = b.value(role);
            int c;
            if (isNumeric(x) && isNumeric(y)) {
                const double dx = x.toDouble(), dy = y.toDouble();
                c = dx < dy ? -1 : dx > dy ? 1 : 0;
            } else {
                c = naturalCompare(textOf(x), textOf(y));
            }
            return ascending ? c < 0 : c > 0;
        });
    }

    const int oldCount = int(m_rows.size());
    const int newCount = int(rows.size());
    if (newCount < oldCount) {
        beginRemoveRows(QModelIndex(), newCount, oldCount - 1);
        m_rows.resize(newCount);
        endRemoveRows();
    }
    const int common = qMin(oldCount, newCount);
    int firstChanged = -1, lastChanged = -1;
    for (int i = 0; i < common; ++i) {
        if (m_rows.at(i) != rows.at(i)) {
            m_rows[i] = rows.at(i);
            if (firstChanged < 0)
                firstChanged = i;
            lastChanged = i;
        }
    }
    if (firstChanged >= 0)
        Q_EMIT dataChanged(index(firstChanged), index(lastChanged));
    if (newCount > oldCount) {
        beginInsertRows(QModelIndex(), oldCount, newCount - 1);
        for (int i = oldCount; i < newCount; ++i)
            m_rows.append(rows.at(i));
        endInsertRows();
    }
    if (newCount != oldCount)
        Q_EMIT countChanged();
}
