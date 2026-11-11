// SPDX-License-Identifier: GPL-3.0-or-later
//
// SVG icon engine, linked into the application as a static Qt plugin.
//
// Icon themes such as Breeze consist of SVG files. QIcon can only load those
// through the "svg" icon engine plugin of the Debian package qt6-svg-plugins.
// Where that package is missing (as in the build chroot) QIcon::fromTheme()
// returns icons that cannot be painted and every themed file icon is blank.
// This engine renders the files with QSvgRenderer, which the application
// links anyway. Qt asks plugins found in its plugin directories first, so the
// system plugin is used whenever it is installed.

#include <QIconEngine>
#include <QIconEnginePlugin>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QSvgRenderer>

namespace {

class SvgIconEngine : public QIconEngine
{
public:
    SvgIconEngine() = default;
    explicit SvgIconEngine(const QString &file)
        : m_file(file)
    {
    }

    void addFile(const QString &fileName, const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        Q_UNUSED(size)
        Q_UNUSED(mode)
        Q_UNUSED(state)
        if (m_file.isEmpty())
            m_file = fileName;
    }

    QIconEngine *clone() const override { return new SvgIconEngine(m_file); }
    QString key() const override { return QStringLiteral("svg"); }
    bool isNull() override { return m_file.isEmpty(); }

    QSize actualSize(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        Q_UNUSED(mode)
        Q_UNUSED(state)
        return size;
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        Q_UNUSED(state)
        const QSize pixels = (QSizeF(size) * scale).toSize();
        if (m_file.isEmpty() || pixels.isEmpty())
            return {};
        const QString cacheKey = QStringLiteral("nexus-files-svg:%1:%2x%3:%4")
            .arg(m_file).arg(pixels.width()).arg(pixels.height()).arg(int(mode));
        QPixmap pixmap;
        if (QPixmapCache::find(cacheKey, &pixmap))
            return pixmap;

        QSvgRenderer renderer(m_file);
        if (!renderer.isValid())
            return {};
        QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            if (mode == QIcon::Disabled)
                painter.setOpacity(0.45);
            // Keep the aspect ratio of the drawing, centred in the target.
            QSizeF drawing = renderer.defaultSize();
            if (drawing.isEmpty())
                drawing = pixels;
            drawing.scale(pixels, Qt::KeepAspectRatio);
            const QRectF target(QPointF((pixels.width() - drawing.width()) / 2.0,
                                        (pixels.height() - drawing.height()) / 2.0), drawing);
            renderer.render(&painter, target);
        }
        pixmap = QPixmap::fromImage(image);
        pixmap.setDevicePixelRatio(scale);
        QPixmapCache::insert(cacheKey, pixmap);
        return pixmap;
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        const qreal scale = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
    }

private:
    QString m_file;
};

} // namespace

class NexusFilesSvgIconPlugin : public QIconEnginePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QIconEngineFactoryInterface_iid FILE "svgiconengine.json")

public:
    QIconEngine *create(const QString &filename) override
    {
        return filename.isEmpty() ? new SvgIconEngine : new SvgIconEngine(filename);
    }
};

#include "svgiconengine.moc"
