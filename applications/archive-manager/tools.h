// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

// Stateless helpers for the QML side: what can be created, which tools are
// installed, and the directory listing behind the file chooser.
class ArchiveTools : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QVariantList createFormats READ createFormats CONSTANT)
    Q_PROPERTY(QVariantList tools READ tools CONSTANT)

public:
    using QObject::QObject;

    // [{ id, label, extension, encrypt, cipher, store, available, tool }]
    QVariantList createFormats() const;
    // [{ name, available, version, usedFor }]
    QVariantList tools() const;

    Q_INVOKABLE QString formatForName(const QString &fileName) const;      // "" when unknown
    Q_INVOKABLE QString baseName(const QString &fileName) const;
    Q_INVOKABLE bool looksLikeArchive(const QString &path) const;
    // Name proposed for a new archive made of these files (no extension).
    Q_INVOKABLE QString suggestedName(const QStringList &inputs) const;

    Q_INVOKABLE QString homePath() const;
    Q_INVOKABLE QString parentPath(const QString &path) const;
    Q_INVOKABLE QString joinPath(const QString &folder, const QString &name) const;
    Q_INVOKABLE QString fileName(const QString &path) const;
    Q_INVOKABLE bool isDirectory(const QString &path) const;
    Q_INVOKABLE bool exists(const QString &path) const;
    Q_INVOKABLE QString localPath(const QUrl &url) const;
    Q_INVOKABLE QVariantList listDirectory(const QString &path, bool showHidden) const;
};

// How the application was started; exposed to QML as "launch".
class Launch : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mode MEMBER mode CONSTANT)               // "browse" | "extract" | "create"
    Q_PROPERTY(QString archive MEMBER archive CONSTANT)
    Q_PROPERTY(QString destination MEMBER destination CONSTANT)
    Q_PROPERTY(QStringList inputs MEMBER inputs CONSTANT)
    Q_PROPERTY(QString format MEMBER format CONSTANT)
    Q_PROPERTY(QString level MEMBER level CONSTANT)
    Q_PROPERTY(QString conflict MEMBER conflict CONSTANT)
public:
    using QObject::QObject;
    QString mode = QStringLiteral("browse");
    QString archive;
    QString destination;
    QStringList inputs;
    QString format;
    QString level = QStringLiteral("normal");
    QString conflict = QStringLiteral("rename");
};
