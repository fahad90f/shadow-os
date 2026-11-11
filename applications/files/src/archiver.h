// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>

#include <functional>

// Archive handling with bsdtar (libarchive-tools), run asynchronously with an
// argument vector through Shadow::Command.
//
// Extraction always goes into a fresh temporary folder inside the destination
// and is moved into place when bsdtar succeeded: nothing that already exists is
// overwritten, an archive with several top-level entries ends up in one folder
// named after it, and a failed or cancelled extraction leaves nothing behind.
// bsdtar itself refuses absolute paths and ".." in archive members.
class Archiver : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // bsdtar is installed
    Q_PROPERTY(bool available READ available CONSTANT)
    // shadow-archive-manager is installed
    Q_PROPERTY(bool managerAvailable READ managerAvailable CONSTANT)
    // [{suffix, label}] formats offered by compress()
    Q_PROPERTY(QVariantList formats READ formats CONSTANT)

public:
    static Archiver *instance();
    static Archiver *create(QQmlEngine *, QJSEngine *);

    static bool isArchive(const QString &mimeType, const QString &fileName);

    bool available() const;
    bool managerAvailable() const;
    QVariantList formats() const;

    // Extract a local archive into the local folder `destDir`.
    Q_INVOKABLE void extract(const QUrl &archive, const QUrl &destDir);
    // Create `destDir`/`name``suffix` from local files and folders.
    Q_INVOKABLE void compress(const QList<QUrl> &urls, const QUrl &destDir, const QString &name, const QString &suffix);
    Q_INVOKABLE void openInManager(const QUrl &archive);

    // File name without its archive suffix ("src.tar.gz" → "src").
    Q_INVOKABLE QString baseName(const QString &fileName) const;
    // Default archive name for a selection.
    Q_INVOKABLE QString suggestedName(const QList<QUrl> &urls) const;

private:
    explicit Archiver(QObject *parent = nullptr);
    // Runs bsdtar as a cancellable entry of the operations list. `finish` is
    // called when it exits and returns an error text or an empty string.
    void runTar(const QString &title, const QString &failureTitle, const QStringList &arguments,
                std::function<QString(bool ok)> finish);
};
