// SPDX-License-Identifier: GPL-3.0-or-later
#include "trayicons.h"

#include "trayhost.h"
#include "trayutil.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDir>
#include <QFileInfo>
#include <QHashFunctions>
#include <QIcon>
#include <QImageReader>
#include <QPainter>
#include <QStringList>
#include <QSvgRenderer>
#include <QThread>
#include <QUrlQuery>
#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace {

int longSide(const QImage &image)
{
    return qMax(image.width(), image.height());
}

bool isBlank(const QImage &image)
{
    if (image.isNull())
        return true;
    if (!image.hasAlphaChannel())
        return false;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(line[x]) != 0)
                return false;
        }
    }
    return true;
}

// An image file (SVG or anything Qt reads) as a square icon. Files that are
// not regular, too large or of unreasonable dimensions are refused.
QImage fromFile(const QString &path, int side)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0 || info.size() > TrayLimits::MaxIconFileBytes)
        return {};

    if (path.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".svgz"), Qt::CaseInsensitive)) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid())
            return {};
        QSizeF natural = renderer.defaultSize();
        if (natural.isEmpty())
            natural = QSizeF(side, side);
        const QSizeF fit = natural.scaled(QSizeF(side, side), Qt::KeepAspectRatio);
        QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&painter, QRectF((side - fit.width()) / 2.0, (side - fit.height()) / 2.0,
                                         fit.width(), fit.height()));
        painter.end();
        return isBlank(image) ? QImage() : image;
    }

    QImageReader reader(path);
    const QSize natural = reader.size();
    if (!natural.isValid() || natural.isEmpty() || natural.width() > TrayLimits::MaxIconFileSide
        || natural.height() > TrayLimits::MaxIconFileSide)
        return {};
    return TrayIcons::fitted(reader.read(), side);
}

// An icon of the icon theme in use; null if the theme has none of that name.
QImage fromIconTheme(const QString &name, int side)
{
    QIcon icon = QIcon::fromTheme(name);
    if (icon.isNull()) {
        // Some clients append the file suffix; the theme knows the bare name.
        for (const char *suffix : {".png", ".svg", ".xpm"}) {
            const QLatin1StringView s(suffix);
            if (name.endsWith(s, Qt::CaseInsensitive)) {
                icon = QIcon::fromTheme(name.left(name.size() - s.size()));
                break;
            }
        }
    }
    if (icon.isNull())
        return {};
    const QImage image = icon.pixmap(QSize(side, side), 1.0).toImage();
    return isBlank(image) ? QImage() : TrayIcons::fitted(image, side);
}

// Looks 'name' up below an item's IconThemePath, the way icons are laid out
// there: <path>/<name>.<ext> (what libappindicator clients write) or
// <path>/<theme>/<size>/<context>/<name>.<ext>. Returns the file name, or an
// empty string. The number of file system probes is bounded and nothing is
// scanned, because the path is whatever the client says it is.
QString findInThemePath(const QString &themePath, const QString &name, int side)
{
    static const QStringList extensions{QStringLiteral(".png"), QStringLiteral(".svg"), QStringLiteral(".svgz"),
                                        QStringLiteral(".xpm")};
    const QDir base(themePath);
    if (!base.isAbsolute() || !base.exists())
        return {};
    for (const QString &extension : extensions) {
        const QString file = base.filePath(name + extension);
        if (QFileInfo(file).isFile())
            return file;
    }

    QStringList themes;
    const QString current = QIcon::themeName();
    if (!current.isEmpty() && current != QLatin1String("hicolor") && !current.contains(u'/'))
        themes << current;
    themes << QStringLiteral("hicolor");

    // Size directories from the best match for 'side' to the worst: the
    // nearest size that is large enough first, scalable icons, then smaller.
    static const int sizes[] = {16, 22, 24, 32, 48, 64, 96, 128, 256, 512};
    QStringList sizeDirs;
    for (const int size : sizes) {
        if (size >= side)
            sizeDirs << QStringLiteral("%1x%1").arg(size);
    }
    sizeDirs << QStringLiteral("scalable") << QStringLiteral("symbolic");
    for (auto it = std::rbegin(sizes); it != std::rend(sizes); ++it) {
        if (*it < side)
            sizeDirs << QStringLiteral("%1x%1").arg(*it);
    }
    static const QStringList contexts{QStringLiteral("status"), QStringLiteral("apps"), QStringLiteral("panel"),
                                      QStringLiteral("devices"), QStringLiteral("actions"),
                                      QStringLiteral("categories"), QStringLiteral("places"),
                                      QStringLiteral("emblems"), QStringLiteral("mimetypes")};

    for (const QString &theme : std::as_const(themes)) {
        const QDir themeDir(base.filePath(theme));
        if (!themeDir.exists())
            continue;
        for (const QString &sizeDir : std::as_const(sizeDirs)) {
            const QDir sized(themeDir.filePath(sizeDir));
            if (!sized.exists())
                continue;
            for (const QString &context : contexts) {
                const QDir contextDir(sized.filePath(context));
                if (!contextDir.exists())
                    continue;
                for (const QString &extension : extensions) {
                    const QString file = contextDir.filePath(name + extension);
                    if (QFileInfo(file).isFile())
                        return file;
                }
            }
        }
    }
    return {};
}

// Relative luminance of an sRGB colour (WCAG 2).
double luminance(double red, double green, double blue)
{
    const auto linear = [](double channel) {
        channel /= 255.0;
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(red) + 0.7152 * linear(green) + 0.0722 * linear(blue);
}

} // namespace

QImage TrayIconSource::render(int side) const
{
    QImage image;
    if (name.startsWith(u'/')) {
        image = fromFile(name, side);
    } else if (!name.isEmpty() && !name.contains(u'/')) {
        image = fromIconTheme(name, side);
        if (image.isNull() && !themePath.isEmpty()) {
            const QString file = findInThemePath(themePath, name, side);
            if (!file.isEmpty())
                image = fromFile(file, side);
        }
    }
    if (image.isNull() && !pixmaps.isEmpty()) {
        // The smallest pixmap that is large enough; failing that, the largest.
        const QImage *best = &pixmaps.constLast();
        for (const QImage &candidate : pixmaps) {
            if (longSide(candidate) >= side) {
                best = &candidate;
                break;
            }
        }
        image = TrayIcons::fitted(*best, side);
    }
    return image;
}

void TrayIcons::readPixmaps(const QVariant &dbusValue, TrayIconSource &into)
{
    into.pixmaps.clear();
    into.pixmapHash = 0;
    if (dbusValue.userType() != qMetaTypeId<QDBusArgument>())
        return;
    const QDBusArgument argument = dbusValue.value<QDBusArgument>();
    if (argument.currentSignature() != QLatin1String("a(iiay)"))
        return;

    QList<QImage> images;
    size_t hash = 0;
    int entry = 0;
    // Every container that is entered is left again, also when its content
    // is not wanted: an abandoned QDBusArgument keeps the whole message alive.
    argument.beginArray();
    while (!argument.atEnd()) {
        argument.beginStructure();
        if (entry++ < TrayLimits::MaxPixmapEntries) {
            int width = 0, height = 0;
            argument >> width >> height;
            if (width > 0 && height > 0 && width <= TrayLimits::MaxPixmapSide
                && height <= TrayLimits::MaxPixmapSide) {
                // The pixel data is only copied out of the message once the
                // dimensions are known to be acceptable.
                QByteArray data;
                argument >> data;
                if (data.size() == qsizetype(width) * height * 4) {
                    QImage image(width, height, QImage::Format_ARGB32);
                    if (!image.isNull()) {
                        hash = qHashMulti(hash, width, height);
                        hash = qHashBits(data.constData(), size_t(data.size()), hash);
                        const uchar *source = reinterpret_cast<const uchar *>(data.constData());
                        for (int y = 0; y < height; ++y) {
                            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
                            for (int x = 0; x < width; ++x, source += 4)
                                line[x] = qFromBigEndian<quint32>(source);
                        }
                        if (longSide(image) > TrayLimits::KeptPixmapSide) {
                            image = image.scaled(TrayLimits::KeptPixmapSide, TrayLimits::KeptPixmapSide,
                                                 Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        }
                        images.append(image);
                    }
                }
            }
        }
        argument.endStructure();
    }
    argument.endArray();

    std::stable_sort(images.begin(), images.end(),
                     [](const QImage &a, const QImage &b) { return longSide(a) < longSide(b); });
    QList<QImage> kept;
    for (const QImage &image : std::as_const(images)) {
        if (kept.isEmpty() || longSide(kept.constLast()) != longSide(image))
            kept.append(image);
    }
    // Too many sizes: drop the ones whose next larger neighbour is closest,
    // since that neighbour serves the same purpose. The largest always stays.
    while (kept.size() > TrayLimits::MaxKeptPixmaps) {
        qsizetype redundant = 0;
        double smallestStep = 0;
        for (qsizetype i = 0; i + 1 < kept.size(); ++i) {
            const double step = double(longSide(kept.at(i + 1))) / longSide(kept.at(i));
            if (i == 0 || step < smallestStep) {
                smallestStep = step;
                redundant = i;
            }
        }
        kept.removeAt(redundant);
    }
    into.pixmaps = kept;
    into.pixmapHash = hash;
}

QImage TrayIcons::fromData(const QByteArray &data, int maxSide)
{
    if (data.isEmpty() || data.size() > TrayLimits::MaxMenuIconBytes)
        return {};
    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    // "icon-data" is PNG by specification; no other decoder is offered to
    // data that comes from another program.
    QImageReader reader(&buffer, "png");
    const QSize natural = reader.size();
    if (!natural.isValid() || natural.isEmpty() || natural.width() > TrayLimits::MaxMenuIconSide
        || natural.height() > TrayLimits::MaxMenuIconSide)
        return {};
    QImage image = reader.read();
    if (!image.isNull() && longSide(image) > maxSide)
        image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

QImage TrayIcons::fitted(const QImage &image, int side)
{
    if (image.isNull() || side <= 0)
        return {};
    QImage scaled = image;
    scaled.setDevicePixelRatio(1.0);
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

void TrayIcons::ensureLegible(QImage &image, const QColor &foreground, const QColor &background)
{
    if (image.isNull() || !foreground.isValid() || !background.isValid())
        return;
    QImage pixels = image.convertToFormat(QImage::Format_ARGB32);

    // One grey tone throughout? Pixels that are nearly transparent do not
    // count; their colour is mostly rounding noise.
    int darkest = 255, lightest = 0;
    double sum = 0, weight = 0;
    for (int y = 0; y < pixels.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
        for (int x = 0; x < pixels.width(); ++x) {
            const QRgb pixel = line[x];
            const int alpha = qAlpha(pixel);
            if (alpha < 32)
                continue;
            const int red = qRed(pixel), green = qGreen(pixel), blue = qBlue(pixel);
            if (qMax(red, qMax(green, blue)) - qMin(red, qMin(green, blue)) > 36)
                return;                     // a coloured icon
            const int grey = qGray(pixel);
            darkest = qMin(darkest, grey);
            lightest = qMax(lightest, grey);
            sum += double(grey) * alpha;
            weight += alpha;
        }
    }
    if (weight <= 0 || lightest - darkest > 72)
        return;                             // empty, or shaded rather than symbolic

    const double tone = sum / weight;
    const double iconLuminance = luminance(tone, tone, tone);
    const double backLuminance = luminance(background.red(), background.green(), background.blue());
    const double contrast = (qMax(iconLuminance, backLuminance) + 0.05) / (qMin(iconLuminance, backLuminance) + 0.05);
    if (contrast >= 3.0)
        return;                             // visible as it is

    const QRgb colour = foreground.rgb() & 0x00ffffff;
    for (int y = 0; y < pixels.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(pixels.scanLine(y));
        for (int x = 0; x < pixels.width(); ++x)
            line[x] = colour | (line[x] & 0xff000000);
    }
    image = pixels.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

TrayImageProvider::TrayImageProvider(TrayHost *host)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_host(host)
{
}

QImage TrayImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    int side = qMax(requestedSize.width(), requestedSize.height());
    if (side <= 0)
        side = 22;
    side = qBound(8, side, TrayLimits::MaxRenderSide);

    QImage image;
    // Off the GUI thread (an Image that loads asynchronously) nothing of the
    // tray may be touched; the request then yields an empty picture.
    const QCoreApplication *app = QCoreApplication::instance();
    if (app && QThread::currentThread() == app->thread() && m_host) {
        const qsizetype mark = id.indexOf(u'?');
        const QStringList parts = (mark < 0 ? id : id.left(mark)).split(u'/');
        const QUrlQuery query(mark < 0 ? QString() : id.mid(mark + 1));
        const QColor foreground = QColor::fromString(u'#' + query.queryItemValue(QStringLiteral("fg")));
        const QColor background = QColor::fromString(u'#' + query.queryItemValue(QStringLiteral("bg")));
        if (parts.size() >= 2 && parts.at(0) == QLatin1String("item"))
            image = m_host->itemImage(parts.at(1).toInt(), side, foreground, background);
        else if (parts.size() >= 3 && parts.at(0) == QLatin1String("menu"))
            image = m_host->menuImage(parts.at(1).toInt(), parts.at(2).toInt(), side, foreground, background);
    }
    if (image.isNull()) {
        // Never a null image: QML would log a warning for an icon that has
        // merely gone away between two frames.
        image = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
    }
    if (size)
        *size = image.size();
    return image;
}
