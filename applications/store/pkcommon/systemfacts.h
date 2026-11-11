// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// Facts about the package system that do not need PackageKit: they are read
// from files APT and dpkg maintain, or from dpkg's own query tools. They work
// in a chroot, in a rescue shell and when the daemon is down — which is when
// some of them matter most.
//
// Every function here blocks on file or process I/O. Call them from a worker
// thread (QtConcurrent::run) in a GUI, or directly in --dump mode.
namespace NexusPk {

// ---- APT history --------------------------------------------------------------
// /var/log/apt/history.log and its rotations (history.log.1, history.log.2.gz …):
// one entry per apt/PackageKit run that changed packages.
//
//   { directory, files: [{ file, entries, error }], total, truncated,
//     entries: [ { start, end, startText, commandLine, requestedBy, viaPackageKit,
//                  error, summary, packageCount, counts: { install: n, … },
//                  actions: [ { action: "install" | "upgrade" | "downgrade" | "reinstall" |
//                               "remove" | "purge",
//                               packages: [ { name, arch, version, automatic } |
//                                           { name, arch, from, to } ] } ],
//                  source } ] }   — newest first
QJsonObject readAptHistory(int limit = 0, const QString &directory = QStringLiteral("/var/log/apt"));
// The entries of one history file's text, oldest first (exposed for tests and --dump).
QJsonArray parseAptHistory(const QByteArray &text, const QString &sourceName);

// ---- restart required -----------------------------------------------------------
//   { required, flagFile, flagFilePresent, message, packages: [..],
//     runningKernel, installedKernels: [..], newestKernel, newerKernelInstalled }
// "required" is Debian's convention: /run/reboot-required exists (package
// maintainer scripts create it; /run/reboot-required.pkgs names them).
QJsonObject readRebootState();

// ---- dpkg health -----------------------------------------------------------------
//   { dpkgAvailable, healthy, audit: { exitCode, output, error },
//     interruptedUpdates: [..], brokenPackages: [ { package, architecture, version,
//     status, state, meaning } ], checkedAt }
// healthy == false means a package operation was interrupted or left packages
// half-installed: `dpkg --audit` reports something, /var/lib/dpkg/updates is
// not empty, or dpkg-query lists packages in an intermediate state.
QJsonObject checkDpkgHealth();

// ---- APT sources -------------------------------------------------------------------
//   { sources: [ { file, format, types: [..], uri, suite, components: [..], enabled,
//                  signedBy, trustedOption, allowInsecure, metadataFile, metadata,
//                  verification, verificationText, origin, label, codename, date, validUntil,
//                  category, categoryText } ], errors: [..] }
// verification: "signed" (APT stored a signed Release for it), "unsigned",
// "signature-check-disabled" (trusted=yes), "not-downloaded" or "disabled".
// category: "debian" (the Debian base), "nexus" or "third-party".
QJsonObject readAptSources(const QString &root = QString());

// Classify a repository by what PackageKit reports about it (ID and description).
QString repoCategory(const QString &repoIdOrUri, const QString &origin = QString());
QString repoCategoryText(const QString &category);

// dpkg's version ordering (digits numerically, '~' before everything).
int compareVersions(const QString &a, const QString &b);

} // namespace NexusPk
