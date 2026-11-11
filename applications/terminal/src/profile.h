// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QSettings;

// A named set of terminal settings: what to run and how it looks.
struct Profile
{
    QString id;                                             // stable key in the settings file
    QString name;

    // What the terminal runs:
    //   "login-shell"  the user's login shell from the passwd database
    //   "shell"        the shell in 'shell' (one of /etc/shells)
    //   "custom"       the command line in 'command' (argument vector, no shell)
    QString commandMode = QStringLiteral("login-shell");
    QString shell;
    QString command;
    bool loginShell = false;                                // pass -l to the shell

    // Where a new terminal starts: "inherit" (directory of the active
    // terminal), "home" or "custom" ('workingDir').
    QString workingDirMode = QStringLiteral("inherit");
    QString workingDir;

    QString fontFamily;                                     // monospace family
    int fontSize = 11;                                      // points
    QString colorScheme = QStringLiteral("auto");           // "auto" follows the system appearance
    QString cursorShape = QStringLiteral("block");          // block | underline | ibeam
    bool cursorBlink = true;
    int scrollbackLines = 10000;                            // -1 unlimited, 0 none
    QString bell = QStringLiteral("visual");                // none | visual | sound
    int opacity = 100;                                      // percent, 30–100
    bool holdOnExit = false;                                // keep the pane when the shell exits

    static Profile builtinDefault();
    static QStringList commandModes();
    static QStringList workingDirModes();
    static QStringList cursorShapes();
    static QStringList bellModes();
    static constexpr int MinFontSize = 6;
    static constexpr int MaxFontSize = 72;
    static constexpr int MinOpacity = 30;
    static constexpr int MaxScrollback = 1000000;

    // Bring every field into its valid range (used after loading and editing).
    void sanitize();

    // The program and arguments this profile starts, resolved against the
    // system as it is now. On failure returns an empty list and sets *error.
    // *note is set when a substitution was made (configured shell missing).
    QStringList resolveCommand(QString *error, QString *note = nullptr) const;
    // Short description of what is run, for lists: "Bash (login shell)".
    QString commandSummary() const;

    QJsonObject toJson() const;
    void load(QSettings &settings);     // from the current group
    void save(QSettings &settings) const;

    bool operator==(const Profile &other) const = default;
};

// The user's profiles, stored in terminal.conf:
//
//   [Profiles]            Default=<id>   Order=<id>, <id>, ...
//   [Profile_<id>]        one section per profile
//
// There is always at least one profile; without a settings file that is the
// built-in "Default".
class ProfileManager : public QObject
{
    Q_OBJECT
public:
    static ProfileManager *instance();

    QList<Profile> profiles() const { return m_profiles; }
    QString defaultId() const { return m_defaultId; }
    Profile defaultProfile() const;
    // The profile with this id; the default profile if there is none.
    Profile profile(const QString &id) const;
    bool contains(const QString &id) const;
    // Profile by display name (for --profile); empty id if not found.
    Profile byName(const QString &name) const;

    // Replace all profiles (from the editor) and persist them.
    void setProfiles(const QList<Profile> &profiles, const QString &defaultId);

    static QString newId();
    static QString uniqueName(const QList<Profile> &profiles, const QString &base);

    // Data behind "--dump profiles".
    QJsonObject toJson() const;

Q_SIGNALS:
    void changed();

private:
    explicit ProfileManager(QObject *parent = nullptr);
    void load();

    QList<Profile> m_profiles;
    QString m_defaultId;
};
