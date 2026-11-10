// SPDX-License-Identifier: GPL-3.0-or-later
#include "shellsettings.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace {
QSettings settings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("nexus"), QStringLiteral("horizon"));
}
const QStringList kFillModes = {QStringLiteral("crop"), QStringLiteral("fit"), QStringLiteral("stretch"),
                                QStringLiteral("center"), QStringLiteral("tile")};
} // namespace

QString ShellSettings::configPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/nexus/horizon.conf");
}

ShellSettings::ShellSettings(QObject *parent)
    : QObject(parent)
{
    load();
    watch();
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { load(); watch(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { load(); watch(); });
}

void ShellSettings::watch()
{
    const QString path = configPath();
    const QString dir = QFileInfo(path).absolutePath();
    QDir().mkpath(dir);
    if (!m_watcher.directories().contains(dir))
        m_watcher.addPath(dir);
    if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
        m_watcher.addPath(path);
}

void ShellSettings::load()
{
    QSettings s = settings();
    s.sync();

    const QString wallpaper = s.value(QStringLiteral("Desktop/Wallpaper"),
                                      QStringLiteral("/usr/share/backgrounds/nexus/nexus-horizon.jpg")).toString();
    const QString wallpaperLight = s.value(QStringLiteral("Desktop/WallpaperLight"),
                                           QStringLiteral("/usr/share/backgrounds/nexus/nexus-daylight.jpg")).toString();
    QString fill = s.value(QStringLiteral("Desktop/WallpaperFill"), QStringLiteral("crop")).toString();
    if (!kFillModes.contains(fill))
        fill = QStringLiteral("crop");
    if (wallpaper != m_wallpaper || wallpaperLight != m_wallpaperLight || fill != m_wallpaperFill) {
        m_wallpaper = wallpaper;
        m_wallpaperLight = wallpaperLight;
        m_wallpaperFill = fill;
        Q_EMIT wallpaperChanged();
    }

    // QSettings turns a comma-separated value into a list; a single entry stays a string.
    const QStringList pinned = s.value(QStringLiteral("Dock/Pinned")).toStringList();
    QStringList clean;
    for (const QString &id : pinned) {
        const QString t = id.trimmed();
        if (!t.isEmpty() && !clean.contains(t))
            clean << t;
    }
    if (clean != m_dockPinned) {
        m_dockPinned = clean;
        Q_EMIT dockPinnedChanged();
    }

    const bool autoHide = s.value(QStringLiteral("Dock/AutoHide"), false).toBool();
    const int iconSize = qBound(32, s.value(QStringLiteral("Dock/IconSize"), 44).toInt(), 72);
    if (autoHide != m_dockAutoHide || iconSize != m_dockIconSize) {
        m_dockAutoHide = autoHide;
        m_dockIconSize = iconSize;
        Q_EMIT dockChanged();
    }

    const bool c24 = s.value(QStringLiteral("Panel/Clock24h"), true).toBool();
    const bool secs = s.value(QStringLiteral("Panel/ClockSeconds"), false).toBool();
    const bool date = s.value(QStringLiteral("Panel/ClockDate"), true).toBool();
    if (c24 != m_clock24h || secs != m_clockSeconds || date != m_clockDate) {
        m_clock24h = c24;
        m_clockSeconds = secs;
        m_clockDate = date;
        Q_EMIT panelChanged();
    }
}

void ShellSettings::store(const QString &key, const QVariant &value)
{
    QSettings s = settings();
    s.setValue(key, value);
    s.sync();
    watch();
}

void ShellSettings::setWallpaper(const QString &path)
{
    if (path == m_wallpaper) return;
    m_wallpaper = path;
    store(QStringLiteral("Desktop/Wallpaper"), path);
    Q_EMIT wallpaperChanged();
}

void ShellSettings::setWallpaperLight(const QString &path)
{
    if (path == m_wallpaperLight) return;
    m_wallpaperLight = path;
    store(QStringLiteral("Desktop/WallpaperLight"), path);
    Q_EMIT wallpaperChanged();
}

void ShellSettings::setWallpaperFill(const QString &mode)
{
    if (mode == m_wallpaperFill || !kFillModes.contains(mode)) return;
    m_wallpaperFill = mode;
    store(QStringLiteral("Desktop/WallpaperFill"), mode);
    Q_EMIT wallpaperChanged();
}

void ShellSettings::setDockPinned(const QStringList &ids)
{
    if (ids == m_dockPinned) return;
    m_dockPinned = ids;
    store(QStringLiteral("Dock/Pinned"), ids);
    Q_EMIT dockPinnedChanged();
}

void ShellSettings::setDockAutoHide(bool hide)
{
    if (hide == m_dockAutoHide) return;
    m_dockAutoHide = hide;
    store(QStringLiteral("Dock/AutoHide"), hide);
    Q_EMIT dockChanged();
}

void ShellSettings::setDockIconSize(int size)
{
    size = qBound(32, size, 72);
    if (size == m_dockIconSize) return;
    m_dockIconSize = size;
    store(QStringLiteral("Dock/IconSize"), size);
    Q_EMIT dockChanged();
}

void ShellSettings::setClock24h(bool on)
{
    if (on == m_clock24h) return;
    m_clock24h = on;
    store(QStringLiteral("Panel/Clock24h"), on);
    Q_EMIT panelChanged();
}

void ShellSettings::setClockSeconds(bool on)
{
    if (on == m_clockSeconds) return;
    m_clockSeconds = on;
    store(QStringLiteral("Panel/ClockSeconds"), on);
    Q_EMIT panelChanged();
}

void ShellSettings::setClockDate(bool on)
{
    if (on == m_clockDate) return;
    m_clockDate = on;
    store(QStringLiteral("Panel/ClockDate"), on);
    Q_EMIT panelChanged();
}

QVariantMap ShellSettings::toMap() const
{
    return {
        {QStringLiteral("configFile"), configPath()},
        {QStringLiteral("wallpaper"), m_wallpaper},
        {QStringLiteral("wallpaperLight"), m_wallpaperLight},
        {QStringLiteral("wallpaperFill"), m_wallpaperFill},
        {QStringLiteral("dockPinned"), m_dockPinned},
        {QStringLiteral("dockAutoHide"), m_dockAutoHide},
        {QStringLiteral("dockIconSize"), m_dockIconSize},
        {QStringLiteral("clock24h"), m_clock24h},
        {QStringLiteral("clockSeconds"), m_clockSeconds},
        {QStringLiteral("clockDate"), m_clockDate},
    };
}

bool ShellSettings::isReadableFile(const QString &path) const
{
    const QFileInfo info(path);
    return !path.isEmpty() && info.isFile() && info.isReadable();
}
