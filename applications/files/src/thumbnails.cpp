// SPDX-License-Identifier: GPL-3.0-or-later
#include "thumbnails.h"

#include <KIO/PreviewJob>

#include <QCoreApplication>
#include <QImageReader>
#include <QMimeDatabase>
#include <QMutexLocker>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QSvgRenderer>
#include <QTimer>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;

namespace {

constexpr int kCacheKiB = 96 * 1024;     // memory budget of the in-memory cache
constexpr int kBatch = 12;               // items per KIO::PreviewJob
constexpr qint64 kMaxFallbackBytes = 64ll * 1024 * 1024;

struct PreviewTypes {
    QSet<QString> exact;        // "image/png"
    QStringList prefixes;       // "video/" from "video/*"
    QSet<QString> qtImages;     // decodable by QImageReader
};

const PreviewTypes &previewTypes()
{
    static const PreviewTypes types = [] {
        PreviewTypes t;
        const QStringList kio = KIO::PreviewJob::supportedMimeTypes();
        for (const QString &m : kio) {
            if (m.endsWith("/*"_L1))
                t.prefixes << m.left(m.size() - 1);
            else
                t.exact.insert(m);
        }
        const QList<QByteArray> qt = QImageReader::supportedMimeTypes();
        for (const QByteArray &m : qt)
            t.qtImages.insert(QString::fromLatin1(m));
        return t;
    }();
    return types;
}

bool isSvg(const QString &mime)
{
    return mime == "image/svg+xml"_L1 || mime == "image/svg+xml-compressed"_L1;
}

bool qtCanDecode(const QString &mime)
{
    const PreviewTypes &t = previewTypes();
    if (t.qtImages.contains(mime) || isSvg(mime))
        return true;
    // Aliases and sub-types ("image/x-png", SVGZ → SVG …).
    const QMimeType type = QMimeDatabase().mimeTypeForName(mime);
    if (!type.isValid())
        return false;
    const QStringList parents = type.allAncestors() + type.aliases();
    return std::any_of(parents.cbegin(), parents.cend(), [&t](const QString &p) { return t.qtImages.contains(p); });
}

// SVG drawings are rendered directly: the image format plugin for them
// (qt6-svg-plugins) is optional, QtSvg itself is always linked.
QImage renderSvg(const QString &path, int size)
{
    QSvgRenderer renderer(path);
    if (!renderer.isValid())
        return {};
    QSizeF drawing = renderer.defaultSize();
    if (drawing.isEmpty())
        drawing = QSizeF(size, size);
    drawing.scale(QSizeF(size, size), Qt::KeepAspectRatio);
    QImage image(drawing.toSize().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(image.size())));
    return image;
}

QImage loadScaled(const QString &path, int size, bool svg)
{
    if (svg)
        return renderSvg(path, size);
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (original.isValid() && !original.isEmpty()) {
        const bool vector = reader.format() == "svg" || reader.format() == "svgz";
        if (vector || original.width() > size || original.height() > size)
            reader.setScaledSize(original.scaled(size, size, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (!image.isNull() && (image.width() > size || image.height() > size))
        image = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

} // namespace

ThumbnailStore::ThumbnailStore(QObject *parent)
    : QObject(parent)
    , m_cache(kCacheKiB)
{
    m_pool.setMaxThreadCount(2);
}

ThumbnailStore *ThumbnailStore::instance()
{
    static ThumbnailStore *self = new ThumbnailStore(qApp);
    return self;
}

bool ThumbnailStore::canPreview(const KFileItem &item)
{
    if (item.isNull() || item.isDir() || item.size() == 0)
        return false;
    // Extension based type: cheap, and thumbnails are not worth reading every
    // file's header for.
    const QString mime = item.currentMimeType().name();
    const PreviewTypes &t = previewTypes();
    if (t.exact.contains(mime))
        return true;
    for (const QString &prefix : t.prefixes) {
        if (mime.startsWith(prefix))
            return true;
    }
    return item.isLocalFile() && mime.startsWith("image/"_L1) && qtCanDecode(mime);
}

QString ThumbnailStore::keyFor(const KFileItem &item, int pixelSize)
{
    return item.url().toString() + u'|' + QString::number(item.time(KFileItem::ModificationTime).toMSecsSinceEpoch())
        + u'|' + QString::number(item.size()) + u'|' + QString::number(pixelSize);
}

QString ThumbnailStore::lookup(const KFileItem &item, int pixelSize)
{
    const QString key = keyFor(item, pixelSize);
    {
        QMutexLocker lock(&m_mutex);
        if (m_cache.contains(key))
            return u"image://nxthumb/"_s + m_tokenByKey.value(key);
    }
    if (m_failed.contains(key) || m_queuedKeys.contains(key))
        return {};
    m_queuedKeys.insert(key);
    m_queue.append({item, pixelSize, key});
    if (m_queue.size() == 1)
        QTimer::singleShot(0, this, &ThumbnailStore::pump);
    return {};
}

QImage ThumbnailStore::image(const QString &token) const
{
    QMutexLocker lock(&m_mutex);
    const QString key = m_keyByToken.value(token);
    if (const QImage *img = m_cache.object(key))
        return *img;
    return {};
}

void ThumbnailStore::pump()
{
    if (m_runningJobs > 0 || m_queue.isEmpty())
        return;
    // Newest requests first: they belong to what is on screen now.
    const int size = m_queue.last().size;
    QList<Request> batch;
    for (qsizetype i = m_queue.size() - 1; i >= 0 && batch.size() < kBatch; --i) {
        if (m_queue.at(i).size == size) {
            batch.append(m_queue.at(i));
            m_queue.removeAt(i);
        }
    }
    startPreviewJob(batch);
}

void ThumbnailStore::startPreviewJob(const QList<Request> &batch)
{
    KFileItemList items;
    QHash<QUrl, Request> byUrl;
    for (const Request &r : batch) {
        items.append(r.item);
        byUrl.insert(r.item.url(), r);
    }
    const int size = batch.first().size;
    const QStringList plugins = KIO::PreviewJob::availablePlugins();
    KIO::PreviewJob *job = KIO::filePreview(items, QSize(size, size), &plugins);
    job->setDevicePixelRatio(1.0);
    ++m_runningJobs;

    auto pending = std::make_shared<QHash<QUrl, Request>>(byUrl);
    connect(job, &KIO::PreviewJob::gotPreview, this, [this, pending](const KFileItem &item, const QPixmap &preview) {
        const auto it = pending->constFind(item.url());
        if (it == pending->constEnd())
            return;
        const Request r = *it;
        pending->erase(it);
        if (preview.isNull())
            fallback(r);
        else
            store(r, preview.toImage());
    });
    connect(job, &KIO::PreviewJob::failed, this, [this, pending](const KFileItem &item) {
        const auto it = pending->constFind(item.url());
        if (it == pending->constEnd())
            return;
        const Request r = *it;
        pending->erase(it);
        fallback(r);
    });
    connect(job, &KJob::result, this, [this, pending] {
        // Items the job never reported on (e.g. it was aborted).
        const QList<Request> rest = pending->values();
        pending->clear();
        for (const Request &r : rest)
            fallback(r);
        --m_runningJobs;
        pump();
    });
}

void ThumbnailStore::fallback(const Request &request)
{
    const KFileItem &item = request.item;
    if (!item.isLocalFile() || item.size() > KIO::filesize_t(kMaxFallbackBytes)
        || !qtCanDecode(item.currentMimeType().name())) {
        giveUp(request);
        return;
    }
    const QString path = item.localPath();
    const int size = request.size;
    const bool svg = isSvg(item.currentMimeType().name());
    const QPointer<ThumbnailStore> self(this);
    (void)QtConcurrent::run(&m_pool, [self, request, path, size, svg] {
        const QImage image = loadScaled(path, size, svg);
        QMetaObject::invokeMethod(qApp, [self, request, image] {
            if (!self)
                return;
            if (image.isNull())
                self->giveUp(request);
            else
                self->store(request, image);
        }, Qt::QueuedConnection);
    });
}

void ThumbnailStore::store(const Request &request, const QImage &image)
{
    {
        QMutexLocker lock(&m_mutex);
        QString token = m_tokenByKey.value(request.key);
        if (token.isEmpty()) {
            token = QString::number(m_nextToken++);
            m_tokenByKey.insert(request.key, token);
            m_keyByToken.insert(token, request.key);
        }
        const int cost = qMax(1, int(image.sizeInBytes() / 1024));
        m_cache.insert(request.key, new QImage(image), cost);
    }
    m_queuedKeys.remove(request.key);
    Q_EMIT ready(request.item.url(), request.size);
}

void ThumbnailStore::giveUp(const Request &request)
{
    m_queuedKeys.remove(request.key);
    m_failed.insert(request.key);
}

// ---- image provider ------------------------------------------------------------

ThumbnailImageProvider::ThumbnailImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage ThumbnailImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    QImage image = ThumbnailStore::instance()->image(id);
    if (image.isNull()) {
        // The thumbnail was evicted between the model handing out its URL and
        // the Image loading it; the model re-requests it on the next read.
        image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
    } else if (requestedSize.isValid() && !requestedSize.isEmpty()
               && (image.width() > requestedSize.width() || image.height() > requestedSize.height())) {
        image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size)
        *size = image.size();
    return image;
}
