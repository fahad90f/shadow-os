// SPDX-License-Identifier: GPL-3.0-or-later
#include "iconprovider.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QPainter>
#include <QSvgRenderer>
#include <QUrl>
#include <QUrlQuery>

namespace Shadow {

namespace {
QString iconPath(const QString &name)
{
    return QStringLiteral(":/shadow/icons/") + name + QStringLiteral(".svg");
}
} // namespace

SymbolicIconProvider::SymbolicIconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

bool SymbolicIconProvider::exists(const QString &name)
{
    return QFile::exists(iconPath(name));
}

QImage SymbolicIconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    // id = "<name>?color=%23rrggbb"
    const int q = id.indexOf(QLatin1Char('?'));
    const QString name = q < 0 ? id : id.left(q);
    QColor color(Qt::white);
    if (q >= 0) {
        const QUrlQuery query(id.mid(q + 1));
        const QColor c(QUrl::fromPercentEncoding(query.queryItemValue(QStringLiteral("color")).toLatin1()));
        if (c.isValid())
            color = c;
    }

    QSize target = requestedSize;
    if (!target.isValid() || target.isEmpty())
        target = QSize(24, 24);

    QImage image(target, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QFile file(iconPath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        // A missing glyph is a bug in the caller; draw an unmistakable
        // placeholder instead of nothing so it is noticed during development.
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(255, 77, 106), qMax(1.0, target.width() / 12.0)));
        p.drawRect(QRectF(1, 1, target.width() - 2, target.height() - 2));
        p.drawLine(QPointF(1, 1), QPointF(target.width() - 1, target.height() - 1));
        qWarning("nxicon: unknown symbolic icon '%s'", qPrintable(name));
    } else {
        QByteArray svg = file.readAll();
        svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
        QSvgRenderer renderer(svg);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(color.alphaF());
        renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(target)));
    }
    if (size)
        *size = target;
    return image;
}

ThemeIconProvider::ThemeIconProvider()
    : QQuickImageProvider(QQuickImageProvider::Pixmap)
{
}

namespace {

// Renders an SVG file (or resource) centred into a transparent pixmap of the
// requested size, keeping its aspect ratio. Uses the SVG renderer directly so
// it does not depend on the image-format plugin being installed.
QPixmap renderSvg(const QString &path, const QSize &target, const QColor &currentColor = QColor())
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QByteArray svg = file.readAll();
    if (currentColor.isValid())
        svg.replace("currentColor", currentColor.name(QColor::HexRgb).toLatin1());
    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return {};
    QPixmap pixmap(target);
    pixmap.fill(Qt::transparent);
    QSizeF natural = renderer.defaultSize();
    if (natural.isEmpty())
        natural = target;
    const QSizeF fitted = natural.scaled(target, Qt::KeepAspectRatio);
    const QRectF rect((target.width() - fitted.width()) / 2.0, (target.height() - fitted.height()) / 2.0,
                      fitted.width(), fitted.height());
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    renderer.render(&p, rect);
    return pixmap;
}

QPixmap fromFile(const QString &path, const QSize &target)
{
    if (!QFileInfo(path).isFile())
        return {};
    if (path.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".svgz"), Qt::CaseInsensitive)) {
        const QPixmap svg = renderSvg(path, target);
        if (!svg.isNull())
            return svg;
    }
    const QPixmap raster(path);
    if (raster.isNull())
        return {};
    return raster.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QPixmap fromTheme(const QString &name, const QSize &target)
{
    if (name.isEmpty())
        return {};
    QIcon icon = QIcon::fromTheme(name);
    if (icon.isNull()) {
        // Old launchers write the icon as "name.png"; the theme knows it as "name".
        static const QStringList suffixes{QStringLiteral(".png"), QStringLiteral(".svg"), QStringLiteral(".xpm")};
        for (const QString &suffix : suffixes) {
            if (name.endsWith(suffix, Qt::CaseInsensitive)) {
                icon = QIcon::fromTheme(name.left(name.size() - suffix.size()));
                break;
            }
        }
    }
    return icon.isNull() ? QPixmap() : icon.pixmap(target);
}

} // namespace

QPixmap ThemeIconProvider::requestPixmap(const QString &id, QSize *size, const QSize &requestedSize)
{
    QSize target = requestedSize;
    if (!target.isValid() || target.isEmpty())
        target = QSize(48, 48);

    const QString name = QUrl::fromPercentEncoding(id.toUtf8());
    QPixmap pixmap = name.startsWith(QLatin1Char('/')) ? fromFile(name, target) : fromTheme(name, target);
    if (pixmap.isNull())
        pixmap = fromTheme(QStringLiteral("application-x-executable"), target);
    if (pixmap.isNull()) {
        // Last resort, always available: the generic glyph compiled into the
        // library. An entry without a usable icon must still be recognisable
        // as an entry, never an empty square.
        pixmap = renderSvg(QStringLiteral(":/shadow/icons/app-generic.svg"), target, QColor(0x9A, 0xA7, 0xBD));
    }
    if (size)
        *size = pixmap.size();
    return pixmap;
}

} // namespace Shadow
