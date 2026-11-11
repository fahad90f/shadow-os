// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "packagemodel.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include <Transaction>

#include <functional>

namespace NexusPk {

struct RepoEntry
{
    QString id;             // PackageKit repository ID (APT: "<file>:<type> <uri> <suite> <components>")
    QString description;
    bool enabled = false;
};

struct UpdateDetailEntry
{
    QString packageId;
    QStringList updates;        // package IDs this update replaces (the installed version)
    QStringList obsoletes;
    QStringList vendorUrls;
    QStringList bugzillaUrls;
    QStringList cveUrls;
    PackageKit::Transaction::Restart restart = PackageKit::Transaction::RestartUnknown;
    QString updateText;
    QString changelog;
    PackageKit::Transaction::UpdateState state = PackageKit::Transaction::UpdateStateUnknown;
    QDateTime issued;
    QDateTime updated;
    QJsonObject toJson() const;
};

struct PackageDetails
{
    QString packageId;
    QString summary;
    QString description;
    QString url;
    QString license;
    QString group;              // PackageKit group key
    // PackageKit 1.3 reports both sizes: "size" is what the package occupies
    // once installed and "download-size" what has to be fetched. Older
    // daemons only send "size" (download size for packages not installed).
    qulonglong size = 0;
    bool hasDownloadSize = false;
    qulonglong downloadSize = 0;
    // Bytes to fetch for this package, as well as the daemon lets us know.
    qulonglong bytesToDownload() const { return hasDownloadSize ? downloadSize : size; }
    QJsonObject toJson() const;
};

// Everything a finished read-only transaction produced.
struct QueryResult
{
    bool ok = false;                                            // exit == success
    PackageKit::Transaction::Exit exit = PackageKit::Transaction::ExitUnknown;
    bool hasError = false;
    PackageKit::Transaction::Error error = PackageKit::Transaction::ErrorUnknown;
    QString errorDetails;                                       // verbatim from PackageKit
    uint runtimeMs = 0;
    qulonglong downloadSize = 0;                                // largest DownloadSizeRemaining seen

    QList<PackageEntry> packages;
    QList<PackageDetails> details;
    QList<RepoEntry> repos;
    QList<UpdateDetailEntry> updateDetails;

    // "PackageKit error no-network: Cannot download …" / "The transaction was cancelled".
    QString errorMessage() const;
    QJsonObject errorJson() const;
};

// Runs one PackageKit transaction that only reports data (search, resolve,
// details, repository list, updates, simulations …), collects what it emits
// and hands the result to a callback when PackageKit says it is finished.
//
// The callback runs once, on the context object's thread; it is dropped if the
// context is destroyed first. PackageKit-Qt deletes the transaction itself.
class Query : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const QueryResult &)>;

    static Query *run(QObject *context, PackageKit::Transaction *transaction, Callback done);

    // Ask PackageKit to cancel; the callback still fires (exit = cancelled).
    void cancel();
    PackageKit::Transaction *transaction() const { return m_transaction; }

private:
    Query(QObject *context, PackageKit::Transaction *transaction, Callback done);
    void finish(PackageKit::Transaction::Exit exit, uint runtime);

    QPointer<PackageKit::Transaction> m_transaction;
    Callback m_done;
    QueryResult m_result;
    bool m_finished = false;
};

// What PackageKit knows about one package name (Resolve).
struct ResolvedPackage
{
    QString name;
    bool found = false;             // PackageKit knows a package of this name
    bool installed = false;
    PackageEntry installedEntry;    // valid if installed
    // The version PackageKit would install: not installed, for this system's
    // architecture (or "all"). If the package is installed and no other
    // version is offered, there is no candidate.
    bool hasCandidate = false;
    PackageEntry candidate;
    // Only versions for a foreign architecture are offered.
    bool foreignOnly = false;
    QList<PackageEntry> all;
    QJsonObject toJson() const;
};

// Resolve package names to their installed and candidate package IDs.
// 'done' gets one entry per requested name (found == false if unknown) and
// the transaction result (for the error, if any).
void resolvePackages(QObject *context, const QStringList &names,
                     std::function<void(const QHash<QString, ResolvedPackage> &, const QueryResult &)> done);

} // namespace NexusPk
