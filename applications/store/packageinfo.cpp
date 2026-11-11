// SPDX-License-Identifier: GPL-3.0-or-later
#include "packageinfo.h"

#include <pkjob.h>
#include <pkservice.h>
#include <pkutil.h>
#include <systemfacts.h>

#include <Daemon>

#include <QJsonArray>
#include <QTimer>

#include <algorithm>

using ShadowPk::Service;
using T = PackageKit::Transaction;

PackageInfo::PackageInfo(QObject *parent)
    : QObject(parent)
{
    // Follow the daemon appearing or disappearing.
    connect(Service::instance(), &Service::probed, this, [this] {
        if (m_state == QLatin1String("unavailable") || m_state == QLatin1String("idle"))
            refresh();
    });
}

void PackageInfo::setPackageName(const QString &name)
{
    if (name == m_name)
        return;
    m_name = name;
    Q_EMIT packageNameChanged();
    refresh();
}

QString PackageInfo::arch() const
{
    return candidateAvailable() || m_resolved.foreignOnly ? m_resolved.candidate.arch : m_resolved.installedEntry.arch;
}

QString PackageInfo::archRelation() const
{
    return ShadowPk::archRelation(arch());
}

QString PackageInfo::repo() const
{
    return candidateAvailable() ? m_resolved.candidate.repo : m_resolved.installedEntry.repo;
}

QString PackageInfo::repoText() const
{
    return ShadowPk::repoText(repo());
}

QString PackageInfo::repoCategory() const
{
    const QString r = repo();
    if (r.isEmpty() || r == QLatin1String("local"))
        return {};
    // The origin part of a repository ID is the Release file's "Origin", lower-cased.
    return ShadowPk::repoCategory(QString(), ShadowPk::describeRepo(r).value(QStringLiteral("origin")).toString());
}

void PackageInfo::reset()
{
    m_error.clear();
    m_resolved = ShadowPk::ResolvedPackage();
    m_summary.clear();
    m_description.clear();
    m_url.clear();
    m_license.clear();
    m_group.clear();
    m_installedSize = -1;
    m_downloadSize = -1;
    m_simulationState = QStringLiteral("idle");
    m_simulationError.clear();
    m_changes.clear();
    m_trust = QStringLiteral("unknown");
    m_totalDownloadSize = -1;
    m_dependsState = QStringLiteral("idle");
    m_dependsError.clear();
    m_depends.clear();
}

void PackageInfo::refresh()
{
    const quint64 serial = ++m_serial;
    reset();
    m_pending = 0;

    if (m_name.isEmpty()) {
        m_state = QStringLiteral("idle");
        Q_EMIT changed();
        QTimer::singleShot(0, this, [this, serial] { if (serial == m_serial) Q_EMIT settled(); });
        return;
    }
    if (!Service::instance()->available()) {
        m_state = QStringLiteral("unavailable");
        m_error = Service::instance()->error();
        Q_EMIT changed();
        QTimer::singleShot(0, this, [this, serial] { if (serial == m_serial) Q_EMIT settled(); });
        return;
    }

    m_state = QStringLiteral("loading");
    Q_EMIT changed();
    const QString name = m_name;
    ShadowPk::resolvePackages(this, {name}, [this, serial, name](const QHash<QString, ShadowPk::ResolvedPackage> &found,
                                                              const ShadowPk::QueryResult &result) {
        if (serial != m_serial)
            return;
        if (!result.ok) {
            m_state = QStringLiteral("error");
            m_error = result.errorMessage();
            Q_EMIT changed();
            Q_EMIT settled();
            return;
        }
        m_resolved = found.value(name);
        if (!m_resolved.found) {
            m_state = QStringLiteral("unknown");
            Q_EMIT changed();
            Q_EMIT settled();
            return;
        }
        m_state = QStringLiteral("ready");
        m_summary = m_resolved.hasCandidate ? m_resolved.candidate.summary : m_resolved.installedEntry.summary;
        Q_EMIT changed();
        loadDetails();
        simulate();
        loadDepends();
        if (m_pending == 0)
            Q_EMIT settled();
    });
}

void PackageInfo::queryDone()
{
    Q_EMIT changed();
    if (--m_pending == 0)
        Q_EMIT settled();
}

void PackageInfo::loadDetails()
{
    QStringList ids;
    if (m_resolved.installed)
        ids << m_resolved.installedEntry.id;
    if (m_resolved.hasCandidate)
        ids << m_resolved.candidate.id;
    if (ids.isEmpty())
        return;
    const quint64 serial = m_serial;
    ++m_pending;
    ShadowPk::Query::run(this, PackageKit::Daemon::getDetails(ids), [this, serial](const ShadowPk::QueryResult &result) {
        if (serial != m_serial)
            return;
        for (const ShadowPk::PackageDetails &d : result.details) {
            const bool forCandidate = m_resolved.hasCandidate && d.packageId == m_resolved.candidate.id;
            const bool forInstalled = m_resolved.installed && d.packageId == m_resolved.installedEntry.id;
            if (!forCandidate && !forInstalled)
                continue;
            // Text fields: prefer the version on offer; sizes: per version.
            if (forCandidate || m_description.isEmpty()) {
                m_description = d.description;
                m_url = d.url;
                m_license = d.license == QLatin1String("unknown") ? QString() : d.license;
                m_group = d.group;
                if (!d.summary.isEmpty())
                    m_summary = d.summary;
            }
            if (forCandidate) {
                // What installing or updating would fetch.
                m_downloadSize = d.hasDownloadSize ? static_cast<double>(d.downloadSize)
                                                   : static_cast<double>(d.size);
                if (d.hasDownloadSize && !m_resolved.installed)
                    m_installedSize = static_cast<double>(d.size);
            }
            if (forInstalled)
                m_installedSize = static_cast<double>(d.size);
        }
        queryDone();
    });
}

void PackageInfo::simulate()
{
    if (!candidateAvailable())
        return;
    const quint64 serial = m_serial;
    const QStringList ids{m_resolved.candidate.id};
    const T::TransactionFlags flags = T::TransactionFlagSimulate | T::TransactionFlagOnlyTrusted;
    PackageKit::Transaction *t = m_resolved.installed ? PackageKit::Daemon::updatePackages(ids, flags)
                                                      : PackageKit::Daemon::installPackages(ids, flags);
    m_simulationState = QStringLiteral("running");
    ++m_pending;
    ShadowPk::Query::run(this, t, [this, serial](const ShadowPk::QueryResult &result) {
        if (serial != m_serial)
            return;
        if (result.exit == T::ExitSuccess || result.exit == T::ExitNeedUntrusted) {
            m_changes = ShadowPk::summarizeChanges(result.packages, result.downloadSize, {m_name});
            const bool untrusted = result.exit == T::ExitNeedUntrusted
                                   || m_changes.value(QStringLiteral("untrustedCount")).toInt() > 0;
            m_trust = untrusted ? QStringLiteral("untrusted") : QStringLiteral("trusted");
            m_totalDownloadSize = static_cast<double>(result.downloadSize);
            m_simulationState = QStringLiteral("ready");
            if (untrusted && result.hasError)
                m_simulationError = result.errorMessage();
        } else {
            m_simulationState = QStringLiteral("error");
            m_simulationError = result.errorMessage();
        }
        queryDone();
    });
}

void PackageInfo::loadDepends()
{
    const QString id = candidateAvailable() ? m_resolved.candidate.id : m_resolved.installedEntry.id;
    if (id.isEmpty())
        return;
    if (!Service::instance()->supports(T::RoleDependsOn)) {
        m_dependsState = QStringLiteral("unsupported");
        return;
    }
    const quint64 serial = m_serial;
    m_dependsState = QStringLiteral("loading");
    ++m_pending;
    ShadowPk::Query::run(this, PackageKit::Daemon::dependsOn(id, T::FilterNone, false),
                        [this, serial](const ShadowPk::QueryResult &result) {
        if (serial != m_serial)
            return;
        if (!result.ok) {
            m_dependsState = QStringLiteral("error");
            m_dependsError = result.errorMessage();
            queryDone();
            return;
        }
        QList<ShadowPk::PackageEntry> entries = result.packages;
        std::sort(entries.begin(), entries.end(), [](const ShadowPk::PackageEntry &a, const ShadowPk::PackageEntry &b) {
            return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
        });
        QVariantList list;
        QString last;
        for (const ShadowPk::PackageEntry &e : std::as_const(entries)) {
            const QString key = e.name + QLatin1Char(':') + e.arch;
            if (key == last)
                continue;
            last = key;
            list << QVariantMap{
                {QStringLiteral("packageId"), e.id},
                {QStringLiteral("name"), e.name},
                {QStringLiteral("version"), e.version},
                {QStringLiteral("arch"), e.arch},
                {QStringLiteral("installed"), e.installed},
                {QStringLiteral("summary"), e.summary},
            };
        }
        m_depends = list;
        m_dependsState = QStringLiteral("ready");
        queryDone();
    });
}

QJsonObject PackageInfo::toJson() const
{
    QJsonObject o{
        {QStringLiteral("name"), m_name},
        {QStringLiteral("state"), m_state},
    };
    if (!m_error.isEmpty())
        o.insert(QStringLiteral("error"), m_error);
    if (m_state != QLatin1String("ready"))
        return o;

    o.insert(QStringLiteral("installed"), installed());
    if (installed()) {
        o.insert(QStringLiteral("installedVersion"), installedVersion());
        o.insert(QStringLiteral("installedPackageId"), installedId());
        o.insert(QStringLiteral("installedAsDependency"), automatic());
    }
    o.insert(QStringLiteral("candidateAvailable"), candidateAvailable());
    if (m_resolved.hasCandidate) {
        o.insert(QStringLiteral("candidateVersion"), candidateVersion());
        o.insert(QStringLiteral("candidatePackageId"), candidateId());
    }
    o.insert(QStringLiteral("updateAvailable"), updateAvailable());
    o.insert(QStringLiteral("architecture"), arch());
    o.insert(QStringLiteral("systemArchitecture"), ShadowPk::systemArchitecture());
    o.insert(QStringLiteral("architectureRelation"), archRelation());
    o.insert(QStringLiteral("availableForThisArchitecture"), !foreignOnly() && (installed() || candidateAvailable()));
    o.insert(QStringLiteral("originRepository"), repo());
    o.insert(QStringLiteral("originRepositoryText"), repoText());
    o.insert(QStringLiteral("originCategory"), repoCategory());
    o.insert(QStringLiteral("summary"), m_summary);
    o.insert(QStringLiteral("description"), m_description);
    o.insert(QStringLiteral("url"), m_url);
    o.insert(QStringLiteral("license"), m_license);
    o.insert(QStringLiteral("group"), m_group);
    if (m_installedSize >= 0)
        o.insert(QStringLiteral("installedSize"), static_cast<qint64>(m_installedSize));
    if (m_downloadSize >= 0)
        o.insert(QStringLiteral("downloadSize"), static_cast<qint64>(m_downloadSize));

    QJsonObject simulation{{QStringLiteral("state"), m_simulationState}};
    if (m_simulationState == QLatin1String("ready")) {
        simulation.insert(QStringLiteral("trust"), m_trust);
        simulation.insert(QStringLiteral("totalDownloadSize"), static_cast<qint64>(m_totalDownloadSize));
        simulation.insert(QStringLiteral("changes"), QJsonObject::fromVariantMap(m_changes));
    }
    if (!m_simulationError.isEmpty())
        simulation.insert(QStringLiteral("error"), m_simulationError);
    o.insert(QStringLiteral("simulation"), simulation);

    QJsonObject depends{{QStringLiteral("state"), m_dependsState}};
    if (m_dependsState == QLatin1String("ready"))
        depends.insert(QStringLiteral("packages"), QJsonArray::fromVariantList(m_depends));
    if (!m_dependsError.isEmpty())
        depends.insert(QStringLiteral("error"), m_dependsError);
    o.insert(QStringLiteral("dependsOn"), depends);
    return o;
}
