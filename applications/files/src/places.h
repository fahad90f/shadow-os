// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonArray>
#include <QObject>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>

class KDirWatch;

// The fixed part of the sidebar and what the user adds to it:
//
//   places     Home and the XDG user folders that exist (QStandardPaths),
//              the Trash and the file system root
//   bookmarks  folders the user pinned
//   servers    network locations saved from the "Connect to server" dialog
//
// Bookmarks and servers are stored with QSettings in
// ~/.config/shadow/files.conf ([Bookmarks], [Servers]).
class Places : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QVariantList places READ places NOTIFY placesChanged)
    Q_PROPERTY(QVariantList bookmarks READ bookmarks NOTIFY bookmarksChanged)
    Q_PROPERTY(QVariantList servers READ servers NOTIFY serversChanged)
    Q_PROPERTY(bool trashEmpty READ trashEmpty NOTIFY trashChanged)
    Q_PROPERTY(QUrl homeUrl READ homeUrl CONSTANT)

public:
    static Places *instance();
    static Places *create(QQmlEngine *, QJSEngine *);

    QVariantList places() const { return m_places; }
    QVariantList bookmarks() const { return m_bookmarks; }
    QVariantList servers() const { return m_servers; }
    bool trashEmpty() const { return m_trashEmpty; }
    QUrl homeUrl() const;

    // Re-check which standard folders exist (they can be created at any time).
    Q_INVOKABLE void refresh();

    Q_INVOKABLE bool isBookmarked(const QUrl &url) const;
    Q_INVOKABLE void addBookmark(const QUrl &url, const QString &title = QString());
    Q_INVOKABLE void removeBookmark(const QUrl &url);
    Q_INVOKABLE void renameBookmark(const QUrl &url, const QString &title);

    Q_INVOKABLE void addServer(const QUrl &url, const QString &title = QString());
    Q_INVOKABLE void removeServer(const QUrl &url);

    QJsonArray toJson() const;

Q_SIGNALS:
    void placesChanged();
    void bookmarksChanged();
    void serversChanged();
    void trashChanged();

private:
    explicit Places(QObject *parent = nullptr);
    void loadUserEntries();
    void save(const QString &group, const QVariantList &entries);
    void readTrashState();

    QVariantList m_places;
    QVariantList m_bookmarks;
    QVariantList m_servers;
    bool m_trashEmpty = true;
    KDirWatch *m_trashWatch = nullptr;
};
