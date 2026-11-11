// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <Transaction>

// Helpers shared by NEXUS Store and NEXUS Updater for talking to PackageKit.
//
// PackageKit identifies everything by enumerations and by "package IDs" of
// the form  name;version;arch;data . The functions here turn both into the
// stable machine-readable keys used in --dump output (PackageKit's own dashed
// spelling, e.g. "no-network") and into text for the UI.
namespace NexusPk {

// ---- enumerations -> PackageKit's dashed keys ------------------------------
QString errorKey(PackageKit::Transaction::Error value);        // "package-download-failed"
QString exitKey(PackageKit::Transaction::Exit value);          // "success", "need-untrusted"
QString statusKey(PackageKit::Transaction::Status value);      // "download", "install"
QString infoKey(PackageKit::Transaction::Info value);          // "installed", "security"
QString restartKey(PackageKit::Transaction::Restart value);    // "none", "system", "security-session"
QString roleKey(PackageKit::Transaction::Role value);          // "install-packages"
QString groupKey(PackageKit::Transaction::Group value);        // "admin-tools"
QString updateStateKey(PackageKit::Transaction::UpdateState value);
QString sigTypeKey(PackageKit::Transaction::SigType value);
QString mediaTypeKey(PackageKit::Transaction::MediaType value);
QStringList roleKeys(qulonglong roleBitfield);

// PackageKit (1.2.6 and later) packs two values into the "info" number of a
// package: the state in the lower 16 bits and, for updates, the severity in
// the upper 16 bits (0x00080008 = "security" update of severity "security").
// PackageKit-Qt hands the raw number through, so it is split here.
PackageKit::Transaction::Info infoBase(PackageKit::Transaction::Info raw);
PackageKit::Transaction::Info infoSeverity(PackageKit::Transaction::Info raw);     // InfoUnknown if none

// ---- enumerations -> text for people ----------------------------------------
QString statusText(PackageKit::Transaction::Status value);
QString errorText(PackageKit::Transaction::Error value);
QString infoText(PackageKit::Transaction::Info value);
QString restartText(PackageKit::Transaction::Restart value);
QString roleText(PackageKit::Transaction::Role value);

// Restart levels ordered by how disruptive they are (0 = none … 3 = system).
int restartRank(PackageKit::Transaction::Restart value);
bool restartIsSecurity(PackageKit::Transaction::Restart value);

// ---- package IDs ---------------------------------------------------------------
struct PackageId
{
    QString id;
    QString name;
    QString version;
    QString arch;
    QString data;           // backend specific; APT: "[auto:|manual:]<origin>-<suite>-<component>" or "local"

    bool valid = false;
    bool installed = false; // the ID describes the installed version
    bool automatic = false; // installed as a dependency (APT "auto:")
    QString repo;           // data without the installed/auto/manual prefix

    static PackageId parse(const QString &packageId);
};

// "debian-trixie-main" -> { origin: "debian", suite: "trixie", component: "main" };
// empty object when the string does not have that shape ("local", "").
QJsonObject describeRepo(const QString &repo);
// Text for the UI: "Debian · trixie · main", "Installed locally (not from a repository)".
QString repoText(const QString &repo);

// How a package architecture relates to this system: "native", "all", "foreign" or "unknown".
QString archRelation(const QString &packageArch);

// ---- this system -----------------------------------------------------------------
// Debian architecture name of the system ("amd64", "arm64") as dpkg reports it;
// falls back to the CPU architecture Qt was built for if dpkg is missing.
QString systemArchitecture();
QStringList foreignArchitectures();

// Name of the account behind a numeric user id ("root", "alice"); the number as text if unknown.
QString userNameForUid(uint uid);

} // namespace NexusPk
