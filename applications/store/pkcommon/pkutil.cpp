// SPDX-License-Identifier: GPL-3.0-or-later
#include "pkutil.h"

#include <command.h>

#include <QCoreApplication>
#include <QMetaEnum>
#include <QSysInfo>

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace NexusPk {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("NexusPk", text);
}

// "PackageDownloadFailed" -> "package-download-failed"
QString dashed(const QString &camel)
{
    QString out;
    out.reserve(camel.size() + 8);
    for (qsizetype i = 0; i < camel.size(); ++i) {
        const QChar c = camel.at(i);
        if (c.isUpper() && i > 0)
            out += QLatin1Char('-');
        out += c.toLower();
    }
    return out;
}

template <typename Enum>
QString keyOf(Enum value, QLatin1StringView prefix)
{
    const QMetaEnum meta = QMetaEnum::fromType<Enum>();
    const char *key = meta.valueToKey(static_cast<int>(value));
    if (!key)
        return QStringLiteral("unknown-%1").arg(static_cast<int>(value));
    QString name = QString::fromLatin1(key);
    if (name.startsWith(prefix))
        name.remove(0, prefix.size());
    return dashed(name);
}

} // namespace

QString errorKey(PackageKit::Transaction::Error value)
{
    return keyOf(value, QLatin1StringView("Error"));
}

QString exitKey(PackageKit::Transaction::Exit value)
{
    return keyOf(value, QLatin1StringView("Exit"));
}

QString statusKey(PackageKit::Transaction::Status value)
{
    return keyOf(value, QLatin1StringView("Status"));
}

QString infoKey(PackageKit::Transaction::Info value)
{
    return keyOf(value, QLatin1StringView("Info"));
}

QString restartKey(PackageKit::Transaction::Restart value)
{
    return keyOf(value, QLatin1StringView("Restart"));
}

QString roleKey(PackageKit::Transaction::Role value)
{
    return keyOf(value, QLatin1StringView("Role"));
}

QString groupKey(PackageKit::Transaction::Group value)
{
    return keyOf(value, QLatin1StringView("Group"));
}

QString updateStateKey(PackageKit::Transaction::UpdateState value)
{
    return keyOf(value, QLatin1StringView("UpdateState"));
}

QString sigTypeKey(PackageKit::Transaction::SigType value)
{
    return keyOf(value, QLatin1StringView("SigType"));
}

QString mediaTypeKey(PackageKit::Transaction::MediaType value)
{
    return keyOf(value, QLatin1StringView("MediaType"));
}

QStringList roleKeys(qulonglong roleBitfield)
{
    QStringList out;
    const QMetaEnum meta = QMetaEnum::fromType<PackageKit::Transaction::Role>();
    for (int i = 0; i < meta.keyCount(); ++i) {
        const int value = meta.value(i);
        if (value <= 0 || value >= 64)
            continue;
        if (roleBitfield & (1ULL << value))
            out << roleKey(static_cast<PackageKit::Transaction::Role>(value));
    }
    return out;
}

PackageKit::Transaction::Info infoBase(PackageKit::Transaction::Info raw)
{
    return static_cast<PackageKit::Transaction::Info>(static_cast<uint>(raw) & 0xffffu);
}

PackageKit::Transaction::Info infoSeverity(PackageKit::Transaction::Info raw)
{
    return static_cast<PackageKit::Transaction::Info>((static_cast<uint>(raw) >> 16) & 0xffffu);
}

QString statusText(PackageKit::Transaction::Status value)
{
    using T = PackageKit::Transaction;
    switch (value) {
    case T::StatusUnknown: return tr("Working");
    case T::StatusWait: return tr("Waiting in the queue");
    case T::StatusSetup: return tr("Starting");
    case T::StatusRunning: return tr("Running");
    case T::StatusQuery: return tr("Querying");
    case T::StatusInfo: return tr("Getting information");
    case T::StatusRemove: return tr("Removing packages");
    case T::StatusRefreshCache: return tr("Refreshing software lists");
    case T::StatusDownload: return tr("Downloading packages");
    case T::StatusInstall: return tr("Installing packages");
    case T::StatusUpdate: return tr("Updating packages");
    case T::StatusCleanup: return tr("Cleaning up");
    case T::StatusObsolete: return tr("Removing obsolete packages");
    case T::StatusDepResolve: return tr("Resolving dependencies");
    case T::StatusSigCheck: return tr("Checking signatures");
    case T::StatusTestCommit: return tr("Testing changes");
    case T::StatusCommit: return tr("Applying changes");
    case T::StatusRequest: return tr("Requesting data");
    case T::StatusFinished: return tr("Finished");
    case T::StatusCancel: return tr("Cancelling");
    case T::StatusDownloadRepository: return tr("Downloading repository information");
    case T::StatusDownloadPackagelist: return tr("Downloading package lists");
    case T::StatusDownloadFilelist: return tr("Downloading file lists");
    case T::StatusDownloadChangelog: return tr("Downloading change logs");
    case T::StatusDownloadGroup: return tr("Downloading group information");
    case T::StatusDownloadUpdateinfo: return tr("Downloading update information");
    case T::StatusRepackaging: return tr("Repackaging files");
    case T::StatusLoadingCache: return tr("Loading the package cache");
    case T::StatusScanApplications: return tr("Scanning installed applications");
    case T::StatusGeneratePackageList: return tr("Generating package lists");
    case T::StatusWaitingForLock: return tr("Waiting for another package manager to finish");
    case T::StatusWaitingForAuth: return tr("Waiting for authentication");
    case T::StatusScanProcessList: return tr("Checking running applications");
    case T::StatusCheckExecutableFiles: return tr("Checking applications in use");
    case T::StatusCheckLibraries: return tr("Checking libraries in use");
    case T::StatusCopyFiles: return tr("Copying files");
    case T::StatusRunHook: return tr("Running hooks");
    }
    return statusKey(value);
}

QString errorText(PackageKit::Transaction::Error value)
{
    using T = PackageKit::Transaction;
    switch (value) {
    case T::ErrorUnknown: return tr("Unknown error");
    case T::ErrorOom: return tr("Out of memory");
    case T::ErrorNoNetwork: return tr("No network connection");
    case T::ErrorNotSupported: return tr("Not supported by the package back-end");
    case T::ErrorInternalError: return tr("Internal error");
    case T::ErrorGpgFailure: return tr("Signature verification failed");
    case T::ErrorPackageIdInvalid: return tr("Invalid package identifier");
    case T::ErrorPackageNotInstalled: return tr("The package is not installed");
    case T::ErrorPackageNotFound: return tr("The package was not found");
    case T::ErrorPackageAlreadyInstalled: return tr("The package is already installed");
    case T::ErrorPackageDownloadFailed: return tr("A package could not be downloaded");
    case T::ErrorGroupNotFound: return tr("The group was not found");
    case T::ErrorGroupListInvalid: return tr("The group list is not valid");
    case T::ErrorDepResolutionFailed: return tr("Dependencies could not be resolved");
    case T::ErrorFilterInvalid: return tr("The search filter is not valid");
    case T::ErrorCreateThreadFailed: return tr("A worker thread could not be started");
    case T::ErrorTransactionError: return tr("The transaction failed");
    case T::ErrorTransactionCancelled: return tr("The transaction was cancelled");
    case T::ErrorNoCache: return tr("No package cache is available");
    case T::ErrorRepoNotFound: return tr("The repository was not found");
    case T::ErrorCannotRemoveSystemPackage: return tr("A protected system package cannot be removed");
    case T::ErrorProcessKill: return tr("The package manager process was killed");
    case T::ErrorFailedInitialization: return tr("The package back-end could not be initialised");
    case T::ErrorFailedFinalise: return tr("The package back-end could not finish cleanly");
    case T::ErrorFailedConfigParsing: return tr("The package manager configuration could not be read");
    case T::ErrorCannotCancel: return tr("The task cannot be cancelled");
    case T::ErrorCannotGetLock: return tr("The package database is locked by another program");
    case T::ErrorNoPackagesToUpdate: return tr("There are no packages to update");
    case T::ErrorCannotWriteRepoConfig: return tr("The repository configuration could not be written");
    case T::ErrorLocalInstallFailed: return tr("The local package could not be installed");
    case T::ErrorBadGpgSignature: return tr("The signature is not valid");
    case T::ErrorMissingGpgSignature: return tr("The signature is missing");
    case T::ErrorCannotInstallSourcePackage: return tr("Source packages cannot be installed");
    case T::ErrorRepoConfigurationError: return tr("Repository configuration error");
    case T::ErrorNoLicenseAgreement: return tr("The licence agreement was not accepted");
    case T::ErrorFileConflicts: return tr("Files conflict with another package");
    case T::ErrorPackageConflicts: return tr("Packages conflict with each other");
    case T::ErrorRepoNotAvailable: return tr("A repository is not available");
    case T::ErrorInvalidPackageFile: return tr("The package file is not valid");
    case T::ErrorPackageInstallBlocked: return tr("Installation of the package is blocked");
    case T::ErrorPackageCorrupt: return tr("The package is corrupt");
    case T::ErrorAllPackagesAlreadyInstalled: return tr("All packages are already installed");
    case T::ErrorFileNotFound: return tr("A file was not found");
    case T::ErrorNoMoreMirrorsToTry: return tr("No more mirrors to try");
    case T::ErrorNoDistroUpgradeData: return tr("No distribution upgrade data is available");
    case T::ErrorIncompatibleArchitecture: return tr("The package is not available for this architecture");
    case T::ErrorNoSpaceOnDevice: return tr("Not enough free disk space");
    case T::ErrorMediaChangeRequired: return tr("A media change is required");
    case T::ErrorNotAuthorized: return tr("Not authorised");
    case T::ErrorUpdateNotFound: return tr("The update was not found");
    case T::ErrorCannotInstallRepoUnsigned: return tr("Packages from an unsigned source cannot be installed");
    case T::ErrorCannotUpdateRepoUnsigned: return tr("Packages from an unsigned source cannot be updated");
    case T::ErrorCannotGetFilelist: return tr("The file list is not available");
    case T::ErrorCannotGetRequires: return tr("The reverse dependencies are not available");
    case T::ErrorCannotDisableRepository: return tr("The repository cannot be disabled");
    case T::ErrorRestrictedDownload: return tr("The download is restricted");
    case T::ErrorPackageFailedToConfigure: return tr("A package failed to configure");
    case T::ErrorPackageFailedToBuild: return tr("A package failed to build");
    case T::ErrorPackageFailedToInstall: return tr("A package failed to install");
    case T::ErrorPackageFailedToRemove: return tr("A package could not be removed");
    case T::ErrorUpdateFailedDueToRunningProcess: return tr("A running program prevents the update");
    case T::ErrorPackageDatabaseChanged: return tr("The package database changed during the operation");
    case T::ErrorProvideTypeNotSupported: return tr("The provide type is not supported");
    case T::ErrorInstallRootInvalid: return tr("The install root is not valid");
    case T::ErrorCannotFetchSources: return tr("Software sources could not be downloaded");
    case T::ErrorCancelledPriority: return tr("Cancelled in favour of a more important task");
    case T::ErrorUnfinishedTransaction: return tr("An earlier package operation was interrupted");
    case T::ErrorLockRequired: return tr("A package database lock is required");
    }
    return errorKey(value);
}

QString infoText(PackageKit::Transaction::Info value)
{
    using T = PackageKit::Transaction;
    switch (value) {
    case T::InfoUnknown: return tr("Unknown");
    case T::InfoInstalled: return tr("Installed");
    case T::InfoAvailable: return tr("Available");
    case T::InfoLow: return tr("Low priority");
    case T::InfoEnhancement: return tr("Enhancement");
    case T::InfoNormal: return tr("Update");
    case T::InfoBugfix: return tr("Bug fix");
    case T::InfoImportant: return tr("Important");
    case T::InfoSecurity: return tr("Security");
    case T::InfoBlocked: return tr("Held back");
    case T::InfoDownloading: return tr("Downloading");
    case T::InfoUpdating: return tr("Updating");
    case T::InfoInstalling: return tr("Installing");
    case T::InfoRemoving: return tr("Removing");
    case T::InfoCleanup: return tr("Cleaning up");
    case T::InfoObsoleting: return tr("Obsoleting");
    case T::InfoCollectionInstalled: return tr("Installed collection");
    case T::InfoCollectionAvailable: return tr("Available collection");
    case T::InfoFinished: return tr("Finished");
    case T::InfoReinstalling: return tr("Reinstalling");
    case T::InfoDowngrading: return tr("Downgrading");
    case T::InfoPreparing: return tr("Preparing");
    case T::InfoDecompressing: return tr("Unpacking");
    case T::InfoUntrusted: return tr("Untrusted");
    case T::InfoTrusted: return tr("Trusted");
    case T::InfoUnavailable: return tr("Unavailable");
    case T::InfoCritical: return tr("Critical");
    case T::InfoInstall: return tr("To be installed");
    case T::InfoRemove: return tr("To be removed");
    case T::InfoObsolete: return tr("To be made obsolete");
    case T::InfoDowngrade: return tr("To be downgraded");
    }
    return infoKey(value);
}

QString restartText(PackageKit::Transaction::Restart value)
{
    using T = PackageKit::Transaction;
    switch (value) {
    case T::RestartUnknown: return tr("Unknown");
    case T::RestartNone: return tr("No restart needed");
    case T::RestartApplication: return tr("Restart the application");
    case T::RestartSession: return tr("Log out and back in");
    case T::RestartSystem: return tr("Restart the computer");
    case T::RestartSecuritySession: return tr("Log out and back in (security update)");
    case T::RestartSecuritySystem: return tr("Restart the computer (security update)");
    }
    return restartKey(value);
}

QString roleText(PackageKit::Transaction::Role value)
{
    using T = PackageKit::Transaction;
    switch (value) {
    case T::RoleInstallPackages: return tr("Install packages");
    case T::RoleInstallFiles: return tr("Install package files");
    case T::RoleRemovePackages: return tr("Remove packages");
    case T::RoleUpdatePackages: return tr("Update packages");
    case T::RoleRefreshCache: return tr("Refresh software lists");
    case T::RoleRepairSystem: return tr("Repair package system");
    case T::RoleRepoEnable: return tr("Enable or disable a repository");
    case T::RoleRepoSetData: return tr("Change a repository");
    case T::RoleRepoRemove: return tr("Remove a repository");
    case T::RoleInstallSignature: return tr("Trust a signing key");
    case T::RoleAcceptEula: return tr("Accept a licence agreement");
    case T::RoleUpgradeSystem: return tr("Upgrade the distribution");
    case T::RoleDownloadPackages: return tr("Download packages");
    case T::RoleGetUpdates: return tr("Check for updates");
    default: break;
    }
    return roleKey(value);
}

int restartRank(PackageKit::Transaction::Restart value)
{
    using T = PackageKit::Transaction;
    switch (value) {
    case T::RestartApplication: return 1;
    case T::RestartSession:
    case T::RestartSecuritySession: return 2;
    case T::RestartSystem:
    case T::RestartSecuritySystem: return 3;
    case T::RestartUnknown:
    case T::RestartNone: break;
    }
    return 0;
}

bool restartIsSecurity(PackageKit::Transaction::Restart value)
{
    return value == PackageKit::Transaction::RestartSecuritySession
        || value == PackageKit::Transaction::RestartSecuritySystem;
}

PackageId PackageId::parse(const QString &packageId)
{
    PackageId p;
    p.id = packageId;
    const QStringList parts = packageId.split(QLatin1Char(';'));
    if (parts.size() != 4 || parts.at(0).isEmpty())
        return p;
    p.valid = true;
    p.name = parts.at(0);
    p.version = parts.at(1);
    p.arch = parts.at(2);
    p.data = parts.at(3);

    // APT back-end: "manual:<repo>" / "auto:<repo>" for installed packages,
    // "+manual:" / "+auto:" while an installation is pending, plain "<repo>"
    // for available ones. Older releases used "installed:<repo>" or "installed".
    QString rest = p.data;
    if (rest.startsWith(QLatin1Char('+')))
        rest.remove(0, 1);
    else if (rest.startsWith(QLatin1String("auto:")) || rest.startsWith(QLatin1String("manual:"))
             || rest.startsWith(QLatin1String("installed")))
        p.installed = true;
    if (rest.startsWith(QLatin1String("auto:"))) {
        p.automatic = true;
        rest.remove(0, 5);
    } else if (rest.startsWith(QLatin1String("manual:"))) {
        rest.remove(0, 7);
    } else if (rest.startsWith(QLatin1String("installed:"))) {
        rest.remove(0, 10);
    } else if (rest == QLatin1String("installed")) {
        rest.clear();
    }
    p.repo = rest;
    return p;
}

QJsonObject describeRepo(const QString &repo)
{
    // <origin>-<suite>-<component>; the origin never contains '-', the suite
    // and the component may ("trixie-security", "non-free-firmware").
    static const QStringList components = {
        QStringLiteral("non-free-firmware"), QStringLiteral("non-free"), QStringLiteral("contrib"),
        QStringLiteral("main"), QStringLiteral("restricted"), QStringLiteral("universe"),
        QStringLiteral("multiverse"),
    };
    const qsizetype first = repo.indexOf(QLatin1Char('-'));
    if (first <= 0 || first == repo.size() - 1)
        return {};
    const QString origin = repo.left(first);
    const QString rest = repo.mid(first + 1);
    QString suite, component;
    for (const QString &c : components) {
        if (rest.endsWith(QLatin1Char('-') + c)) {
            component = c;
            suite = rest.left(rest.size() - c.size() - 1);
            break;
        }
    }
    if (component.isEmpty()) {
        const qsizetype last = rest.lastIndexOf(QLatin1Char('-'));
        if (last <= 0)
            return {};
        suite = rest.left(last);
        component = rest.mid(last + 1);
    }
    if (suite.isEmpty() || component.isEmpty())
        return {};
    return QJsonObject{
        {QStringLiteral("origin"), origin},
        {QStringLiteral("suite"), suite},
        {QStringLiteral("component"), component},
    };
}

QString repoText(const QString &repo)
{
    if (repo.isEmpty())
        return tr("Not reported by PackageKit");
    if (repo == QLatin1String("local"))
        return tr("Installed locally (not available from an enabled repository)");
    const QJsonObject d = describeRepo(repo);
    if (d.isEmpty())
        return repo;
    QString origin = d.value(QStringLiteral("origin")).toString();
    if (origin == QLatin1String("debian"))
        origin = QStringLiteral("Debian");
    else if (origin.startsWith(QLatin1String("nexus")))
        origin = QStringLiteral("NEXUS") + origin.mid(5);
    return QStringLiteral("%1 · %2 · %3").arg(origin, d.value(QStringLiteral("suite")).toString(),
                                               d.value(QStringLiteral("component")).toString());
}

QString archRelation(const QString &packageArch)
{
    if (packageArch.isEmpty())
        return QStringLiteral("unknown");
    if (packageArch == QLatin1String("all"))
        return QStringLiteral("all");
    if (packageArch == systemArchitecture())
        return QStringLiteral("native");
    return QStringLiteral("foreign");
}

QString systemArchitecture()
{
    static QString arch;
    if (!arch.isEmpty())
        return arch;
    // dpkg answers from a compiled-in table and its config directory: instant.
    const Nexus::CommandResult r = Nexus::Command::runSync(QStringLiteral("dpkg"),
                                                           {QStringLiteral("--print-architecture")}, 3000);
    if (r.ok())
        arch = r.outText().trimmed();
    if (arch.isEmpty()) {
        const QString cpu = QSysInfo::currentCpuArchitecture();
        if (cpu == QLatin1String("x86_64")) arch = QStringLiteral("amd64");
        else if (cpu == QLatin1String("arm64")) arch = QStringLiteral("arm64");
        else if (cpu == QLatin1String("i386")) arch = QStringLiteral("i386");
        else if (cpu == QLatin1String("arm")) arch = QStringLiteral("armhf");
        else arch = cpu;
    }
    return arch;
}

QStringList foreignArchitectures()
{
    const Nexus::CommandResult r = Nexus::Command::runSync(QStringLiteral("dpkg"),
                                                           {QStringLiteral("--print-foreign-architectures")}, 3000);
    if (!r.ok())
        return {};
    return r.outText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QString userNameForUid(uint uid)
{
    struct passwd pwd {};
    struct passwd *result = nullptr;
    char buffer[4096];
    if (getpwuid_r(static_cast<uid_t>(uid), &pwd, buffer, sizeof(buffer), &result) == 0 && result && result->pw_name)
        return QString::fromLocal8Bit(result->pw_name);
    return QString::number(uid);
}

} // namespace NexusPk
