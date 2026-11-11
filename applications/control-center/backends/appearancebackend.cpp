// SPDX-License-Identifier: GPL-3.0-or-later
#include "appearancebackend.h"

#include "dumpregistry.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

SHADOW_CC_DUMP("appearance", [] { return QJsonValue(AppearanceBackend().toJson()); })

namespace {
// Same store as the shell (desktop/horizon-shell/src/shellsettings.cpp).
QSettings shellSettings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("shadow"), QStringLiteral("horizon"));
}
QSettings appearanceSettings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("shadow"), QStringLiteral("appearance"));
}

const QStringList kFillModes = {QStringLiteral("crop"), QStringLiteral("fit"), QStringLiteral("stretch"),
                                QStringLiteral("center"), QStringLiteral("tile")};

QString toLocalPath(const QString &pathOrUrl)
{
    if (pathOrUrl.startsWith(QLatin1String("file:")))
        return QUrl(pathOrUrl).toLocalFile();
    return pathOrUrl;
}

QString prettyName(const QString &path)
{
    QString base = QFileInfo(path).completeBaseName();
    if (base.startsWith(QLatin1String("shadow-")))
        base = base.mid(6);
    base.replace(QLatin1Char('-'), QLatin1Char(' ')).replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!base.isEmpty())
        base[0] = base.at(0).toUpper();
    return base;
}
} // namespace

AppearanceBackend::AppearanceBackend(QObject *parent)
    : QObject(parent)
{
    load();
    watch();
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { load(); watch(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { load(); watch(); });
}

QString AppearanceBackend::wallpaperDirectory() const
{
    const QString override = qEnvironmentVariable("SHADOW_WALLPAPER_DIR");
    return override.isEmpty() ? QStringLiteral("/usr/share/backgrounds/shadow") : override;
}

QString AppearanceBackend::settingsFile() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/shadow/horizon.conf");
}

QStringList AppearanceBackend::imageNameFilters() const
{
    QStringList patterns;
    const QList<QByteArray> formats = QImageReader::supportedImageFormats();
    for (const QByteArray &format : formats)
        patterns << QStringLiteral("*.") + QString::fromLatin1(format);
    return {tr("Images (%1)").arg(patterns.join(QLatin1Char(' ')))};
}

void AppearanceBackend::watch()
{
    // QSettings replaces the file (rename), which drops a plain file watch —
    // watch the directory too and re-arm after every event.
    const QString path = settingsFile();
    const QString dir = QFileInfo(path).absolutePath();
    QDir().mkpath(dir);
    if (!m_watcher.directories().contains(dir))
        m_watcher.addPath(dir);
    if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
        m_watcher.addPath(path);
    const QString bundled = wallpaperDirectory();
    if (QFileInfo(bundled).isDir() && !m_watcher.directories().contains(bundled))
        m_watcher.addPath(bundled);
}

void AppearanceBackend::load()
{
    QSettings s = shellSettings();
    s.sync();
    // Defaults are the ones of the shell.
    const QString dark = s.value(QStringLiteral("Desktop/Wallpaper"),
                                 QStringLiteral("/usr/share/backgrounds/shadow/shadow-horizon.jpg")).toString();
    const QString light = s.value(QStringLiteral("Desktop/WallpaperLight"),
                                  QStringLiteral("/usr/share/backgrounds/shadow/shadow-daylight.jpg")).toString();
    QString fill = s.value(QStringLiteral("Desktop/WallpaperFill"), QStringLiteral("crop")).toString();
    if (!kFillModes.contains(fill))
        fill = QStringLiteral("crop");
    const bool autoHide = s.value(QStringLiteral("Dock/AutoHide"), false).toBool();
    const int iconSize = qBound(32, s.value(QStringLiteral("Dock/IconSize"), 44).toInt(), 72);
    const bool c24 = s.value(QStringLiteral("Panel/Clock24h"), true).toBool();
    const bool secs = s.value(QStringLiteral("Panel/ClockSeconds"), false).toBool();
    const bool date = s.value(QStringLiteral("Panel/ClockDate"), true).toBool();

    const bool changed = dark != m_wallpaperDark || light != m_wallpaperLight || fill != m_wallpaperFill
                         || autoHide != m_dockAutoHide || iconSize != m_dockIconSize || c24 != m_clock24h
                         || secs != m_clockSeconds || date != m_clockDate;
    m_wallpaperDark = dark;
    m_wallpaperLight = light;
    m_wallpaperFill = fill;
    m_dockAutoHide = autoHide;
    m_dockIconSize = iconSize;
    m_clock24h = c24;
    m_clockSeconds = secs;
    m_clockDate = date;

    loadWallpapers();
    if (changed)
        Q_EMIT shellChanged();
}

void AppearanceBackend::loadWallpapers()
{
    QVariantList list;
    QStringList seen;
    auto add = [&](const QString &path, bool bundled) {
        if (path.isEmpty() || seen.contains(path) || !isReadableImage(path))
            return;
        seen << path;
        list << QVariantMap{
            {QStringLiteral("path"), path},
            {QStringLiteral("url"), QUrl::fromLocalFile(path).toString()},
            {QStringLiteral("name"), prettyName(path)},
            {QStringLiteral("bundled"), bundled},
        };
    };

    QStringList nameFilters;
    const QList<QByteArray> formats = QImageReader::supportedImageFormats();
    for (const QByteArray &format : formats)
        nameFilters << QStringLiteral("*.") + QString::fromLatin1(format);
    const QDir dir(wallpaperDirectory());
    const QFileInfoList files = dir.entryInfoList(nameFilters, QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &file : files)
        add(file.absoluteFilePath(), true);

    // Images the user chose from elsewhere stay in the grid while they are in
    // use, and the ones chosen earlier are remembered by this page only.
    add(m_wallpaperDark, false);
    add(m_wallpaperLight, false);
    QSettings s = shellSettings();
    const QStringList recent = s.value(QStringLiteral("ControlCenter/RecentWallpapers")).toStringList();
    for (const QString &path : recent)
        add(path, false);

    if (list != m_wallpapers) {
        m_wallpapers = list;
        Q_EMIT wallpapersChanged();
    }
}

void AppearanceBackend::store(const QString &key, const QVariant &value)
{
    QSettings s = shellSettings();
    s.setValue(key, value);
    s.sync();
    if (s.status() != QSettings::NoError)
        Q_EMIT failed(tr("%1 could not be written.").arg(settingsFile()));
    load();
    watch();
}

bool AppearanceBackend::isReadableImage(const QString &path) const
{
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable())
        return false;
    QImageReader reader(path);
    return reader.canRead();
}

QString AppearanceBackend::fileUrl(const QString &path) const
{
    return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
}

QSize AppearanceBackend::imageSize(const QString &path) const
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (reader.transformation() & QImageIOHandler::TransformationRotate90)
        size.transpose();
    return size;
}

bool AppearanceBackend::setWallpaper(const QString &pathOrUrl, bool light)
{
    const QString path = QFileInfo(toLocalPath(pathOrUrl)).absoluteFilePath();
    if (!isReadableImage(path)) {
        Q_EMIT failed(tr("“%1” is not an image that can be shown as wallpaper.").arg(QFileInfo(path).fileName()));
        return false;
    }
    if (!path.startsWith(wallpaperDirectory() + QLatin1Char('/'))) {
        QSettings s = shellSettings();
        QStringList recent = s.value(QStringLiteral("ControlCenter/RecentWallpapers")).toStringList();
        recent.removeAll(path);
        recent.prepend(path);
        while (recent.size() > 8)
            recent.removeLast();
        s.setValue(QStringLiteral("ControlCenter/RecentWallpapers"), recent);
    }
    store(light ? QStringLiteral("Desktop/WallpaperLight") : QStringLiteral("Desktop/Wallpaper"), path);
    return true;
}

void AppearanceBackend::setWallpaperFill(const QString &mode)
{
    if (kFillModes.contains(mode) && mode != m_wallpaperFill)
        store(QStringLiteral("Desktop/WallpaperFill"), mode);
}

void AppearanceBackend::setDockAutoHide(bool hide)
{
    if (hide != m_dockAutoHide)
        store(QStringLiteral("Dock/AutoHide"), hide);
}

void AppearanceBackend::setDockIconSize(int size)
{
    size = qBound(32, size, 72);
    if (size != m_dockIconSize)
        store(QStringLiteral("Dock/IconSize"), size);
}

void AppearanceBackend::setClock24h(bool on)
{
    if (on != m_clock24h)
        store(QStringLiteral("Panel/Clock24h"), on);
}

void AppearanceBackend::setClockSeconds(bool on)
{
    if (on != m_clockSeconds)
        store(QStringLiteral("Panel/ClockSeconds"), on);
}

void AppearanceBackend::setClockDate(bool on)
{
    if (on != m_clockDate)
        store(QStringLiteral("Panel/ClockDate"), on);
}

QJsonObject AppearanceBackend::toJson() const
{
    QSettings a = appearanceSettings();
    a.beginGroup(QStringLiteral("Appearance"));
    QJsonArray wallpapers;
    for (const QVariant &w : m_wallpapers)
        wallpapers << QJsonObject::fromVariantMap(w.toMap());
    return {
        {QStringLiteral("appearanceFile"),
         QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/shadow/appearance.conf")},
        {QStringLiteral("colorScheme"), a.value(QStringLiteral("ColorScheme"), QStringLiteral("dark")).toString()},
        {QStringLiteral("accent"), a.value(QStringLiteral("Accent"), QStringLiteral("cyan")).toString()},
        {QStringLiteral("reduceMotion"), a.value(QStringLiteral("ReduceMotion"), false).toBool()},
        {QStringLiteral("transparency"), a.value(QStringLiteral("Transparency"), true).toBool()},
        {QStringLiteral("fontScale"), a.value(QStringLiteral("FontScale"), 1.0).toDouble()},
        {QStringLiteral("shellFile"), settingsFile()},
        {QStringLiteral("wallpaperDirectory"), wallpaperDirectory()},
        {QStringLiteral("wallpapers"), wallpapers},
        {QStringLiteral("wallpaperDark"), m_wallpaperDark},
        {QStringLiteral("wallpaperLight"), m_wallpaperLight},
        {QStringLiteral("wallpaperFill"), m_wallpaperFill},
        {QStringLiteral("dockAutoHide"), m_dockAutoHide},
        {QStringLiteral("dockIconSize"), m_dockIconSize},
        {QStringLiteral("clock24h"), m_clock24h},
        {QStringLiteral("clockSeconds"), m_clockSeconds},
        {QStringLiteral("clockDate"), m_clockDate},
    };
}
