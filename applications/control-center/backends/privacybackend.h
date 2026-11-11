// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <memory>

// Privacy page.
//
// * Screen lock: KScreenLocker's kscreenlockerrc, group [Daemon], keys
//   Autolock (bool), Timeout (minutes), LockOnResume (bool), LockGrace
//   (seconds). Read through KConfig, so the system defaults in
//   /etc/xdg/kscreenlockerrc apply until the user changes a value; written to
//   ~/.config/kscreenlockerrc and applied by calling
//   org.kde.screensaver /ScreenSaver configure on the session bus (the locker
//   lives inside KWin). Whether that service exists is reported.
// * History: ~/.local/share/recently-used.xbel (the freedesktop list of
//   recently used files, counted by its <bookmark> elements),
//   ~/.local/share/RecentDocuments (KDE applications' list) and the launcher's
//   usage counters ~/.config/shadow/horizon-usage.conf ([Launches]).
// * Clean-up: sizes are measured on disk (lstat, symlinks not followed) in a
//   worker thread: the trash (~/.local/share/Trash), the thumbnail cache
//   (~/.cache/thumbnails) and the user's own regular files in /tmp and
//   /var/tmp that have not been modified for 24 hours.
// * Network identity: the effective values of NetworkManager's configuration
//   (NetworkManager.conf and conf.d directories, merged in NetworkManager's
//   order) for wifi.scan-rand-mac-address, wifi.cloned-mac-address,
//   ethernet.cloned-mac-address and ipv6.ip6-privacy, each with the file it
//   comes from. Read-only. Sections limited to particular devices
//   ([device-…] / [connection-…] with match-device) are not evaluated.
//
// There is no location service in Shadow OS, so the page has no such section.
class PrivacyBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // ---- screen lock
    Q_PROPERTY(bool lockConfigurable READ lockConfigurable CONSTANT)
    Q_PROPERTY(bool lockerRunning READ lockerRunning NOTIFY lockChanged)
    Q_PROPERTY(bool autolock READ autolock NOTIFY lockChanged)
    Q_PROPERTY(int lockTimeoutMinutes READ lockTimeoutMinutes NOTIFY lockChanged)
    Q_PROPERTY(bool lockOnResume READ lockOnResume NOTIFY lockChanged)
    Q_PROPERTY(int lockGraceSeconds READ lockGraceSeconds NOTIFY lockChanged)
    // ---- history
    Q_PROPERTY(int recentFilesCount READ recentFilesCount NOTIFY historyChanged)
    Q_PROPERTY(bool recentFilesPresent READ recentFilesPresent NOTIFY historyChanged)
    Q_PROPERTY(int launcherUsageCount READ launcherUsageCount NOTIFY historyChanged)
    Q_PROPERTY(QString recentFilesPath READ recentFilesPath CONSTANT)
    Q_PROPERTY(QString launcherUsagePath READ launcherUsagePath CONSTANT)
    // ---- clean-up: [{id, bytes, files, locations: [string]}]; id: trash | thumbnails | temporary
    Q_PROPERTY(QVariantList cleanup READ cleanup NOTIFY cleanupChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY cleanupChanged)
    Q_PROPERTY(QString cleaning READ cleaning NOTIFY cleanupChanged)
    // ---- network identity
    Q_PROPERTY(bool networkManagerInstalled READ networkManagerInstalled CONSTANT)
    Q_PROPERTY(QVariantList macSettings READ macSettings NOTIFY networkChanged)   // [{key, value, source}]
    Q_PROPERTY(QString shadowPrivacyFile READ shadowPrivacyFile NOTIFY networkChanged)

public:
    explicit PrivacyBackend(QObject *parent = nullptr);
    ~PrivacyBackend() override;

    bool lockConfigurable() const;
    bool lockerRunning() const { return m_lockerRunning; }
    bool autolock() const { return m_autolock; }
    int lockTimeoutMinutes() const { return m_timeout; }
    bool lockOnResume() const { return m_lockOnResume; }
    int lockGraceSeconds() const { return m_grace; }

    int recentFilesCount() const { return m_recentCount; }
    bool recentFilesPresent() const { return m_recentPresent; }
    int launcherUsageCount() const { return m_usageCount; }
    QString recentFilesPath() const;
    QString launcherUsagePath() const;

    QVariantList cleanup() const { return m_cleanup; }
    bool scanning() const { return m_scanning; }
    QString cleaning() const { return m_cleaning; }

    bool networkManagerInstalled() const;
    QVariantList macSettings() const { return m_macSettings; }
    QString shadowPrivacyFile() const { return m_shadowPrivacyFile; }

    Q_INVOKABLE void setAutolock(bool on);
    Q_INVOKABLE void setLockTimeoutMinutes(int minutes);
    Q_INVOKABLE void setLockOnResume(bool on);
    Q_INVOKABLE void setLockGraceSeconds(int seconds);

    Q_INVOKABLE void clearRecentFiles();
    Q_INVOKABLE void clearLauncherUsage();

    Q_INVOKABLE void scan();
    Q_INVOKABLE void clean(const QString &id);

    Q_INVOKABLE void refresh();
    QJsonObject toJson() const;

Q_SIGNALS:
    void lockChanged();
    void historyChanged();
    void cleanupChanged();
    void networkChanged();
    void failed(const QString &message);
    void succeeded(const QString &message);

private:
    void loadLock();
    void writeLock(const char *key, const QVariant &value);
    void loadHistory();
    void loadNetwork();
    void startWorker(const QString &cleanId);

    bool m_lockerRunning = false;
    bool m_autolock = true;
    int m_timeout = 5;
    bool m_lockOnResume = true;
    int m_grace = 5;
    int m_recentCount = 0;
    bool m_recentPresent = false;
    int m_usageCount = 0;
    QVariantList m_cleanup;
    bool m_scanning = false;
    QString m_cleaning;
    QVariantList m_macSettings;
    QString m_shadowPrivacyFile;
    std::shared_ptr<bool> m_alive;
};
