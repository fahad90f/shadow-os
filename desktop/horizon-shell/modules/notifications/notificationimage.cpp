// SPDX-License-Identifier: GPL-3.0-or-later
#include "notificationimage.h"

#include "notificationserver.h"
#include "notificationtext.h"

#include <QCache>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDateTime>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QPainter>
#include <QStringList>
#include <QThread>

namespace {

int longSide(const QImage &image)
{
    return qMax(image.width(), image.height());
}

QImage scaledDown(const QImage &image)
{
    if (image.isNull() || longSide(image) <= NotificationImage::KeptSide)
        return image;
    return image.scaled(NotificationImage::KeptSide, NotificationImage::KeptSide, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
}

// Small caches of decoded files and theme icons, so that a client repeating
// the same picture hundreds of times costs one decode. A null image is a
// cached "there is none".
struct CachedImage { QImage image; };
QCache<QString, CachedImage> &fileCache()
{
    static QCache<QString, CachedImage> cache(24);
    return cache;
}
QCache<QString, CachedImage> &iconCache()
{
    static QCache<QString, CachedImage> cache(96);
    return cache;
}

} // namespace

QImage NotificationImage::fromRawData(const QVariant &hint)
{
    if (hint.userType() != qMetaTypeId<QDBusArgument>())
        return {};
    const QDBusArgument argument = hint.value<QDBusArgument>();
    if (argument.currentSignature() != QLatin1String("(iiibiiay)"))
        return {};

    int width = 0, height = 0, rowStride = 0, bitsPerSample = 0, channels = 0;
    bool hasAlpha = false;
    QImage image;
    argument.beginStructure();
    argument >> width >> height >> rowStride >> hasAlpha >> bitsPerSample >> channels;
    const bool plausible = width > 0 && height > 0 && width <= MaxRawSide && height <= MaxRawSide
        && bitsPerSample == 8 && (channels == 3 || channels == 4) && hasAlpha == (channels == 4)
        && rowStride >= width * channels && rowStride <= MaxRawSide * 4 + 64;
    // The pixels are only copied out of the message when the header is acceptable.
    QByteArray data;
    if (plausible)
        argument >> data;
    argument.endStructure();
    if (!plausible)
        return {};

    // The last row may be shorter than the row stride.
    const qsizetype needed = qsizetype(height - 1) * rowStride + qsizetype(width) * channels;
    if (data.size() < needed)
        return {};
    const QImage wrapped(reinterpret_cast<const uchar *>(data.constData()), width, height, rowStride,
                         channels == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
    // scaled()/copy() detach from 'data', which goes away with this function.
    image = longSide(wrapped) > KeptSide ? scaledDown(wrapped) : wrapped.copy();
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage NotificationImage::fromFile(const QString &path)
{
    if (path.isEmpty() || !path.startsWith(u'/'))
        return {};
    const QFileInfo info(path);
    // Regular files only: no devices, pipes or directories; /proc and other
    // synthetic files report a size of zero.
    if (!info.isFile() || info.size() <= 0 || info.size() > MaxFileBytes)
        return {};
    const QString key = path + u'\n' + QString::number(info.size()) + u'\n'
        + QString::number(info.lastModified().toMSecsSinceEpoch());
    if (const CachedImage *cached = fileCache().object(key))
        return cached->image;

    QImage image;
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    static const QList<QByteArray> formats{"png", "jpeg", "jpg", "svg", "svgz", "gif", "webp", "bmp", "xpm", "ico"};
    const QSize natural = reader.size();
    if (formats.contains(reader.format().toLower()) && natural.isValid() && !natural.isEmpty()
        && natural.width() <= MaxFileSide && natural.height() <= MaxFileSide) {
        // Let the decoder scale where it can (JPEG, SVG): cheaper and sharper.
        if (qMax(natural.width(), natural.height()) > KeptSide)
            reader.setScaledSize(natural.scaled(KeptSide, KeptSide, Qt::KeepAspectRatio));
        image = scaledDown(reader.read());
        if (!image.isNull())
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    fileCache().insert(key, new CachedImage{image});
    return image;
}

QImage NotificationImage::fromIconName(const QString &name)
{
    if (name.isEmpty())
        return {};
    if (const CachedImage *cached = iconCache().object(name))
        return cached->image;
    QImage image;
    const QIcon icon = QIcon::fromTheme(name);
    if (!icon.isNull()) {
        image = icon.pixmap(QSize(KeptSide, KeptSide), 1.0).toImage();
        if (!image.isNull())
            image = scaledDown(image).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    iconCache().insert(name, new CachedImage{image});
    return image;
}

QImage NotificationImage::fromNameOrPath(const QString &value)
{
    if (value.isEmpty())
        return {};
    const QString path = NotificationText::localFilePath(value);
    if (!path.isEmpty())
        return fromFile(path);
    if (NotificationText::looksLikeUrl(value))
        return {};                      // never fetched
    return fromIconName(NotificationText::iconName(value));
}

QImage NotificationImage::fitted(const QImage &image, int side)
{
    if (image.isNull() || side <= 0)
        return {};
    QImage scaled = image;
    if (scaled.width() != side || scaled.height() != side)
        scaled = scaled.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (scaled.width() == side && scaled.height() == side)
        return scaled.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage canvas(side, side, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.drawImage((side - scaled.width()) / 2, (side - scaled.height()) / 2, scaled);
    painter.end();
    return canvas;
}

NotificationImageProvider::NotificationImageProvider(NotificationServer *server)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_server(server)
{
}

QImage NotificationImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    int side = qMax(requestedSize.width(), requestedSize.height());
    if (side <= 0)
        side = 48;
    side = qBound(8, side, NotificationImage::MaxRenderSide);

    QImage image;
    // The server lives on the GUI thread; an Image that loads asynchronously
    // gets an empty picture rather than a data race.
    const QCoreApplication *app = QCoreApplication::instance();
    if (app && QThread::currentThread() == app->thread() && m_server) {
        const QStringList parts = id.split(u'/');
        if (parts.size() >= 2) {
            const uint notification = parts.at(1).toUInt();
            if (parts.at(0) == QLatin1String("image"))
                image = NotificationImage::fitted(m_server->image(notification), side);
            else if (parts.at(0) == QLatin1String("icon"))
                image = NotificationImage::fitted(m_server->appIcon(notification), side);
        }
    }
    if (image.isNull()) {
        // Never a null image: QML would log a warning for a picture whose
        // notification has merely gone away between two frames.
        image = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
    }
    if (size)
        *size = image.size();
    return image;
}
