// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>

#include <functional>

class QProcess;

// The panic action of the shell (global shortcut "panic"): cut this computer
// off every network, lock the screen and clear the clipboard, in that order.
//
// 1. Network: org.freedesktop.systemd1.Manager.StartUnit("shadow-panic.service",
//    "replace") on the system bus. The unit runs "shadow-panic engage" as
//    root; the polkit rule 49-shadow-panic.rules allows exactly this start
//    without a password for a local, active session of a member of "sudo".
//    Interactive authorisation is NOT allowed: a password dialog in the middle
//    of a panic would delay the cut. If systemd refuses (no such unit, not
//    authorised, no system bus), the shell turns NetworkManager networking
//    off instead (org.freedesktop.NetworkManager.Enable(false), allowed for the
//    active session) and says so: that is not a firewall cut.
// 2. Screen: org.freedesktop.ScreenSaver.Lock on the session bus (KWin's
//    screen locker); if that fails, logind's Session.Lock on the caller's
//    session. Skipped when /etc/shadow-os/panic.json has "lock_session": false
//    (shadow-panic then does not lock either).
// 3. Clipboard: "wl-copy --clear" and "wl-copy --primary --clear"
//    (wl-clipboard), then "wl-paste --list-types" must answer "Nothing is
//    copied". wl-clipboard uses the wlr data-control protocol, which KWin
//    offers without keyboard focus, so this works while another window has
//    the focus and while the screen is locked. Skipped with
//    "clear_clipboard": false.
//
// The three requests are sent at once; none waits for another. Whether the
// network really is cut is what shadow-panic writes to /run/shadow/panic.json,
// which the status module shows (the panic banner); this object only knows
// what it asked for and what was answered.
//
// Release: "pkexec /usr/sbin/shadow-panic release" (administrator, every
// time), plus NetworkManager Enable(true) if the fallback turned networking
// off. A panic asked for while that release still runs wins: a release still
// waiting for the administrator is killed; one the administrator already
// authorised runs as root and cannot be killed, so it is left to end (never
// waited for) and the panic service is started again once it has.
//
// A release made elsewhere (the Control Center's "Reconnect…", sudo
// shadow-panic release) also ends the request: the status module's banner
// passes panic.json's state on (setServiceState()), and a "released" after
// the service has answered this request with "engaged" or "error" makes the
// phase "idle" again.
class Panic : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the shell as the 'panic' context property")
    // "idle"      nothing asked in this session (or released, or dismissed)
    // "starting"  StartUnit sent, no answer yet
    // "started"   systemd accepted the start job; panic.json tells the rest
    // "failed"    the panic service could not be started (see 'error')
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    // The fallback when the service could not be started:
    // "" | "pending" | "networking-off" | "failed"
    Q_PROPERTY(QString fallback READ fallback NOTIFY changed)
    Q_PROPERTY(QString fallbackError READ fallbackError NOTIFY changed)
    // "" | "pending" | "locked" (the screen locker confirmed it) |
    // "requested" (logind accepted the request) | "skipped" | "failed"
    Q_PROPERTY(QString lockResult READ lockResult NOTIFY changed)
    Q_PROPERTY(QString lockError READ lockError NOTIFY changed)
    // "" | "pending" | "cleared" | "skipped" | "failed"
    Q_PROPERTY(QString clipboardResult READ clipboardResult NOTIFY changed)
    Q_PROPERTY(QString clipboardError READ clipboardError NOTIFY changed)
    // When engage() was last called (ms since the epoch; 0: never).
    Q_PROPERTY(qint64 requestedAt READ requestedAt NOTIFY changed)
    Q_PROPERTY(bool releasing READ releasing NOTIFY changed)
    Q_PROPERTY(QString releaseError READ releaseError NOTIFY changed)

public:
    explicit Panic(QObject *parent = nullptr);
    ~Panic() override;

    QString phase() const { return m_phase; }
    QString error() const { return m_error; }
    QString fallback() const { return m_fallback; }
    QString fallbackError() const { return m_fallbackError; }
    QString lockResult() const { return m_lock; }
    QString lockError() const { return m_lockError; }
    QString clipboardResult() const { return m_clipboard; }
    QString clipboardError() const { return m_clipboardError; }
    qint64 requestedAt() const { return m_requestedAt; }
    bool releasing() const { return m_releasing; }
    QString releaseError() const { return m_releaseError; }

    Q_INVOKABLE void engage();
    // 'serviceEngaged': /run/shadow/panic.json records a panic (the status
    // module knows). Then the release goes through pkexec; otherwise only
    // the fallback (if any) is undone.
    Q_INVOKABLE void release(bool serviceEngaged);
    // Forgets a request that is over (failed, or nothing to release).
    Q_INVOKABLE void dismiss();
    // What /run/shadow/panic.json says now, as the status module reads it:
    // "none" | "engaged" | "released" | "error" (the panic banner calls this
    // whenever it changes).
    Q_INVOKABLE void setServiceState(const QString &state);

    struct Config {
        bool lockSession = true;
        bool clearClipboard = true;
        QString problem;        // why the file was not used (defaults then)
    };
    // /etc/shadow-os/panic.json (SHADOW_PANIC_CONFIG overrides the path).
    // A missing or broken file means the defaults: a panic must still work.
    static Config readConfig();
    // horizon-shell --dump panic
    static QJsonObject dump();

Q_SIGNALS:
    void changed();
    // Something the user must know about (the shell shows it as a toast when
    // no panic banner is loaded).
    void failed(const QString &message);
    void releaseFinished(bool ok);

private:
    void startUnit();
    void networkingFallback();
    void setNetworking(bool enabled, const std::function<void(const QString &error)> &done);
    void lockScreen();
    void lockThroughLogind(const QString &screenSaverError);
    void clearClipboard();
    void runClipboardStep(int step);
    void finishRelease(bool ok, const QString &error);
    void withdrawRelease();
    void reassert(bool mayHaveReleased);
    void endIfReleasedElsewhere();
    void set(QString *field, const QString &value);

    QString m_phase = QStringLiteral("idle");
    QString m_error;
    QString m_fallback;
    QString m_fallbackError;
    bool m_fallbackNetworkingOff = false;
    QString m_lock;
    QString m_lockError;
    QString m_clipboard;
    QString m_clipboardError;
    QStringList m_clipboardWarnings;
    bool m_primaryCleared = false;
    QProcess *m_clipboardProcess = nullptr;
    qint64 m_requestedAt = 0;
    bool m_releasing = false;
    QString m_releaseError;
    QProcess *m_releaseProcess = nullptr;
    int m_withdrawnReleases = 0;   // authorised releases a panic withdrew that still run
    QString m_serviceState;        // panic.json's state (setServiceState())
    bool m_serviceAnswered = false;  // the service answered this request: engaged or error
    quint64 m_generation = 0;     // engage() calls; stale answers are ignored
};
