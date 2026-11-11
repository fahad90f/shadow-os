// SPDX-License-Identifier: GPL-3.0-or-later
#include <nexusapp.h>

#include <QQmlContext>

// nexus-ui-gallery                      the component overview
// nexus-ui-gallery --case <name>        one focused test page (Case<Name>.qml):
//                                       dialogs, segmented, rows, table, toast,
//                                       page, motion, glyphs
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
                       "(dialogs, segmented, rows, table, toast, page, motion, glyphs)."),
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
        };
        if (!known.contains(name)) {
            fprintf(stderr, "nexus-ui-gallery: unknown case '%s' (known: %s)\n", qPrintable(name),
                    qPrintable(known.join(QStringLiteral(", "))));
            return 2;
        }
        type = QStringLiteral("Case") + name.at(0).toUpper() + name.mid(1);
    }
    return app.run(QStringLiteral("org.nexus.gallery"), type);
}
