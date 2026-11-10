// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>

// Desktop-shell configuration, stored in ~/.config/nexus/horizon.conf with the
// distribution default in /etc/xdg/nexus/horizon.conf.
//
//   [Desktop]
//   Wallpaper=/usr/share/backgrounds/nexus/nexus-horizon.jpg   image shown in dark appearance
//   WallpaperLight=/usr/share/backgrounds/nexus/nexus-daylight.jpg   image shown in light appearance
//   WallpaperFill=crop            crop | fit | stretch | center | tile
//   [Dock]
//   Pinned=org.nexus.files.desktop,org.nexus.terminal.desktop,…   desktop ids, in order
//   AutoHide=false
//   IconSize=44                   32–72
//   [Panel]
//   Clock24h=true
//   ClockSeconds=false
//   ClockDate=true
//
// The file is watched: the Control Center writes it and the shell follows
// immediately. Unknown keys are preserved.
class ShellSettings : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the shell as the 'shellSettings' context property")
    Q_PROPERTY(QString wallpaper READ wallpaper WRITE setWallpaper NOTIFY wallpaperChanged)
    Q_PROPERTY(QString wallpaperLight READ wallpaperLight WRITE setWallpaperLight NOTIFY wallpaperChanged)
    Q_PROPERTY(QString wallpaperFill READ wallpaperFill WRITE setWallpaperFill NOTIFY wallpaperChanged)
    Q_PROPERTY(QStringList dockPinned READ dockPinned WRITE setDockPinned NOTIFY dockPinnedChanged)
    Q_PROPERTY(bool dockAutoHide READ dockAutoHide WRITE setDockAutoHide NOTIFY dockChanged)
    Q_PROPERTY(int dockIconSize READ dockIconSize WRITE setDockIconSize NOTIFY dockChanged)
    Q_PROPERTY(bool clock24h READ clock24h WRITE setClock24h NOTIFY panelChanged)
    Q_PROPERTY(bool clockSeconds READ clockSeconds WRITE setClockSeconds NOTIFY panelChanged)
    Q_PROPERTY(bool clockDate READ clockDate WRITE setClockDate NOTIFY panelChanged)

public:
    explicit ShellSettings(QObject *parent = nullptr);
    static QString configPath();

    QString wallpaper() const { return m_wallpaper; }
    QString wallpaperLight() const { return m_wallpaperLight; }
    QString wallpaperFill() const { return m_wallpaperFill; }
    QStringList dockPinned() const { return m_dockPinned; }
    bool dockAutoHide() const { return m_dockAutoHide; }
    int dockIconSize() const { return m_dockIconSize; }
    bool clock24h() const { return m_clock24h; }
    bool clockSeconds() const { return m_clockSeconds; }
    bool clockDate() const { return m_clockDate; }

    void setWallpaper(const QString &path);
    void setWallpaperLight(const QString &path);
    void setWallpaperFill(const QString &mode);
    void setDockPinned(const QStringList &ids);
    void setDockAutoHide(bool hide);
    void setDockIconSize(int size);
    void setClock24h(bool on);
    void setClockSeconds(bool on);
    void setClockDate(bool on);

    QVariantMap toMap() const;
    // True if 'path' names a readable file (an unset or stale wallpaper path
    // falls back to the built-in gradient instead of an image error).
    Q_INVOKABLE bool isReadableFile(const QString &path) const;

Q_SIGNALS:
    void wallpaperChanged();
    void dockPinnedChanged();
    void dockChanged();
    void panelChanged();

private:
    void load();
    void store(const QString &key, const QVariant &value);
    void watch();

    QFileSystemWatcher m_watcher;
    QString m_wallpaper, m_wallpaperLight, m_wallpaperFill;
    QStringList m_dockPinned;
    bool m_dockAutoHide = false;
    int m_dockIconSize = 44;
    bool m_clock24h = true, m_clockSeconds = false, m_clockDate = true;
};
