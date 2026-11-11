// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QSize>
#include <QVariantList>

// Desktop side of the Appearance page: wallpaper and the dock/panel options
// of the Shadow Horizon shell.
//
// Everything is stored in ~/.config/shadow/horizon.conf (system default:
// /etc/xdg/shadow/horizon.conf), the file the shell watches — keys are
// documented in desktop/horizon-shell/src/shellsettings.h:
//
//   [Desktop] Wallpaper, WallpaperLight, WallpaperFill
//   [Dock]    AutoHide, IconSize
//   [Panel]   Clock24h, ClockSeconds, ClockDate
//
// Colour scheme, accent, transparency, motion and font scale are not handled
// here: the page binds them to the Appearance singleton of org.shadow.ui.
//
// Bundled wallpapers are the images in /usr/share/backgrounds/shadow. For an
// uninstalled build tree the directory can be overridden with the environment
// variable SHADOW_WALLPAPER_DIR (developer override, e.g. /src/assets/wallpapers).
class AppearanceBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList wallpapers READ wallpapers NOTIFY wallpapersChanged)
    Q_PROPERTY(QString wallpaperDirectory READ wallpaperDirectory CONSTANT)
    Q_PROPERTY(QString wallpaperDark READ wallpaperDark NOTIFY shellChanged)
    Q_PROPERTY(QString wallpaperLight READ wallpaperLight NOTIFY shellChanged)
    Q_PROPERTY(QString wallpaperFill READ wallpaperFill NOTIFY shellChanged)
    Q_PROPERTY(bool dockAutoHide READ dockAutoHide NOTIFY shellChanged)
    Q_PROPERTY(int dockIconSize READ dockIconSize NOTIFY shellChanged)
    Q_PROPERTY(bool clock24h READ clock24h NOTIFY shellChanged)
    Q_PROPERTY(bool clockSeconds READ clockSeconds NOTIFY shellChanged)
    Q_PROPERTY(bool clockDate READ clockDate NOTIFY shellChanged)
    Q_PROPERTY(QString settingsFile READ settingsFile CONSTANT)
    Q_PROPERTY(QStringList imageNameFilters READ imageNameFilters CONSTANT)

public:
    explicit AppearanceBackend(QObject *parent = nullptr);

    QVariantList wallpapers() const { return m_wallpapers; }
    QString wallpaperDirectory() const;
    QString wallpaperDark() const { return m_wallpaperDark; }
    QString wallpaperLight() const { return m_wallpaperLight; }
    QString wallpaperFill() const { return m_wallpaperFill; }
    bool dockAutoHide() const { return m_dockAutoHide; }
    int dockIconSize() const { return m_dockIconSize; }
    bool clock24h() const { return m_clock24h; }
    bool clockSeconds() const { return m_clockSeconds; }
    bool clockDate() const { return m_clockDate; }
    QString settingsFile() const;
    QStringList imageNameFilters() const;

    // 'light' selects the image of the light appearance (WallpaperLight),
    // otherwise the one of the dark appearance (Wallpaper). The file must be
    // an image Qt can decode; local path or file:// URL.
    Q_INVOKABLE bool setWallpaper(const QString &pathOrUrl, bool light);
    Q_INVOKABLE void setWallpaperFill(const QString &mode);
    Q_INVOKABLE void setDockAutoHide(bool hide);
    Q_INVOKABLE void setDockIconSize(int size);
    Q_INVOKABLE void setClock24h(bool on);
    Q_INVOKABLE void setClockSeconds(bool on);
    Q_INVOKABLE void setClockDate(bool on);
    Q_INVOKABLE bool isReadableImage(const QString &path) const;
    Q_INVOKABLE QString fileUrl(const QString &path) const;
    // Pixel size of an image file (empty size if it cannot be read).
    Q_INVOKABLE QSize imageSize(const QString &path) const;

    QJsonObject toJson() const;

Q_SIGNALS:
    void wallpapersChanged();
    void shellChanged();
    void failed(const QString &message);

private:
    void load();
    void loadWallpapers();
    void store(const QString &key, const QVariant &value);
    void watch();

    QFileSystemWatcher m_watcher;
    QVariantList m_wallpapers;
    QString m_wallpaperDark, m_wallpaperLight, m_wallpaperFill;
    bool m_dockAutoHide = false;
    int m_dockIconSize = 44;
    bool m_clock24h = true, m_clockSeconds = false, m_clockDate = true;
};
