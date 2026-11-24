// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>

namespace StatusModule {

// The documents the network guards publish for the desktop, and what they
// mean. Pure functions, so the self test (tests/killswitch-selftest.cpp) can
// feed them anything.
//
//   /run/shadow/killswitch.json   written by shadow-killswitch (root, 0644)
//   /run/shadow/panic.json        written by shadow-panic (root, 0644)
//
// Both formats are described in docs/security/kill-switch.md. The rule of
// that contract is kept here: only "active && loaded && no error" means the
// kill switch protects anything. A file that cannot be read or parsed, a
// field of the wrong type or a document that contradicts itself is never
// read as "protected"; it is an error the user is shown.
namespace NetGuard {

// Larger state files are not read (the real ones are about 1 KiB).
constexpr qint64 kMaxStateFileSize = 256 * 1024;

struct FileRead {
    bool exists = false;
    QByteArray data;
    QString error;          // why an existing file could not be read
};
FileRead readStateFile(const QString &path);

struct KillSwitchState {
    bool present = false;           // the file exists
    bool readable = false;          // it was read and is a JSON object
    QString problem;                // why the document cannot be trusted
    QString mode;                   // "off" | "tor" | "vpn" | "tor-or-vpn"; empty when unknown
    bool active = false;            // mode != off
    bool loaded = false;            // the tool found its table in the kernel
    QString error;                  // the tool's own "error" (null: empty)
    QString since;
    bool lan = false;
    int endpoints = 0;
    bool escapeHatch = false;       // shadow.killswitch=off kept it off in this boot
    bool runtimeOverride = false;
    QString persistentMode;         // the mode after a restart
    qint64 droppedPackets = -1;     // -1: not reported
    QString updated;

    // The documented rule: active, loaded, no error (and a sound document).
    bool isProtected() const;
    // A problem with the document, the tool's error, a mode other than off
    // whose table is not loaded, or "off" with the table still loaded.
    bool hasError() const;
    // Off in this boot only because of the escape hatch, while the saved
    // mode is something else.
    bool bypassed() const;
    // "unknown" (no file) | "off" | "on" | "bypassed" | "error"
    QString state() const;
    // What is wrong, as one sentence for the user; empty without an error.
    QString errorText() const;
    QJsonObject toJson() const;
};

struct PanicState {
    bool present = false;
    bool readable = false;
    QString problem;
    bool engaged = false;
    bool loaded = false;
    bool loadedKnown = false;       // "loaded" was true or false (not null)
    QString error;
    QString since;
    QString released;
    bool clearClipboard = true;     // a missing value means true, as in the tool
    bool lockSession = true;
    QStringList warnings;
    qint64 droppedPackets = -1;

    // Every connection is cut: engaged, the table loaded, no error.
    bool isCut() const;
    bool hasError() const;
    // "none" (no file, or never engaged) | "released" | "engaged" | "error"
    QString state() const;
    QString errorText() const;
    QJsonObject toJson() const;
};

KillSwitchState parseKillSwitch(const FileRead &file);
PanicState parsePanic(const FileRead &file);

} // namespace NetGuard

// The network kill switch and panic state, as the root tools publish it.
//
// Source: the directory /run/shadow (SHADOW_STATUS_RUN_DIR overrides it for
// tests) is watched with QFileSystemWatcher — the directory itself, so files
// that are created, atomically replaced (renamed over) or removed are
// noticed, and the two files, for writes in place. While the directory does
// not exist its nearest existing parent is watched, so its creation is
// noticed too. Nothing is polled unless watching fails (then every 5 s).
// Changes are coalesced for 50 ms and only real changes are signalled.
//
// Read only: changing the kill switch needs an administrator (Control
// Center, pkexec shadow-killswitch); engaging and releasing a panic is the
// shell core's job (src/panic.h).
//
// "Available" means killswitch.json exists and is a JSON object. The
// properties below are what QML shows; see NetGuard above for the rules.
class KillSwitchBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(bool watching READ watching NOTIFY changed)
    // ---- kill switch
    Q_PROPERTY(bool present READ present NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool isProtected READ isProtected NOTIFY changed)
    Q_PROPERTY(bool hasError READ hasError NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(bool indicatorVisible READ indicatorVisible NOTIFY changed)
    Q_PROPERTY(QString since READ since NOTIFY changed)
    Q_PROPERTY(bool lan READ lan NOTIFY changed)
    Q_PROPERTY(int endpoints READ endpoints NOTIFY changed)
    Q_PROPERTY(bool escapeHatch READ escapeHatch NOTIFY changed)
    Q_PROPERTY(QString persistentMode READ persistentMode NOTIFY changed)
    Q_PROPERTY(bool runtimeOverride READ runtimeOverride NOTIFY changed)
    // ---- panic
    Q_PROPERTY(bool panicPresent READ panicPresent NOTIFY changed)
    Q_PROPERTY(QString panicState READ panicState NOTIFY changed)
    Q_PROPERTY(bool panicEngaged READ panicEngaged NOTIFY changed)
    Q_PROPERTY(bool panicCut READ panicCut NOTIFY changed)
    Q_PROPERTY(QString panicErrorText READ panicErrorText NOTIFY changed)
    Q_PROPERTY(QString panicSince READ panicSince NOTIFY changed)
    Q_PROPERTY(QStringList panicWarnings READ panicWarnings NOTIFY changed)

public:
    explicit KillSwitchBackend(QObject *parent = nullptr);

    QString directory() const { return m_dir; }
    bool watching() const { return m_watching; }

    bool present() const { return m_ks.present; }
    QString state() const { return m_ks.state(); }
    QString mode() const { return m_ks.mode; }
    bool active() const { return m_ks.active; }
    bool loaded() const { return m_ks.loaded; }
    bool isProtected() const { return m_ks.isProtected(); }
    bool hasError() const { return m_ks.present && m_ks.hasError(); }
    QString errorText() const { return m_ks.errorText(); }
    bool indicatorVisible() const;
    QString since() const { return m_ks.since; }
    bool lan() const { return m_ks.lan; }
    int endpoints() const { return m_ks.endpoints; }
    bool escapeHatch() const { return m_ks.escapeHatch; }
    QString persistentMode() const { return m_ks.persistentMode; }
    bool runtimeOverride() const { return m_ks.runtimeOverride; }

    bool panicPresent() const { return m_panic.present; }
    QString panicState() const { return m_panic.state(); }
    bool panicEngaged() const { return m_panic.engaged; }
    bool panicCut() const { return m_panic.isCut(); }
    QString panicErrorText() const { return m_panic.errorText(); }
    QString panicSince() const { return m_panic.since; }
    QStringList panicWarnings() const { return m_panic.warnings; }

    // Reads both files again now.
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void changed();

protected:
    QJsonObject details() const override;

private:
    void scheduleReload();
    void reload();
    void updateWatches();

    QString m_dir;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_poll;
    bool m_watching = false;
    NetGuard::KillSwitchState m_ks;
    NetGuard::PanicState m_panic;
    QJsonObject m_lastKs;
    QJsonObject m_lastPanic;
};

} // namespace StatusModule
