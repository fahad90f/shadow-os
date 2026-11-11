// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "manifest.h"
#include "packagedb.h"
#include "pkjob.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace Arsenal {

// Everything the Arsenal manager shows and does, for QML (context property
// "arsenal") and for --dump.
//
// Data sources:
//   manifests      <profiles-dir>/*.json (package nexus-arsenal-data)
//   installed?     dpkg-query, candidate versions and sizes: apt-cache (PackageDb)
//   size of a profile   apt-get install --simulate (PackageDb::estimate)
//   changes        PackageKit only (PkJob); never apt or dpkg directly
//   acknowledgement     ~/.config/nexus/arsenal.conf
class Controller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString profilesDir READ profilesDir CONSTANT)
    Q_PROPERTY(QString architecture READ architecture CONSTANT)
    Q_PROPERTY(QString loadError READ loadError CONSTANT)
    Q_PROPERTY(QStringList loadWarnings READ loadWarnings CONSTANT)
    // Incremented whenever anything the query methods return may have changed.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(bool stateReady READ stateReady NOTIFY revisionChanged)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY revisionChanged)
    Q_PROPERTY(QString dpkgError READ dpkgError NOTIFY revisionChanged)
    Q_PROPERTY(QString aptError READ aptError NOTIFY revisionChanged)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY revisionChanged)
    Q_PROPERTY(QVariantList categories READ categories CONSTANT)
    Q_PROPERTY(int toolTotal READ toolTotal NOTIFY revisionChanged)
    Q_PROPERTY(int installedTotal READ installedTotal NOTIFY revisionChanged)
    Q_PROPERTY(bool acknowledged READ acknowledged NOTIFY acknowledgedChanged)
    Q_PROPERTY(QString acknowledgedAt READ acknowledgedAt NOTIFY acknowledgedChanged)
    Q_PROPERTY(bool terminalAvailable READ terminalAvailable CONSTANT)
    Q_PROPERTY(QString initialPage READ initialPage CONSTANT)
    Q_PROPERTY(QObject *packageKit READ packageKitObject CONSTANT)
    Q_PROPERTY(QObject *job READ jobObject CONSTANT)

public:
    explicit Controller(const QString &profilesDir, QObject *parent = nullptr);

    QString profilesDir() const { return m_manifests.directory(); }
    QString architecture() const { return m_arch; }
    QString loadError() const { return m_manifests.error(); }
    QStringList loadWarnings() const { return m_manifests.warnings(); }
    int revision() const { return m_revision; }
    bool stateReady() const { return m_db.ready(); }
    bool refreshing() const { return m_db.busy(); }
    QString dpkgError() const { return m_db.dpkgError(); }
    QString aptError() const { return m_db.aptError(); }
    QVariantList profiles() const;
    QVariantList categories() const;
    int toolTotal() const;
    int installedTotal() const;
    bool acknowledged() const;
    QString acknowledgedAt() const;
    bool terminalAvailable() const;
    QString initialPage() const { return m_initialPage; }
    void setInitialPage(const QString &page) { m_initialPage = page; }
    // Size estimates run apt-get --simulate per profile; dumps that do not show them switch them off.
    void setEstimatesEnabled(bool enabled) { m_estimatesEnabled = enabled; }
    // The value of --page in the arguments of a second launch ("" if none).
    Q_INVOKABLE QString pageFromArguments(const QStringList &arguments) const;
    PkService *packageKit() { return &m_service; }
    PkJob *job() { return &m_job; }
    QObject *packageKitObject() { return &m_service; }
    QObject *jobObject() { return &m_job; }
    PackageDb *db() { return &m_db; }
    bool estimatesPending() const;

    // ---- queries -----------------------------------------------------------------
    Q_INVOKABLE QVariantMap profile(const QString &id) const;
    // Tools of a profile grouped by category:
    //   [{ id, name, icon, description, tools: [tool...] }]
    // stateFilter: "all" | "installed" | "missing"; interfaceFilter: "all" | "cli" | "gui"
    Q_INVOKABLE QVariantList toolGroups(const QString &profileId, const QString &query,
                                        const QString &stateFilter, const QString &interfaceFilter) const;
    // Every tool of every profile, each once, filtered the same way (+ category id or "").
    Q_INVOKABLE QVariantList searchTools(const QString &query, const QString &stateFilter,
                                         const QString &interfaceFilter, const QString &category) const;
    Q_INVOKABLE QVariantMap tool(const QString &package) const;
    // External (not installable) tools of one profile, or of all profiles for "".
    Q_INVOKABLE QVariantList externalTools(const QString &profileId) const;
    // The exact apt command that does what the button would do.
    //   kind: install-profile | install-profile-tools | remove-profile | install-tool | remove-tool
    Q_INVOKABLE QString aptCommand(const QString &kind, const QString &target) const;
    // Human-readable command line "Show usage in terminal" runs.
    Q_INVOKABLE QString terminalCommand(const QString &package) const;

    // ---- actions -----------------------------------------------------------------
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void acknowledge();
    Q_INVOKABLE bool installProfile(const QString &id);
    Q_INVOKABLE bool installProfileTools(const QString &id);
    Q_INVOKABLE bool removeProfile(const QString &id);
    Q_INVOKABLE bool installTool(const QString &package);
    Q_INVOKABLE bool removeTool(const QString &package);
    // Start the tool's graphical interface from its desktop entry. Returns an
    // error message, or an empty string when the program was started.
    Q_INVOKABLE QString launch(const QString &package);
    // Open NEXUS Terminal showing the tool's usage. Same return convention.
    Q_INVOKABLE QString openInTerminal(const QString &package);
    Q_INVOKABLE void copyText(const QString &text);
    Q_INVOKABLE bool openUrl(const QString &url);
    // Path of an installed Arsenal document (legal-use.md, ...), or "" if it is not installed.
    Q_INVOKABLE QString documentPath(const QString &name) const;
    Q_INVOKABLE bool openDocument(const QString &name);

    // ---- diagnostics ---------------------------------------------------------------
    QJsonArray dumpProfiles() const;
    QJsonArray dumpTools() const;
    QJsonObject dumpState() const;

    // Split the Exec value of a desktop entry into an argument vector (field
    // codes removed). Empty list: the value is malformed.
    static QStringList parseExec(const QString &exec);

Q_SIGNALS:
    void revisionChanged();
    void acknowledgedChanged();
    void estimatesFinished();

private:
    QVariantMap toolMap(const Tool &tool) const;
    QVariantMap profileMap(const Profile &profile) const;
    QString toolState(const Tool &tool, const PackageState &state) const;
    QString removeBlockedReason(const Tool &tool, const PackageState &state) const;
    bool matches(const Tool &tool, const QVariantMap &map, const QStringList &terms,
                 const QString &stateFilter, const QString &interfaceFilter) const;
    QStringList missingTools(const QString &profileId) const;
    QString resolveIcon(const QString &name) const;
    QString desktopFile(const QString &desktopId) const;
    QString executable(const QString &command) const;
    void requestEstimates();
    void bump();

    Manifests m_manifests;
    PackageDb m_db;
    PkService m_service;
    PkJob m_job;
    QString m_arch;
    QString m_initialPage;
    bool m_estimatesEnabled = true;
    int m_revision = 0;
};

} // namespace Arsenal
