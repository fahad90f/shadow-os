// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <pkquery.h>

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// What PackageKit knows about one package, for the application page:
//
//   Resolve          installed version, candidate version, architecture, origin repository
//   GetDetails       installed size, download size, licence, homepage, description
//   Install/Update   (simulated, only-trusted) the dependencies that would be installed,
//                    packages that would be removed, the total download size and whether
//                    every package can be authenticated
//
// Nothing is installed or changed by this object.
class PackageInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString packageName READ packageName WRITE setPackageName NOTIFY packageNameChanged)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool installed READ installed NOTIFY changed)
    Q_PROPERTY(bool automatic READ automatic NOTIFY changed)
    Q_PROPERTY(QString installedId READ installedId NOTIFY changed)
    Q_PROPERTY(QString installedVersion READ installedVersion NOTIFY changed)
    Q_PROPERTY(QString installedArch READ installedArch NOTIFY changed)
    Q_PROPERTY(QString installedRepo READ installedRepo NOTIFY changed)
    Q_PROPERTY(bool candidateAvailable READ candidateAvailable NOTIFY changed)
    Q_PROPERTY(QString candidateId READ candidateId NOTIFY changed)
    Q_PROPERTY(QString candidateVersion READ candidateVersion NOTIFY changed)
    Q_PROPERTY(QString candidateArch READ candidateArch NOTIFY changed)
    Q_PROPERTY(QString candidateRepo READ candidateRepo NOTIFY changed)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY changed)
    Q_PROPERTY(bool foreignOnly READ foreignOnly NOTIFY changed)
    Q_PROPERTY(QString arch READ arch NOTIFY changed)
    Q_PROPERTY(QString archRelation READ archRelation NOTIFY changed)
    Q_PROPERTY(QString repo READ repo NOTIFY changed)
    Q_PROPERTY(QString repoText READ repoText NOTIFY changed)
    Q_PROPERTY(QString repoCategory READ repoCategory NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString description READ description NOTIFY changed)
    Q_PROPERTY(QString url READ url NOTIFY changed)
    Q_PROPERTY(QString license READ license NOTIFY changed)
    Q_PROPERTY(QString group READ group NOTIFY changed)
    Q_PROPERTY(double installedSize READ installedSize NOTIFY changed)
    Q_PROPERTY(double downloadSize READ downloadSize NOTIFY changed)
    Q_PROPERTY(QString simulationState READ simulationState NOTIFY changed)
    Q_PROPERTY(QString simulationError READ simulationError NOTIFY changed)
    Q_PROPERTY(QVariantMap changes READ changes NOTIFY changed)
    Q_PROPERTY(QString trust READ trust NOTIFY changed)
    Q_PROPERTY(double totalDownloadSize READ totalDownloadSize NOTIFY changed)
    Q_PROPERTY(QString dependsState READ dependsState NOTIFY changed)
    Q_PROPERTY(QString dependsError READ dependsError NOTIFY changed)
    Q_PROPERTY(QVariantList depends READ depends NOTIFY changed)

public:
    explicit PackageInfo(QObject *parent = nullptr);

    QString packageName() const { return m_name; }
    void setPackageName(const QString &name);

    // "idle"        no package name given
    // "loading"     asking PackageKit
    // "ready"       PackageKit knows the package
    // "unknown"     PackageKit knows no package of that name (not in any enabled repository)
    // "unavailable" PackageKit is not reachable (error() has the D-Bus error)
    // "error"       the query failed (error() has PackageKit's message)
    QString state() const { return m_state; }
    QString error() const { return m_error; }

    bool installed() const { return m_resolved.installed; }
    bool automatic() const { return m_resolved.installedEntry.automatic; }
    QString installedId() const { return m_resolved.installedEntry.id; }
    QString installedVersion() const { return m_resolved.installedEntry.version; }
    QString installedArch() const { return m_resolved.installedEntry.arch; }
    QString installedRepo() const { return m_resolved.installedEntry.repo; }
    bool candidateAvailable() const { return m_resolved.hasCandidate && !m_resolved.foreignOnly; }
    QString candidateId() const { return m_resolved.candidate.id; }
    QString candidateVersion() const { return m_resolved.candidate.version; }
    QString candidateArch() const { return m_resolved.candidate.arch; }
    QString candidateRepo() const { return m_resolved.candidate.repo; }
    // Installed, and PackageKit offers a different version.
    bool updateAvailable() const { return m_resolved.installed && candidateAvailable(); }
    // Offered only for an architecture other than this system's.
    bool foreignOnly() const { return m_resolved.foreignOnly; }
    // Of the version the page is about: the candidate if there is one, else the installed one.
    QString arch() const;
    // "native" | "all" | "foreign" | "unknown"
    QString archRelation() const;
    QString repo() const;
    QString repoText() const;
    // "debian" | "shadow" | "third-party" | "" (not from a repository / unknown)
    QString repoCategory() const;

    QString summary() const { return m_summary; }
    QString description() const { return m_description; }
    QString url() const { return m_url; }
    QString license() const { return m_license; }
    QString group() const { return m_group; }
    // Bytes; -1 when PackageKit did not report it.
    double installedSize() const { return m_installedSize; }
    double downloadSize() const { return m_downloadSize; }

    // "idle" (nothing to simulate) | "running" | "ready" | "error"
    QString simulationState() const { return m_simulationState; }
    QString simulationError() const { return m_simulationError; }
    // See ShadowPk::summarizeChanges().
    QVariantMap changes() const { return m_changes; }
    // "unknown" | "trusted" (every package can be authenticated) | "untrusted"
    QString trust() const { return m_trust; }
    // Bytes to download for the package and its dependencies; -1 if unknown.
    double totalDownloadSize() const { return m_totalDownloadSize; }

    // The packages this one depends on directly (PackageKit DependsOn, not
    // recursive), of the version the page is about.
    // "idle" | "loading" | "ready" | "error" | "unsupported" (the back-end has no DependsOn)
    QString dependsState() const { return m_dependsState; }
    QString dependsError() const { return m_dependsError; }
    // [ { packageId, name, version, arch, installed, summary } ]
    QVariantList depends() const { return m_depends; }

    Q_INVOKABLE void refresh();
    QJsonObject toJson() const;

Q_SIGNALS:
    void packageNameChanged();
    void changed();
    // All queries of a refresh() have finished.
    void settled();

private:
    void reset();
    void loadDetails();
    void simulate();
    void loadDepends();
    void queryDone();

    QString m_name;
    QString m_state = QStringLiteral("idle");
    QString m_error;
    ShadowPk::ResolvedPackage m_resolved;
    QString m_summary, m_description, m_url, m_license, m_group;
    double m_installedSize = -1;
    double m_downloadSize = -1;
    QString m_simulationState = QStringLiteral("idle");
    QString m_simulationError;
    QVariantMap m_changes;
    QString m_trust = QStringLiteral("unknown");
    double m_totalDownloadSize = -1;
    QString m_dependsState = QStringLiteral("idle");
    QString m_dependsError;
    QVariantList m_depends;
    quint64 m_serial = 0;
    int m_pending = 0;
};
