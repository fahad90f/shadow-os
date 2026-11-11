// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "packagemodel.h"
#include "pkquery.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

namespace ShadowPk {

// The list of available updates, as both SHADOW Updater and the Updates view
// of SHADOW Store show it.
//
//   check(true)   RefreshCache, then GetUpdates
//   check(false)  GetUpdates from the lists already on disk
//
// then, for the updates found: Resolve(installed) for the version currently
// installed and GetDetails for the download size. Change logs are fetched one
// package at a time on request (GetUpdateDetail downloads them).
//
// Grouping — from PackageKit's update classification and the package name:
//   security  PackageKit says "security"
//   kernel    linux-image-*, linux-headers-*, linux-modules-*, linux-kbuild-*
//   shadow     shadow-*, libshadow*
//   other     everything else
//   blocked   held back by the package manager (cannot be selected)
class UpdateChecker : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY progressChanged)
    Q_PROPERTY(int percentage READ percentage NOTIFY progressChanged)
    Q_PROPERTY(QObject *model READ modelObject CONSTANT)
    Q_PROPERTY(int count READ count NOTIFY resultChanged)
    Q_PROPERTY(int securityCount READ securityCount NOTIFY resultChanged)
    Q_PROPERTY(int kernelCount READ kernelCount NOTIFY resultChanged)
    Q_PROPERTY(int shadowCount READ shadowCount NOTIFY resultChanged)
    Q_PROPERTY(int otherCount READ otherCount NOTIFY resultChanged)
    Q_PROPERTY(int blockedCount READ blockedCount NOTIFY resultChanged)
    Q_PROPERTY(double totalSize READ totalSize NOTIFY resultChanged)
    Q_PROPERTY(bool sizesKnown READ sizesKnown NOTIFY resultChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString refreshError READ refreshError NOTIFY stateChanged)
    Q_PROPERTY(bool refreshed READ refreshed NOTIFY stateChanged)
    Q_PROPERTY(QDateTime lastChecked READ lastChecked NOTIFY stateChanged)
    Q_PROPERTY(QDateTime lastRefreshed READ lastRefreshed NOTIFY stateChanged)

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // "idle" (not checked yet) | "refreshing" | "loading" | "ready" | "error"
    QString state() const { return m_state; }
    bool busy() const { return m_state == QLatin1String("refreshing") || m_state == QLatin1String("loading"); }
    QString statusText() const { return m_statusText; }
    int percentage() const { return m_percentage; }
    PackageModel *model() { return &m_model; }
    QObject *modelObject() { return &m_model; }
    // Installable updates (everything except held-back packages).
    int count() const { return m_counts.value(QStringLiteral("total")); }
    int securityCount() const { return m_counts.value(QStringLiteral("security")); }
    int kernelCount() const { return m_counts.value(QStringLiteral("kernel")); }
    int shadowCount() const { return m_counts.value(QStringLiteral("shadow")); }
    int otherCount() const { return m_counts.value(QStringLiteral("other")); }
    int blockedCount() const { return m_counts.value(QStringLiteral("blocked")); }
    double totalSize() const { return static_cast<double>(m_totalSize); }
    bool sizesKnown() const { return m_sizesKnown; }
    // Why the list could not be read (verbatim from PackageKit / D-Bus).
    QString error() const { return m_error; }
    // Why refreshing the software lists failed, verbatim — signature and
    // download problems end up here. The list is then from the previous refresh.
    QString refreshError() const { return m_refreshError; }
    // True if the last check refreshed the software lists successfully.
    bool refreshed() const { return m_refreshed; }
    QDateTime lastChecked() const { return m_lastChecked; }
    // When PackageKit last refreshed the software lists (invalid if never / unknown).
    QDateTime lastRefreshed() const { return m_lastRefreshed; }

    Q_INVOKABLE void check(bool refresh);
    Q_INVOKABLE void cancel();

    // Change log and update details of one update; asynchronous, cached.
    Q_INVOKABLE void loadDetail(const QString &packageId);
    // { state: "none" | "loading" | "ready" | "error", changelog, updateText, cveUrls, … , error }
    Q_INVOKABLE QVariantMap detail(const QString &packageId) const;
    // Stop detail requests that are still running. The APT back-end fetches
    // changelogs from the network one request at a time, and a transaction
    // started meanwhile would wait behind them.
    Q_INVOKABLE void cancelDetails();

    QStringList idsInGroup(const QString &group) const;
    QStringList installableIds() const;
    QJsonObject toJson() const;

    static QString groupFor(const PackageEntry &entry);
    static bool isKernelPackage(const QString &name);
    static bool isShadowPackage(const QString &name);

Q_SIGNALS:
    void stateChanged();
    void progressChanged();
    void resultChanged();
    void detailChanged(const QString &packageId);
    // A check() ran to its end (successfully or not).
    void checked();

private:
    void setState(const QString &state);
    void trackProgress(PackageKit::Transaction *transaction);
    void startGetUpdates();
    void gotUpdates(const QueryResult &result);
    void resolveInstalled(int offset);
    void fetchSizes(int offset);
    void finishCheck();
    void recount();
    void queryLastRefresh();

    QString m_state = QStringLiteral("idle");
    QString m_statusText;
    int m_percentage = -1;
    PackageModel m_model;
    QHash<QString, int> m_counts;
    qulonglong m_totalSize = 0;
    bool m_sizesKnown = false;
    QString m_error;
    QString m_refreshError;
    bool m_refreshed = false;
    QDateTime m_lastChecked;
    QDateTime m_lastRefreshed;
    QPointer<Query> m_query;
    QPointer<PackageKit::Transaction> m_transaction;
    quint64 m_serial = 0;
    QStringList m_pendingNames;
    QStringList m_pendingIds;
    QHash<QString, QVariantMap> m_details;
    QHash<QString, QPointer<Query>> m_detailQueries;
    bool m_detailsCancelled = false;
};

} // namespace ShadowPk
