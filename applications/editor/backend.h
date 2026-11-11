// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

// Application-level services for the QML side: recent files, the saved
// session, the lists behind the menus (syntaxes, encodings, fonts) and the
// directory listing used by the file chooser.
class EditorBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QStringList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    Q_PROPERTY(QStringList encodings READ encodings CONSTANT)
    Q_PROPERTY(QStringList monospaceFamilies READ monospaceFamilies CONSTANT)
    Q_PROPERTY(QVariantList syntaxes READ syntaxes CONSTANT)
    Q_PROPERTY(qreal maxFileBytes READ maxFileBytes CONSTANT)

public:
    explicit EditorBackend(QObject *parent = nullptr);

    QStringList recentFiles() const { return m_recent; }
    QStringList encodings() const;
    QStringList monospaceFamilies() const;
    QVariantList syntaxes() const;     // [{ name, section }]
    qreal maxFileBytes() const;

    Q_INVOKABLE void addRecentFile(const QString &path);
    Q_INVOKABLE void clearRecentFiles();

    // Session: [{ path, position }] and the index of the active tab.
    Q_INVOKABLE void saveSession(const QVariantList &files, int current);
    Q_INVOKABLE QVariantMap session() const;

    // ---- paths -------------------------------------------------------------
    Q_INVOKABLE QString homePath() const;
    Q_INVOKABLE QString parentPath(const QString &path) const;
    Q_INVOKABLE QString joinPath(const QString &folder, const QString &name) const;
    Q_INVOKABLE QString fileName(const QString &path) const;
    Q_INVOKABLE bool isDirectory(const QString &path) const;
    Q_INVOKABLE bool exists(const QString &path) const;
    Q_INVOKABLE QString localPath(const QUrl &url) const;
    Q_INVOKABLE QString absolutePath(const QString &path) const;
    // Folders first, then files: [{ name, path, directory, size }]
    Q_INVOKABLE QVariantList listDirectory(const QString &path, bool showHidden) const;

Q_SIGNALS:
    void recentFilesChanged();

private:
    QStringList m_recent;
};
