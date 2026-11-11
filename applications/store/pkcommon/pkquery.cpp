// SPDX-License-Identifier: GPL-3.0-or-later
#include "pkquery.h"

#include "pkutil.h"
#include "systemfacts.h"

#include <Daemon>
#include <Details>

#include <QCoreApplication>
#include <QJsonArray>
#include <QTimer>

namespace NexusPk {

QJsonObject UpdateDetailEntry::toJson() const
{
    QJsonObject o{
        {QStringLiteral("packageId"), packageId},
        {QStringLiteral("updates"), QJsonArray::fromStringList(updates)},
        {QStringLiteral("obsoletes"), QJsonArray::fromStringList(obsoletes)},
        {QStringLiteral("vendorUrls"), QJsonArray::fromStringList(vendorUrls)},
        {QStringLiteral("bugzillaUrls"), QJsonArray::fromStringList(bugzillaUrls)},
        {QStringLiteral("cveUrls"), QJsonArray::fromStringList(cveUrls)},
        {QStringLiteral("restart"), restartKey(restart)},
        {QStringLiteral("updateText"), updateText},
        {QStringLiteral("changelog"), changelog},
        {QStringLiteral("state"), updateStateKey(state)},
    };
    if (issued.isValid())
        o.insert(QStringLiteral("issued"), issued.toString(Qt::ISODate));
    if (updated.isValid())
        o.insert(QStringLiteral("updated"), updated.toString(Qt::ISODate));
    return o;
}

QJsonObject PackageDetails::toJson() const
{
    QJsonObject o{
        {QStringLiteral("packageId"), packageId},
        {QStringLiteral("summary"), summary},
        {QStringLiteral("description"), description},
        {QStringLiteral("url"), url},
        {QStringLiteral("license"), license},
        {QStringLiteral("group"), group},
        {QStringLiteral("size"), static_cast<qint64>(size)},
    };
    if (hasDownloadSize)
        o.insert(QStringLiteral("downloadSize"), static_cast<qint64>(downloadSize));
    return o;
}

QString QueryResult::errorMessage() const
{
    if (ok)
        return {};
    if (hasError) {
        return QCoreApplication::translate("NexusPk", "%1 (PackageKit error “%2”)%3")
            .arg(errorText(error), errorKey(error),
                 errorDetails.trimmed().isEmpty() ? QString() : QStringLiteral(": ") + errorDetails.trimmed());
    }
    if (exit == PackageKit::Transaction::ExitCancelled || exit == PackageKit::Transaction::ExitCancelledPriority)
        return QCoreApplication::translate("NexusPk", "The request was cancelled");
    return QCoreApplication::translate("NexusPk", "PackageKit ended the request without a result (exit “%1”)")
        .arg(exitKey(exit));
}

QJsonObject QueryResult::errorJson() const
{
    QJsonObject o{
        {QStringLiteral("exit"), exitKey(exit)},
        {QStringLiteral("message"), errorMessage()},
    };
    if (hasError) {
        o.insert(QStringLiteral("code"), errorKey(error));
        o.insert(QStringLiteral("details"), errorDetails);
    }
    return o;
}

Query *Query::run(QObject *context, PackageKit::Transaction *transaction, Callback done)
{
    return new Query(context, transaction, std::move(done));
}

Query::Query(QObject *context, PackageKit::Transaction *transaction, Callback done)
    : QObject(context), m_transaction(transaction), m_done(std::move(done))
{
    using T = PackageKit::Transaction;
    if (!transaction) {
        // Report through the event loop, like a real transaction would.
        QTimer::singleShot(0, this, [this] {
            m_result.hasError = true;
            m_result.error = T::ErrorInternalError;
            m_result.errorDetails = QCoreApplication::translate("NexusPk", "PackageKit did not create a transaction");
            finish(T::ExitFailed, 0);
        });
        return;
    }

    connect(transaction, &T::package, this, [this](T::Info info, const QString &packageId, const QString &summary) {
        m_result.packages.append(PackageEntry::fromId(packageId, info, summary));
    });
    connect(transaction, &T::details, this, [this](const PackageKit::Details &values) {
        PackageDetails d;
        d.packageId = values.packageId();
        d.summary = values.summary();
        d.description = values.description();
        d.url = values.url();
        d.license = values.license();
        d.group = groupKey(values.group());
        d.size = values.size();
        d.hasDownloadSize = values.contains(QStringLiteral("download-size"));
        d.downloadSize = values.value(QStringLiteral("download-size")).toULongLong();
        m_result.details.append(d);
    });
    connect(transaction, &T::repoDetail, this, [this](const QString &repoId, const QString &description, bool enabled) {
        m_result.repos.append(RepoEntry{repoId, description, enabled});
    });
    connect(transaction, &T::updateDetail, this,
            [this](const QString &packageId, const QStringList &updates, const QStringList &obsoletes,
                   const QStringList &vendorUrls, const QStringList &bugzillaUrls, const QStringList &cveUrls,
                   T::Restart restart, const QString &updateText, const QString &changelog, T::UpdateState state,
                   const QDateTime &issued, const QDateTime &updated) {
        UpdateDetailEntry d;
        d.packageId = packageId;
        d.updates = updates;
        d.obsoletes = obsoletes;
        d.vendorUrls = vendorUrls;
        d.bugzillaUrls = bugzillaUrls;
        d.cveUrls = cveUrls;
        d.restart = restart;
        d.updateText = updateText;
        d.changelog = changelog;
        d.state = state;
        d.issued = issued;
        d.updated = updated;
        m_result.updateDetails.append(d);
    });
    connect(transaction, &T::downloadSizeRemainingChanged, this, [this] {
        if (m_transaction)
            m_result.downloadSize = qMax(m_result.downloadSize, m_transaction->downloadSizeRemaining());
    });
    connect(transaction, &T::errorCode, this, [this](T::Error error, const QString &details) {
        m_result.hasError = true;
        m_result.error = error;
        m_result.errorDetails = details;
    });
    connect(transaction, &T::finished, this, &Query::finish);
    // PackageKit-Qt deletes a transaction whose daemon vanished without
    // always announcing "finished"; never leave the caller waiting.
    connect(transaction, &QObject::destroyed, this, [this] {
        if (m_finished)
            return;
        if (!m_result.hasError) {
            m_result.hasError = true;
            m_result.error = T::ErrorInternalError;
            m_result.errorDetails = QCoreApplication::translate("NexusPk", "The PackageKit transaction ended unexpectedly");
        }
        finish(T::ExitFailed, 0);
    });
}

void Query::cancel()
{
    if (m_transaction && !m_finished)
        m_transaction->cancel();
}

void Query::finish(PackageKit::Transaction::Exit exit, uint runtime)
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_transaction)
        m_result.downloadSize = qMax(m_result.downloadSize, m_transaction->downloadSizeRemaining());
    m_result.exit = exit;
    m_result.runtimeMs = runtime;
    m_result.ok = exit == PackageKit::Transaction::ExitSuccess;
    const Callback done = std::move(m_done);
    m_done = nullptr;
    deleteLater();
    if (done)
        done(m_result);
}

QJsonObject ResolvedPackage::toJson() const
{
    QJsonObject o{
        {QStringLiteral("name"), name},
        {QStringLiteral("found"), found},
        {QStringLiteral("installed"), installed},
        {QStringLiteral("hasCandidate"), hasCandidate},
        {QStringLiteral("foreignArchitectureOnly"), foreignOnly},
    };
    if (installed)
        o.insert(QStringLiteral("installedPackage"), installedEntry.toJson());
    if (hasCandidate)
        o.insert(QStringLiteral("candidate"), candidate.toJson());
    return o;
}

void resolvePackages(QObject *context, const QStringList &names,
                     std::function<void(const QHash<QString, ResolvedPackage> &, const QueryResult &)> done)
{
    Query::run(context, PackageKit::Daemon::resolve(names, PackageKit::Transaction::FilterNone),
               [names, done](const QueryResult &result) {
        QHash<QString, ResolvedPackage> out;
        for (const QString &name : names) {
            ResolvedPackage r;
            r.name = name;
            out.insert(name, r);
        }
        const QString native = systemArchitecture();
        for (const PackageEntry &e : result.packages) {
            auto it = out.find(e.name);
            if (it == out.end())
                continue;
            ResolvedPackage &r = it.value();
            r.found = true;
            r.all.append(e);
            if (e.installed) {
                // With several architectures installed, prefer the native one.
                if (!r.installed || e.arch == native) {
                    r.installed = true;
                    r.installedEntry = e;
                }
                continue;
            }
            const bool usable = e.arch == native || e.arch == QLatin1String("all");
            const bool currentUsable = r.hasCandidate
                && (r.candidate.arch == native || r.candidate.arch == QLatin1String("all"));
            if (!r.hasCandidate || (usable && !currentUsable)
                || (usable == currentUsable && compareVersions(e.version, r.candidate.version) > 0)) {
                r.hasCandidate = true;
                r.candidate = e;
            }
        }
        for (auto it = out.begin(); it != out.end(); ++it) {
            ResolvedPackage &r = it.value();
            if (r.hasCandidate && r.candidate.arch != native && r.candidate.arch != QLatin1String("all")) {
                // Offered only for another architecture: not installable as a native package.
                r.foreignOnly = !r.installed;
            }
        }
        done(out, result);
    });
}

} // namespace NexusPk
