// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>

// Startup applications: XDG autostart entries (Desktop Application Autostart
// Specification).
//
//   user entries     $XDG_CONFIG_HOME/autostart/*.desktop      (add, remove, enable, disable)
//   system entries   <each of $XDG_CONFIG_DIRS>/autostart/*.desktop — normally
//                    /etc/xdg/autostart — which cannot be changed without
//                    administrator rights but can be switched off for this
//                    user by a file of the same name in the user directory.
//
// An entry is disabled with "Hidden=true" (the specification's way) plus
// "X-GNOME-Autostart-enabled=false" (understood by sessions that keep hidden
// entries listed). A system entry is disabled by a user file that is a copy
// of the system file with those keys and "X-NEXUS-Autostart-Override=true";
// enabling it again removes that copy.
class StartupBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(QVariantList applications READ applications NOTIFY applicationsChanged)
    Q_PROPERTY(QString userDirectory READ userDirectory CONSTANT)
    Q_PROPERTY(QStringList systemDirectories READ systemDirectories CONSTANT)

public:
    explicit StartupBackend(QObject *parent = nullptr);

    // Each entry: id (file name), name, comment, icon, command, path (file in
    // effect), enabled, system (exists in a system directory), userFile (a
    // file exists in the user directory), overridden (user file shadows a
    // system file), condition (why the session may skip it; empty if none).
    QVariantList entries() const { return m_entries; }
    // Installed applications that can be added: id, name, comment, icon,
    // command, path. Loaded on demand by loadApplications().
    QVariantList applications() const { return m_applications; }
    QString userDirectory() const;
    QStringList systemDirectories() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void loadApplications();
    Q_INVOKABLE bool setEnabled(const QString &id, bool enabled);
    // Deletes the user's file. For an entry that also exists in a system
    // directory this restores the system default.
    Q_INVOKABLE bool remove(const QString &id);
    Q_INVOKABLE bool addApplication(const QString &desktopFilePath);
    Q_INVOKABLE bool addCommand(const QString &name, const QString &command);
    // Empty if 'command' can be started; otherwise the reason.
    Q_INVOKABLE QString commandProblem(const QString &command) const;

    QJsonObject toJson() const;

Q_SIGNALS:
    void entriesChanged();
    void applicationsChanged();
    void failed(const QString &message);

private:
    void watch();

    QFileSystemWatcher m_watcher;
    QTimer m_reload;
    QVariantList m_entries;
    QVariantList m_applications;
};
