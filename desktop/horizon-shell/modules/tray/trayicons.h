// SPDX-License-Identifier: GPL-3.0-or-later
//
// Icons of tray items and of their menu entries: decoding what clients send
// (theme names, files, raw pixmaps, encoded images) into bounded images, and
// the image provider through which QML displays them.
#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QQuickImageProvider>
#include <QString>
#include <QVariant>

class TrayHost;

// An icon as a StatusNotifierItem describes it: an icon-theme name (or an
// absolute file name), a set of pixmaps, or both. Names win over pixmaps.
struct TrayIconSource
{
    QString name;
    QString themePath;          // the item's IconThemePath: an extra place to look the name up
    QList<QImage> pixmaps;      // ascending by size; count and size bounded (TrayLimits)
    size_t pixmapHash = 0;      // of the data as it was received, to notice changes cheaply

    bool isEmpty() const { return name.isEmpty() && pixmaps.isEmpty(); }
    bool sameAs(const TrayIconSource &other) const
    {
        return name == other.name && themePath == other.themePath && pixmapHash == other.pixmapHash
            && pixmaps.size() == other.pixmaps.size();
    }

    // The icon as a square image of 'side' device pixels, or a null image if
    // neither the name nor the pixmaps yield one. GUI thread only (icon-theme
    // lookups).
    QImage render(int side) const;
};

namespace TrayIcons {

// Fills 'into.pixmaps' from an IconPixmap property value: a(iiay) — width,
// height and ARGB32 pixels in network byte order.
void readPixmaps(const QVariant &dbusValue, TrayIconSource &into);

// An encoded image (the PNG "icon-data" of a menu entry) as an image of at
// most 'maxSide' pixels; null if it is not an image or unreasonably large.
QImage fromData(const QByteArray &data, int maxSide);

// 'image' centred in a transparent square of 'side' pixels, scaled to fit.
QImage fitted(const QImage &image, int side);

// Symbolic icons are drawn in one grey tone meant for a particular kind of
// background. When such an icon would barely be visible on 'background' it
// is recoloured with 'foreground'; every other icon is left untouched.
void ensureLegible(QImage &image, const QColor &foreground, const QColor &background);

} // namespace TrayIcons

// image://nxtray/item/<item serial>/<icon generation>?fg=<rrggbb>&bg=<rrggbb>
// image://nxtray/menu/<item serial>/<entry id>/<menu revision>?fg=<rrggbb>&bg=<rrggbb>
//
// "fg" and "bg" are the colours the icon is shown with and on (see
// TrayIcons::ensureLegible). The generation/revision only makes the URL
// change when the picture does.
//
// Images are produced on the GUI thread (icon-theme lookups are only safe
// there), so the QML Image must load synchronously, which is its default.
class TrayImageProvider : public QQuickImageProvider
{
public:
    explicit TrayImageProvider(TrayHost *host);
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    static QString scheme() { return QStringLiteral("nxtray"); }

private:
    QPointer<TrayHost> m_host;
};
