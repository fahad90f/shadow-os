// SPDX-License-Identifier: GPL-3.0-or-later
#include "applications.h"

#include <KIO/ApplicationLauncherJob>
#include <KSycoca>
#include <KWaylandExtras>
#include <KWindowSystem>
#include <QSettings>

#include <algorithm>

namespace {
QSettings usage()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("nexus"), QStringLiteral("horizon-usage"));
}

// Launcher groups, in display order, and the freedesktop main categories they collect.
const QList<QPair<QString, QStringList>> &groupTable()
{
    static const QList<QPair<QString, QStringList>> table = {
        {QStringLiteral("Security"), {QStringLiteral("Security"), QStringLiteral("X-NEXUS-Security")}},
        {QStringLiteral("Development"), {QStringLiteral("Development"), QStringLiteral("IDE")}},
        {QStringLiteral("Internet"), {QStringLiteral("Network")}},
        {QStringLiteral("Office"), {QStringLiteral("Office")}},
        {QStringLiteral("Graphics"), {QStringLiteral("Graphics")}},
        {QStringLiteral("Multimedia"), {QStringLiteral("AudioVideo"), QStringLiteral("Audio"), QStringLiteral("Video")}},
        {QStringLiteral("Games"), {QStringLiteral("Game")}},
        {QStringLiteral("Science"), {QStringLiteral("Science"), QStringLiteral("Education")}},
        {QStringLiteral("System"), {QStringLiteral("System"), QStringLiteral("Settings")}},
        {QStringLiteral("Utilities"), {QStringLiteral("Utility")}},
    };
    return table;
}
} // namespace

Applications::Applications(QObject *parent)
    : QAbstractListModel(parent)
{
    reload();
    connect(KSycoca::self(), &KSycoca::databaseChanged, this, [this] {
        beginResetModel();
        reload();
        endResetModel();
        Q_EMIT countChanged();
    });
}

QString Applications::groupFor(const KService::Ptr &service)
{
    const QStringList categories = service->categories();
    for (const auto &[group, members] : groupTable()) {
        for (const QString &c : members) {
            if (categories.contains(c))
                return group;
        }
    }
    return QStringLiteral("Other");
}

void Applications::reload()
{
    m_all.clear();
    m_byId.clear();
    m_appIdCache.clear();

    QSettings counts = usage();
    counts.beginGroup(QStringLiteral("Launches"));

    const KService::List services = KService::allServices();
    for (const KService::Ptr &s : services) {
        if (!s->isApplication() || s->noDisplay() || s->exec().isEmpty() || !s->showOnCurrentPlatform())
            continue;
        AppEntry e;
        e.desktopId = s->storageId();
        e.name = s->name();
        e.genericName = s->genericName();
        e.comment = s->comment();
        e.icon = s->icon();
        e.keywords = s->keywords();
        e.group = groupFor(s);
        e.launches = counts.value(e.desktopId, 0).toInt();
        m_all.append(e);
    }
    std::sort(m_all.begin(), m_all.end(), [](const AppEntry &a, const AppEntry &b) {
        return QString::localeAwareCompare(a.name.toLower(), b.name.toLower()) < 0;
    });
    for (int i = 0; i < m_all.size(); ++i)
        m_byId.insert(m_all.at(i).desktopId, i);

    m_groups.clear();
    for (const auto &[group, members] : groupTable()) {
        Q_UNUSED(members)
        if (std::any_of(m_all.cbegin(), m_all.cend(), [&](const AppEntry &e) { return e.group == group; }))
            m_groups << group;
    }
    if (std::any_of(m_all.cbegin(), m_all.cend(), [](const AppEntry &e) { return e.group == QLatin1String("Other"); }))
        m_groups << QStringLiteral("Other");

    refilter();
    Q_EMIT reloaded();
}

void Applications::refilter()
{
    m_visible.clear();
    const QStringList words = m_filter.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);

    // Rank: name starts with query (0) → a word in the name starts with it (1)
    // → name contains it (2) → generic name/keywords/comment contain it (3).
    QList<QPair<int, int>> ranked;
    for (int i = 0; i < m_all.size(); ++i) {
        const AppEntry &e = m_all.at(i);
        if (!m_group.isEmpty() && words.isEmpty() && e.group != m_group)
            continue;
        if (words.isEmpty()) {
            ranked.append({0, i});
            continue;
        }
        const QString name = e.name.toLower();
        const QString extra = (e.genericName + QLatin1Char(' ') + e.keywords.join(QLatin1Char(' ')) + QLatin1Char(' ')
                               + e.comment + QLatin1Char(' ') + e.desktopId).toLower();
        int worst = 0;
        bool all = true;
        for (const QString &w : words) {
            int rank;
            if (name.startsWith(w)) rank = 0;
            else if (name.contains(QLatin1Char(' ') + w)) rank = 1;
            else if (name.contains(w)) rank = 2;
            else if (extra.contains(w)) rank = 3;
            else { all = false; break; }
            worst = qMax(worst, rank);
        }
        if (all)
            ranked.append({worst, i});
    }
    if (!words.isEmpty()) {
        std::stable_sort(ranked.begin(), ranked.end(), [this](const auto &a, const auto &b) {
            if (a.first != b.first) return a.first < b.first;
            return m_all.at(a.second).launches > m_all.at(b.second).launches;
        });
    }
    for (const auto &r : std::as_const(ranked))
        m_visible.append(r.second);
}

int Applications::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_visible.size());
}

QVariant Applications::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};
    const AppEntry &e = m_all.at(m_visible.at(index.row()));
    switch (role) {
    case DesktopIdRole: return e.desktopId;
    case NameRole: case Qt::DisplayRole: return e.name;
    case GenericNameRole: return e.genericName;
    case CommentRole: return e.comment;
    case IconRole: return e.icon;
    case GroupRole: return e.group;
    case LaunchesRole: return e.launches;
    }
    return {};
}

QHash<int, QByteArray> Applications::roleNames() const
{
    return {{DesktopIdRole, "desktopId"}, {NameRole, "name"}, {GenericNameRole, "genericName"},
            {CommentRole, "comment"}, {IconRole, "icon"}, {GroupRole, "group"}, {LaunchesRole, "launches"}};
}

void Applications::setFilter(const QString &filter)
{
    if (filter == m_filter)
        return;
    beginResetModel();
    m_filter = filter;
    refilter();
    endResetModel();
    Q_EMIT filterChanged();
    Q_EMIT countChanged();
}

void Applications::setGroup(const QString &group)
{
    if (group == m_group)
        return;
    beginResetModel();
    m_group = group;
    refilter();
    endResetModel();
    Q_EMIT groupChanged();
    Q_EMIT countChanged();
}

const AppEntry *Applications::entry(const QString &desktopId) const
{
    const auto it = m_byId.constFind(desktopId);
    return it == m_byId.constEnd() ? nullptr : &m_all.at(*it);
}

QVariantMap Applications::get(const QString &desktopId) const
{
    const AppEntry *e = entry(desktopId);
    if (!e)
        return {};
    return {{QStringLiteral("desktopId"), e->desktopId}, {QStringLiteral("name"), e->name},
            {QStringLiteral("icon"), e->icon}, {QStringLiteral("genericName"), e->genericName}};
}

QVariantList Applications::frequent(int limit) const
{
    QList<const AppEntry *> used;
    for (const AppEntry &e : m_all) {
        if (e.launches > 0)
            used.append(&e);
    }
    std::sort(used.begin(), used.end(), [](const AppEntry *a, const AppEntry *b) { return a->launches > b->launches; });
    QVariantList out;
    for (int i = 0; i < used.size() && i < limit; ++i)
        out << get(used.at(i)->desktopId);
    return out;
}

bool Applications::launchAt(int row, QWindow *origin)
{
    if (row < 0 || row >= m_visible.size())
        return false;
    return launch(m_all.at(m_visible.at(row)).desktopId, origin);
}

bool Applications::launch(const QString &desktopId, QWindow *origin)
{
    KService::Ptr service = KService::serviceByStorageId(desktopId);
    if (!service) {
        Q_EMIT launchFailed(desktopId, tr("The application is no longer installed."));
        return false;
    }
    auto *job = new KIO::ApplicationLauncherJob(service);
    // Hand our focus to the application so its window appears in front.
    if (origin && KWindowSystem::isPlatformWayland()) {
        const int serial = KWaylandExtras::lastInputSerial(origin);
        connect(KWaylandExtras::self(), &KWaylandExtras::xdgActivationTokenArrived, job,
                [job, serial](int tokenSerial, const QString &token) {
            if (tokenSerial == serial)
                job->setStartupId(token.toUtf8());
        });
        KWaylandExtras::requestXdgActivationToken(origin, serial, service->desktopEntryName());
    }
    const QString name = service->name();
    connect(job, &KJob::result, this, [this, job, name] {
        if (job->error())
            Q_EMIT launchFailed(name, job->errorString());
    });
    job->start();

    const auto it = m_byId.constFind(desktopId);
    if (it != m_byId.constEnd()) {
        AppEntry &e = m_all[*it];
        ++e.launches;
        QSettings counts = usage();
        counts.setValue(QStringLiteral("Launches/") + desktopId, e.launches);
        const int row = int(m_visible.indexOf(*it));
        if (row >= 0)
            Q_EMIT dataChanged(index(row), index(row), {LaunchesRole});
    }
    return true;
}

QString Applications::desktopIdForAppId(const QString &appId, const QString &resourceName) const
{
    if (appId.isEmpty() && resourceName.isEmpty())
        return {};
    const QString key = appId + QLatin1Char('\n') + resourceName;
    const auto cached = m_appIdCache.constFind(key);
    if (cached != m_appIdCache.constEnd())
        return *cached;

    QString found;
    const QStringList candidates = {appId, resourceName};
    for (const QString &c : candidates) {
        if (c.isEmpty())
            continue;
        // 1. the Wayland app_id is the desktop file name (the freedesktop convention)
        if (m_byId.contains(c + QStringLiteral(".desktop"))) { found = c + QStringLiteral(".desktop"); break; }
        if (KService::Ptr s = KService::serviceByDesktopName(c)) {
            if (m_byId.contains(s->storageId())) { found = s->storageId(); break; }
        }
        // 2. StartupWMClass, desktop entry name or executable name, case-insensitively
        const QString lower = c.toLower();
        const KService::List services = KService::allServices();
        for (const KService::Ptr &s : services) {
            if (!m_byId.contains(s->storageId()))
                continue;
            const QString wmClass = s->property<QString>(QStringLiteral("StartupWMClass")).toLower();
            const QString exec = s->exec().section(QLatin1Char(' '), 0, 0).section(QLatin1Char('/'), -1).toLower();
            if ((!wmClass.isEmpty() && wmClass == lower) || s->desktopEntryName().toLower() == lower || exec == lower) {
                found = s->storageId();
                break;
            }
        }
        if (!found.isEmpty())
            break;
    }
    m_appIdCache.insert(key, found);
    return found;
}
