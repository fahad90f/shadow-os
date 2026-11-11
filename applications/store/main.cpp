// SPDX-License-Identifier: GPL-3.0-or-later
#include "packageinfo.h"
#include "store.h"

#include <shadowapp.h>
#include <pkservice.h>

#include <QCommandLineOption>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>

#include <functional>
#include <vector>

using ShadowPk::Service;

namespace {

// Spin the event loop until 'condition' holds; 'signal' of 'sender' is what
// may change it. Returns false on timeout.
bool waitUntil(Shadow::App &app, QObject *sender, const char *signal, const std::function<bool()> &condition,
               int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        const int remaining = timeoutMs - int(timer.elapsed());
        if (remaining <= 0 || !app.waitFor(sender, signal, remaining))
            return condition();
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    // "--search <text>" travels as the positional argument "search:<text>":
    // a second launch hands only its positional arguments to the instance
    // that is already running, and the search must arrive there too.
    std::vector<QByteArray> storage;
    storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (i > 0 && arg == "--search" && i + 1 < argc)
            storage.push_back(QByteArray("search:") + argv[++i]);
        else if (i > 0 && arg.startsWith("--search="))
            storage.push_back(QByteArray("search:") + arg.mid(9));
        else if (i > 0 && arg == "--page" && i + 1 < argc)
            storage.push_back(QByteArray("page:") + argv[++i]);
        else if (i > 0 && arg.startsWith("--page="))
            storage.push_back(QByteArray("page:") + arg.mid(7));
        else
            storage.push_back(arg);
    }
    std::vector<char *> arguments;
    for (QByteArray &a : storage)
        arguments.push_back(a.data());
    int argumentCount = int(arguments.size());

    Shadow::AppInfo info;
    info.id = QStringLiteral("org.shadow.store");
    info.displayName = QStringLiteral("SHADOW Store");
    info.iconName = QStringLiteral("org.shadow.store");
    info.description = QStringLiteral("Browse, install, update and remove software. "
                                      "Open an application by AppStream ID, package name or appstream:// URL.");
    info.singleInstance = true;

    Shadow::App app(argumentCount, arguments.data(), info);
    app.parser().addOption(QCommandLineOption(QStringLiteral("search"), QStringLiteral("Search for <text>."),
                                              QStringLiteral("text")));
    app.parser().addOption(QCommandLineOption(
        QStringLiteral("page"),
        QStringLiteral("Open a view: overview, installed, updates, repos, or a category key (see --dump categories)."),
        QStringLiteral("view")));
    app.parser().addPositionalArgument(
        QStringLiteral("target"),
        QStringLiteral("AppStream ID (org.example.App), package name, or appstream://<id> URL to open. "
                       "With --dump search / --dump component: the search text / the ID or package name."),
        QStringLiteral("[target]"));
    app.setDumpTopics({QStringLiteral("status"), QStringLiteral("categories"), QStringLiteral("search"),
                       QStringLiteral("component"), QStringLiteral("repos"), QStringLiteral("updates"),
                       QStringLiteral("installed")});
    if (!app.processArguments())
        return 0;

    if (app.dumpTopic().isEmpty())
        return app.run(QStringLiteral("org.shadow.store"));

    // ---- diagnostic mode ------------------------------------------------------
    const QString topic = app.dumpTopic();
    const QString argument = app.parser().positionalArguments().value(0);
    static const QStringList topics = {QStringLiteral("status"), QStringLiteral("categories"), QStringLiteral("search"),
                                       QStringLiteral("component"), QStringLiteral("repos"), QStringLiteral("updates"),
                                       QStringLiteral("installed")};
    if (!topics.contains(topic))
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    if ((topic == QLatin1String("search") || topic == QLatin1String("component")) && argument.isEmpty())
        return app.dumpError(QStringLiteral("--dump %1 needs an argument (%2)").arg(
            topic, topic == QLatin1String("search") ? QStringLiteral("the text to search for")
                                                    : QStringLiteral("an AppStream ID or a package name")));

    Store store;
    Catalog *catalog = store.catalog();
    Service *service = Service::instance();
    bool probed = false;
    QObject::connect(service, &Service::probed, &store, [&probed] { probed = true; });
    store.initialize();

    const auto catalogLoaded = [catalog] { return !catalog->loading() && catalog->state() != QLatin1String("idle"); };
    waitUntil(app, catalog, SIGNAL(stateChanged()), catalogLoaded, 120000);
    waitUntil(app, service, SIGNAL(probed()), [&probed] { return probed; }, 40000);

    if (topic == QLatin1String("status"))
        return app.finishDump(store.statusJson());

    if (topic == QLatin1String("categories")) {
        QJsonObject out{
            {QStringLiteral("catalogState"), catalog->state()},
            {QStringLiteral("applicationCount"), catalog->applicationCount()},
            {QStringLiteral("uncategorizedApplications"), catalog->uncategorizedCount()},
            {QStringLiteral("categories"), QJsonArray::fromVariantList(catalog->categories())},
        };
        if (!catalog->error().isEmpty())
            out.insert(QStringLiteral("error"), catalog->error());
        return app.finishDump(out);
    }

    if (topic == QLatin1String("search")) {
        store.searchApplications(argument);
        waitUntil(app, &store, SIGNAL(searchChanged()),
                  [&store] { return store.appSearchState() != QLatin1String("searching"); }, 60000);
        // The same text as a package search through PackageKit (SearchNames),
        // exactly what the "All packages" tab of the search page asks.
        store.searchAllPackages(argument, false);
        waitUntil(app, &store, SIGNAL(searchChanged()),
                  [&store] { return store.packageSearchState() != QLatin1String("searching"); }, 180000);
        QJsonObject packages{
            {QStringLiteral("source"), QStringLiteral("packagekit-search-names")},
            {QStringLiteral("packagekitReachable"), service->available()},
            {QStringLiteral("state"), store.packageSearchState()},
            {QStringLiteral("truncated"), store.packageSearchTruncated()},
            {QStringLiteral("count"), store.searchPackages()->totalCount()},
            {QStringLiteral("results"), store.searchPackages()->toJson()},
        };
        if (!store.packageSearchError().isEmpty())
            packages.insert(QStringLiteral("error"), store.packageSearchError());
        return app.finishDump(QJsonObject{
            {QStringLiteral("query"), argument},
            {QStringLiteral("source"), QStringLiteral("appstream")},
            {QStringLiteral("catalogState"), catalog->state()},
            {QStringLiteral("count"), store.searchApps()->count()},
            {QStringLiteral("results"), store.searchApps()->toJson()},
            {QStringLiteral("packages"), packages},
        });
    }

    if (topic == QLatin1String("component")) {
        const QVariantMap target = store.resolveTarget(argument);
        QJsonObject out{{QStringLiteral("query"), argument}};
        QString packageName;
        if (target.value(QStringLiteral("view")).toString() == QLatin1String("app")) {
            const QJsonObject component = catalog->detailsJson(target.value(QStringLiteral("id")).toString());
            out.insert(QStringLiteral("component"), component);
            packageName = component.value(QStringLiteral("packageName")).toString();
        } else {
            out.insert(QStringLiteral("component"), QJsonValue());
            out.insert(QStringLiteral("componentNote"),
                       QStringLiteral("AppStream has no component with this ID or package name"));
            packageName = target.value(QStringLiteral("name")).toString();
        }
        if (packageName.isEmpty()) {
            out.insert(QStringLiteral("package"), QJsonValue());
        } else {
            PackageInfo package;
            bool settled = false;
            QObject::connect(&package, &PackageInfo::settled, &store, [&settled] { settled = true; });
            package.setPackageName(packageName);
            waitUntil(app, &package, SIGNAL(settled()), [&settled] { return settled; }, 120000);
            out.insert(QStringLiteral("package"), package.toJson());
        }
        out.insert(QStringLiteral("packagekitReachable"), service->available());
        return app.finishDump(out);
    }

    if (topic == QLatin1String("repos")) {
        waitUntil(app, &store, SIGNAL(reposChanged()),
                  [&store] { return store.reposState() != QLatin1String("loading") && store.reposState() != QLatin1String("idle"); },
                  60000);
        return app.finishDump(store.reposJson());
    }

    if (topic == QLatin1String("updates")) {
        // Reads the lists on disk; it does not refresh them.
        ShadowPk::UpdateChecker *checker = store.updateChecker();
        waitUntil(app, checker, SIGNAL(stateChanged()), [checker] { return !checker->busy() && checker->state() != QLatin1String("idle"); },
                  300000);
        QJsonObject out = checker->toJson();
        out.insert(QStringLiteral("packagekitReachable"), service->available());
        return app.finishDump(out);
    }

    // installed
    waitUntil(app, &store, SIGNAL(installedChanged()),
              [&store] { return store.installedState() == QLatin1String("ready") || store.installedState() == QLatin1String("error"); },
              180000);
    return app.finishDump(store.installedJson());
}
