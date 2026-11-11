// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalog.h"

#include <packagemodel.h>
#include <pkjob.h>
#include <pkquery.h>
#include <updatechecker.h>

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

// Back-end of SHADOW Store.
//
// Data sources
//   applications     AppStream (Catalog): the distribution's DEP-11 catalogue and installed metainfo
//   packages         PackageKit: Resolve, GetDetails, SearchNames/SearchDetails, GetPackages(installed)
//   changes          PackageKit: InstallPackages / RemovePackages / UpdatePackages (simulated first),
//                    RefreshCache, RepoEnable — all authorised by PackageKit through polkit
//   updates          PackageKit: GetUpdates, GetUpdateDetail (ShadowPk::UpdateChecker)
//   repositories     PackageKit GetRepoList; signature state from /etc/apt and /var/lib/apt/lists
//   installed (fallback when PackageKit is unreachable): dpkg-query, read-only
//
// The store never runs as root, never calls apt or dpkg to change anything,
// and shows nothing that does not come from these sources.
class Store : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *service READ service CONSTANT)
    Q_PROPERTY(QObject *catalog READ catalogObject CONSTANT)
    Q_PROPERTY(QObject *updates READ updatesObject CONSTANT)
    Q_PROPERTY(QObject *job READ job NOTIFY jobChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged)
    Q_PROPERTY(QString jobTarget READ jobTarget NOTIFY jobChanged)
    Q_PROPERTY(QString systemArchitecture READ systemArchitecture CONSTANT)
    Q_PROPERTY(bool updaterAvailable READ updaterAvailable CONSTANT)

    Q_PROPERTY(QString installedState READ installedState NOTIFY installedChanged)
    Q_PROPERTY(QString installedSource READ installedSource NOTIFY installedChanged)
    Q_PROPERTY(QString installedError READ installedError NOTIFY installedChanged)
    Q_PROPERTY(QObject *installedPackages READ installedPackagesObject CONSTANT)
    Q_PROPERTY(QObject *installedApps READ installedAppsObject CONSTANT)
    Q_PROPERTY(int installedPackageCount READ installedPackageCount NOTIFY installedChanged)
    Q_PROPERTY(int installedAppCount READ installedAppCount NOTIFY installedChanged)

    Q_PROPERTY(QObject *categoryApps READ categoryAppsObject CONSTANT)
    Q_PROPERTY(QString currentCategory READ currentCategory NOTIFY currentCategoryChanged)
    Q_PROPERTY(QObject *recentApps READ recentAppsObject CONSTANT)
    Q_PROPERTY(QObject *searchApps READ searchAppsObject CONSTANT)
    Q_PROPERTY(QString appSearchState READ appSearchState NOTIFY searchChanged)
    Q_PROPERTY(QString appSearchText READ appSearchText NOTIFY searchChanged)
    Q_PROPERTY(QObject *searchPackages READ searchPackagesObject CONSTANT)
    Q_PROPERTY(QString packageSearchState READ packageSearchState NOTIFY searchChanged)
    Q_PROPERTY(QString packageSearchText READ packageSearchText NOTIFY searchChanged)
    Q_PROPERTY(QString packageSearchError READ packageSearchError NOTIFY searchChanged)
    Q_PROPERTY(bool packageSearchTruncated READ packageSearchTruncated NOTIFY searchChanged)

    Q_PROPERTY(QVariantList repos READ repos NOTIFY reposChanged)
    Q_PROPERTY(QString reposState READ reposState NOTIFY reposChanged)
    Q_PROPERTY(QString reposError READ reposError NOTIFY reposChanged)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY reposChanged)

public:
    explicit Store(QObject *parent = nullptr);

    QObject *service() const;
    Catalog *catalog() { return &m_catalog; }
    QObject *catalogObject() { return &m_catalog; }
    ShadowPk::UpdateChecker *updateChecker() { return &m_updates; }
    QObject *updatesObject() { return &m_updates; }
    QObject *job() const { return m_job; }
    bool busy() const { return m_job && m_job->active(); }
    // Package name the current job is about ("" for jobs that are not about one package).
    QString jobTarget() const { return m_jobTarget; }
    QString systemArchitecture() const;
    bool updaterAvailable() const { return !m_updaterPath.isEmpty(); }

    // "idle" | "loading" | "ready" | "error"
    QString installedState() const { return m_installedState; }
    // "packagekit" | "dpkg" (read-only fallback while PackageKit is unreachable)
    QString installedSource() const { return m_installedSource; }
    QString installedError() const { return m_installedError; }
    ShadowPk::PackageModel *installedPackages() { return &m_installedPackages; }
    QObject *installedPackagesObject() { return &m_installedPackages; }
    AppListModel *installedApps() { return &m_installedApps; }
    QObject *installedAppsObject() { return &m_installedApps; }
    int installedPackageCount() const { return m_installedPackages.totalCount(); }
    int installedAppCount() const { return m_installedApps.count(); }

    QObject *categoryAppsObject() { return &m_categoryApps; }
    QString currentCategory() const { return m_currentCategory; }
    QObject *recentAppsObject() { return &m_recentApps; }
    AppListModel *searchApps() { return &m_searchApps; }
    QObject *searchAppsObject() { return &m_searchApps; }
    // "idle" | "searching" | "done"
    QString appSearchState() const { return m_appSearchState; }
    QString appSearchText() const { return m_appSearchText; }
    ShadowPk::PackageModel *searchPackages() { return &m_searchPackages; }
    QObject *searchPackagesObject() { return &m_searchPackages; }
    // "idle" | "searching" | "done" | "error" | "unavailable"
    QString packageSearchState() const { return m_packageSearchState; }
    QString packageSearchText() const { return m_packageSearchText; }
    QString packageSearchError() const { return m_packageSearchError; }
    bool packageSearchTruncated() const { return m_packageSearchTruncated; }

    // [ { repoId, description, enabled, category, categoryText, uri, suite, verification,
    //     verificationText, signedBy, origin } ] as PackageKit lists them
    QVariantList repos() const { return m_repos; }
    // "idle" | "loading" | "ready" | "error" | "unavailable"
    QString reposState() const { return m_reposState; }
    QString reposError() const { return m_reposError; }
    // The APT sources as read from /etc/apt (ShadowPk::readAptSources()).
    QVariantList sources() const { return m_sources; }

    Q_INVOKABLE void initialize();
    Q_INVOKABLE void showCategory(const QString &key);
    Q_INVOKABLE void searchApplications(const QString &text);
    Q_INVOKABLE void searchAllPackages(const QString &text, bool includeDescriptions);
    Q_INVOKABLE void refreshInstalled();
    Q_INVOKABLE void refreshRepos();

    // ---- operations (each becomes the current job) --------------------------
    Q_INVOKABLE void installPackage(const QString &packageId, const QString &displayName);
    Q_INVOKABLE void removePackage(const QString &packageId, const QString &displayName);
    // Remove an installed package by name (rows of the Installed view).
    Q_INVOKABLE void removeInstalled(const QString &packageName, const QString &displayName);
    Q_INVOKABLE void updatePackage(const QString &packageId, const QString &displayName);
    Q_INVOKABLE void installUpdates(bool selectedOnly);
    Q_INVOKABLE void refreshMetadata();
    Q_INVOKABLE void setRepoEnabled(const QString &repoId, bool enabled, const QString &description);
    Q_INVOKABLE void dismissJob();

    // ---- helpers for the UI --------------------------------------------------
    // Where a command-line argument or appstream: URL leads:
    //   { view: "app", id } | { view: "package", name } | { view: "search", text } |
    //   { view: "page", key } (key: overview, installed, updates, repos or cat:<category>) | { view: "" }
    Q_INVOKABLE QVariantMap resolveTarget(const QString &argument) const;
    Q_INVOKABLE QString appIdForPackage(const QString &packageName) const;
    Q_INVOKABLE bool isInstalled(const QString &packageName) const;
    // Opens http(s) links in the browser; anything else is refused.
    Q_INVOKABLE bool openUrl(const QString &url) const;
    // Start SHADOW Updater (optionally on one of its pages, e.g. "recovery").
    Q_INVOKABLE bool openUpdater(const QString &page = QString()) const;
    Q_INVOKABLE void copyToClipboard(const QString &text) const;

    QJsonObject statusJson() const;
    QJsonObject reposJson() const;
    QJsonObject installedJson() const;

Q_SIGNALS:
    void jobChanged();
    void installedChanged();
    void currentCategoryChanged();
    void searchChanged();
    void reposChanged();
    // The set of installed packages or their versions may have changed.
    void packagesChanged();
    void jobFinished(bool success, const QString &kind, const QString &title);

private:
    void startJob(ShadowPk::Job *job, const QString &target);
    void onJobFinished(bool success);
    void rebuildInstalledApps();
    void loadInstalledFromDpkg();
    void serviceProbed();

    Catalog m_catalog;
    ShadowPk::UpdateChecker m_updates;
    QPointer<ShadowPk::Job> m_job;
    QString m_jobTarget;
    QString m_updaterPath;
    bool m_initialized = false;

    QString m_installedState = QStringLiteral("idle");
    QString m_installedSource;
    QString m_installedError;
    ShadowPk::PackageModel m_installedPackages;
    QSet<QString> m_installedNames;
    AppListModel m_installedApps;
    quint64 m_installedSerial = 0;

    AppListModel m_categoryApps;
    QString m_currentCategory;
    AppListModel m_recentApps;
    AppListModel m_searchApps;
    QString m_appSearchState = QStringLiteral("idle");
    QString m_appSearchText;
    quint64 m_appSearchSerial = 0;
    ShadowPk::PackageModel m_searchPackages;
    QString m_packageSearchState = QStringLiteral("idle");
    QString m_packageSearchText;
    QString m_packageSearchError;
    bool m_packageSearchTruncated = false;
    quint64 m_packageSearchSerial = 0;
    QPointer<ShadowPk::Query> m_packageSearch;

    QVariantList m_repos;
    QString m_reposState = QStringLiteral("idle");
    QString m_reposError;
    QVariantList m_sources;
    quint64 m_reposSerial = 0;
};
