// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSettings>
#include <QString>

// A saved SSH destination. Deliberately has no field for a password or a
// passphrase: authentication happens inside the terminal, by ssh itself.
struct SshTarget
{
    QString name;           // label in the list; defaults to user@host
    QString host;
    QString user;
    int port = 22;
    QString identityFile;
    QString jumpHost;

    QString label() const;          // name, or user@host[:port]
    QString destination() const;    // user@host[:port]
    QJsonObject toJson() const;
    bool operator==(const SshTarget &other) const = default;
};

// Everything NEXUS Terminal persists, in ~/.config/nexus/terminal.conf
// (QSettings native format; /etc/xdg/nexus/terminal.conf supplies system
// defaults). Profiles and key bindings have their own front-ends
// (ProfileManager, Shortcuts) but live in the same file.
class Settings : public QObject
{
    Q_OBJECT
public:
    static Settings *instance();

    QString filePath() const;
    // The underlying store, for the front-ends named above.
    QSettings &store() { return m_settings; }

    // ---- window ---------------------------------------------------------
    bool compactMode() const;               // menu bar hidden
    void setCompactMode(bool on);
    bool showStatusBar() const;
    void setShowStatusBar(bool on);
    bool showScrollBar() const;
    void setShowScrollBar(bool on);

    // ---- clipboard ------------------------------------------------------
    bool copyOnSelect() const;
    void setCopyOnSelect(bool on);
    bool confirmMultilinePaste() const;
    void setConfirmMultilinePaste(bool on);
    bool trimPastedNewlines() const;
    void setTrimPastedNewlines(bool on);

    // ---- tabs and session -----------------------------------------------
    bool confirmCloseRunning() const;
    void setConfirmCloseRunning(bool on);
    // "ask" (offer in a banner), "always" or "never".
    QString restoreSession() const;
    void setRestoreSession(const QString &mode);

    // ---- SSH --------------------------------------------------------------
    QList<SshTarget> sshTargets() const;
    void setSshTargets(const QList<SshTarget> &targets);

    // ---- saved session ----------------------------------------------------
    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);
    QJsonObject savedSession() const;       // empty object if there is none
    void setSavedSession(const QJsonObject &session);
    void clearSavedSession();

    // Data behind "--dump settings".
    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();

private:
    explicit Settings(QObject *parent = nullptr);
    bool flag(const char *key, bool fallback) const;
    void setFlag(const char *key, bool on);

    QSettings m_settings;
};
