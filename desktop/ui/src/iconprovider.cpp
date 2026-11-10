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

namespace Nexus {

namespace {
QString iconPath(const QString &name)
{
    return QStringLiteral(":/nexus/icons/") + name + QStringLiteral(".svg");
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

QPixmap ThemeIconProvider::requestPixmap(const QString &id, QSize *size, const QSize &requestedSize)
{
    QSize target = requestedSize;
    if (!target.isValid() || target.isEmpty())
        target = QSize(48, 48);

    const QString name = QUrl::fromPercentEncoding(id.toUtf8());
    QIcon icon;
    if (name.startsWith(QLatin1Char('/')) && QFileInfo::exists(name))
        icon = QIcon(name);
    else if (!name.isEmpty())
        icon = QIcon::fromTheme(name);
    if (icon.isNull())
        icon = QIcon::fromTheme(QStringLiteral("application-x-executable"));
    if (icon.isNull())
        icon = QIcon(QStringLiteral(":/nexus/icons/app-generic.svg"));

    QPixmap pixmap = icon.pixmap(target);
    if (size)
        *size = pixmap.size();
    return pixmap;
}

} // namespace Nexus
