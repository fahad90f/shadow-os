// SPDX-License-Identifier: GPL-3.0-or-later
#include "schemes.h"

#include "termwidget.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSettings>

namespace Schemes {

namespace {

const char kDark[] = "ShadowObsidian";
const char kLight[] = "ShadowDaylight";
const char kEmbedded[] = ":/terminal/color-schemes";

QString g_directory;

QString schemeFile(const QString &dir, const QString &name)
{
    return dir + QLatin1Char('/') + name + QStringLiteral(".colorscheme");
}

QString findDirectory()
{
    // Installed and uninstalled binaries find the data the same way: relative
    // to the executable first, then at the configured install location.
    const QStringList candidates = {
        QDir::cleanPath(QCoreApplication::applicationDirPath() + QLatin1Char('/')
                        + QStringLiteral(SHADOW_TERMINAL_DATA_RELDIR) + QStringLiteral("/color-schemes")),
        QStringLiteral(SHADOW_TERMINAL_DATA_DIR) + QStringLiteral("/color-schemes"),
    };
    for (const QString &dir : candidates) {
        if (QFile::exists(schemeFile(dir, QString::fromLatin1(kDark)))
            && QFile::exists(schemeFile(dir, QString::fromLatin1(kLight))))
            return dir;
    }
    return QString::fromLatin1(kEmbedded);
}

// Every scheme SHADOW ships of its own (the generated Obsidian/Daylight pair
// and the standalone built-ins) has a "Shadow" file-name prefix; they are
// grouped ahead of the stock QTermWidget schemes and shown by their
// Description.
bool isShadow(const QString &name)
{
    return name.startsWith(QLatin1String("Shadow"));
}

// Sort rank: Obsidian first, Daylight second, the other SHADOW schemes next
// (alphabetically), then everything QTermWidget ships.
int rank(const QString &name)
{
    if (name == QLatin1String(kDark))
        return 0;
    if (name == QLatin1String(kLight))
        return 1;
    return isShadow(name) ? 2 : 3;
}

} // namespace

void registerSchemes()
{
    if (!g_directory.isEmpty())
        return;
    g_directory = findDirectory();
    QTermWidget::addCustomColorSchemeDir(g_directory);
}

QString directory()
{
    return g_directory;
}

QStringList available()
{
    QStringList names = QTermWidget::availableColorSchemes();
    names.removeDuplicates();
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        const int ra = rank(a), rb = rank(b);
        if (ra != rb)
            return ra < rb;
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return names;
}

QString label(const QString &name)
{
    if (isShadow(name) && !g_directory.isEmpty()) {
        // The display name is the scheme file's own description. (QSettings
        // presents the keys of an INI file's [General] section without a group.)
        const QSettings file(schemeFile(g_directory, name), QSettings::IniFormat);
        const QString description = file.value(QStringLiteral("Description")).toString();
        if (!description.isEmpty())
            return description;
    }
    return name;
}

QString automatic(bool dark)
{
    return QString::fromLatin1(dark ? kDark : kLight);
}

QString resolve(const QString &profileValue, bool dark)
{
    if (profileValue.isEmpty() || profileValue == QLatin1String("auto"))
        return automatic(dark);
    if (!QTermWidget::availableColorSchemes().contains(profileValue))
        return automatic(dark);
    return profileValue;
}

QJsonObject toJson()
{
    QJsonArray list;
    const QStringList names = available();
    for (const QString &name : names) {
        list.append(QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("label"), label(name)},
            {QStringLiteral("origin"), isShadow(name) ? QStringLiteral("shadow") : QStringLiteral("qtermwidget")},
        });
    }
    return QJsonObject{
        {QStringLiteral("shadowDirectory"), g_directory},
        {QStringLiteral("shadowEmbeddedFallback"), g_directory == QLatin1String(kEmbedded)},
        {QStringLiteral("automatic"),
         QJsonObject{
             {QStringLiteral("dark"), automatic(true)},
             {QStringLiteral("light"), automatic(false)},
         }},
        {QStringLiteral("shadowAvailable"),
         names.contains(QString::fromLatin1(kDark)) && names.contains(QString::fromLatin1(kLight))},
        {QStringLiteral("schemes"), list},
    };
}

} // namespace Schemes
