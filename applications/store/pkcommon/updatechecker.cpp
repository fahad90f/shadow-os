// SPDX-License-Identifier: GPL-3.0-or-later
#include "updatechecker.h"

#include "pkservice.h"
#include "pkutil.h"

#include <Daemon>

#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonArray>
#include <QTimer>

#include <algorithm>
#include <limits>

namespace NexusPk {

using T = PackageKit::Transaction;

namespace {
// PackageKit refuses very long ID lists in one call (MaximumItemsToResolve).
constexpr int kChunk = 500;

int groupOrder(const QString &group)
{
    if (group == QLatin1String("security")) return 0;
    if (group == QLatin1String("kernel")) return 1;
    if (group == QLatin1String("nexus")) return 2;
    if (group == QLatin1String("other")) return 3;
    return 4;
}
} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
{
}

bool UpdateChecker::isKernelPackage(const QString &name)
{
    static const QStringList prefixes = {
        QStringLiteral("linux-image"), QStringLiteral("linux-headers"), QStringLiteral("linux-modules"),
        QStringLiteral("linux-kbuild"),
    };
    return std::any_of(prefixes.cbegin(), prefixes.cend(), [&name](const QString &p) { return name.startsWith(p); });
}

bool UpdateChecker::isNexusPackage(const QString &name)
{
    return name.startsWith(QLatin1String("nexus-")) || name.startsWith(QLatin1String("libnexus"));
}

QString UpdateChecker::groupFor(const PackageEntry &entry)
{
    if (entry.info == T::InfoBlocked)
        return QStringLiteral("blocked");
    if (entry.level() == T::InfoSecurity)
        return QStringLiteral("security");
    if (isKernelPackage(entry.name))
        return QStringLiteral("kernel");
    if (isNexusPackage(entry.name))
        return QStringLiteral("nexus");
    return QStringLiteral("other");
}

void UpdateChecker::setState(const QString &state)
{
    if (state == m_state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void UpdateChecker::trackProgress(PackageKit::Transaction *transaction)
{
    m_transaction = transaction;
    m_percentage = -1;
    m_statusText.clear();
    Q_EMIT progressChanged();
    if (!transaction)
        return;
    auto update = [this] {
        PackageKit::Transaction *t = m_transaction;
        if (!t)
            return;
        m_statusText = NexusPk::statusText(t->status());
        const uint p = t->percentage();
        m_percentage = p <= 100 ? int(p) : -1;
        Q_EMIT progressChanged();
    };
    connect(transaction, &T::statusChanged, this, update);
    connect(transaction, &T::percentageChanged, this, update);
}

void UpdateChecker::cancel()
{
    if (m_query)
        m_query->cancel();
}

void UpdateChecker::check(bool refresh)
{
    if (busy())
        return;
    const quint64 serial = ++m_serial;
    m_error.clear();
    m_refreshError.clear();
    m_refreshed = false;

    Service *service = Service::instance();
    if (!service->available()) {
        // Nothing to ask: report the exact reason, keep whatever was listed out of sight.
        m_model.clear();
        recount();
        m_error = service->error();
        setState(QStringLiteral("loading"));
        QTimer::singleShot(0, this, [this, serial] {
            if (serial != m_serial)
                return;
            setState(QStringLiteral("error"));
            Q_EMIT checked();
        });
        return;
    }

    if (!refresh) {
        startGetUpdates();
        return;
    }

    setState(QStringLiteral("refreshing"));
    // A check the user asked for forces the refresh; the headless periodic
    // check lets the back-end skip sources it refreshed recently.
    PackageKit::Transaction *t = PackageKit::Daemon::refreshCache(service->interactive());
    trackProgress(t);
    m_query = Query::run(this, t, [this, serial](const QueryResult &result) {
        if (serial != m_serial)
            return;
        m_refreshed = result.ok;
        if (!result.ok)
            m_refreshError = result.errorMessage();
        if (result.exit == T::ExitCancelled || result.exit == T::ExitCancelledPriority) {
            m_error = result.errorMessage();
            setState(QStringLiteral("error"));
            Q_EMIT checked();
            return;
        }
        // Even if refreshing failed, show what the lists on disk say — with
        // the refresh error displayed next to it.
        startGetUpdates();
    });
}

void UpdateChecker::startGetUpdates()
{
    const quint64 serial = m_serial;
    setState(QStringLiteral("loading"));
    Q_EMIT stateChanged();
    PackageKit::Transaction *t = PackageKit::Daemon::getUpdates(T::FilterNone);
    trackProgress(t);
    m_query = Query::run(this, t, [this, serial](const QueryResult &result) {
        if (serial != m_serial)
            return;
        gotUpdates(result);
    });
}

void UpdateChecker::gotUpdates(const QueryResult &result)
{
    if (!result.ok) {
        m_model.clear();
        recount();
        m_error = result.errorMessage();
        setState(QStringLiteral("error"));
        Q_EMIT checked();
        return;
    }

    QList<PackageEntry> entries = result.packages;
    for (PackageEntry &e : entries) {
        e.group = groupFor(e);
        if (isKernelPackage(e.name))
            e.tags << QStringLiteral("kernel");
        if (isNexusPackage(e.name))
            e.tags << QStringLiteral("nexus");
        e.selected = e.group != QLatin1String("blocked");
        e.installed = false;
    }
    std::stable_sort(entries.begin(), entries.end(), [](const PackageEntry &a, const PackageEntry &b) {
        const int ga = groupOrder(a.group), gb = groupOrder(b.group);
        if (ga != gb)
            return ga < gb;
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    m_model.setEntries(entries);
    m_details.clear();
    recount();

    m_pendingNames.clear();
    m_pendingIds.clear();
    for (const PackageEntry &e : std::as_const(entries)) {
        m_pendingNames << e.name;
        m_pendingIds << e.id;
    }
    m_pendingNames.removeDuplicates();
    m_sizesKnown = false;
    if (entries.isEmpty()) {
        m_sizesKnown = true;
        finishCheck();
        return;
    }
    resolveInstalled(0);
}

void UpdateChecker::resolveInstalled(int offset)
{
    if (offset >= m_pendingNames.size()) {
        fetchSizes(0);
        return;
    }
    const quint64 serial = m_serial;
    const QStringList chunk = m_pendingNames.mid(offset, kChunk);
    PackageKit::Transaction *t = PackageKit::Daemon::resolve(chunk, T::FilterInstalled);
    trackProgress(t);
    m_query = Query::run(this, t, [this, serial, offset](const QueryResult &result) {
        if (serial != m_serial)
            return;
        if (result.ok) {
            // name[:arch] -> installed version
            QHash<QString, QString> installed;
            for (const PackageEntry &p : result.packages) {
                installed.insert(p.name + QLatin1Char(':') + p.arch, p.version);
                if (!installed.contains(p.name))
                    installed.insert(p.name, p.version);
            }
            m_model.mutateAll([&installed](PackageEntry &e) {
                QString version = installed.value(e.name + QLatin1Char(':') + e.arch);
                if (version.isEmpty())
                    version = installed.value(e.name);
                if (!version.isEmpty())
                    e.installedVersion = version;
            });
        }
        // A failure here only leaves "installed version" unknown for these rows.
        resolveInstalled(offset + kChunk);
    });
}

void UpdateChecker::fetchSizes(int offset)
{
    if (offset >= m_pendingIds.size()) {
        m_sizesKnown = true;
        recount();
        finishCheck();
        return;
    }
    const quint64 serial = m_serial;
    const QStringList chunk = m_pendingIds.mid(offset, kChunk);
    PackageKit::Transaction *t = PackageKit::Daemon::getDetails(chunk);
    trackProgress(t);
    m_query = Query::run(this, t, [this, serial, offset](const QueryResult &result) {
        if (serial != m_serial)
            return;
        if (!result.ok) {
            // Sizes stay unknown; the list itself is still valid.
            m_sizesKnown = false;
            recount();
            finishCheck();
            return;
        }
        QHash<QString, qulonglong> sizes;
        for (const PackageDetails &d : result.details)
            sizes.insert(d.packageId, d.bytesToDownload());
        m_model.mutateAll([&sizes](PackageEntry &e) {
            const auto it = sizes.constFind(e.id);
            if (it != sizes.constEnd())
                e.size = it.value();
        });
        fetchSizes(offset + kChunk);
    });
}

void UpdateChecker::recount()
{
    m_counts.clear();
    m_totalSize = 0;
    for (const PackageEntry &e : m_model.entries()) {
        m_counts[e.group] += 1;
        if (e.group != QLatin1String("blocked")) {
            m_counts[QStringLiteral("total")] += 1;
            m_totalSize += e.size;
        }
    }
    Q_EMIT resultChanged();
}

void UpdateChecker::finishCheck()
{
    m_transaction.clear();
    m_statusText.clear();
    m_percentage = -1;
    m_lastChecked = QDateTime::currentDateTime();
    Q_EMIT progressChanged();
    setState(QStringLiteral("ready"));
    Q_EMIT stateChanged();
    queryLastRefresh();
    Q_EMIT checked();
}

void UpdateChecker::queryLastRefresh()
{
    if (!Service::instance()->available())
        return;
    auto *watcher = new QDBusPendingCallWatcher(PackageKit::Daemon::getTimeSinceAction(T::RoleRefreshCache), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<uint> reply = *w;
        if (reply.isError())
            return;
        const uint seconds = reply.value();
        // PackageKit answers UINT_MAX when the action never ran.
        m_lastRefreshed = seconds == std::numeric_limits<uint>::max()
            ? QDateTime() : QDateTime::currentDateTime().addSecs(-static_cast<qint64>(seconds));
        Q_EMIT stateChanged();
    });
}

void UpdateChecker::loadDetail(const QString &packageId)
{
    if (packageId.isEmpty())
        return;
    const QString state = m_details.value(packageId).value(QStringLiteral("state")).toString();
    if (state == QLatin1String("loading") || state == QLatin1String("ready"))
        return;
    if (!Service::instance()->available()) {
        m_details.insert(packageId, QVariantMap{{QStringLiteral("state"), QStringLiteral("error")},
                                                {QStringLiteral("error"), Service::instance()->error()}});
        Q_EMIT detailChanged(packageId);
        return;
    }
    m_details.insert(packageId, QVariantMap{{QStringLiteral("state"), QStringLiteral("loading")}});
    Q_EMIT detailChanged(packageId);
    m_detailQueries[packageId] = Query::run(this, PackageKit::Daemon::getUpdateDetail(packageId), [this, packageId](const QueryResult &result) {
        m_detailQueries.remove(packageId);
        QVariantMap map;
        const bool cancelled = result.exit == T::ExitCancelled || result.exit == T::ExitCancelledPriority;
        if (cancelled && m_detailsCancelled) {
            map.insert(QStringLiteral("state"), QStringLiteral("error"));
            map.insert(QStringLiteral("error"), tr("Stopped because another operation was started. Close and open the details again to load them."));
        } else if (!result.ok) {
            map.insert(QStringLiteral("state"), QStringLiteral("error"));
            map.insert(QStringLiteral("error"), result.errorMessage());
        } else if (result.updateDetails.isEmpty()) {
            map.insert(QStringLiteral("state"), QStringLiteral("error"));
            map.insert(QStringLiteral("error"), tr("PackageKit returned no details for this update."));
        } else {
            const UpdateDetailEntry &d = result.updateDetails.constFirst();
            map = d.toJson().toVariantMap();
            map.insert(QStringLiteral("state"), QStringLiteral("ready"));
            map.insert(QStringLiteral("restartText"), restartText(d.restart));
            QStringList replaced;
            for (const QString &id : d.updates) {
                const PackageId p = PackageId::parse(id);
                replaced << (p.valid ? p.name + QLatin1Char(' ') + p.version : id);
            }
            map.insert(QStringLiteral("replaces"), replaced);
        }
        m_details.insert(packageId, map);
        Q_EMIT detailChanged(packageId);
    });
}

void UpdateChecker::cancelDetails()
{
    m_detailsCancelled = false;
    for (const QPointer<Query> &query : std::as_const(m_detailQueries)) {
        if (query) {
            m_detailsCancelled = true;
            query->cancel();
        }
    }
}

QVariantMap UpdateChecker::detail(const QString &packageId) const
{
    const auto it = m_details.constFind(packageId);
    if (it == m_details.constEnd())
        return QVariantMap{{QStringLiteral("state"), QStringLiteral("none")}};
    return it.value();
}

QStringList UpdateChecker::idsInGroup(const QString &group) const
{
    QStringList ids;
    for (const PackageEntry &e : m_model.entries()) {
        if (e.group == group)
            ids << e.id;
    }
    return ids;
}

QStringList UpdateChecker::installableIds() const
{
    QStringList ids;
    for (const PackageEntry &e : m_model.entries()) {
        if (e.group != QLatin1String("blocked"))
            ids << e.id;
    }
    return ids;
}

QJsonObject UpdateChecker::toJson() const
{
    QJsonObject o{
        {QStringLiteral("state"), m_state},
        {QStringLiteral("count"), count()},
        {QStringLiteral("securityCount"), securityCount()},
        {QStringLiteral("kernelCount"), kernelCount()},
        {QStringLiteral("nexusCount"), nexusCount()},
        {QStringLiteral("otherCount"), otherCount()},
        {QStringLiteral("blockedCount"), blockedCount()},
        {QStringLiteral("refreshed"), m_refreshed},
        {QStringLiteral("updates"), m_model.toJson()},
    };
    if (m_sizesKnown)
        o.insert(QStringLiteral("downloadSize"), static_cast<qint64>(m_totalSize));
    if (!m_error.isEmpty())
        o.insert(QStringLiteral("error"), m_error);
    if (!m_refreshError.isEmpty())
        o.insert(QStringLiteral("refreshError"), m_refreshError);
    if (m_lastChecked.isValid())
        o.insert(QStringLiteral("checkedAt"), m_lastChecked.toString(Qt::ISODate));
    if (m_lastRefreshed.isValid())
        o.insert(QStringLiteral("listsRefreshedAt"), m_lastRefreshed.toString(Qt::ISODate));
    return o;
}

} // namespace NexusPk
