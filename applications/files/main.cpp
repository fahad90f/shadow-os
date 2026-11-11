// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices.h"
#include "foldermodel.h"
#include "interaction.h"
#include "launcher.h"
#include "operations.h"
#include "places.h"
#include "thumbnails.h"
#include "util.h"

#include <shadowapp.h>

#include <QCommandLineOption>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QtPlugin>

using namespace Qt::StringLiterals;

// Built-in fallback for the "svg" icon engine (src/svgiconengine.cpp).
Q_IMPORT_PLUGIN(ShadowFilesSvgIconPlugin)

namespace {

// File and folder icons come from the freedesktop icon theme. A session that
// has no icon theme configured resolves to the bare "hicolor" theme, which
// has none of them; fall back to an installed complete theme in that case.
void ensureIconTheme()
{
    if (QIcon::hasThemeIcon(u"folder"_s) && QIcon::hasThemeIcon(u"text-x-generic"_s))
        return;
    static const char *const candidates[] = {"breeze", "breeze-dark", "Adwaita", "oxygen"};
    const QStringList paths = QIcon::themeSearchPaths();
    for (const char *candidate : candidates) {
        const QString name = QString::fromLatin1(candidate);
        for (const QString &path : paths) {
            if (QFile::exists(path + u'/' + name + u"/index.theme"_s)) {
                QIcon::setFallbackThemeName(name);
                return;
            }
        }
    }
}

// KService (KApplicationTrader, "Open with") only knows the applications that
// an XDG menu definition includes: <prefix>applications.menu in a "menus"
// directory of the XDG configuration path. Desktop environments ship one; a
// session without it would show no applications at all. In that case a
// minimal "all installed applications" menu is provided to this process (and
// the programs it starts) through XDG_CONFIG_DIRS.
void ensureApplicationsMenu()
{
    const QString menuName = qEnvironmentVariable("XDG_MENU_PREFIX") + u"applications.menu"_s;
    if (!QStandardPaths::locate(QStandardPaths::GenericConfigLocation, u"menus/"_s + menuName).isEmpty())
        return;
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/shadow/files-xdg"_s;
    const QString path = root + u"/menus/"_s + menuName;
    if (!QFile::exists(path)) {
        if (!QDir().mkpath(root + u"/menus"_s))
            return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        file.write("<!DOCTYPE Menu PUBLIC \"-//freedesktop//DTD Menu 1.0//EN\"\n"
                   " \"http://www.freedesktop.org/standards/menu-spec/menu-1.0.dtd\">\n"
                   "<Menu>\n"
                   "  <Name>Applications</Name>\n"
                   "  <DefaultAppDirs/>\n"
                   "  <DefaultDirectoryDirs/>\n"
                   "  <Include><All/></Include>\n"
                   "</Menu>\n");
    }
    QByteArray dirs = qgetenv("XDG_CONFIG_DIRS");
    if (dirs.isEmpty())
        dirs = "/etc/xdg";
    qputenv("XDG_CONFIG_DIRS", dirs + ':' + QFile::encodeName(root));
}

} // namespace

int main(int argc, char **argv)
{
    Shadow::App app(argc, argv, {
        QStringLiteral("org.shadow.files"),
        QStringLiteral("Files"),
        QStringLiteral("org.shadow.files"),
        QStringLiteral("Browse and manage files, drives and network locations"),
    });

    const QCommandLineOption selectOption(
        QStringLiteral("select"),
        QStringLiteral("Open the folder that contains <file> and select the file."),
        QStringLiteral("file"));
    app.parser().addOption(selectOption);
    app.parser().addPositionalArgument(
        QStringLiteral("locations"),
        QStringLiteral("Folders or URLs (sftp://, smb://, ftp://, webdav://, trash:/ …) to open, one tab each."),
        QStringLiteral("[path-or-url...]"));
    app.setDumpTopics({QStringLiteral("places"), QStringLiteral("devices"), QStringLiteral("list"),
                       QStringLiteral("protocols")});
    if (!app.processArguments())
        return 0;

    ensureIconTheme();
    ensureApplicationsMenu();
    JobUiDelegateFactory::install();

    const QStringList arguments = app.parser().positionalArguments();
    const QString cwd = QDir::currentPath();

    // ---- diagnostic mode ---------------------------------------------------------
    if (!app.dumpTopic().isEmpty()) {
        const QString topic = app.dumpTopic();
        if (topic == "places"_L1)
            return app.finishDump(Places::instance()->toJson());
        if (topic == "devices"_L1) {
            const Devices *devices = Devices::instance();
            return app.finishDump(QJsonObject{
                {u"devices"_s, devices->toJson()},
                {u"emptyReason"_s, devices->emptyReason()},
            });
        }
        if (topic == "protocols"_L1)
            return app.finishDump(Launcher::instance()->protocolsJson());
        if (topic == "list"_L1) {
            const QUrl url = arguments.isEmpty() ? QUrl::fromLocalFile(QDir::homePath())
                                                 : Util::urlFromUserInput(arguments.first(), cwd);
            FolderModel model;
            model.setShowHidden(true);
            if (!model.loadBlocking(url)) {
                const QString reason = model.error().isEmpty() ? QStringLiteral("listing timed out")
                    : model.error() + u": "_s + model.errorDetail();
                return app.dumpError(QStringLiteral("cannot list '%1': %2").arg(Util::displayPath(url), reason));
            }
            return app.finishDump(QJsonObject{
                {u"url"_s, model.url().toString()},
                {u"path"_s, model.url().isLocalFile() ? model.url().toLocalFile() : QString()},
                {u"count"_s, model.count()},
                {u"entries"_s, model.toJson()},
            });
        }
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }

    // ---- window ------------------------------------------------------------------
    QList<QUrl> locations;
    for (const QString &argument : arguments) {
        const QUrl url = Util::urlFromUserInput(argument, cwd);
        if (url.isValid() && !url.isEmpty())
            locations << url;
    }
    QUrl select;
    if (app.parser().isSet(selectOption))
        select = Util::urlFromUserInput(app.parser().value(selectOption), cwd);
    Launcher::instance()->setStartup(locations, select);

    // Created on the GUI thread before QML (and the image loader thread) use them.
    ThumbnailStore::instance();
    FileOperations::instance();
    app.engine().addImageProvider(QStringLiteral("nxthumb"), new ThumbnailImageProvider);

    return app.run(QStringLiteral("org.shadow.files"));
}
