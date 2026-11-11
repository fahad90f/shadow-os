// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHADOW Archive Manager.
//
//   shadow-archive-manager [archive]                    browse an archive
//   shadow-archive-manager --extract <archive> [--to <dir>]
//   shadow-archive-manager --create <output> <files…>
//   shadow-archive-manager --list <archive>             JSON listing, no window
//   shadow-archive-manager --test <archive>             integrity test, no window
//   … --headless                                       --extract/--create without a window
//
// See README.md for every option and the exit codes.
#include "archivesession.h"
#include "formats.h"
#include "tools.h"

#include <shadowapp.h>

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>

#include <cstdio>
#include <iostream>
#include <string>

namespace {

enum ExitCode { Ok = 0, Failed = 1, Usage = 2, Refused = 3, PasswordProblem = 4 };

void say(const QString &text)
{
    std::fprintf(stderr, "%s\n", qPrintable(text));
}

struct Headless
{
    QString mode;           // list | test | extract | create | add | delete
    QStringList entries;    // extract: only these; delete: these
    QString archive;
    QString destination;
    bool destinationGiven = false;
    QStringList inputs;
    QVariantMap options;
    bool force = false;
    bool havePassword = false;
    QString password;
};

int runHeadless(Shadow::App &app, const Headless &job)
{
    ArchiveSession session;
    QEventLoop loop;
    int rc = Failed;
    bool passwordUsed = false;
    bool changed = false;

    QObject::connect(&session, &ArchiveSession::passwordRequired, &loop, [&](bool wrong) {
        if (job.havePassword && !passwordUsed && !wrong) {
            passwordUsed = true;
            session.submitPassword(job.password);
            return;
        }
        say(wrong ? QStringLiteral("The password is wrong.")
                  : QStringLiteral("This archive needs a password; give it on standard input with --password-stdin."));
        rc = PasswordProblem;
        loop.quit();
    });
    QObject::connect(&session, &ArchiveSession::finished, &loop,
                     [&](const QString &kind, bool ok, const QString &message, const QStringList &details) {
        if (kind == QLatin1String("list") && ok && job.mode != QLatin1String("list")) {
            if (session.busy())
                return;     // the re-listing after a change
            if (job.mode == QLatin1String("test")) {
                session.test();
                return;
            }
            if (job.mode == QLatin1String("extract")) {
                QString destination = job.destination;
                if (!job.destinationGiven) {
                    // "Extract here": a single top-level item goes next to the
                    // archive, anything else into a folder named after it.
                    destination = QFileInfo(session.archivePath()).absolutePath();
                    if (session.rootItemCount() != 1)
                        destination += QLatin1Char('/') + session.suggestedFolderName();
                }
                const QVariantMap estimate = session.extractionEstimate({}, destination);
                if (!estimate.value(QStringLiteral("enough")).toBool()) {
                    say(QStringLiteral("Not enough free space in %1: %2 bytes needed, %3 available.")
                            .arg(destination).arg(estimate.value(QStringLiteral("expanded")).toLongLong())
                            .arg(estimate.value(QStringLiteral("free")).toLongLong()));
                    rc = Refused;
                    loop.quit();
                    return;
                }
                if (estimate.value(QStringLiteral("suspicious")).toBool() && !job.force) {
                    say(QStringLiteral("Refused: this archive expands to %1 bytes, %2 times its own size (possible decompression bomb). "
                                       "Use --force to extract it anyway.")
                            .arg(estimate.value(QStringLiteral("expanded")).toLongLong())
                            .arg(qRound64(estimate.value(QStringLiteral("ratio")).toDouble())));
                    rc = Refused;
                    loop.quit();
                    return;
                }
                session.extract(destination, job.entries, job.options);
                return;
            }
            if (job.mode == QLatin1String("add") || job.mode == QLatin1String("delete")) {
                if (changed) {
                    rc = Ok;        // this is the listing after the change
                    loop.quit();
                    return;
                }
                if (!session.canModify()) {
                    say(session.readOnlyReason());
                    loop.quit();
                    return;
                }
                if (job.mode == QLatin1String("add"))
                    session.addFiles(job.inputs);
                else
                    session.deleteEntries(job.entries);
                return;
            }
            return;
        }
        if (!message.isEmpty())
            say(message);
        for (const QString &detail : details)
            say(QStringLiteral("  - ") + detail);
        if ((kind == QLatin1String("add") || kind == QLatin1String("delete")) && ok) {
            changed = true;     // the session lists the archive again; wait for that
            return;
        }
        if (kind == QLatin1String("list") && ok) {
            rc = app.finishDump(QJsonObject{
                {QStringLiteral("archive"), session.archivePath()},
                {QStringLiteral("format"), session.formatId()},
                {QStringLiteral("encrypted"), session.encrypted()},
                {QStringLiteral("entryCount"), session.entryCount()},
                {QStringLiteral("totalSize"), session.totalSize()},
                {QStringLiteral("archiveSize"), session.archiveSize()},
                {QStringLiteral("hazardCount"), session.hazardCount()},
                {QStringLiteral("bombSuspected"), session.bombSuspected()},
                {QStringLiteral("warning"), session.listWarning()},
                {QStringLiteral("entries"), session.toJson()},
            });
        } else {
            rc = ok ? Ok : Failed;
        }
        loop.quit();
    });

    QMetaObject::invokeMethod(&session, [&] {
        if (job.mode == QLatin1String("create"))
            session.create(job.archive, job.inputs, job.options);
        else
            session.open(job.archive);
    }, Qt::QueuedConnection);
    loop.exec();
    return rc;
}

} // namespace

int main(int argc, char **argv)
{
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--") break;
        if (arg == "--headless" || arg == "--list" || arg == "--test" || arg == "--add" || arg == "--delete")
            headless = true;
    }
    if (headless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    Shadow::App app(argc, argv, {
        QStringLiteral("org.shadow.archivemanager"),
        QStringLiteral("Archive Manager"),
        QStringLiteral("org.shadow.archivemanager"),
        QStringLiteral("Open, extract and create archives"),
    });
    app.setDumpTopics({QStringLiteral("formats"), QStringLiteral("tools")});

    QCommandLineParser &parser = app.parser();
    const QCommandLineOption extractOption(QStringLiteral("extract"), QStringLiteral("Extract <archive> (to --to, or next to the archive)."));
    const QCommandLineOption toOption(QStringLiteral("to"), QStringLiteral("Folder to extract into (created if needed)."), QStringLiteral("dir"));
    const QCommandLineOption createOption(QStringLiteral("create"), QStringLiteral("Create the archive <output> from the given files and folders."));
    const QCommandLineOption listOption(QStringLiteral("list"), QStringLiteral("Print the contents of <archive> as JSON and exit."));
    const QCommandLineOption testOption(QStringLiteral("test"), QStringLiteral("Test the integrity of <archive> and exit (0: intact)."));
    const QCommandLineOption headlessOption(QStringLiteral("headless"), QStringLiteral("Run --extract/--create without a window."));
    const QCommandLineOption formatOption(QStringLiteral("format"), QStringLiteral("Format for --create: zip, 7z, tar, tar.gz, tar.bz2, tar.xz, tar.zst, cpio (default: from the file name)."), QStringLiteral("id"));
    const QCommandLineOption levelOption(QStringLiteral("level"), QStringLiteral("Compression for --create: store (zip/7z), fast, normal, best."), QStringLiteral("level"), QStringLiteral("normal"));
    const QCommandLineOption overwriteOption(QStringLiteral("overwrite"), QStringLiteral("Existing files when extracting: rename (default), skip, overwrite."), QStringLiteral("mode"), QStringLiteral("rename"));
    const QCommandLineOption forceOption(QStringLiteral("force"), QStringLiteral("With --headless --extract: extract even if the archive looks like a decompression bomb."));
    const QCommandLineOption passwordOption(QStringLiteral("password-stdin"), QStringLiteral("With --headless/--list/--test: read the password (one line) from standard input."));
    const QCommandLineOption namesOption(QStringLiteral("encrypt-names"), QStringLiteral("With --create --format 7z and a password: encrypt the file names too."));
    const QCommandLineOption entryOption(QStringLiteral("entry"), QStringLiteral("With --headless --extract: extract only this entry (a path inside the archive; may be repeated)."), QStringLiteral("path"));
    const QCommandLineOption addOption(QStringLiteral("add"), QStringLiteral("Add the given files and folders to the top level of <archive>; no window."));
    const QCommandLineOption deleteOption(QStringLiteral("delete"), QStringLiteral("Delete the given entries (paths inside the archive) from <archive>; no window."));
    parser.addOptions({entryOption, addOption, deleteOption, extractOption, toOption, createOption, listOption, testOption, headlessOption, formatOption,
                       levelOption, overwriteOption, forceOption, passwordOption, namesOption});
    parser.addPositionalArgument(QStringLiteral("archive"), QStringLiteral("Archive to open, extract, list or test; with --create, the archive to write followed by its contents."),
                                 QStringLiteral("[archive] [files...]"));
    if (!app.processArguments())
        return 0;

    if (!app.dumpTopic().isEmpty()) {
        if (app.dumpTopic() == QLatin1String("formats"))
            return app.finishDump(Formats::describe());
        if (app.dumpTopic() == QLatin1String("tools"))
            return app.finishDump(QJsonArray::fromVariantList(ArchiveTools().tools()));
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(app.dumpTopic()));
    }

    const QStringList positional = parser.positionalArguments();
    const int modes = int(parser.isSet(extractOption)) + int(parser.isSet(createOption)) + int(parser.isSet(listOption))
                      + int(parser.isSet(testOption)) + int(parser.isSet(addOption)) + int(parser.isSet(deleteOption));
    if (modes > 1) {
        say(QStringLiteral("Use only one of --extract, --create, --list, --test, --add and --delete."));
        return Usage;
    }
    const QString level = parser.value(levelOption);
    const QString conflict = parser.value(overwriteOption);
    if (!QStringList{QStringLiteral("store"), QStringLiteral("fast"), QStringLiteral("normal"), QStringLiteral("best")}.contains(level)
        || !QStringList{QStringLiteral("rename"), QStringLiteral("skip"), QStringLiteral("overwrite")}.contains(conflict)) {
        say(QStringLiteral("Invalid --level or --overwrite value; see --help."));
        return Usage;
    }
    if (modes == 1 && positional.isEmpty()) {
        say(QStringLiteral("An archive file name is required; see --help."));
        return Usage;
    }
    if ((parser.isSet(createOption) || parser.isSet(addOption) || parser.isSet(deleteOption)) && positional.size() < 2) {
        say(QStringLiteral("usage: shadow-archive-manager --create <output> <files...> | --add <archive> <files...> | --delete <archive> <entries...>"));
        return Usage;
    }
    auto absolute = [](const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); };

    if (headless) {
        Headless job;
        job.mode = parser.isSet(listOption) ? QStringLiteral("list") : parser.isSet(testOption) ? QStringLiteral("test")
                 : parser.isSet(createOption) ? QStringLiteral("create") : parser.isSet(extractOption) ? QStringLiteral("extract")
                 : parser.isSet(addOption) ? QStringLiteral("add") : parser.isSet(deleteOption) ? QStringLiteral("delete") : QString();
        if (job.mode.isEmpty()) {
            say(QStringLiteral("--headless needs --extract or --create."));
            return Usage;
        }
        job.archive = absolute(positional.first());
        job.force = parser.isSet(forceOption);
        if (parser.isSet(passwordOption)) {
            std::string line;
            std::getline(std::cin, line);
            job.password = QString::fromUtf8(line.c_str());
            job.havePassword = true;
        }
        if (job.mode == QLatin1String("extract")) {
            job.destinationGiven = parser.isSet(toOption);
            if (job.destinationGiven)
                job.destination = absolute(parser.value(toOption));
            job.options = {{QStringLiteral("conflict"), conflict}};
            job.entries = parser.values(entryOption);
        } else if (job.mode == QLatin1String("add")) {
            for (const QString &input : positional.mid(1))
                job.inputs << absolute(input);
        } else if (job.mode == QLatin1String("delete")) {
            job.entries = positional.mid(1);
        } else if (job.mode == QLatin1String("create")) {
            for (const QString &input : positional.mid(1))
                job.inputs << absolute(input);
            job.options = {{QStringLiteral("format"), parser.value(formatOption)}, {QStringLiteral("level"), level},
                           {QStringLiteral("password"), job.password},
                           {QStringLiteral("encryptNames"), parser.isSet(namesOption)}};
        }
        return runHeadless(app, job);
    }

    // Owned by the application context so it outlives the QML engine.
    auto *launchObject = new Launch(app.context());
    Launch &launch = *launchObject;
    launch.level = level;
    launch.conflict = conflict;
    launch.format = parser.value(formatOption);
    if (parser.isSet(createOption)) {
        launch.mode = QStringLiteral("create");
        launch.archive = absolute(positional.first());
        for (const QString &input : positional.mid(1))
            launch.inputs << absolute(input);
    } else if (!positional.isEmpty()) {
        launch.mode = parser.isSet(extractOption) ? QStringLiteral("extract") : QStringLiteral("browse");
        launch.archive = absolute(positional.first());
        if (parser.isSet(toOption))
            launch.destination = absolute(parser.value(toOption));
    }
    app.expose(QStringLiteral("launch"), launchObject);
    return app.run(QStringLiteral("org.shadow.archivemanager"));
}
