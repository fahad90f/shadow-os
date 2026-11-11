// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>

// Starting other programs and the pieces around it: opening files with their
// default or a chosen application (KApplicationTrader +
// KIO::ApplicationLauncherJob), a terminal in a folder, and the network
// protocols KIO can reach on this system (KProtocolInfo).
class Launcher : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // shadow-terminal or x-terminal-emulator is installed
    Q_PROPERTY(bool terminalAvailable READ terminalAvailable CONSTANT)
    // [{scheme, label, available, defaultPort, package}] for "Connect to server"
    Q_PROPERTY(QVariantList protocols READ protocols CONSTANT)
    // At least one of them is available; otherwise "Connect to server" is not offered.
    Q_PROPERTY(bool networkAvailable READ networkAvailable CONSTANT)
    // Locations given on the command line, one tab each.
    Q_PROPERTY(QList<QUrl> startLocations READ startLocations CONSTANT)
    // --select <file>
    Q_PROPERTY(QUrl selectOnStart READ selectOnStart CONSTANT)

public:
    static Launcher *instance();
    static Launcher *create(QQmlEngine *, QJSEngine *);

    void setStartup(const QList<QUrl> &locations, const QUrl &select);
    QList<QUrl> startLocations() const { return m_startLocations; }
    QUrl selectOnStart() const { return m_select; }

    bool terminalAvailable() const;
    QVariantList protocols() const;
    bool networkAvailable() const;
    QJsonObject protocolsJson() const;

    // Open a file with the default application of its type. If there is none,
    // openWithRequested() asks the window to show the chooser.
    Q_INVOKABLE void openFile(const QUrl &url, const QString &mimeType);
    // Applications that declare support for the type, the default first:
    // [{id, name, icon, isDefault}]
    Q_INVOKABLE QVariantList applicationsFor(const QString &mimeType) const;
    // Every installed application with a launcher entry.
    Q_INVOKABLE QVariantList allApplications() const;
    Q_INVOKABLE void openWith(const QList<QUrl> &urls, const QString &applicationId,
                              const QString &mimeType = QString(), bool makeDefault = false);

    Q_INVOKABLE void openTerminal(const QUrl &folder);
    Q_INVOKABLE void copyText(const QString &text);

    Q_INVOKABLE bool protocolAvailable(const QString &scheme) const;
    // URL for the "Connect to server" dialog; empty if the input is not valid.
    Q_INVOKABLE QUrl serverUrl(const QString &scheme, const QString &host, int port, const QString &path,
                               const QString &user) const;

Q_SIGNALS:
    void openWithRequested(const QUrl &url, const QString &mimeType);

private:
    explicit Launcher(QObject *parent = nullptr);

    QList<QUrl> m_startLocations;
    QUrl m_select;
};
