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
//   connection.stable-id ("${CONNECTION}/${BOOT}": a new private address at
//   every boot), ethernet.cloned-mac-address and ipv6.ip6-privacy, each with the file
//   and section it comes from. Read-only. NetworkManager's section precedence
//   is followed: named [connection-…] / [device-…] sections come before the
//   literal [connection] / [device] (later files first), and one without
//   match-device applies to every device, e.g. the [connection-shadow-escape]
//   of /run/NetworkManager/conf.d/93-shadow-escape.conf, which
//   shadow-netprivacy.service writes for a boot with shadow.netprivacy=off
//   (every device keeps its own MAC address; reported as
//   netPrivacyEscapeFile). Sections limited to particular devices (with
//   match-device) are not evaluated.
//
// * Hide Me: runs shadow-anon (distro/base/rootfs/usr/bin/shadow-anon) through
//   pkexec (directly as root) and reads its state record
//   /run/shadow-anon-state.json (ghost_mode, threat_model, transparent_proxy,
//   dns_protected, bt_killed, usb_killed, kernel_hardened, error) and the
//   rotation timer's interval drop-in. shadow-anon exits 0 when done, 3 when
//   a protection could not be switched on or off (the reason on stderr and
//   in "error"), 1 when not root or misused; pkexec itself exits 126 when the
//   authentication was cancelled or refused and 127 when the request was not
//   authorised (or the program cannot be run). A failure is reported with the
//   tool's own last stderr line. Whether the kill switch is in force is NOT
//   read here: the page takes it from KillSwitchBackend (the same file the
//   Network kill switch section shows), so the two can never disagree.
// * OS integrity: shadow-harden, run the same way; it exits 1 with the reason
//   on stderr when something could not be applied.
//
// Test hooks (development only): SHADOW_ANON_TOOL, SHADOW_HARDEN_TOOL (the
// programs), SHADOW_ANON_STATE (the state record), SHADOW_ANON_TIMER_DROPIN
// (the timer drop-in) and SHADOW_NM_CONFIG_ROOT (a directory read in place of
// "/" for NetworkManager's configuration files and /usr/sbin/NetworkManager).
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
    // [{key, section ("connection"|"device"), group (the section the value is
    // from, e.g. "connection-shadow-escape"; empty when unset), set, value, source}]
    Q_PROPERTY(QVariantList macSettings READ macSettings NOTIFY networkChanged)
    Q_PROPERTY(QString shadowPrivacyFile READ shadowPrivacyFile NOTIFY networkChanged)
    // The escape hatch's NetworkManager file when this boot has shadow.netprivacy=off.
    Q_PROPERTY(QString netPrivacyEscapeFile READ netPrivacyEscapeFile NOTIFY networkChanged)
    // ---- Hide Me (shadow-anon)
    Q_PROPERTY(bool ghostMode READ ghostMode NOTIFY anonChanged)
    Q_PROPERTY(QString threatModel READ threatModel NOTIFY anonChanged) // "basic"|"enhanced"|"maximum"
    Q_PROPERTY(bool torRunning READ torRunning NOTIFY anonChanged)
    Q_PROPERTY(bool transparentProxy READ transparentProxy NOTIFY anonChanged)
    Q_PROPERTY(bool dnsProtected READ dnsProtected NOTIFY anonChanged)
    Q_PROPERTY(bool bluetoothKilled READ bluetoothKilled NOTIFY anonChanged)
    Q_PROPERTY(bool usbStorageKilled READ usbStorageKilled NOTIFY anonChanged)
    Q_PROPERTY(bool kernelHardened READ kernelHardened NOTIFY anonChanged)
    Q_PROPERTY(bool autoRotateActive READ autoRotateActive NOTIFY anonChanged)
    Q_PROPERTY(bool anonBusy READ anonBusy NOTIFY anonChanged)
    // The command running now ("ghost-on", "rotate", …; empty when idle).
    Q_PROPERTY(QString anonAction READ anonAction NOTIFY anonChanged)
    Q_PROPERTY(QString anonOutput READ anonOutput NOTIFY anonChanged)
    // The "error" shadow-anon recorded for its last change (also one made by
    // the boot unit or the rotation timer); empty after a change that worked.
    Q_PROPERTY(QString anonError READ anonError NOTIFY anonChanged)
    Q_PROPERTY(bool anonToolInstalled READ anonToolInstalled CONSTANT)
    // New Tor circuits every this many seconds while the timer runs.
    Q_PROPERTY(int autoRotateIntervalSeconds READ autoRotateIntervalSeconds NOTIFY anonChanged)
    // shadow-anon raises anything shorter to this (Tor's NEWNYM spacing).
    Q_PROPERTY(int autoRotateMinimumSeconds READ autoRotateMinimumSeconds CONSTANT)
    // ---- OS Integrity (shadow-harden)
    Q_PROPERTY(bool aideInitialized READ aideInitialized NOTIFY hardenChanged)
    Q_PROPERTY(QString aideLastCheck READ aideLastCheck NOTIFY hardenChanged)
    Q_PROPERTY(int aideChanges READ aideChanges NOTIFY hardenChanged)
    Q_PROPERTY(bool appArmorEnforcing READ appArmorEnforcing NOTIFY hardenChanged)
    Q_PROPERTY(bool auditdRunning READ auditdRunning NOTIFY hardenChanged)
    Q_PROPERTY(bool bootHardened READ bootHardened NOTIFY hardenChanged)
    Q_PROPERTY(bool hardenBusy READ hardenBusy NOTIFY hardenChanged)
    Q_PROPERTY(QString hardenOutput READ hardenOutput NOTIFY hardenChanged)
    Q_PROPERTY(QString hardeningLevel READ hardeningLevel NOTIFY hardenChanged) // standard|hardened|paranoid

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
    QString netPrivacyEscapeFile() const { return m_netPrivacyEscapeFile; }

    bool ghostMode() const { return m_ghostMode; }
    QString threatModel() const { return m_threatModel; }
    bool torRunning() const { return m_torRunning; }
    bool transparentProxy() const { return m_transparentProxy; }
    bool dnsProtected() const { return m_dnsProtected; }
    bool bluetoothKilled() const { return m_btKilled; }
    bool usbStorageKilled() const { return m_usbKilled; }
    bool kernelHardened() const { return m_kernelHardened; }
    bool autoRotateActive() const { return m_autoRotateActive; }
    bool anonBusy() const { return m_anonBusy; }
    QString anonAction() const { return m_anonAction; }
    QString anonOutput() const { return m_anonOutput; }
    QString anonError() const { return m_anonError; }
    bool anonToolInstalled() const;
    int autoRotateIntervalSeconds() const { return m_autoRotateInterval; }
    static int autoRotateMinimumSeconds();

    bool aideInitialized() const { return m_aideInitialized; }
    QString aideLastCheck() const { return m_aideLastCheck; }
    int aideChanges() const { return m_aideChanges; }
    bool appArmorEnforcing() const { return m_appArmorEnforcing; }
    bool auditdRunning() const { return m_auditdRunning; }
    bool bootHardened() const { return m_bootHardened; }
    bool hardenBusy() const { return m_hardenBusy; }
    QString hardenOutput() const { return m_hardenOutput; }
    QString hardeningLevel() const { return m_hardeningLevel; }

    Q_INVOKABLE void setAutolock(bool on);
    Q_INVOKABLE void setLockTimeoutMinutes(int minutes);
    Q_INVOKABLE void setLockOnResume(bool on);
    Q_INVOKABLE void setLockGraceSeconds(int seconds);

    Q_INVOKABLE void clearRecentFiles();
    Q_INVOKABLE void clearLauncherUsage();

    Q_INVOKABLE void scan();
    Q_INVOKABLE void clean(const QString &id);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void refreshAnon();

    // Hide Me actions (all require root; called via pkexec)
    Q_INVOKABLE void rotateIdentity();
    Q_INVOKABLE void enableGhostMode();
    Q_INVOKABLE void disableGhostMode();
    Q_INVOKABLE void setThreatModel(const QString &model);
    Q_INVOKABLE void nukeIdentity();
    Q_INVOKABLE void runLeakTest();
    Q_INVOKABLE void wipeLogsNow();
    Q_INVOKABLE void hardenKernelNow();
    Q_INVOKABLE void killBluetooth(bool kill);
    Q_INVOKABLE void killUsbStorage(bool kill);
    Q_INVOKABLE void enableTransparentProxy(bool enable);
    Q_INVOKABLE void enableDnsProtect(bool enable);
    // New Tor circuits on a timer; intervals below autoRotateMinimumSeconds
    // are raised to it (as shadow-anon does). Never changes the MAC address.
    Q_INVOKABLE void startAutoRotate(int intervalSeconds);
    Q_INVOKABLE void stopAutoRotate();
    // Hide Me's kill switch: shadow-anon kill-switch-on hands over to
    // shadow-killswitch mode "tor" (until the next restart); kill-switch-off
    // puts back the mode that was in force before.
    Q_INVOKABLE void enableKillSwitch(bool enable);

    // OS Integrity actions (all require root; called via pkexec)
    Q_INVOKABLE void refreshHarden();
    Q_INVOKABLE void aideInit();
    Q_INVOKABLE void aideCheck();
    Q_INVOKABLE void enforceAppArmor();
    Q_INVOKABLE void applyBootHardening();
    Q_INVOKABLE void hardenFilesystems();
    Q_INVOKABLE void setHardeningLevel(const QString &level);

    // True when no command runs and the state has been read (used by --dump
    // and the test harness).
    bool settled() const;
    QJsonObject toJson() const;

Q_SIGNALS:
    void lockChanged();
    void historyChanged();
    void cleanupChanged();
    void networkChanged();
    void anonChanged();
    void hardenChanged();
    void failed(const QString &message);
    void succeeded(const QString &message);

private:
    void loadLock();
    void writeLock(const char *key, const QVariant &value);
    void loadHistory();
    void loadNetwork();
    void startWorker(const QString &cleanId);
    void runShadowAnon(const QString &action, const QStringList &args, const QString &successMessage);
    void runShadowHarden(const QStringList &args);

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
    QString m_netPrivacyEscapeFile;
    // Hide Me state
    bool m_ghostMode = false;
    QString m_threatModel = QStringLiteral("basic");
    bool m_torRunning = false;
    bool m_transparentProxy = false;
    bool m_dnsProtected = false;
    bool m_btKilled = false;
    bool m_usbKilled = false;
    bool m_kernelHardened = false;
    bool m_autoRotateActive = false;
    bool m_anonBusy = false;
    QString m_anonAction;
    QString m_anonOutput;
    QString m_anonError;
    int m_autoRotateInterval = 300;
    int m_anonReads = 0;                 // state reads still running
    // OS Integrity state
    bool m_aideInitialized = false;
    QString m_aideLastCheck;
    int m_aideChanges = 0;
    bool m_appArmorEnforcing = false;
    bool m_auditdRunning = false;
    bool m_bootHardened = false;
    QString m_hardeningLevel = QStringLiteral("hardened");
    bool m_hardenBusy = false;
    QString m_hardenOutput;
    int m_hardenReads = 0;
    std::shared_ptr<bool> m_alive;
};
