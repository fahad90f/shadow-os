// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

namespace Arsenal {

// What dpkg and APT know about one package on this system.
struct PackageState
{
    // dpkg-query
    bool installed = false;
    QString installedVersion;
    qint64 installedBytes = 0;          // Installed-Size of the installed version
    bool essential = false;
    QString priority;
    // apt-cache show (the candidate version)
    bool available = false;             // a version exists in the configured repositories
    QString candidateVersion;
    qint64 downloadBytes = 0;
    qint64 candidateInstalledBytes = 0;
    QString description;                // long description, paragraphs separated by blank lines
    QString homepage;
};

// What APT would have to download and install for a set of packages.
struct SizeEstimate
{
    QString state = QStringLiteral("none");  // none | pending | ready | error
    int packages = 0;                   // packages APT would install, dependencies included
    qint64 downloadBytes = 0;
    qint64 installedBytes = 0;
    QString error;
};

// Read-only view of the package database, through the tools that own it:
//
//   dpkg-query -W -f ...              installed state and version
//   apt-cache show --no-all-versions  candidate version, sizes, description
//   apt-get install --simulate        the dependency closure for size estimates
//
// Nothing here needs privileges or changes the system.
class PackageDb : public QObject
{
    Q_OBJECT
public:
    explicit PackageDb(QObject *parent = nullptr);

    // Query dpkg and APT for 'packages'; refreshed() follows, always asynchronously.
    void refresh(const QStringList &packages);
    bool ready() const { return m_ready; }
    bool busy() const { return m_pending > 0; }
    // Why installed states are unknown (dpkg-query missing or failing); empty if fine.
    QString dpkgError() const { return m_dpkgError; }
    // Why candidate information is unknown; empty if fine.
    QString aptError() const { return m_aptError; }
    PackageState state(const QString &package) const { return m_states.value(package); }

    // Estimate what installing 'packages' would download and occupy; estimated(key)
    // follows. Estimates run one after the other.
    void estimate(const QString &key, const QStringList &packages);
    SizeEstimate estimateOf(const QString &key) const { return m_estimates.value(key); }
    void clearEstimates();

    static QHash<QString, PackageState> parseDpkgQuery(const QByteArray &output);
    static void parseAptShow(const QByteArray &output, QHash<QString, PackageState> *states);
    // "Inst <name> [old] (<version> ...)" lines of apt-get --simulate → name=version
    static QStringList parseSimulation(const QByteArray &output);

Q_SIGNALS:
    void refreshed();
    void estimated(const QString &key);

private:
    void finishOne();
    void runNextEstimate();
    void finishEstimate(const QString &key, const SizeEstimate &estimate);

    QHash<QString, PackageState> m_states;
    QHash<QString, PackageState> m_incoming;
    int m_pending = 0;
    quint64 m_serial = 0;
    bool m_ready = false;
    QString m_dpkgError;
    QString m_aptError;

    QHash<QString, SizeEstimate> m_estimates;
    QList<QPair<QString, QStringList>> m_queue;
    bool m_estimating = false;
    quint64 m_estimateSerial = 0;
};

} // namespace Arsenal
