// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>
#include <memory>

namespace AppStream {
class Pool;
}

// One software component as the lists need it. Everything comes from the
// AppStream metadata on this system (DEP-11 catalogue downloaded by APT,
// metainfo files of installed software).
struct AppEntry
{
    QString id;
    QString name;
    QString summary;
    QString packageName;        // first DEB package of the component ("" if the metadata names none)
    QString kind;               // AppStream kind: "desktop-application", "console-application", …
    bool application = false;   // desktop, console or web application
    quint32 categories = 0;     // bit per NEXUS category (index into Catalog::categoryDefinitions())
    QString localIcon;          // icon file on disk (cached or local icon), if any
    QString stockIcon;          // icon-theme name, if any
    QString remoteIcon;         // https URL of the icon on the catalogue's media server, if any
    qint64 releaseTime = 0;     // newest upstream release known to AppStream (seconds since the epoch), 0 = none
    QString releaseVersion;
};

// The software catalogue: AppStream pool, an index of it for fast browsing,
// the NEXUS category mapping, searching and full component details.
//
// Loading and searching run on worker threads; the index is swapped in on
// the GUI thread when a load finishes.
class Catalog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(int componentCount READ componentCount NOTIFY stateChanged)
    Q_PROPERTY(int applicationCount READ applicationCount NOTIFY stateChanged)
    Q_PROPERTY(int uncategorizedCount READ uncategorizedCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY stateChanged)
    Q_PROPERTY(QString appstreamVersion READ appstreamVersion CONSTANT)

public:
    struct CategoryDefinition
    {
        QString key;
        QString title;
        QString icon;
        QStringList appstream;      // freedesktop menu categories that map to it
    };

    explicit Catalog(QObject *parent = nullptr);
    ~Catalog() override;

    // "idle" | "loading" | "ready" (possibly with zero components) | "error"
    QString state() const { return m_state; }
    bool loading() const { return m_state == QLatin1String("loading"); }
    QString error() const { return m_error; }
    int componentCount() const { return m_componentCount; }
    int applicationCount() const { return m_applicationCount; }
    int uncategorizedCount() const { return m_uncategorizedCount; }
    // [ { key, title, icon, count, appstreamCategories } ] in display order
    QVariantList categories() const;
    QString appstreamVersion() const;

    static const QList<CategoryDefinition> &categoryDefinitions();

    // (Re)load the AppStream pool. loaded() fires when done, successfully or not.
    Q_INVOKABLE void load();

    const QList<AppEntry> &entries() const { return m_entries; }
    int indexOfId(const QString &id) const;
    // The component that ships the package (applications preferred), -1 if none.
    int indexOfPackage(const QString &packageName) const;
    QList<int> applicationsInCategory(const QString &key) const;
    QList<int> applications() const;
    // Applications with the newest upstream release dates, newest first.
    QList<int> recentlyReleased(int limit) const;

    // AppStream full-text search (name, summary, keywords, …), best match
    // first. Only applications are returned. Runs on a worker thread.
    void search(const QString &text, QObject *context, std::function<void(const QList<int> &)> done);

    // Everything AppStream knows about one component, ready for the
    // application page and for --dump component; empty if unknown.
    Q_INVOKABLE QVariantMap details(const QString &id) const;
    QJsonObject detailsJson(const QString &id) const;

    // Icon to show for an entry: an image://nxtheme/ URL for a local file or
    // theme icon, the remote URL (to be fetched lazily), or "".
    QString iconSource(const AppEntry &entry, bool *remote = nullptr) const;
    QJsonObject entryJson(int index) const;
    QJsonObject statusJson() const;

Q_SIGNALS:
    void stateChanged();
    void loaded();

private:
    struct Loaded;
    void apply(const Loaded &result);

    QString m_state = QStringLiteral("idle");
    QString m_error;
    std::shared_ptr<AppStream::Pool> m_pool;
    QList<AppEntry> m_entries;
    QHash<QString, int> m_byId;
    QHash<QString, int> m_byPackage;
    QList<int> m_categoryCounts;
    int m_componentCount = 0;
    int m_applicationCount = 0;
    int m_uncategorizedCount = 0;
    quint64 m_loadSerial = 0;
    mutable QHash<QString, bool> m_themeIconKnown;
};

// A list of catalogue entries for the views: a category, search results,
// installed applications, recently released ones.
//
// Roles: appId, name, summary, packageName, icon, iconRemote, kind,
// installed, releaseDate.
class AppListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
    enum Role {
        AppIdRole = Qt::UserRole + 1,
        NameRole, SummaryRole, PackageNameRole, IconRole, IconRemoteRole, KindRole, InstalledRole, ReleaseDateRole,
    };

    AppListModel(Catalog *catalog, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_visible.size()); }
    QString filter() const { return m_filter; }
    // Case-insensitive substring filter on name and summary.
    void setFilter(const QString &filter);

    // Entries by catalogue index, in the order given.
    void setIndexes(const QList<int> &indexes);
    void clear() { setIndexes({}); }
    // Package names installed on this system (for the "installed" role).
    void setInstalledPackages(const QSet<QString> *installed);
    void installedChanged();
    Q_INVOKABLE void sortByName();
    Q_INVOKABLE void sortByRelease();

    QJsonArray toJson() const;

Q_SIGNALS:
    void countChanged();
    void filterChanged();

private:
    void rebuild();

    Catalog *m_catalog;
    const QSet<QString> *m_installed = nullptr;
    QList<int> m_indexes;
    QList<int> m_visible;
    QString m_filter;
};
