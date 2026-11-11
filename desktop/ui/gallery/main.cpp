// SPDX-License-Identifier: GPL-3.0-or-later
#include <nexusapp.h>

#include <QQmlContext>

// nexus-ui-gallery                      the component overview
// nexus-ui-gallery --case <name>        one focused test page (Case<Name>.qml):
//                                       dialogs, segmented, rows, table, toast,
//                                       page, motion, glyphs, filedialog,
//                                       folderdialog
// nexus-ui-gallery --case <name> --selftest
//                                       the page also runs its scripted checks,
//                                       prints NEXUS-CASE-OK <name> or
//                                       NEXUS-CASE-FAILED … and exits 4 on failure
int main(int argc, char **argv)
{
    Nexus::App app(argc, argv, {
        QStringLiteral("org.nexus.gallery"),
        QStringLiteral("NexusUI Gallery"),
        QStringLiteral("nexus"),
        QStringLiteral("Visual reference of the NEXUS design system components"),
    });
    QCommandLineOption caseOption(QStringLiteral("case"),
        QStringLiteral("Open the test page <name> instead of the overview "
                       "(dialogs, segmented, rows, table, toast, page, motion, glyphs, filedialog, folderdialog)."),
        QStringLiteral("name"));
    QCommandLineOption selfTestOption(QStringLiteral("selftest"),
        QStringLiteral("Run the scripted checks of the test page and exit 4 if one fails."));
    app.parser().addOption(caseOption);
    app.parser().addOption(selfTestOption);
    if (!app.processArguments())
        return 0;

    app.engine().rootContext()->setContextProperty(QStringLiteral("gallerySelfTest"),
                                                   app.parser().isSet(selfTestOption));
    QString type = QStringLiteral("Main");
    if (app.parser().isSet(caseOption)) {
        const QString name = app.parser().value(caseOption).toLower();
        static const QStringList known = {
            QStringLiteral("dialogs"), QStringLiteral("segmented"), QStringLiteral("rows"),
            QStringLiteral("table"), QStringLiteral("toast"), QStringLiteral("page"),
            QStringLiteral("motion"), QStringLiteral("glyphs"),
            QStringLiteral("filedialog"), QStringLiteral("folderdialog"),
        };
        if (!known.contains(name)) {
            fprintf(stderr, "nexus-ui-gallery: unknown case '%s' (known: %s)\n", qPrintable(name),
                    qPrintable(known.join(QStringLiteral(", "))));
            return 2;
        }
        type = QStringLiteral("Case") + name.at(0).toUpper() + name.mid(1);
    }
    const bool selfTest = app.parser().isSet(selfTestOption);
    // Under the smoke-test hook the application exits after a delay; give the
    // scripted checks (a few seconds at most) time to finish first.
    if (selfTest && qEnvironmentVariableIsEmpty("NEXUS_SCREENSHOT_DELAY"))
        qputenv("NEXUS_SCREENSHOT_DELAY", "6000");
    const int rc = app.run(QStringLiteral("org.nexus.gallery"), type);
    if (selfTest && rc == 0) {
        // A run that ended before the checks did must not count as a pass.
        const auto roots = app.engine().rootObjects();
        const QObject *checks = roots.isEmpty() ? nullptr
            : roots.first()->findChild<QObject *>(QStringLiteral("caseChecks"));
        if (!checks || !checks->property("done").toBool()) {
            fprintf(stderr, "NEXUS-CASE-FAILED %s: the scripted checks did not finish\n", qPrintable(type));
            return 4;
        }
    }
    return rc;
}
