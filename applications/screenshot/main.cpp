// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller.h"
#include "screengrabber.h"

#include <nexusapp.h>

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace {
// With --clipboard the picture has to be served to other applications after
// the command has returned (on Wayland the clipboard content stays in the
// process that set it). The command therefore splits in two before Qt
// starts: the child does the work and keeps the clipboard, the parent waits
// for the child's result and returns it as the exit code.
int forkForClipboard(int argc, char **argv)
{
    bool clipboard = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--clipboard") == 0)
            clipboard = true;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--version") == 0
                 || std::strcmp(argv[i], "-v") == 0 || std::strncmp(argv[i], "--dump", 6) == 0 || std::strcmp(argv[i], "--help-all") == 0)
            return -1;
    }
    if (!clipboard)
        return -1;
    int fds[2];
    if (::pipe(fds) != 0)
        return -1;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::setsid();
        return fds[1];
    }
    ::close(fds[1]);
    int code = ExitCode::CaptureFailed;
    ssize_t n;
    do {
        n = ::read(fds[0], &code, sizeof code);
    } while (n < 0 && errno == EINTR);
    if (n != ssize_t(sizeof code)) {
        // The child ended without reporting (usage error, crash): pass on its status.
        int status = 0;
        ::waitpid(pid, &status, 0);
        code = WIFEXITED(status) ? WEXITSTATUS(status) : ExitCode::CaptureFailed;
    }
    ::_exit(code);
}
} // namespace

int main(int argc, char **argv)
{
    const int statusFd = forkForClipboard(argc, argv);

    Nexus::App app(argc, argv, {
        QStringLiteral("org.nexus.screenshot"),
        QStringLiteral("Screenshot"),
        QStringLiteral("org.nexus.screenshot"),
        QStringLiteral("Capture the screen, a monitor, a window or a region.\n\n"
                       "Without --output or --clipboard the picture opens in a window where it can be\n"
                       "annotated, saved and copied. With either option nothing is shown (except the\n"
                       "region selection) and the exit code tells the result:\n"
                       "  0  success                 3  selection cancelled by the user\n"
                       "  1  capture failed          4  output file could not be written\n"
                       "  2  invalid command line    5  clipboard could not be set"),
    });
    app.setDumpTopics({QStringLiteral("backends")});

    QCommandLineParser &parser = app.parser();
    const QCommandLineOption fullOption(u"full"_s, u"Capture all screens (default)."_s);
    const QCommandLineOption monitorOption(u"monitor"_s, u"Capture one monitor; names are listed by --list-monitors."_s, u"name"_s);
    const QCommandLineOption windowOption(u"window"_s, u"Capture the active window."_s);
    const QCommandLineOption regionOption(u"region"_s, u"Select a region with the mouse on a frozen picture of the screen."_s);
    const QCommandLineOption delayOption(u"delay"_s, u"Wait <seconds> (0–60) before capturing."_s, u"seconds"_s);
    const QCommandLineOption pointerOption(u"pointer"_s, u"Include the mouse pointer in the picture."_s);
    const QCommandLineOption outputOption(u"output"_s, u"Write the picture to <file> (.png, .jpg, .webp) and exit."_s, u"file"_s);
    const QCommandLineOption clipboardOption(u"clipboard"_s, u"Copy the picture to the clipboard and return; a background process keeps it available until the clipboard is replaced."_s);
    const QCommandLineOption listOption(u"list-monitors"_s, u"Print the monitor names, one per line, and exit."_s);
    parser.addOptions({listOption, fullOption, monitorOption, windowOption, regionOption, delayOption, pointerOption, outputOption, clipboardOption});

    if (!app.processArguments())
        return 0;

    // The controller must outlive the QML engine (which Nexus::App destroys
    // before the application object), so it belongs to the application.
    auto *controllerObject = new Controller(qApp);
    Controller &controller = *controllerObject;

    if (!app.dumpTopic().isEmpty()) {
        if (app.dumpTopic() != "backends"_L1)
            return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(app.dumpTopic()));
        QJsonObject o = ScreenGrabber::probe();
        o["writableFormats"_L1] = QJsonArray::fromStringList(controller.formats());
        o["saveDirectory"_L1] = controller.saveDirectory();
#ifdef NEXUS_HAVE_KGUIADDONS
        o["clipboard"_L1] = u"KSystemClipboard (data-control protocol on Wayland)"_s;
#else
        o["clipboard"_L1] = u"QClipboard (needs a focused window on Wayland)"_s;
#endif
        return app.finishDump(o);
    }

    if (parser.isSet(listOption)) {
        const QVariantList screens = controller.screens();
        for (const QVariant &s : screens) {
            const QVariantMap m = s.toMap();
            std::printf("%s\t%dx%d+%d+%d\n", qPrintable(m.value(u"name"_s).toString()), m.value(u"width"_s).toInt(),
                        m.value(u"height"_s).toInt(), m.value(u"x"_s).toInt(), m.value(u"y"_s).toInt());
        }
        return 0;
    }

    auto usage = [](const QString &text) {
        std::fprintf(stderr, "nexus-screenshot: %s\n", qPrintable(text));
        return int(ExitCode::Usage);
    };
    const int modes = int(parser.isSet(fullOption)) + int(parser.isSet(monitorOption)) + int(parser.isSet(windowOption)) + int(parser.isSet(regionOption));
    if (modes > 1)
        return usage(u"--full, --monitor, --window and --region exclude each other"_s);
    if (!parser.positionalArguments().isEmpty())
        return usage(u"unexpected argument '%1'"_s.arg(parser.positionalArguments().constFirst()));

    if (parser.isSet(delayOption)) {
        bool ok = false;
        const int seconds = parser.value(delayOption).toInt(&ok);
        if (!ok || seconds < 0 || seconds > 60)
            return usage(u"--delay expects a number of seconds from 0 to 60"_s);
        controller.setDelay(seconds);
    }
    if (parser.isSet(monitorOption)) {
        const QString name = parser.value(monitorOption);
        bool known = false;
        const QVariantList screens = controller.screens();
        for (const QVariant &s : screens)
            known = known || s.toMap().value(u"name"_s).toString() == name;
        if (!known)
            return usage(u"no monitor named '%1' (see --list-monitors)"_s.arg(name));
        controller.setMode(u"monitor"_s);
        controller.setMonitor(name);
    } else if (parser.isSet(windowOption)) {
        controller.setMode(u"window"_s);
    } else if (parser.isSet(regionOption)) {
        controller.setMode(u"region"_s);
    }
    controller.setIncludePointer(parser.isSet(pointerOption));

    CliOptions cli;
    cli.output = parser.value(outputOption);
    cli.clipboard = parser.isSet(clipboardOption);
    cli.nonInteractive = parser.isSet(outputOption) || cli.clipboard;
    cli.statusFd = statusFd;
    if (parser.isSet(outputOption)) {
        if (cli.output.isEmpty())
            return usage(u"--output needs a file name"_s);
        // Reject an unusable target before anything is captured.
        const QString suffix = cli.output.section(u'.', -1).toLower();
        if (!cli.output.contains(u'.') || !QStringList{u"png"_s, u"jpg"_s, u"jpeg"_s, u"webp"_s}.contains(suffix))
            return usage(u"--output: unsupported file type, use .png, .jpg or .webp"_s);
        const QString format = suffix == "jpg"_L1 ? u"jpeg"_s : suffix;
        if (!controller.formats().contains(format))
            return usage(u"--output: this system cannot write %1 files"_s.arg(format));
    }
    controller.setCli(cli);

    // The process outlives its window while it serves the clipboard.
    QGuiApplication::setQuitOnLastWindowClosed(false);
    app.expose(QStringLiteral("shot"), &controller);
    controller.start();
    // File/clipboard output of anything but a region needs no user interface at all.
    if (cli.nonInteractive && controller.mode() != "region"_L1)
        return QCoreApplication::exec();
    return app.run(QStringLiteral("org.nexus.screenshot"));
}
