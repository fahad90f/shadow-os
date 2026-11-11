// SPDX-License-Identifier: GPL-3.0-or-later
#include "store.h"

#include <asyncrun.h>
#include <command.h>
#include <pkservice.h>
#include <pkutil.h>
#include <systemfacts.h>

#include <Daemon>

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>

using NexusPk::Job;
using NexusPk::PackageEntry;
using NexusPk::Query;
using NexusPk::QueryResult;
using NexusPk::Service;
using T = PackageKit::Transaction;

namespace {
// More rows than this are not useful in a result list; the user is told to narrow the search.
constexpr int kMaxSearchRows = 1500;
} // namespace

Store::Store(QObject *parent)
    : QObject(parent), m_installedApps(&m_catalog), m_categoryApps(&m_catalog), m_recentApps(&m_catalog),
      m_searchApps(&m_catalog)
{
    for (AppListModel *model : {&m_installedApps, &m_categoryApps, &m_recentApps, &m_searchApps})
        model->setInstalledPackages(&m_installedNames);

    m_updaterPath = QStandardPaths::findExecutable(QStringLiteral("nexus-updater"));
    if (m_updaterPath.isEmpty()) {
        // Uninstalled build tree: the updater sits next to this binary.
        const QString sibling = QCoreApplication::applicationDirPath() + QStringLiteral("/nexus-updater");
        if (QFileInfo(sibling).isExecutable())
            m_updaterPath = sibling;
    }

    // The list models drop their rows when the catalogue is reloaded (their
    // own connection, made first); fill them again from the new index.
    connect(&m_catalog, &Catalog::loaded, this, [this] {
        m_recentApps.setIndexes(m_catalog.recentlyReleased(8));
        if (!m_currentCategory.isEmpty())
            m_categoryApps.setIndexes(m_catalog.applicationsInCategory(m_currentCategory));
        rebuildInstalledApps();
        if (!m_appSearchText.isEmpty())
            searchApplications(m_appSearchText);
    });

    Service *service = Service::instance();
    connect(service, &Service::probed, this, &Store::serviceProbed);
    connect(service, &Service::updatesChanged, this, [this] {
        // Something else changed packages (apt in a terminal, the Updater).
        if (busy())
            return;
        if (!m_updates.busy())
            m_updates.check(false);
        refreshInstalled();
        Q_EMIT packagesChanged();
    });
    connect(service, &Service::repoListChanged, this, [this] {
        if (!busy())
            refreshRepos();
    });
}

QObject *Store::service() const
{
    return Service::instance();
}

QString Store::systemArchitecture() const
{
    return NexusPk::systemArchitecture();
}

void Store::initialize()
{
    if (m_initialized)
        return;
    m_initialized = true;
    m_catalog.load();
    Service::instance()->probe();
}

void Store::serviceProbed()
{
    // PackageKit works through its queue one transaction at a time. Let the
    // page the user is looking at ask first (its handler of this signal runs
    // right after this one), then the quick lists, and the long list of all
    // installed packages last.
    m_installedState = QStringLiteral("loading");
    Q_EMIT installedChanged();
    QTimer::singleShot(0, this, [this] {
        if (!m_updates.busy())
            m_updates.check(false);
        refreshRepos();
    });
    QTimer::singleShot(Service::instance()->available() ? 1200 : 0, this, [this] { refreshInstalled(); });
}

// ---------------------------------------------------------------- installed

void Store::refreshInstalled()
{
    const quint64 serial = ++m_installedSerial;
    m_installedState = QStringLiteral("loading");
    Q_EMIT installedChanged();

    if (!Service::instance()->available()) {
        m_installedError.clear();
        loadInstalledFromDpkg();
        return;
    }
    Query::run(this, PackageKit::Daemon::getPackages(T::FilterInstalled), [this, serial](const QueryResult &result) {
        if (serial != m_installedSerial)
            return;
        if (!result.ok) {
            // Keep the reason and show what dpkg itself reports instead.
            m_installedError = result.errorMessage();
            loadInstalledFromDpkg();
            return;
        }
        QList<PackageEntry> entries = result.packages;
        std::sort(entries.begin(), entries.end(), [](const PackageEntry &a, const PackageEntry &b) {
            return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
        });
        m_installedNames.clear();
        for (const PackageEntry &e : std::as_const(entries))
            m_installedNames.insert(e.name);
        m_installedPackages.setEntries(entries);
        m_installedSource = QStringLiteral("packagekit");
        m_installedError.clear();
        m_installedState = QStringLiteral("ready");
        rebuildInstalledApps();
        Q_EMIT installedChanged();
    });
}

void Store::loadInstalledFromDpkg()
{
    // Read-only fallback: the dpkg database lists what is installed even when
    // PackageKit cannot be asked. Nothing can be removed in this mode.
    const quint64 serial = m_installedSerial;
    NexusPk::runAsync(this, [] {
        return Nexus::Command::runSync(
            QStringLiteral("dpkg-query"),
            {QStringLiteral("-W"),
             QStringLiteral("-f=${db:Status-Abbrev}\\t${Package}\\t${Version}\\t${Architecture}\\t${binary:Summary}\\n")},
            60000);
    }, [this, serial](const Nexus::CommandResult &r) {
        if (serial != m_installedSerial)
            return;
        QList<PackageEntry> entries;
        m_installedNames.clear();
        if (r.ok()) {
            const QStringList lines = r.outText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                const QStringList f = line.split(QLatin1Char('\t'));
                if (f.size() < 4 || !f.at(0).startsWith(QLatin1String("ii")))
                    continue;
                PackageEntry e;
                e.name = f.at(1);
                e.version = f.at(2);
                e.arch = f.at(3);
                e.summary = f.value(4);
                e.info = T::InfoInstalled;
                e.installed = true;
                entries.append(e);
                m_installedNames.insert(e.name);
            }
            m_installedSource = QStringLiteral("dpkg");
            m_installedState = QStringLiteral("ready");
        } else {
            m_installedSource.clear();
            m_installedState = QStringLiteral("error");
            const QString reason = r.exitCode == -1 ? tr("dpkg-query could not be started")
                                                    : tr("dpkg-query failed: %1").arg(r.errText().trimmed());
            m_installedError = m_installedError.isEmpty() ? reason : m_installedError + QLatin1Char('\n') + reason;
        }
        m_installedPackages.setEntries(entries);
        rebuildInstalledApps();
        Q_EMIT installedChanged();
    });
}

void Store::rebuildInstalledApps()
{
    QList<int> indexes;
    const QList<AppEntry> &entries = m_catalog.entries();
    for (int i = 0; i < entries.size(); ++i) {
        const AppEntry &e = entries.at(i);
        if (e.application && !e.packageName.isEmpty() && m_installedNames.contains(e.packageName))
            indexes << i;
    }
    std::sort(indexes.begin(), indexes.end(), [&entries](int a, int b) {
        return QString::localeAwareCompare(entries.at(a).name, entries.at(b).name) < 0;
    });
    m_installedApps.setIndexes(indexes);
    for (AppListModel *model : {&m_categoryApps, &m_recentApps, &m_searchApps})
        model->installedChanged();
    Q_EMIT installedChanged();
}

bool Store::isInstalled(const QString &packageName) const
{
    return m_installedNames.contains(packageName);
}

// ---------------------------------------------------------------- browsing

void Store::showCategory(const QString &key)
{
    m_currentCategory = key;
    m_categoryApps.setFilter(QString());
    m_categoryApps.setIndexes(m_catalog.applicationsInCategory(key));
    Q_EMIT currentCategoryChanged();
}

void Store::searchApplications(const QString &text)
{
    const quint64 serial = ++m_appSearchSerial;
    m_appSearchText = text.trimmed();
    if (m_appSearchText.isEmpty()) {
        m_searchApps.clear();
        m_appSearchState = QStringLiteral("idle");
        Q_EMIT searchChanged();
        return;
    }
    m_appSearchState = QStringLiteral("searching");
    Q_EMIT searchChanged();
    if (m_catalog.loading())
        return;     // runs again when the catalogue is there
    m_catalog.search(m_appSearchText, this, [this, serial](const QList<int> &found) {
        if (serial != m_appSearchSerial)
            return;
        // AppStream ranks by how well name, summary and keywords match. Keep
        // that order, but put applications whose *name* matches what was typed
        // in front — that is what people look for first.
        QList<int> indexes = found;
        const QString needle = m_appSearchText.toLower();
        const QStringList words = needle.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QList<AppEntry> &entries = m_catalog.entries();
        auto rank = [&](int i) {
            const QString name = entries.at(i).name.toLower();
            if (name == needle) return 0;
            if (name.startsWith(needle)) return 1;
            if (name.contains(needle)) return 2;
            if (std::all_of(words.cbegin(), words.cend(), [&name](const QString &w) { return name.contains(w); })) return 3;
            return 4;
        };
        std::stable_sort(indexes.begin(), indexes.end(), [&rank](int a, int b) { return rank(a) < rank(b); });
        m_searchApps.setIndexes(indexes);
        m_appSearchState = QStringLiteral("done");
        Q_EMIT searchChanged();
    });
}

void Store::searchAllPackages(const QString &text, bool includeDescriptions)
{
    const quint64 serial = ++m_packageSearchSerial;
    if (m_packageSearch)
        m_packageSearch->cancel();
    m_packageSearchText = text.trimmed();
    m_packageSearchError.clear();
    m_packageSearchTruncated = false;
    m_searchPackages.clear();

    if (m_packageSearchText.size() < 2) {
        m_packageSearchState = QStringLiteral("idle");
        Q_EMIT searchChanged();
        return;
    }
    // PackageKit rejects these characters in search terms; say so up front.
    static const QRegularExpression forbidden(QStringLiteral("[*?$`'\"^\\[\\]{}\\\\<>|]"));
    const QRegularExpressionMatch bad = forbidden.match(m_packageSearchText);
    if (bad.hasMatch()) {
        m_packageSearchState = QStringLiteral("error");
        m_packageSearchError = tr("PackageKit does not accept the character “%1” in a search.").arg(bad.captured(0));
        Q_EMIT searchChanged();
        return;
    }
    if (!Service::instance()->available()) {
        m_packageSearchState = QStringLiteral("unavailable");
        m_packageSearchError = Service::instance()->error();
        Q_EMIT searchChanged();
        return;
    }

    m_packageSearchState = QStringLiteral("searching");
    Q_EMIT searchChanged();
    PackageKit::Transaction *t = includeDescriptions
        ? PackageKit::Daemon::searchDetails(m_packageSearchText, T::FilterNone)
        : PackageKit::Daemon::searchNames(m_packageSearchText, T::FilterNone);
    const QString needle = m_packageSearchText.toLower();
    m_packageSearch = Query::run(this, t, [this, serial, needle](const QueryResult &result) {
        if (serial != m_packageSearchSerial)
            return;
        if (!result.ok) {
            m_packageSearchState = QStringLiteral("error");
            m_packageSearchError = result.errorMessage();
            Q_EMIT searchChanged();
            return;
        }
        // One row per package: the version on offer, flagged if installed.
        QHash<QString, int> rowOf;
        QList<PackageEntry> rows;
        for (const PackageEntry &e : result.packages) {
            const QString key = e.name + QLatin1Char(':') + e.arch;
            const auto it = rowOf.constFind(key);
            if (it == rowOf.constEnd()) {
                rowOf.insert(key, int(rows.size()));
                PackageEntry row = e;
                if (e.installed)
                    row.installedVersion = e.version;
                rows.append(row);
                continue;
            }
            PackageEntry &row = rows[it.value()];
            if (e.installed) {
                row.installed = true;
                row.automatic = e.automatic;
                row.installedVersion = e.version;
            } else {
                // An available version next to the installed one: show it as the version on offer.
                const QString installedVersion = row.installedVersion;
                const bool wasInstalled = row.installed;
                const bool automatic = row.automatic;
                row = e;
                row.installed = wasInstalled;
                row.automatic = automatic;
                row.installedVersion = installedVersion;
            }
        }
        auto rank = [&needle](const PackageEntry &e) {
            const QString name = e.name.toLower();
            return name == needle ? 0 : name.startsWith(needle) ? 1 : name.contains(needle) ? 2 : 3;
        };
        std::stable_sort(rows.begin(), rows.end(), [&rank](const PackageEntry &a, const PackageEntry &b) {
            const int ra = rank(a), rb = rank(b);
            if (ra != rb)
                return ra < rb;
            return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
        });
        if (rows.size() > kMaxSearchRows) {
            rows.resize(kMaxSearchRows);
            m_packageSearchTruncated = true;
        }
        m_searchPackages.setEntries(rows);
        m_packageSearchState = QStringLiteral("done");
        Q_EMIT searchChanged();
    });
}

// ---------------------------------------------------------------- repositories

void Store::refreshRepos()
{
    const quint64 serial = ++m_reposSerial;
    m_reposState = QStringLiteral("loading");
    Q_EMIT reposChanged();

    // The sources files and downloaded indexes say which repositories are
    // signed — with or without PackageKit.
    NexusPk::runAsync(this, [] { return NexusPk::readAptSources(); }, [this, serial](const QJsonObject &apt) {
        if (serial != m_reposSerial)
            return;
        m_sources = apt.value(QStringLiteral("sources")).toArray().toVariantList();

        if (!Service::instance()->available()) {
            m_repos.clear();
            m_reposState = QStringLiteral("unavailable");
            m_reposError = Service::instance()->error();
            Q_EMIT reposChanged();
            return;
        }
        Query::run(this, PackageKit::Daemon::getRepoList(T::FilterNone), [this, serial](const QueryResult &result) {
            if (serial != m_reposSerial)
                return;
            if (!result.ok) {
                m_repos.clear();
                m_reposState = QStringLiteral("error");
                m_reposError = result.errorMessage();
                Q_EMIT reposChanged();
                return;
            }
            QVariantList repos;
            for (const NexusPk::RepoEntry &r : result.repos) {
                QVariantMap repo{
                    {QStringLiteral("repoId"), r.id},
                    {QStringLiteral("description"), r.description},
                    {QStringLiteral("enabled"), r.enabled},
                };
                // Match the APT source this entry stands for:
                //   <sources file>:<type>:<uri>/:<suite>:<components>
                QVariantMap source;
                for (const QVariant &v : std::as_const(m_sources)) {
                    const QVariantMap s = v.toMap();
                    QString uri = s.value(QStringLiteral("uri")).toString();
                    if (!uri.endsWith(QLatin1Char('/')))
                        uri += QLatin1Char('/');
                    const QString needle = QLatin1Char(':') + uri + QLatin1Char(':')
                                           + s.value(QStringLiteral("suite")).toString() + QLatin1Char(':');
                    if (r.id.startsWith(s.value(QStringLiteral("file")).toString() + QLatin1Char(':')) && r.id.contains(needle)) {
                        source = s;
                        break;
                    }
                }
                const QString category = source.isEmpty() ? NexusPk::repoCategory(r.id)
                                                          : source.value(QStringLiteral("category")).toString();
                repo.insert(QStringLiteral("category"), category);
                repo.insert(QStringLiteral("categoryText"), NexusPk::repoCategoryText(category));
                repo.insert(QStringLiteral("matched"), !source.isEmpty());
                for (const char *key : {"uri", "suite", "file", "verification", "verificationText", "signedBy", "origin", "components"})
                    repo.insert(QLatin1String(key), source.value(QLatin1String(key)));
                if (source.isEmpty()) {
                    repo.insert(QStringLiteral("verification"), QStringLiteral("unknown"));
                    repo.insert(QStringLiteral("verificationText"),
                                tr("The signature state of this entry could not be read from the APT sources files"));
                }
                repos << repo;
            }
            m_repos = repos;
            m_reposError.clear();
            m_reposState = QStringLiteral("ready");
            Q_EMIT reposChanged();
        });
    });
}

// ---------------------------------------------------------------- operations

void Store::startJob(Job *job, const QString &target)
{
    if (busy()) {
        job->deleteLater();
        return;
    }
    if (m_job)
        m_job->deleteLater();
    m_job = job;
    m_jobTarget = target;
    connect(job, &Job::phaseChanged, this, &Store::jobChanged);
    connect(job, &Job::finished, this, &Store::onJobFinished);
    Q_EMIT jobChanged();
    // A changelog download still in flight would keep the transaction waiting.
    m_updates.cancelDetails();
    job->start();
}

void Store::onJobFinished(bool success)
{
    Job *job = m_job;
    if (!job)
        return;
    const QString kind = job->kind();
    if (kind == QLatin1String("refresh")) {
        // New lists: new catalogue, possibly new updates.
        if (success)
            m_catalog.load();
        refreshRepos();
    } else if (kind == QLatin1String("repository")) {
        refreshRepos();
    }
    if (kind != QLatin1String("repository")) {
        refreshInstalled();
        if (!m_updates.busy())
            m_updates.check(false);
    }
    Q_EMIT jobChanged();
    Q_EMIT packagesChanged();
    Q_EMIT jobFinished(success, kind, job->title());
}

void Store::dismissJob()
{
    if (!m_job || m_job->active())
        return;
    m_job->deleteLater();
    m_job.clear();
    m_jobTarget.clear();
    Q_EMIT jobChanged();
}

void Store::installPackage(const QString &packageId, const QString &displayName)
{
    if (packageId.isEmpty())
        return;
    startJob(NexusPk::Jobs::install({packageId}, tr("Installing %1").arg(displayName), this),
             NexusPk::PackageId::parse(packageId).name);
}

void Store::removePackage(const QString &packageId, const QString &displayName)
{
    if (packageId.isEmpty())
        return;
    startJob(NexusPk::Jobs::remove({packageId}, tr("Removing %1").arg(displayName), this),
             NexusPk::PackageId::parse(packageId).name);
}

void Store::removeInstalled(const QString &packageName, const QString &displayName)
{
    // The package ID of the installed version, as PackageKit listed it.
    for (const PackageEntry &e : m_installedPackages.entries()) {
        if (e.name == packageName && !e.id.isEmpty()) {
            removePackage(e.id, displayName.isEmpty() ? packageName : displayName);
            return;
        }
    }
}

void Store::updatePackage(const QString &packageId, const QString &displayName)
{
    if (packageId.isEmpty())
        return;
    startJob(NexusPk::Jobs::update({packageId}, tr("Updating %1").arg(displayName), this),
             NexusPk::PackageId::parse(packageId).name);
}

void Store::installUpdates(bool selectedOnly)
{
    const QStringList ids = selectedOnly ? m_updates.model()->selectedIds() : m_updates.installableIds();
    if (ids.isEmpty())
        return;
    startJob(NexusPk::Jobs::update(ids, ids.size() == 1 ? tr("Installing 1 update") : tr("Installing %1 updates").arg(ids.size()), this),
             QString());
}

void Store::refreshMetadata()
{
    startJob(NexusPk::Jobs::refresh(true, this), QString());
}

void Store::setRepoEnabled(const QString &repoId, bool enabled, const QString &description)
{
    if (repoId.isEmpty())
        return;
    startJob(NexusPk::Jobs::repoEnable(repoId, enabled,
                                       enabled ? tr("Enabling repository “%1”").arg(description)
                                               : tr("Disabling repository “%1”").arg(description), this),
             QString());
}

// ---------------------------------------------------------------- helpers

QVariantMap Store::resolveTarget(const QString &argument) const
{
    QString arg = argument.trimmed();
    if (arg.isEmpty())
        return {{QStringLiteral("view"), QString()}};
    if (arg.startsWith(QLatin1String("search:")))
        return {{QStringLiteral("view"), QStringLiteral("search")}, {QStringLiteral("text"), arg.mid(7)}};

    if (arg.startsWith(QLatin1String("page:"))) {
        // One of the store's own views (nexus-store --page updates).
        QString key = arg.mid(5).trimmed().toLower();
        if (key == QLatin1String("repositories"))
            key = QStringLiteral("repos");
        static const QStringList views = {QStringLiteral("overview"), QStringLiteral("installed"),
                                          QStringLiteral("updates"), QStringLiteral("repos")};
        if (views.contains(key))
            return {{QStringLiteral("view"), QStringLiteral("page")}, {QStringLiteral("key"), key}};
        const QVariantList categories = m_catalog.categories();
        for (const QVariant &c : categories) {
            if (c.toMap().value(QStringLiteral("key")).toString() == key)
                return {{QStringLiteral("view"), QStringLiteral("page")}, {QStringLiteral("key"), QStringLiteral("cat:") + key}};
        }
        return {{QStringLiteral("view"), QString()}};
    }

    bool fromUrl = false;
    if (arg.startsWith(QLatin1String("appstream:"), Qt::CaseInsensitive)) {
        // appstream://org.example.App  or  appstream:org.example.App
        fromUrl = true;
        QString id = arg.mid(10);
        while (id.startsWith(QLatin1Char('/')))
            id.remove(0, 1);
        id = id.section(QLatin1Char('?'), 0, 0).section(QLatin1Char('#'), 0, 0);
        id = QUrl::fromPercentEncoding(id.toUtf8());
        while (id.endsWith(QLatin1Char('/')))
            id.chop(1);
        arg = id;
        if (arg.isEmpty())
            return {{QStringLiteral("view"), QString()}};
    }

    int index = m_catalog.indexOfId(arg);
    if (index < 0)
        index = m_catalog.indexOfId(arg + QStringLiteral(".desktop"));
    if (index < 0 && arg.endsWith(QLatin1String(".desktop")))
        index = m_catalog.indexOfId(arg.chopped(8));
    if (index >= 0)
        return {{QStringLiteral("view"), QStringLiteral("app")}, {QStringLiteral("id"), m_catalog.entries().at(index).id}};

    if (!fromUrl) {
        index = m_catalog.indexOfPackage(arg);
        if (index >= 0)
            return {{QStringLiteral("view"), QStringLiteral("app")}, {QStringLiteral("id"), m_catalog.entries().at(index).id}};
        static const QRegularExpression packageName(QStringLiteral("^[a-z0-9][a-z0-9+.-]+$"));
        if (packageName.match(arg).hasMatch())
            return {{QStringLiteral("view"), QStringLiteral("package")}, {QStringLiteral("name"), arg}};
    }
    return {{QStringLiteral("view"), QStringLiteral("search")}, {QStringLiteral("text"), arg}};
}

QString Store::appIdForPackage(const QString &packageName) const
{
    const int index = m_catalog.indexOfPackage(packageName);
    return index >= 0 ? m_catalog.entries().at(index).id : QString();
}

bool Store::openUrl(const QString &url) const
{
    const QUrl u(url);
    if (!u.isValid() || (u.scheme() != QLatin1String("https") && u.scheme() != QLatin1String("http")) || u.host().isEmpty())
        return false;
    return QDesktopServices::openUrl(u);
}

bool Store::openUpdater(const QString &page) const
{
    // Only the updater's own page names are passed on.
    static const QStringList pages = {QStringLiteral("updates"), QStringLiteral("history"), QStringLiteral("log"),
                                      QStringLiteral("recovery"), QStringLiteral("sources"), QStringLiteral("settings")};
    const QStringList arguments = pages.contains(page) ? QStringList{page} : QStringList();
    return !m_updaterPath.isEmpty() && QProcess::startDetached(m_updaterPath, arguments);
}

void Store::copyToClipboard(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}

// ---------------------------------------------------------------- diagnostics

QJsonObject Store::statusJson() const
{
    return QJsonObject{
        {QStringLiteral("packagekit"), Service::instance()->toJson()},
        {QStringLiteral("appstream"), m_catalog.statusJson()},
        {QStringLiteral("architecture"), QJsonObject{
            {QStringLiteral("system"), NexusPk::systemArchitecture()},
            {QStringLiteral("foreign"), QJsonArray::fromStringList(NexusPk::foreignArchitectures())},
        }},
        {QStringLiteral("updater"), QJsonObject{
            {QStringLiteral("available"), updaterAvailable()},
            {QStringLiteral("path"), m_updaterPath},
        }},
    };
}

QJsonObject Store::reposJson() const
{
    QJsonObject pk{
        {QStringLiteral("reachable"), Service::instance()->available()},
        {QStringLiteral("state"), m_reposState},
        {QStringLiteral("repositories"), QJsonArray::fromVariantList(m_repos)},
    };
    if (!m_reposError.isEmpty())
        pk.insert(QStringLiteral("error"), m_reposError);
    return QJsonObject{
        {QStringLiteral("packagekit"), pk},
        {QStringLiteral("aptSources"), QJsonArray::fromVariantList(m_sources)},
    };
}

QJsonObject Store::installedJson() const
{
    QJsonObject o{
        {QStringLiteral("state"), m_installedState},
        {QStringLiteral("source"), m_installedSource},
        {QStringLiteral("packageCount"), installedPackageCount()},
        {QStringLiteral("applicationCount"), installedAppCount()},
        {QStringLiteral("applications"), m_installedApps.toJson()},
        {QStringLiteral("packages"), m_installedPackages.toJson()},
    };
    if (!m_installedError.isEmpty())
        o.insert(QStringLiteral("error"), m_installedError);
    return o;
}
