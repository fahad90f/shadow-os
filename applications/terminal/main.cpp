// SPDX-License-Identifier: GPL-3.0-or-later
//
// NEXUS Terminal — terminal emulator (Qt Widgets + QTermWidget).
//
// The one NEXUS application that does not go through Nexus::App: there is no
// QML engine here. It follows the same contract by its own means —
// --help/--version/--dump <topic> and the test hooks NEXUS_SMOKE_TEST,
// NEXUS_SCREENSHOT, NEXUS_SCREENSHOT_DELAY and NEXUS_WINDOW_SIZE
// (docs/applications/development.md).
#include "mainwindow.h"
#include "profile.h"
#include "schemes.h"
#include "settings.h"
#include "shells.h"
#include "shortcuts.h"
#include "sshdialog.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSocketNotifier>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

namespace {

const char kAppId[] = "org.nexus.terminal";

QStringList dumpTopics()
{
    return {QStringLiteral("profiles"), QStringLiteral("schemes"), QStringLiteral("shells"),
            QStringLiteral("shortcuts"), QStringLiteral("ssh"), QStringLiteral("settings")};
}

int finishDump(const QJsonValue &value)
{
    const QJsonDocument doc = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    std::fputs(doc.toJson(QJsonDocument::Indented).constData(), stdout);
    std::fflush(stdout);
    return 0;
}

int dumpError(const QString &message)
{
    std::fprintf(stderr, "%s: %s\n", kAppId, qPrintable(message));
    std::fprintf(stderr, "available topics: %s\n", qPrintable(dumpTopics().join(QStringLiteral(", "))));
    return 2;
}

QJsonObject sshDump()
{
    QJsonArray targets;
    const QList<SshTarget> saved = Settings::instance()->sshTargets();
    for (const SshTarget &target : saved) {
        QString error;
        const bool valid = Ssh::validate(target, &error);
        QJsonObject obj = target.toJson();
        obj.insert(QStringLiteral("label"), target.label());
        obj.insert(QStringLiteral("valid"), valid);
        obj.insert(QStringLiteral("error"), error);
        // The argument vector a connection would be started with.
        obj.insert(QStringLiteral("command"), valid ? QJsonArray::fromStringList(Ssh::command(target)) : QJsonArray());
        targets.append(obj);
    }
    const QString program = Ssh::program();
    return QJsonObject{
        {QStringLiteral("program"), program.isEmpty() ? QJsonValue() : QJsonValue(program)},
        {QStringLiteral("available"), !program.isEmpty()},
        {QStringLiteral("targets"), targets},
    };
}

// Qt only reports a style sheet it cannot parse through a warning; count
// those so the smoke test can fail on a broken sheet.
QtMessageHandler g_previousHandler = nullptr;

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg && message.contains(QLatin1String("Could not parse"))
        && message.contains(QLatin1String("stylesheet"), Qt::CaseInsensitive))
        Theme::noteStyleSheetError();
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

// SIGTERM, SIGINT and SIGHUP (session logout) close the window the orderly
// way: the session is saved and the shells get their hang-up. A signal
// handler may only write to a descriptor; the event loop does the rest.
int g_signalPipe[2] = {-1, -1};

void onUnixSignal(int)
{
    const char byte = 1;
    const ssize_t written = ::write(g_signalPipe[1], &byte, 1);
    Q_UNUSED(written)
}

void installSignalHandlers(MainWindow *window)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, g_signalPipe) != 0)
        return;
    auto *notifier = new QSocketNotifier(g_signalPipe[0], QSocketNotifier::Read, window);
    QObject::connect(notifier, &QSocketNotifier::activated, window, [window, notifier] {
        notifier->setEnabled(false);
        char byte = 0;
        const ssize_t got = ::read(g_signalPipe[0], &byte, 1);
        Q_UNUSED(got)
        window->closeNow();
    });
    struct sigaction action;
    action.sa_handler = onUnixSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int signal : {SIGTERM, SIGINT, SIGHUP})
        ::sigaction(signal, &action, nullptr);
}

} // namespace

int main(int argc, char **argv)
{
    // "-e <command> [args...]": everything after -e belongs to the command and
    // is never looked at as an option of this program.
    QStringList command;
    bool commandGiven = false;
    int ownArgc = argc;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "-e" || arg == "--command") {
            commandGiven = true;
            for (int j = i + 1; j < argc; ++j)
                command << QFile::decodeName(argv[j]);
            ownArgc = i;
            break;
        }
    }

    // Diagnostic mode never opens a window, so it must work without a display
    // server (serial console, SSH, CI).
    for (int i = 1; i < ownArgc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--dump" || arg.startsWith("--dump=")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            break;
        }
    }

    // The test hooks are for this process only: read them, then take them out
    // of the environment the shells inherit. The same goes for variables a
    // surrounding terminal may have left behind.
    const bool smoke = qEnvironmentVariableIntValue("NEXUS_SMOKE_TEST") == 1;
    const QString screenshot = qEnvironmentVariable("NEXUS_SCREENSHOT");
    const QString forcedSize = qEnvironmentVariable("NEXUS_WINDOW_SIZE");
    bool delayOk = false;
    int delay = qEnvironmentVariableIntValue("NEXUS_SCREENSHOT_DELAY", &delayOk);
    if (!delayOk || delay <= 0)
        delay = smoke && screenshot.isEmpty() ? 1500 : 2500;
    for (const char *name : {"NEXUS_SMOKE_TEST", "NEXUS_SCREENSHOT", "NEXUS_SCREENSHOT_DELAY", "NEXUS_WINDOW_SIZE",
                             "COLUMNS", "LINES", "TERMCAP", "VTE_VERSION", "KONSOLE_VERSION"})
        qunsetenv(name);

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(ownArgc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("nexus"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("nexus.org"));
    QCoreApplication::setApplicationName(QStringLiteral("terminal"));
    QCoreApplication::setApplicationVersion(QStringLiteral(NEXUS_VERSION));
    QGuiApplication::setApplicationDisplayName(QCoreApplication::translate("main", "Terminal"));
    QGuiApplication::setDesktopFileName(QString::fromLatin1(kAppId));

    // QTermWidget reports its inner workings through qDebug(); keep that out
    // of the user's journal.
    QLoggingCategory::setFilterRules(QStringLiteral("default.debug=false"));
    g_previousHandler = qInstallMessageHandler(messageHandler);

    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate(
        "main", "Terminal emulator with tabs, split panes, profiles and SSH connections"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption workdirOption(
        {QStringLiteral("w"), QStringLiteral("working-directory"), QStringLiteral("workdir")},
        QCoreApplication::translate("main", "Start the first terminal in <dir>."), QStringLiteral("dir"));
    const QCommandLineOption profileOption({QStringLiteral("p"), QStringLiteral("profile")},
        QCoreApplication::translate("main", "Use the profile <name> for the first terminal."), QStringLiteral("name"));
    const QCommandLineOption commandOption({QStringLiteral("e"), QStringLiteral("command")},
        QCoreApplication::translate("main", "Run <command> with its arguments instead of the shell. Must be the "
                                            "last option: everything after it belongs to the command."),
        QStringLiteral("command"));
    const QCommandLineOption titleOption({QStringLiteral("T"), QStringLiteral("title")},
        QCoreApplication::translate("main", "Give the first tab the fixed title <title>."), QStringLiteral("title"));
    const QCommandLineOption holdOption(QStringLiteral("hold"),
        QCoreApplication::translate("main", "Keep the tab open after the command given with -e has ended."));
    const QCommandLineOption noRestoreOption(QStringLiteral("no-restore"),
        QCoreApplication::translate("main", "Do not restore or offer the previous session."));
    const QCommandLineOption dumpOption(QStringLiteral("dump"),
        QCoreApplication::translate("main", "Print diagnostic data for <topic> as JSON and exit. Topics: %1.")
            .arg(dumpTopics().join(QStringLiteral(", "))),
        QStringLiteral("topic"));
    parser.addOptions({workdirOption, profileOption, titleOption, commandOption, holdOption, noRestoreOption,
                       dumpOption});
    parser.process(app);

    if (!parser.positionalArguments().isEmpty()) {
        std::fprintf(stderr, "%s: unexpected argument '%s' (use -e to run a command)\n", kAppId,
                     qPrintable(parser.positionalArguments().constFirst()));
        return 2;
    }
    if (commandGiven && command.isEmpty()) {
        std::fprintf(stderr, "%s: -e needs a command\n", kAppId);
        return 2;
    }

    Schemes::registerSchemes();

    if (parser.isSet(dumpOption)) {
        const QString topic = parser.value(dumpOption);
        if (topic == QLatin1String("profiles"))
            return finishDump(ProfileManager::instance()->toJson());
        if (topic == QLatin1String("schemes"))
            return finishDump(Schemes::toJson());
        if (topic == QLatin1String("shells"))
            return finishDump(Shells::toJson());
        if (topic == QLatin1String("shortcuts"))
            return finishDump(Shortcuts::toJson());
        if (topic == QLatin1String("ssh"))
            return finishDump(sshDump());
        if (topic == QLatin1String("settings"))
            return finishDump(Settings::instance()->toJson());
        return dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }

    Theme::instance()->apply();
    QIcon icon = QIcon::fromTheme(QString::fromLatin1(kAppId));
    if (icon.isNull())
        icon = Theme::instance()->icon(QStringLiteral("terminal"), QStringLiteral("accent"), QStringLiteral("accent"));
    QGuiApplication::setWindowIcon(icon);

    StartupOptions options;
    options.workingDirectory = parser.value(workdirOption);
    options.profile = parser.value(profileOption);
    options.title = parser.value(titleOption);
    options.command = command;
    options.hold = parser.isSet(holdOption);
    options.noRestore = parser.isSet(noRestoreOption);

    MainWindow window(options);
    const QStringList size = forcedSize.split(QLatin1Char('x'));
    if (size.size() == 2 && size.at(0).toInt() > 0 && size.at(1).toInt() > 0)
        window.resize(size.at(0).toInt(), size.at(1).toInt());
    window.show();
    installSignalHandlers(&window);

    // ---- test hooks ---------------------------------------------------------------
    int hookResult = -1;
    if (smoke || !screenshot.isEmpty()) {
        // The window closing by itself (its only shell ended) is a failure
        // for the smoke test, not a normal exit.
        QObject::connect(&window, &MainWindow::allTabsClosed, &app, [&hookResult, smoke] {
            if (smoke && hookResult < 0) {
                std::fprintf(stderr, "NEXUS-SMOKE-FAILED %s: the shell ended before the check\n", kAppId);
                std::fflush(stderr);
                hookResult = 2;
            }
        });
        QTimer::singleShot(delay, &window, [&window, &hookResult, smoke, screenshot] {
            int rc = 0;
            if (!screenshot.isEmpty()) {
                const QPixmap image = window.grab();
                if (image.isNull() || !image.save(screenshot)) {
                    std::fprintf(stderr, "NEXUS-SCREENSHOT-FAILED %s\n", qPrintable(screenshot));
                    rc = 3;
                } else {
                    std::printf("NEXUS-SCREENSHOT-SAVED %s %dx%d\n", qPrintable(screenshot), image.width(),
                                image.height());
                }
            }
            if (smoke) {
                QString reason;
                if (!window.shellRunning(&reason)) {
                    std::fprintf(stderr, "NEXUS-SMOKE-FAILED %s: %s\n", kAppId, qPrintable(reason));
                    rc = rc ? rc : 2;
                } else if (Theme::styleSheetErrors() > 0) {
                    std::fprintf(stderr, "NEXUS-SMOKE-FAILED %s: the style sheet could not be parsed\n", kAppId);
                    rc = rc ? rc : 2;
                } else if (rc == 0) {
                    std::printf("NEXUS-SMOKE-OK %s\n", kAppId);
                }
            }
            std::fflush(stdout);
            std::fflush(stderr);
            hookResult = rc;
            window.shutdownTerminals();
            QCoreApplication::exit(rc);
        });
    }

    const int rc = app.exec();
    return hookResult >= 0 ? hookResult : rc;
}
