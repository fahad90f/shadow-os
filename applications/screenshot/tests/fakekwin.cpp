// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test double of KWin's org.kde.KWin.ScreenShot2 D-Bus interface.
//
// KWin offers ScreenShot2 only with OpenGL compositing. The nested test
// session (KWin on Xvfb, no DRM device) composites with QPainter, so the
// real interface does not exist there. This program stands in for it: it
// runs as an X11 client of the Xvfb server that displays the nested
// session, grabs that screen — i.e. the real content of the session — and
// hands it out with the same protocol KWin uses (reply with the image
// geometry, then raw pixels written into the caller's pipe).
//
//   DISPLAY=$NEXUS_NESTED_X QT_QPA_PLATFORM=xcb nexus-screenshot-fakekwin &
//   NEXUS_SCREENSHOT_KWIN_SERVICE=org.nexus.test.FakeKWin nexus-screenshot …
//
// Not covered by the double: single-window capture (answers with an error,
// as it does not know the compositor's windows) and the pointer option.
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QScreen>
#include <QVariantMap>

#include <cstdio>
#include <thread>
#include <unistd.h>

class FakeScreenShot2 : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.ScreenShot2")
    Q_PROPERTY(uint Version READ version CONSTANT)
public:
    uint version() const { return 4; }

public Q_SLOTS:
    QVariantMap CaptureWorkspace(const QVariantMap &options, const QDBusUnixFileDescriptor &pipe)
    {
        Q_UNUSED(options)
        return send(grab(), pipe);
    }
    QVariantMap CaptureScreen(const QString &name, const QVariantMap &options, const QDBusUnixFileDescriptor &pipe)
    {
        Q_UNUSED(options)
        std::printf("fakekwin: CaptureScreen %s\n", qPrintable(name));
        QVariantMap result = send(grab(), pipe);
        result[QStringLiteral("screen")] = name;
        return result;
    }
    QVariantMap CaptureActiveScreen(const QVariantMap &options, const QDBusUnixFileDescriptor &pipe)
    {
        Q_UNUSED(options)
        return send(grab(), pipe);
    }
    QVariantMap CaptureArea(int x, int y, uint width, uint height, const QVariantMap &options, const QDBusUnixFileDescriptor &pipe)
    {
        Q_UNUSED(options)
        const QImage all = grab();
        const QRect area = QRect(x, y, int(width), int(height)).intersected(all.rect());
        if (area.isEmpty()) {
            sendErrorReply(QStringLiteral("org.kde.KWin.ScreenShot2.Error.InvalidArea"), QStringLiteral("Invalid area"));
            return {};
        }
        return send(all.copy(area), pipe);
    }
    QVariantMap CaptureActiveWindow(const QVariantMap &options, const QDBusUnixFileDescriptor &pipe)
    {
        Q_UNUSED(options)
        Q_UNUSED(pipe)
        sendErrorReply(QStringLiteral("org.kde.KWin.ScreenShot2.Error.InvalidWindow"),
                       QStringLiteral("the test double cannot capture single windows"));
        return {};
    }
    QVariantMap CaptureInteractive(uint kind, const QVariantMap &options, const QDBusUnixFileDescriptor &pipe)
    {
        Q_UNUSED(kind)
        Q_UNUSED(options)
        Q_UNUSED(pipe)
        sendErrorReply(QStringLiteral("org.kde.KWin.ScreenShot2.Error.Cancelled"), QStringLiteral("Screenshot got cancelled"));
        return {};
    }

private:
    static QImage grab()
    {
        // Window 0 is the X root window: the whole screen of the Xvfb server.
        return QGuiApplication::primaryScreen()->grabWindow(0).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    QVariantMap send(const QImage &image, const QDBusUnixFileDescriptor &pipe)
    {
        if (image.isNull() || !pipe.isValid()) {
            sendErrorReply(QDBusError::Failed, QStringLiteral("nothing to capture"));
            return {};
        }
        // As KWin does: the reply goes out first, the pixels follow from a thread.
        const int fd = ::dup(pipe.fileDescriptor());
        std::thread([image, fd] {
            const char *data = reinterpret_cast<const char *>(image.constBits());
            qsizetype left = image.sizeInBytes();
            while (left > 0) {
                const ssize_t n = ::write(fd, data, size_t(left));
                if (n <= 0)
                    break;
                data += n;
                left -= n;
            }
            ::close(fd);
        }).detach();
        std::printf("fakekwin: sent %dx%d\n", image.width(), image.height());
        std::fflush(stdout);
        return QVariantMap{
            {QStringLiteral("type"), QStringLiteral("raw")},
            {QStringLiteral("format"), uint(image.format())},
            {QStringLiteral("width"), uint(image.width())},
            {QStringLiteral("height"), uint(image.height())},
            {QStringLiteral("stride"), uint(image.bytesPerLine())},
            {QStringLiteral("scale"), 1.0},
        };
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    FakeScreenShot2 service;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QStringLiteral("org.nexus.test.FakeKWin"))
        || !bus.registerObject(QStringLiteral("/org/kde/KWin/ScreenShot2"), &service,
                               QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties)) {
        std::fprintf(stderr, "fakekwin: cannot register on the session bus\n");
        return 1;
    }
    std::printf("fakekwin: ready\n");
    std::fflush(stdout);
    return app.exec();
}

#include "fakekwin.moc"
