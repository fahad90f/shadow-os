// SPDX-License-Identifier: GPL-3.0-or-later
#include "packagemodel.h"

#include "pkutil.h"

#include <algorithm>

namespace NexusPk {

PackageEntry PackageEntry::fromId(const QString &packageId, PackageKit::Transaction::Info rawInfo, const QString &summary)
{
    const PackageKit::Transaction::Info info = infoBase(rawInfo);
    const PackageId parsed = PackageId::parse(packageId);
    PackageEntry e;
    e.id = packageId;
    e.name = parsed.valid ? parsed.name : packageId;
    e.version = parsed.version;
    e.arch = parsed.arch;
    e.repo = parsed.repo;
    e.summary = summary;
    e.info = info;
    e.severity = infoSeverity(rawInfo);
    e.installed = parsed.installed || info == PackageKit::Transaction::InfoInstalled
                  || info == PackageKit::Transaction::InfoCollectionInstalled;
    e.automatic = parsed.automatic;
    return e;
}

QJsonObject PackageEntry::toJson() const
{
    QJsonObject o{
        {QStringLiteral("packageId"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("version"), version},
        {QStringLiteral("arch"), arch},
        {QStringLiteral("repo"), repo},
        {QStringLiteral("summary"), summary},
        {QStringLiteral("info"), infoKey(info)},
        {QStringLiteral("installed"), installed},
    };
    if (severity != PackageKit::Transaction::InfoUnknown)
        o.insert(QStringLiteral("severity"), infoKey(severity));
    if (installed)
        o.insert(QStringLiteral("automatic"), automatic);
    if (!installedVersion.isEmpty())
        o.insert(QStringLiteral("installedVersion"), installedVersion);
    if (size > 0)
        o.insert(QStringLiteral("size"), static_cast<qint64>(size));
    if (!group.isEmpty())
        o.insert(QStringLiteral("group"), group);
    if (!tags.isEmpty())
        o.insert(QStringLiteral("tags"), QJsonArray::fromStringList(tags));
    return o;
}

PackageModel::PackageModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PackageModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_visible.size());
}

QHash<int, QByteArray> PackageModel::roleNames() const
{
    return {
        {PackageIdRole, "packageId"}, {NameRole, "name"}, {VersionRole, "version"}, {ArchRole, "arch"},
        {RepoRole, "repo"}, {RepoTextRole, "repoText"}, {SummaryRole, "summary"}, {InfoRole, "info"},
        {InfoTextRole, "infoText"}, {InstalledRole, "installed"}, {AutomaticRole, "automatic"},
        {InstalledVersionRole, "installedVersion"}, {SizeRole, "size"}, {GroupRole, "group"},
        {TagsRole, "tags"}, {SelectedRole, "selected"}, {PercentageRole, "percentage"}, {StatusRole, "status"},
    };
}

QVariant PackageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};
    const PackageEntry &e = m_entries.at(m_visible.at(index.row()));
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return e.name;
    case PackageIdRole: return e.id;
    case VersionRole: return e.version;
    case ArchRole: return e.arch;
    case RepoRole: return e.repo;
    case RepoTextRole: return repoText(e.repo);
    case SummaryRole: return e.summary;
    case InfoRole: return infoKey(e.level());
    case InfoTextRole: return infoText(e.level());
    case InstalledRole: return e.installed;
    case AutomaticRole: return e.automatic;
    case InstalledVersionRole: return e.installedVersion;
    case SizeRole: return static_cast<double>(e.size);
    case GroupRole: return e.group;
    case TagsRole: return e.tags;
    case SelectedRole: return e.selected;
    case PercentageRole: return e.percentage;
    case StatusRole: return e.status;
    default: break;
    }
    return {};
}

QVariantMap PackageModel::get(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= m_visible.size())
        return map;
    const QModelIndex idx = index(row, 0);
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.constBegin(); it != names.constEnd(); ++it)
        map.insert(QString::fromLatin1(it.value()), data(idx, it.key()));
    return map;
}

bool PackageModel::matches(const PackageEntry &entry) const
{
    return m_filter.isEmpty() || entry.name.contains(m_filter, Qt::CaseInsensitive)
           || entry.summary.contains(m_filter, Qt::CaseInsensitive);
}

void PackageModel::rebuildVisible()
{
    m_visible.clear();
    m_byId.clear();
    m_visible.reserve(m_entries.size());
    for (int i = 0; i < m_entries.size(); ++i) {
        const PackageEntry &e = m_entries.at(i);
        if (!e.id.isEmpty())
            m_byId.insert(e.id, i);
        if (matches(e))
            m_visible.append(i);
    }
}

void PackageModel::setFilter(const QString &filter)
{
    const QString f = filter.trimmed();
    if (f == m_filter)
        return;
    beginResetModel();
    m_filter = f;
    rebuildVisible();
    endResetModel();
    Q_EMIT filterChanged();
    Q_EMIT countChanged();
}

void PackageModel::setEntries(const QList<PackageEntry> &entries)
{
    beginResetModel();
    m_entries = entries;
    rebuildVisible();
    endResetModel();
    Q_EMIT countChanged();
    Q_EMIT selectionChanged();
}

void PackageModel::clear()
{
    if (m_entries.isEmpty())
        return;
    setEntries({});
}

PackageEntry *PackageModel::find(const QString &packageId)
{
    const auto it = m_byId.constFind(packageId);
    return it == m_byId.constEnd() ? nullptr : &m_entries[it.value()];
}

int PackageModel::visibleRowOf(int entryIndex) const
{
    const auto it = std::lower_bound(m_visible.constBegin(), m_visible.constEnd(), entryIndex);
    if (it == m_visible.constEnd() || *it != entryIndex)
        return -1;
    return int(it - m_visible.constBegin());
}

void PackageModel::emitRowChanged(int entryIndex, const QList<int> &roles)
{
    const int row = visibleRowOf(entryIndex);
    if (row >= 0)
        Q_EMIT dataChanged(index(row, 0), index(row, 0), roles);
}

void PackageModel::upsert(const PackageEntry &entry)
{
    const auto it = entry.id.isEmpty() ? m_byId.constEnd() : m_byId.constFind(entry.id);
    if (it != m_byId.constEnd()) {
        const int i = it.value();
        const bool wasSelected = m_entries.at(i).selected;
        m_entries[i] = entry;
        emitRowChanged(i, {});
        if (wasSelected != entry.selected)
            Q_EMIT selectionChanged();
        return;
    }
    const int i = int(m_entries.size());
    const bool visible = matches(entry);
    if (visible)
        beginInsertRows(QModelIndex(), int(m_visible.size()), int(m_visible.size()));
    m_entries.append(entry);
    if (!entry.id.isEmpty())
        m_byId.insert(entry.id, i);
    if (visible) {
        m_visible.append(i);
        endInsertRows();
    }
    Q_EMIT countChanged();
    if (entry.selected)
        Q_EMIT selectionChanged();
}

void PackageModel::notifyChanged(const QString &packageId)
{
    const auto it = m_byId.constFind(packageId);
    if (it != m_byId.constEnd())
        emitRowChanged(it.value(), {});
}

void PackageModel::notifyAllChanged()
{
    if (!m_visible.isEmpty())
        Q_EMIT dataChanged(index(0, 0), index(int(m_visible.size()) - 1, 0));
    Q_EMIT selectionChanged();
}

void PackageModel::mutateAll(const std::function<void(PackageEntry &)> &edit)
{
    for (PackageEntry &e : m_entries)
        edit(e);
    notifyAllChanged();
}

void PackageModel::setItemProgress(const QString &packageId, const QString &status, int percentage)
{
    PackageEntry *e = find(packageId);
    if (!e) {
        PackageEntry fresh = PackageEntry::fromId(packageId, PackageKit::Transaction::InfoUnknown);
        fresh.status = status;
        fresh.percentage = percentage;
        upsert(fresh);
        return;
    }
    if (e->status == status && e->percentage == percentage)
        return;
    e->status = status;
    e->percentage = percentage;
    emitRowChanged(m_byId.value(packageId), {StatusRole, PercentageRole});
}

void PackageModel::setItemInfo(const QString &packageId, PackageKit::Transaction::Info rawInfo, const QString &summary)
{
    PackageEntry *e = find(packageId);
    if (!e) {
        upsert(PackageEntry::fromId(packageId, rawInfo, summary));
        return;
    }
    e->info = infoBase(rawInfo);
    e->severity = infoSeverity(rawInfo);
    if (!summary.isEmpty())
        e->summary = summary;
    emitRowChanged(m_byId.value(packageId), {InfoRole, InfoTextRole, SummaryRole});
}

void PackageModel::sortBy(const QString &role, bool ascending)
{
    auto key = [&role](const PackageEntry &e) -> QString {
        if (role == QLatin1String("version")) return e.version;
        if (role == QLatin1String("arch")) return e.arch;
        if (role == QLatin1String("repo") || role == QLatin1String("repoText")) return e.repo;
        if (role == QLatin1String("summary")) return e.summary;
        if (role == QLatin1String("info") || role == QLatin1String("infoText")) return infoKey(e.info);
        if (role == QLatin1String("installedVersion")) return e.installedVersion;
        if (role == QLatin1String("group")) return e.group;
        return e.name;
    };
    beginResetModel();
    if (role == QLatin1String("size")) {
        std::stable_sort(m_entries.begin(), m_entries.end(), [ascending](const PackageEntry &a, const PackageEntry &b) {
            return ascending ? a.size < b.size : b.size < a.size;
        });
    } else {
        std::stable_sort(m_entries.begin(), m_entries.end(), [&key, ascending](const PackageEntry &a, const PackageEntry &b) {
            const int c = QString::compare(key(a), key(b), Qt::CaseInsensitive);
            return ascending ? c < 0 : c > 0;
        });
    }
    rebuildVisible();
    endResetModel();
}

int PackageModel::selectedCount() const
{
    return int(std::count_if(m_entries.constBegin(), m_entries.constEnd(),
                             [](const PackageEntry &e) { return e.selected; }));
}

qulonglong PackageModel::selectedSize() const
{
    qulonglong total = 0;
    for (const PackageEntry &e : m_entries) {
        if (e.selected)
            total += e.size;
    }
    return total;
}

QStringList PackageModel::selectedIds() const
{
    QStringList ids;
    for (const PackageEntry &e : m_entries) {
        if (e.selected && !e.id.isEmpty())
            ids << e.id;
    }
    return ids;
}

void PackageModel::setSelected(const QString &packageId, bool selected)
{
    PackageEntry *e = find(packageId);
    if (!e || e->selected == selected)
        return;
    // Held-back updates cannot be installed by selecting them.
    if (selected && e->group == QLatin1String("blocked"))
        return;
    e->selected = selected;
    emitRowChanged(m_byId.value(packageId), {SelectedRole});
    Q_EMIT selectionChanged();
}

void PackageModel::selectAll(bool selected)
{
    bool any = false;
    for (PackageEntry &e : m_entries) {
        const bool want = selected && e.group != QLatin1String("blocked");
        if (e.selected != want) {
            e.selected = want;
            any = true;
        }
    }
    if (!any)
        return;
    if (!m_visible.isEmpty())
        Q_EMIT dataChanged(index(0, 0), index(int(m_visible.size()) - 1, 0), {SelectedRole});
    Q_EMIT selectionChanged();
}

void PackageModel::selectOnlyGroup(const QString &group)
{
    bool any = false;
    for (PackageEntry &e : m_entries) {
        const bool want = e.group == group && group != QLatin1String("blocked");
        if (e.selected != want) {
            e.selected = want;
            any = true;
        }
    }
    if (!any)
        return;
    if (!m_visible.isEmpty())
        Q_EMIT dataChanged(index(0, 0), index(int(m_visible.size()) - 1, 0), {SelectedRole});
    Q_EMIT selectionChanged();
}

QJsonArray PackageModel::toJson() const
{
    QJsonArray array;
    for (const PackageEntry &e : m_entries)
        array.append(e.toJson());
    return array;
}

} // namespace NexusPk
