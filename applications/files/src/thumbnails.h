// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <KFileItem>

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QSet>
#include <QThreadPool>
#include <QUrl>

// Thumbnails of files, generated on demand and kept in memory.
//
// Generation goes through KIO::PreviewJob (thumbnailer plugins of kio-extras,
// kdegraphics-thumbnailers, ffmpegthumbs …; results are cached on disk in
// ~/.cache/thumbnails by KIO). If no plugin produces a preview and the file is
// a local image Qt can decode, it is scaled down with QImageReader on a worker
// thread instead, so image thumbnails do not depend on optional packages.
//
// A finished thumbnail is served to QML as image://nxthumb/<token>. The model
// only hands out that URL once the image exists, so an Image never fails to
// load.
class ThumbnailStore : public QObject
{
    Q_OBJECT
public:
    static ThumbnailStore *instance();

    // True if a preview may exist for this item (by MIME type).
    static bool canPreview(const KFileItem &item);

    // URL of the thumbnail if it is ready; otherwise an empty string, and the
    // thumbnail is scheduled (ready() follows unless generation fails).
    QString lookup(const KFileItem &item, int pixelSize);

    // Called by the image provider (any thread).
    QImage image(const QString &token) const;

Q_SIGNALS:
    void ready(const QUrl &url, int pixelSize);

private:
    explicit ThumbnailStore(QObject *parent = nullptr);

    struct Request {
        KFileItem item;
        int size = 0;
        QString key;
    };

    static QString keyFor(const KFileItem &item, int pixelSize);
    void pump();
    void startPreviewJob(const QList<Request> &batch);
    void fallback(const Request &request);
    void store(const Request &request, const QImage &image);
    void giveUp(const Request &request);

    mutable QMutex m_mutex;
    QCache<QString, QImage> m_cache;            // key → image, cost in KiB
    QHash<QString, QString> m_tokenByKey;
    QHash<QString, QString> m_keyByToken;
    QSet<QString> m_failed;                     // keys that cannot be previewed
    QSet<QString> m_queuedKeys;
    QList<Request> m_queue;                     // newest request last
    int m_runningJobs = 0;
    int m_nextToken = 1;
    QThreadPool m_pool;
};

// image://nxthumb/<token>
class ThumbnailImageProvider : public QQuickImageProvider
{
public:
    ThumbnailImageProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
