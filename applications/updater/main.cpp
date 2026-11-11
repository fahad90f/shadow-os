// SPDX-License-Identifier: GPL-3.0-or-later
#include "updater.h"

#include <shadowapp.h>

#include <Daemon>
#include <pkhistory.h>
#include <pkquery.h>
#include <pkservice.h>
#include <pkutil.h>
#include <systemfacts.h>

#include <QCommandLineOption>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdio>

using ShadowPk::Service;

namespace {

// Wait until PackageKit's reachability is known.
void probeService(Shadow::App &app)
{
    Service *service = Service::instance();
    service->probe();
    app.waitFor(service, SIGNAL(probed()), 35000);
}

QJsonObject statusJson()
{
    return QJsonObject{
        {QStringLiteral("packagekit"), Service::instance()->toJson()},
        {QStringLiteral("architecture"), ShadowPk::systemArchitecture()},
        {QStringLiteral("foreignArchitectures"), QJsonArray::fromStringList(ShadowPk::foreignArchitectures())},
        {QStringLiteral("settings"), Updater::settingsJson()},
        {QStringLiteral("restartRequired"), ShadowPk::readRebootState().value(QStringLiteral("required"))},
    };
}

// `shadow-updater --check`: one headless check for the session (the shell runs
// it periodically when "automatic checking" is on and turns the result into a
// notification). Prints one JSON object; the exit code carries the summary:
//   0  no updates      10  updates available      11  security updates available
//   1  the check failed (PackageKit unreachable, lists could not be read, or
//      refreshing failed and no update is known)
int runCheck(Shadow::App &app)
{
    Service *service = Service::instance();
    // Never pop up an authentication dialog from a background check.
    service->setInteractive(false);
    probeService(app);

    QJsonObject out;
    if (!service->available()) {
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("error"), service->error());
        std::fputs(QJsonDocument(out).toJson(QJsonDocument::Indented).constData(), stdout);
        std::fprintf(stderr, "shadow-updater: PackageKit service is not reachable: %s\n", qPrintable(service->error()));
        return 1;
    }

    ShadowPk::UpdateChecker checker;
    checker.check(true);
    const bool finished = app.waitFor(&checker, SIGNAL(checked()), 15 * 60 * 1000);
    if (!finished) {
        checker.cancel();
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("error"), QStringLiteral("timed out waiting for PackageKit"));
        std::fputs(QJsonDocument(out).toJson(QJsonDocument::Indented).constData(), stdout);
        std::fprintf(stderr, "shadow-updater: timed out waiting for PackageKit\n");
        return 1;
    }
    Updater::storeCheckResult(checker);

    // So that the caller can also announce a pending restart.
    out.insert(QStringLiteral("restartRequired"), ShadowPk::readRebootState().value(QStringLiteral("required")));
    const bool listed = checker.state() == QLatin1String("ready");
    const int updates = checker.count();
    const int security = checker.securityCount();
    // "No updates" may only be claimed after a successful refresh.
    const bool ok = listed && (checker.refreshed() || updates > 0);
    out.insert(QStringLiteral("ok"), ok);
    out.insert(QStringLiteral("refreshed"), checker.refreshed());
    if (listed) {
        out.insert(QStringLiteral("updates"), updates);
        out.insert(QStringLiteral("securityUpdates"), security);
        out.insert(QStringLiteral("heldBack"), checker.blockedCount());
        if (checker.sizesKnown())
            out.insert(QStringLiteral("downloadSize"), static_cast<qint64>(checker.totalSize()));
    }
    if (!checker.error().isEmpty())
        out.insert(QStringLiteral("error"), checker.error());
    if (!checker.refreshError().isEmpty())
        out.insert(QStringLiteral("refreshError"), checker.refreshError());
    std::fputs(QJsonDocument(out).toJson(QJsonDocument::Indented).constData(), stdout);
    std::fflush(stdout);

    if (!checker.refreshError().isEmpty())
        std::fprintf(stderr, "shadow-updater: refreshing the software lists failed: %s\n", qPrintable(checker.refreshError()));
    if (!listed)
        std::fprintf(stderr, "shadow-updater: the update list could not be read: %s\n", qPrintable(checker.error()));
    if (!ok)
        return 1;
    return security > 0 ? 11 : updates > 0 ? 10 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    // The headless check must work without a display server (systemd user
    // timer, shell start-up), exactly like --dump.
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--check") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            break;
        }
    }

    Shadow::AppInfo info;
    info.id = QStringLiteral("org.shadow.updater");
    info.displayName = QStringLiteral("SHADOW Updater");
    info.iconName = QStringLiteral("org.shadow.updater");
    info.description = QStringLiteral("Install system updates, review update history and repair the package system.");
    info.singleInstance = true;

    Shadow::App app(argc, argv, info);
    const QCommandLineOption checkOption(
        QStringLiteral("check"),
        QStringLiteral("Check for updates without opening a window and print the result as JSON. "
                       "Exit code: 0 no updates, 10 updates available, 11 security updates available, 1 check failed."));
    app.parser().addOption(checkOption);
    app.parser().addPositionalArgument(QStringLiteral("page"),
        QStringLiteral("Page to open: updates, history, log, recovery, sources or settings."), QStringLiteral("[page]"));
    app.setDumpTopics({QStringLiteral("status"), QStringLiteral("updates"), QStringLiteral("history"),
                       QStringLiteral("reboot"), QStringLiteral("health"), QStringLiteral("sources"),
                       QStringLiteral("settings"), QStringLiteral("repos"),
                       QStringLiteral("update-details")});
    if (!app.processArguments())
        return 0;

    if (app.dumpTopic().isEmpty() && app.parser().isSet(checkOption))
        return runCheck(app);

    if (!app.dumpTopic().isEmpty()) {
        const QString topic = app.dumpTopic();
        if (topic == QLatin1String("reboot"))
            return app.finishDump(ShadowPk::readRebootState());
        if (topic == QLatin1String("health"))
            return app.finishDump(ShadowPk::checkDpkgHealth());
        if (topic == QLatin1String("sources"))
            return app.finishDump(ShadowPk::readAptSources());
        if (topic == QLatin1String("settings"))
            return app.finishDump(Updater::settingsJson());

        if (topic == QLatin1String("status")) {
            probeService(app);
            return app.finishDump(statusJson());
        }
        if (topic == QLatin1String("updates")) {
            // Reads the lists on disk; it does not refresh them (no network, no polkit).
            probeService(app);
            ShadowPk::UpdateChecker checker;
            checker.check(false);
            app.waitFor(&checker, SIGNAL(checked()), 5 * 60 * 1000);
            QJsonObject out = checker.toJson();
            out.insert(QStringLiteral("packagekitReachable"), Service::instance()->available());
            return app.finishDump(out);
        }
        if (topic == QLatin1String("update-details")) {
            // What the rows of the Updates page show when opened: PackageKit's
            // GetUpdateDetail (changelog, CVE and bug references, restart need)
            // for each available update. Read-only; the lists are not refreshed.
            probeService(app);
            ShadowPk::UpdateChecker checker;
            checker.check(false);
            app.waitFor(&checker, SIGNAL(checked()), 5 * 60 * 1000);
            QJsonObject out{{QStringLiteral("packagekitReachable"), Service::instance()->available()},
                            {QStringLiteral("state"), checker.state()}};
            if (!checker.error().isEmpty())
                out.insert(QStringLiteral("error"), checker.error());
            const int limit = 60;
            QStringList ids;
            for (const ShadowPk::PackageEntry &e : checker.model()->entries()) {
                if (ids.size() < limit)
                    ids << e.id;
            }
            out.insert(QStringLiteral("truncated"), checker.model()->entries().size() > limit);
            const auto pending = [&checker, &ids] {
                for (const QString &id : ids) {
                    const QString state = checker.detail(id).value(QStringLiteral("state")).toString();
                    if (state == QLatin1String("loading") || state == QLatin1String("none"))
                        return true;
                }
                return false;
            };
            for (const QString &id : std::as_const(ids))
                checker.loadDetail(id);
            QElapsedTimer timer;
            timer.start();
            while (pending() && timer.elapsed() < 5 * 60 * 1000)
                app.waitFor(&checker, SIGNAL(detailChanged(QString)), 10000);
            QJsonArray details;
            for (const QString &id : std::as_const(ids)) {
                QJsonObject d = QJsonObject::fromVariantMap(checker.detail(id));
                d.insert(QStringLiteral("packageId"), id);
                details.append(d);
            }
            out.insert(QStringLiteral("details"), details);
            return app.finishDump(out);
        }
        if (topic == QLatin1String("repos")) {
            // The repositories as PackageKit lists them (GetRepoList, read-only),
            // next to what the APT sources files say.
            probeService(app);
            QJsonObject pk{{QStringLiteral("reachable"), Service::instance()->available()}};
            if (!Service::instance()->available()) {
                pk.insert(QStringLiteral("error"), Service::instance()->error());
            } else {
                QEventLoop loop;
                QTimer timeout;
                timeout.setSingleShot(true);
                QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
                bool done = false;
                ShadowPk::Query::run(&loop, PackageKit::Daemon::getRepoList(PackageKit::Transaction::FilterNone),
                                    [&](const ShadowPk::QueryResult &result) {
                    QJsonArray repos;
                    for (const ShadowPk::RepoEntry &r : result.repos) {
                        repos.append(QJsonObject{{QStringLiteral("repoId"), r.id},
                                                 {QStringLiteral("description"), r.description},
                                                 {QStringLiteral("enabled"), r.enabled}});
                    }
                    pk.insert(QStringLiteral("repositories"), repos);
                    if (!result.ok)
                        pk.insert(QStringLiteral("error"), result.errorMessage());
                    done = true;
                    loop.quit();
                });
                timeout.start(60000);
                if (!done)
                    loop.exec();
                if (!done)
                    pk.insert(QStringLiteral("error"), QStringLiteral("timed out waiting for PackageKit"));
            }
            return app.finishDump(QJsonObject{
                {QStringLiteral("packagekit"), pk},
                {QStringLiteral("aptSources"), ShadowPk::readAptSources().value(QStringLiteral("sources"))},
            });
        }
        if (topic == QLatin1String("history")) {
            probeService(app);
            QJsonObject out{{QStringLiteral("apt"), ShadowPk::readAptHistory(0)}};
            QJsonObject pk{{QStringLiteral("reachable"), Service::instance()->available()}};
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            bool done = false;
            ShadowPk::fetchOldTransactions(&loop, 200, [&](const QVariantList &transactions, const QString &error) {
                pk.insert(QStringLiteral("transactions"), QJsonArray::fromVariantList(transactions));
                if (!error.isEmpty())
                    pk.insert(QStringLiteral("error"), error);
                done = true;
                loop.quit();
            });
            timeout.start(60000);
            if (!done)
                loop.exec();
            if (!done)
                pk.insert(QStringLiteral("error"), QStringLiteral("timed out waiting for PackageKit"));
            out.insert(QStringLiteral("packagekit"), pk);
            // The list as the History page shows it.
            out.insert(QStringLiteral("entries"), QJsonArray::fromVariantList(ShadowPk::mergeHistory(
                out.value(QStringLiteral("apt")).toObject().value(QStringLiteral("entries")).toArray().toVariantList(),
                pk.value(QStringLiteral("transactions")).toArray().toVariantList())));
            return app.finishDump(out);
        }
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }

    return app.run(QStringLiteral("org.shadow.updater"));
}
