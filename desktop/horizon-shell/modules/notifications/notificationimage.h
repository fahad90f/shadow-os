// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pictures of notifications. A client can send one as raw pixels (the
// "image-data" hint), as the name of a local file ("image-path", app_icon)
// or as an icon-theme name. All of it is untrusted: dimensions, byte counts
// and file sizes are checked before anything is decoded, nothing is ever
// fetched from a remote location, and what is kept is a small copy.
#pragma once

#include <QImage>
#include <QPointer>
#include <QQuickImageProvider>
#include <QString>
#include <QVariant>

class NotificationServer;

namespace NotificationImage {

inline constexpr int MaxRawSide = 1024;                 // "image-data" larger than this is ignored
inline constexpr int MaxFileBytes = 4 * 1024 * 1024;    // image files larger than this are ignored
inline constexpr int MaxFileSide = 4096;
inline constexpr int KeptSide = 128;                    // what is kept, in pixels (the long side)
inline constexpr int MaxRenderSide = 256;

// The "image-data" hint, signature (iiibiiay): width, height, rowstride,
// has_alpha, bits_per_sample, channels, pixels (RGB or RGBA, 8 bit).
// A null image if the value is not that or is out of bounds.
QImage fromRawData(const QVariant &hint);

// A local image file (absolute path), scaled down. Regular files of a known
// image format only. Recently used files are cached.
QImage fromFile(const QString &path);

// An icon of the icon theme in use; null if the theme has no such icon.
// GUI thread only. Results (also negative ones) are cached.
QImage fromIconName(const QString &name);

// What a client names as an icon or image (app_icon, "image-path"): an icon
// name, an absolute path or a file: URL. Anything else — relative paths,
// http: and other URLs — yields a null image.
QImage fromNameOrPath(const QString &value);

// 'image' centred in a transparent square of 'side' pixels, scaled to fit.
QImage fitted(const QImage &image, int side);

} // namespace NotificationImage

// image://nxnotify/image/<notification id>/<revision>   the notification's picture
// image://nxnotify/icon/<notification id>/<revision>    the sending application's icon
//
// The revision only makes the URL change when the picture does. Requests
// are served from images the server already holds, on the GUI thread (the
// QML Image must load synchronously, which is its default).
class NotificationImageProvider : public QQuickImageProvider
{
public:
    explicit NotificationImageProvider(NotificationServer *server);
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    static QString scheme() { return QStringLiteral("nxnotify"); }

private:
    QPointer<NotificationServer> m_server;
};
