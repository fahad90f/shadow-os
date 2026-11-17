// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>

// Backend for the Wi-Fi Security Audit page.
//
// Drives a privileged helper (wifi-audit-helper, run via pkexec) that:
//   1. Puts the wireless adapter into monitor mode (airmon-ng).
//   2. Captures a WPA/WPA2 handshake from the target network (airodump-ng +
//      aireplay-ng deauth burst to force a reconnection).
//   3. Tests the handshake against a dictionary of passwords (aircrack-ng).
//   4. Restores the adapter and NetworkManager when done.
//
// Structured STATUS:<phase> lines on stdout drive the exposed properties.
// A separate nmcli scan gives the network list for the target selector.
class WifiAuditBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // "idle" | "monitor" | "scan" | "capture" | "deauth" | "cracking"
    // | "found" | "not_found" | "error" | "cleanup"
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(int  progress READ progress NOTIFY changed)
    Q_PROPERTY(int  total    READ total    NOTIFY changed)
    // The discovered password (non-empty only when phase == "found").
    Q_PROPERTY(QString result       READ result       NOTIFY changed)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY changed)
    // Live log lines from the helper (most recent last).
    Q_PROPERTY(QStringList log READ log NOTIFY logChanged)
    // Network list from nmcli (after scanNetworks() completes).
    Q_PROPERTY(QVariantList networks  READ networks  NOTIFY networksChanged)
    Q_PROPERTY(bool         scanBusy  READ scanBusy  NOTIFY scanBusyChanged)
    // Installed wordlist under /usr/share/shadow-arsenal.
    Q_PROPERTY(QString builtinWordlist READ builtinWordlist CONSTANT)

public:
    explicit WifiAuditBackend(QObject *parent = nullptr);
    ~WifiAuditBackend() override;

    QString     phase()        const { return m_phase; }
    bool        running()      const { return m_running; }
    int         progress()     const { return m_progress; }
    int         total()        const { return m_total; }
    QString     result()       const { return m_result; }
    QString     errorMessage() const { return m_error; }
    QStringList log()          const { return m_log; }
    QVariantList networks()    const { return m_networks; }
    bool        scanBusy()     const { return m_scanBusy; }
    QString     builtinWordlist() const;

    // Start a dictionary audit against the given target.
    // channel: "0" means unknown / let the tool discover it.
    // wordlist: empty means use builtinWordlist().
    Q_INVOKABLE void start(const QString &bssid,
                           const QString &ssid,
                           const QString &channel,
                           const QString &wordlist);
    // Abort a running audit and restore the adapter.
    Q_INVOKABLE void stop();
    // Refresh the nearby network list via nmcli (non-privileged).
    Q_INVOKABLE void scanNetworks();

Q_SIGNALS:
    void changed();
    void logChanged();
    void networksChanged();
    void scanBusyChanged();

private:
    void setPhase(const QString &p);
    void appendLog(const QString &line);
    void parseLine(const QString &line);
    void onHelperFinished(int exitCode, QProcess::ExitStatus status);
    void killHelper();

    QString      m_phase    = QStringLiteral("idle");
    bool         m_running  = false;
    int          m_progress = 0;
    int          m_total    = 0;
    QString      m_result;
    QString      m_error;
    QStringList  m_log;
    QVariantList m_networks;
    bool         m_scanBusy = false;

    QProcess *m_helper = nullptr;
    QProcess *m_scan   = nullptr;
    QByteArray m_buf;
};
