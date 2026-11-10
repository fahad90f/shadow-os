// SPDX-License-Identifier: GPL-3.0-or-later
#include <nexusapp.h>

int main(int argc, char **argv)
{
    Nexus::App app(argc, argv, {
        QStringLiteral("org.nexus.gallery"),
        QStringLiteral("NexusUI Gallery"),
        QStringLiteral("nexus"),
        QStringLiteral("Visual reference of the NEXUS design system components"),
    });
    return app.run(QStringLiteral("org.nexus.gallery"));
}
