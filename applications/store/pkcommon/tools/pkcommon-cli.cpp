// SPDX-License-Identifier: GPL-3.0-or-later
//
// nexus-pkcommon-cli — developer and test driver for the PackageKit plumbing
// shared by NEXUS Store and NEXUS Updater. It runs the very same NexusPk::Job
// objects the applications use, from a terminal, so the transaction paths
// (simulate, confirm, untrusted, run, cancel, restart, errors) can be
// exercised over SSH or a serial console in the integration-test VM.
//
// Not installed; built only with -DNEXUS_PKCOMMON_TOOLS=ON.
//
//   nexus-pkcommon-cli resolve <name>...
//   nexus-pkcommon-cli install <name>...     [--yes] [--allow-untrusted] [--cancel-after <s>]
//   nexus-pkcommon-cli remove  <name>...     [--yes]
//   nexus-pkcommon-cli update  [--security]  [--yes] [--allow-untrusted]
//   nexus-pkcommon-cli refresh | repair [--yes] | repos | repo-enable <id> on|off
//
// Questions are answered "no" unless the matching option is given; a job
// that asks therefore ends "cancelled" by default, and nothing is changed.
// Exit code: 0 success, 1 failed, 2 cancelled, 3 usage / PackageKit unreachable.
#include <pkjob.h>
#include <pkquery.h>
#include <pkservice.h>
#include <pkutil.h>
#include <updatechecker.h>

#include <Daemon>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <memory>

using namespace NexusPk;
using T = PackageKit::Transaction;

namespace {

void say(const QString &text)
{
    std::fputs(qPrintable(text + QLatin1Char('\n')), stdout);
    std::fflush(stdout);
}

void runJob(Job *job, bool yes, bool allowUntrusted, int cancelAfter)
{
    auto lastLine = std::make_shared<qsizetype>(0);
    QObject::connect(job, &Job::logChanged, job, [job, lastLine] {
        const QStringList lines = job->logText().split(QLatin1Char('\n'));
        for (; *lastLine < lines.size(); ++*lastLine)
            say(lines.at(*lastLine));
    });
    auto lastPercent = std::make_shared<int>(-2);
    QObject::connect(job, &Job::progressChanged, job, [job, lastPercent] {
        if (job->percentage() != *lastPercent) {
            *lastPercent = job->percentage();
            say(QStringLiteral("    … %1 %  %2  cancellable=%3  remaining=%4 B")
                    .arg(job->percentage()).arg(job->statusText())
                    .arg(job->cancellable() ? QStringLiteral("yes") : QStringLiteral("no"))
                    .arg(static_cast<qulonglong>(job->downloadSizeRemaining())));
        }
    });
    QObject::connect(job, &Job::promptChanged, job, [job, yes, allowUntrusted] {
        const QString kind = job->promptKind();
        if (kind.isEmpty())
            return;
        say(QStringLiteral("QUESTION %1: %2").arg(kind,
            QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(job->prompt())).toJson(QJsonDocument::Compact))));
        const bool accept = kind == QLatin1String("untrusted") ? allowUntrusted : yes;
        say(QStringLiteral("ANSWER %1: %2").arg(kind, accept ? QStringLiteral("yes") : QStringLiteral("no")));
        // Answer from the event loop, like a dialog would.
        QTimer::singleShot(0, job, [job, accept] { job->answer(accept); });
    });
    QObject::connect(job, &Job::finished, job, [job](bool) {
        say(QString::fromUtf8(QJsonDocument(job->toJson()).toJson(QJsonDocument::Indented)));
        const QString result = job->result();
        QCoreApplication::exit(result == QLatin1String("success") ? 0 : result == QLatin1String("cancelled") ? 2 : 1);
    });
    if (cancelAfter > 0) {
        QTimer::singleShot(cancelAfter * 1000, job, [job] {
            say(QStringLiteral("CANCEL requested (cancellable=%1)").arg(job->cancellable() ? QStringLiteral("yes") : QStringLiteral("no")));
            job->cancel();
        });
    }
    job->start();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = QCoreApplication::arguments().mid(1);
    auto flag = [&args](const QString &name) { return args.removeAll(name) > 0; };
    const bool yes = flag(QStringLiteral("--yes"));
    const bool allowUntrusted = flag(QStringLiteral("--allow-untrusted"));
    const bool securityOnly = flag(QStringLiteral("--security"));
    const bool nonInteractive = flag(QStringLiteral("--non-interactive"));
    int cancelAfter = 0;
    const qsizetype cancelIndex = args.indexOf(QStringLiteral("--cancel-after"));
    if (cancelIndex >= 0 && cancelIndex + 1 < args.size()) {
        cancelAfter = args.at(cancelIndex + 1).toInt();
        args.remove(cancelIndex, 2);
    }
    if (args.isEmpty()) {
        std::fputs("usage: nexus-pkcommon-cli resolve|install|remove|update|refresh|repair|repos|repo-enable ...\n", stderr);
        return 3;
    }
    const QString command = args.takeFirst();

    Service *service = Service::instance();
    service->setInteractive(!nonInteractive);
    QObject::connect(service, &Service::probed, &app, [&] {
        if (!service->available()) {
            say(QStringLiteral("PackageKit service is not reachable: %1").arg(service->error()));
            QCoreApplication::exit(3);
            return;
        }
        say(QStringLiteral("PackageKit %1, back-end %2").arg(service->daemonVersion(), service->backendName()));

        if (command == QLatin1String("resolve")) {
            resolvePackages(&app, args, [](const QHash<QString, ResolvedPackage> &found, const QueryResult &result) {
                QJsonArray out;
                for (const ResolvedPackage &r : found)
                    out.append(r.toJson());
                say(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
                if (!result.ok)
                    say(result.errorMessage());
                QCoreApplication::exit(result.ok ? 0 : 1);
            });
        } else if (command == QLatin1String("install") || command == QLatin1String("remove")) {
            const bool install = command == QLatin1String("install");
            resolvePackages(&app, args, [&, install](const QHash<QString, ResolvedPackage> &found, const QueryResult &result) {
                QStringList ids;
                for (const QString &name : std::as_const(args)) {
                    const ResolvedPackage r = found.value(name);
                    if (install && r.hasCandidate && !r.foreignOnly) ids << r.candidate.id;
                    else if (!install && r.installed) ids << r.installedEntry.id;
                    else say(QStringLiteral("%1: %2").arg(name, !r.found ? QStringLiteral("unknown to PackageKit")
                             : install ? QStringLiteral("no installable candidate (already installed, or not available for this architecture)")
                                       : QStringLiteral("not installed")));
                }
                if (ids.isEmpty()) {
                    if (!result.ok)
                        say(result.errorMessage());
                    QCoreApplication::exit(1);
                    return;
                }
                say(QStringLiteral("package IDs: %1").arg(ids.join(QLatin1Char(' '))));
                Job *job = install ? Jobs::install(ids, QStringLiteral("Installing %1").arg(args.join(QLatin1Char(' '))), &app)
                                   : Jobs::remove(ids, QStringLiteral("Removing %1").arg(args.join(QLatin1Char(' '))), &app);
                runJob(job, yes, allowUntrusted, cancelAfter);
            });
        } else if (command == QLatin1String("update")) {
            auto *checker = new UpdateChecker(&app);
            QObject::connect(checker, &UpdateChecker::checked, &app, [&, checker] {
                say(QString::fromUtf8(QJsonDocument(checker->toJson()).toJson(QJsonDocument::Indented)));
                const QStringList ids = securityOnly ? checker->idsInGroup(QStringLiteral("security")) : checker->installableIds();
                if (ids.isEmpty()) {
                    say(QStringLiteral("nothing to update"));
                    QCoreApplication::exit(checker->state() == QLatin1String("ready") ? 0 : 1);
                    return;
                }
                Job *job = Jobs::update(ids, QStringLiteral("Installing %1 updates").arg(ids.size()), &app);
                runJob(job, yes, allowUntrusted, cancelAfter);
            });
            checker->check(false);
        } else if (command == QLatin1String("refresh")) {
            Job *job = Jobs::refresh(true, &app);
            runJob(job, yes, allowUntrusted, cancelAfter);
        } else if (command == QLatin1String("repair")) {
            Job *job = Jobs::repair(&app);
            runJob(job, yes, allowUntrusted, cancelAfter);
        } else if (command == QLatin1String("repos")) {
            Query::run(&app, PackageKit::Daemon::getRepoList(T::FilterNone), [](const QueryResult &result) {
                for (const RepoEntry &r : result.repos)
                    say(QStringLiteral("%1  [%2]  %3").arg(r.enabled ? QStringLiteral("on ") : QStringLiteral("off"), r.id, r.description));
                if (!result.ok)
                    say(result.errorMessage());
                QCoreApplication::exit(result.ok ? 0 : 1);
            });
        } else if (command == QLatin1String("repo-enable") && args.size() == 2) {
            Job *job = Jobs::repoEnable(args.at(0), args.at(1) == QLatin1String("on"),
                                        QStringLiteral("Switching repository %1").arg(args.at(1)), &app);
            runJob(job, yes, allowUntrusted, cancelAfter);
        } else {
            std::fputs("unknown command\n", stderr);
            QCoreApplication::exit(3);
        }
    });
    service->probe();
    return QCoreApplication::exec();
}
