// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

// How this run of the installer was started. Filled in once by main() before
// any back-end object exists; read-only afterwards.
struct Options
{
    // The installation engine. Production: /usr/sbin/shadow-installer-engine,
    // run as root through pkexec (polkit action org.shadow.installer.run).
    QString enginePath;
    bool customEngine = false;          // --engine / $SHADOW_INSTALLER_ENGINE was given

    // TEST MODE: the hardware description comes from this file instead of the
    // engine's probe. Nothing can be installed in this mode (Installer refuses
    // in C++, independent of what the UI offers).
    QString probeFile;
    QString presetFile;                 // --preset: pre-fill the wizard (test mode only)
    QString startStep;                  // --step: open this wizard step (test mode only)
    QString simulateScenario;           // --simulate: play the test engine's event stream (test mode only)

    QString profilesDir;                // tool profile directory
    bool allowInstalledSystem = false;  // --allow-installed-system
    bool liveSession = false;           // /run/shadow/live-session exists
    bool runningAsRoot = false;         // effective uid 0: the engine is started directly, without pkexec

    bool testMode() const { return !probeFile.isEmpty(); }

    static Options &instance();

    static QString defaultEnginePath() { return QStringLiteral("/usr/sbin/shadow-installer-engine"); }
    static QString defaultProfilesDir() { return QStringLiteral("/usr/share/shadow/arsenal/profiles"); }
    static QString liveSessionMarker() { return QStringLiteral("/run/shadow/live-session"); }
    static QString pkexecPath() { return QStringLiteral("/usr/bin/pkexec"); }
};
