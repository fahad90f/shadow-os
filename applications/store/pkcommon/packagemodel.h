// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <Transaction>

#include <functional>

namespace NexusPk {

// One package as PackageKit reported it, plus the optional facts the views
// add (installed version of an update, size, selection, live progress).
struct PackageEntry
{
    QString id;                 // PackageKit package ID; empty for rows read from dpkg
    QString name;
    QString version;
    QString arch;
    QString repo;               // origin part of the ID ("debian-trixie-main", "local")
    QString summary;
    PackageKit::Transaction::Info info = PackageKit::Transaction::InfoUnknown;
    // Updates: how urgent PackageKit rates it (InfoUnknown if it does not say).
    PackageKit::Transaction::Info severity = PackageKit::Transaction::InfoUnknown;
    bool installed = false;
    bool automatic = false;     // installed as a dependency

    QString installedVersion;   // updates: the version currently installed
    qulonglong size = 0;        // bytes; 0 = not known
    QString group;              // updates: "security" | "kernel" | "nexus" | "other" | "blocked"
    QStringList tags;           // updates: extra facts shown as badges ("kernel", "nexus")
    bool selected = false;      // updates: chosen for installation
    int percentage = -1;        // live transactions: progress of this item, -1 = not known
    QString status;             // live transactions: what is happening to this item

    // 'rawInfo' is the value as PackageKit-Qt delivers it (state + severity).
    static PackageEntry fromId(const QString &packageId, PackageKit::Transaction::Info rawInfo,
                               const QString &summary = QString());
    // The classification to show for an update: its severity if given, else its state.
    PackageKit::Transaction::Info level() const
    {
        return severity != PackageKit::Transaction::InfoUnknown ? severity : info;
    }
    QJsonObject toJson() const;
};

// List model over PackageEntry rows for NxDataTable / ListView.
//
// Roles (also the property names delegates use): packageId, name, version,
// arch, repo, repoText, summary, info, infoText, installed, automatic,
// installedVersion, size, group, tags, selected, percentage, status.
class PackageModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(qulonglong selectedSize READ selectedSize NOTIFY selectionChanged)

public:
    enum Role {
        PackageIdRole = Qt::UserRole + 1,
        NameRole, VersionRole, ArchRole, RepoRole, RepoTextRole, SummaryRole, InfoRole, InfoTextRole,
        InstalledRole, AutomaticRole, InstalledVersionRole, SizeRole, GroupRole, TagsRole, SelectedRole,
        PercentageRole, StatusRole,
    };

    explicit PackageModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_visible.size()); }
    int totalCount() const { return int(m_entries.size()); }
    QString filter() const { return m_filter; }
    // Case-insensitive substring match on name and summary.
    void setFilter(const QString &filter);

    const QList<PackageEntry> &entries() const { return m_entries; }
    void setEntries(const QList<PackageEntry> &entries);
    void clear();
    // Add a row, or update the row with the same package ID.
    void upsert(const PackageEntry &entry);
    PackageEntry *find(const QString &packageId);

    // Tell the views that rows changed after editing entries through find().
    void notifyChanged(const QString &packageId);
    void notifyAllChanged();
    // Edit every row in place, then notify the views once.
    void mutateAll(const std::function<void(PackageEntry &)> &edit);

    // Live progress of one item of a running transaction.
    void setItemProgress(const QString &packageId, const QString &status, int percentage);
    void setItemInfo(const QString &packageId, PackageKit::Transaction::Info rawInfo, const QString &summary);

    Q_INVOKABLE void sortBy(const QString &role, bool ascending);
    Q_INVOKABLE QVariantMap get(int row) const;

    // ---- selection (update lists) ---------------------------------------
    int selectedCount() const;
    qulonglong selectedSize() const;
    QStringList selectedIds() const;
    Q_INVOKABLE void setSelected(const QString &packageId, bool selected);
    Q_INVOKABLE void selectAll(bool selected);
    // Select exactly the rows of one group ("security"), deselecting the rest.
    Q_INVOKABLE void selectOnlyGroup(const QString &group);

    QJsonArray toJson() const;

Q_SIGNALS:
    void countChanged();
    void filterChanged();
    void selectionChanged();

private:
    void rebuildVisible();
    bool matches(const PackageEntry &entry) const;
    int visibleRowOf(int entryIndex) const;
    void emitRowChanged(int entryIndex, const QList<int> &roles);

    QList<PackageEntry> m_entries;
    QList<int> m_visible;               // indexes into m_entries that pass the filter
    QHash<QString, int> m_byId;         // package ID -> index into m_entries
    QString m_filter;
};

} // namespace NexusPk
