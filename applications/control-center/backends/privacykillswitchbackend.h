// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantMap>

// Privacy page: the network kill switch and the panic (docs/security/kill-switch.md).
//
// Reading (no privilege):
//   /run/shadow/killswitch.json    the state in this boot, written by
//                                  shadow-killswitch (root, 0644) after every
//                                  change and every root "status"
//   /run/shadow/panic.json         the panic record, written by shadow-panic
//   /etc/shadow-os/killswitch.json the mode after a restart (0644); used when
//                                  the state file does not say it
//   /etc/shadow-os/panic.json      the panic options (optional; missing or
//                                  broken means "all true", as in shadow-panic)
//   /run/shadow-anon-state.json    whether Hide Me engaged the kill switch
//   session bus org.kde.kglobalaccel  the key bound to the shell's "panic"
//                                  action (component shadow-horizon)
// The two state directories are watched; files are replaced atomically by
// the tools, so the directories (not the files) are what is watched, plus a
// slow timer as a safety net.
//
// The rule of the contract is applied here as in the shell's status module:
// only "active && loaded && no error" in a sound document means protected. A
// document that cannot be read, has a field of the wrong type or contradicts
// itself is an error that is shown, never "protected".
//
// Live check: "pkexec shadow-security-status --section killswitch,panic"
// (polkit action org.shadow.security.read-status: no password for an active
// local session) says whether the tables are in the kernel right now. It runs
// once when the page opens and on request; its answer is shown next to the
// recorded state, and a disagreement is reported as an error.
//
// Changing (polkit, administrator; the authorisation is kept a short time):
//   pkexec /usr/sbin/shadow-killswitch set MODE --lan|--no-lan [--runtime]
//   pkexec /usr/sbin/shadow-killswitch reassert
//   pkexec /usr/sbin/shadow-panic release           (administrator every time)
// shadow-panic has no command that changes its options, so they are shown
// read-only.
//
// Test hooks (development only): SHADOW_KILLSWITCH_TOOL, SHADOW_PANIC_TOOL,
// SHADOW_SECURITY_STATUS_TOOL replace the program paths; SHADOW_NETGUARD_RUN_DIR
// (default /run/shadow), SHADOW_NETGUARD_CONFIG_DIR (default /etc/shadow-os)
// and SHADOW_ANON_STATE (default /run/shadow-anon-state.json) the files.
class KillSwitchBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool toolInstalled READ toolInstalled NOTIFY changed)
    Q_PROPERTY(bool panicToolInstalled READ panicToolInstalled NOTIFY changed)
    Q_PROPERTY(QString toolPath READ toolPath CONSTANT)
    Q_PROPERTY(QString statusFile READ statusFile CONSTANT)
    Q_PROPERTY(QString configFile READ configFile CONSTANT)
    Q_PROPERTY(QString panicConfigFile READ panicConfigFile CONSTANT)
    // See toJson() / docs/applications/control-center: kill switch fields.
    Q_PROPERTY(QVariantMap killSwitch READ killSwitch NOTIFY changed)
    Q_PROPERTY(QVariantMap panic READ panic NOTIFY changed)
    // {state: "unknown"|"checking"|"done"|"failed", checked (ISO time),
    //  killSwitchTable/panicTable: true|false|null, reason, mismatch}
    Q_PROPERTY(QVariantMap liveCheck READ liveCheck NOTIFY liveCheckChanged)
    // {keys: "Ctrl+Alt+Shift+Esc" or "", source: "session"|"default", note}
    Q_PROPERTY(QVariantMap panicShortcut READ panicShortcut NOTIFY shortcutChanged)
    // Whether the next change is saved for start-up (no --runtime). Follows
    // the state (a --runtime mode in force means false) until the user sets it.
    Q_PROPERTY(bool keepAfterRestart READ keepAfterRestart WRITE setKeepAfterRestart NOTIFY keepAfterRestartChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyAction READ busyAction NOTIFY busyChanged)

public:
    explicit KillSwitchBackend(QObject *parent = nullptr);

    static QString defaultPanicKeys();

    bool ready() const { return m_ready; }
    bool toolInstalled() const;
    bool panicToolInstalled() const;
    QString toolPath() const;
    QString panicToolPath() const;
    QString statusFile() const;
    QString configFile() const;
    QString panicStatusFile() const;
    QString panicConfigFile() const;
    QVariantMap killSwitch() const { return m_killSwitch; }
    QVariantMap panic() const { return m_panic; }
    QVariantMap liveCheck() const { return m_liveCheck; }
    QVariantMap panicShortcut() const { return m_shortcut; }
    bool keepAfterRestart() const { return m_keepAfterRestart; }
    void setKeepAfterRestart(bool keep);
    bool busy() const { return !m_busyAction.isEmpty(); }
    QString busyAction() const { return m_busyAction; }

    // Re-reads every file (synchronous: a few small files) and the shortcut.
    Q_INVOKABLE void refresh();
    // Asks shadow-security-status whether the tables are in the kernel.
    Q_INVOKABLE void checkLive();
    // shadow-killswitch set MODE with the LAN setting; saved for start-up
    // when keepAfterRestart, else --runtime.
    Q_INVOKABLE void setMode(const QString &mode);
    Q_INVOKABLE void setLan(bool lan);
    // shadow-killswitch reassert (load the mode in force again).
    Q_INVOKABLE void reassert();
    // shadow-panic release.
    Q_INVOKABLE void releasePanic();
    // True when changing from the mode in force to 'mode' closes connections
    // (shadow-killswitch flushes when it engages or a mode becomes stricter).
    Q_INVOKABLE bool closesConnections(const QString &mode, bool lan) const;

    // True once the asynchronous parts (live check, shortcut) have answered;
    // used by --dump.
    bool settled() const;
    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();
    void liveCheckChanged();
    void shortcutChanged();
    void keepAfterRestartChanged();
    void busyChanged();
    void failed(const QString &message);
    void succeeded(const QString &message);

private:
    void load();
    void watch();
    void queryShortcut();
    void runTool(const QString &action, const QString &tool, const QStringList &arguments, const QString &successMessage);
    void runSet(const QString &mode, bool lan, bool persist);

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_safetyNet;
    bool m_ready = false;
    QVariantMap m_killSwitch;
    QVariantMap m_panic;
    QVariantMap m_liveCheck;
    QVariantMap m_shortcut;
    bool m_keepAfterRestart = true;
    bool m_keepTouched = false;          // the user chose; do not follow the state
    int m_lastRuntimeOverride = -1;      // -1: no state seen yet
    QString m_busyAction;
    int m_liveGeneration = 0;
    bool m_shortcutDone = false;
};
