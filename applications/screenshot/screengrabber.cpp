// SPDX-License-Identifier: GPL-3.0-or-later
#include "screengrabber.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QScreen>
#include <QUrl>
#include <QtConcurrentRun>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace {
// The compositor's bus name. SHADOW_SCREENSHOT_KWIN_SERVICE redirects the
// calls to another provider of the same interface — used by the tests, which
// run where KWin cannot offer ScreenShot2 (no OpenGL compositing).
QString kwinService()
{
    const QString override = qEnvironmentVariable("SHADOW_SCREENSHOT_KWIN_SERVICE");
    return override.isEmpty() ? u"org.kde.KWin"_s : override;
}
const QString kKWinPath = u"/org/kde/KWin/ScreenShot2"_s;
const QString kKWinInterface = u"org.kde.KWin.ScreenShot2"_s;
const QString kPortalService = u"org.freedesktop.portal.Desktop"_s;
const QString kPortalPath = u"/org/freedesktop/portal/desktop"_s;
const QString kPortalInterface = u"org.freedesktop.portal.Screenshot"_s;

// Read exactly 'size' bytes of pixel data; gives up when the compositor
// stops writing for 10 seconds.
QByteArray readPipe(int fd, qsizetype size)
{
    QByteArray data;
    data.resize(size);
    qsizetype got = 0;
    while (got < size) {
        pollfd p{fd, POLLIN, 0};
        const int ready = ::poll(&p, 1, 10000);
        if (ready == 0)
            break;
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        const ssize_t n = ::read(fd, data.data() + got, size_t(size - got));
        if (n > 0)
            got += n;
        else if (n == 0 || (errno != EINTR && errno != EAGAIN))
            break;
    }
    ::close(fd);
    data.truncate(got);
    return data;
}

QRect virtualGeometry()
{
    QRect all;
    const auto screens = QGuiApplication::screens();
    for (const QScreen *s : screens)
        all |= s->geometry();
    return all;
}

bool isUnavailableError(const QDBusError &e)
{
    switch (e.type()) {
    case QDBusError::ServiceUnknown:
    case QDBusError::UnknownObject:
    case QDBusError::UnknownInterface:
    case QDBusError::UnknownMethod:
    case QDBusError::Disconnected:
    case QDBusError::NoReply:
        return true;
    default:
        return false;
    }
}
} // namespace

ScreenGrabber::ScreenGrabber(QObject *parent)
    : QObject(parent)
{
}

QJsonObject ScreenGrabber::probe()
{
    QJsonObject o;
    const QDBusConnection bus = QDBusConnection::sessionBus();
    o["sessionBus"_L1] = bus.isConnected();
    bool kwin = false;
    bool portal = false;
    int version = 0;
    if (bus.isConnected()) {
        QDBusMessage get = QDBusMessage::createMethodCall(kwinService(), kKWinPath, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
        get << kKWinInterface << u"Version"_s;
        const QDBusMessage reply = bus.call(get, QDBus::Block, 2000);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            kwin = true;
            version = reply.arguments().constFirst().value<QDBusVariant>().variant().toInt();
        }
        QDBusMessage pget = QDBusMessage::createMethodCall(kPortalService, kPortalPath, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
        pget << kPortalInterface << u"version"_s;
        const QDBusMessage preply = bus.call(pget, QDBus::Block, 2000);
        portal = preply.type() == QDBusMessage::ReplyMessage;
    }
    o["kwinService"_L1] = kwinService();
    o["kwinScreenShot2"_L1] = kwin;
    o["kwinScreenShot2Version"_L1] = version;
    o["portalScreenshot"_L1] = portal;
    o["platform"_L1] = QGuiApplication::platformName();
    return o;
}

void ScreenGrabber::finish(const QImage &image, const QString &error, bool cancelled, const QString &backend)
{
    m_busy = false;
    Q_EMIT finished(image, error, cancelled, backend);
}

void ScreenGrabber::grab(const Request &request)
{
    if (m_busy)
        return;
    m_busy = true;
    m_request = request;
    m_kwinError.clear();

    switch (request.target) {
    case Workspace:
        callKWin(u"CaptureWorkspace"_s, {}, true);
        break;
    case Screen:
        callKWin(u"CaptureScreen"_s, {request.screenName}, false);
        break;
    case ActiveWindow:
        callKWin(u"CaptureActiveWindow"_s, {}, false);
        break;
    case PickWindow:
        callKWin(u"CaptureInteractive"_s, {uint(0)}, false);
        break;
    case Area:
        callKWin(u"CaptureArea"_s, {request.area.x(), request.area.y(), uint(request.area.width()), uint(request.area.height())}, false);
        break;
    }
}

void ScreenGrabber::callKWin(const QString &method, const QVariantList &leadingArguments, bool allowWorkspaceFallback)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        finish({}, tr("There is no D-Bus session bus, so neither the compositor nor the desktop portal can be asked for a screenshot."), false, {});
        return;
    }
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        finish({}, tr("Could not create a pipe for the image data."), false, {});
        return;
    }

    QVariantMap options;
    options[u"include-cursor"_s] = m_request.includePointer;
    options[u"native-resolution"_s] = true;
    if (m_request.target == ActiveWindow || m_request.target == PickWindow) {
        options[u"include-decoration"_s] = m_request.includeDecoration;
        options[u"include-shadow"_s] = false;
    }

    QDBusPendingCallWatcher *watcher = nullptr;
    {
        QDBusMessage call = QDBusMessage::createMethodCall(kwinService(), kKWinPath, kKWinInterface, method);
        QVariantList arguments = leadingArguments;
        arguments << options << QVariant::fromValue(QDBusUnixFileDescriptor(fds[1]));
        call.setArguments(arguments);
        // Interactive window picking waits for the user.
        const int timeout = m_request.target == PickWindow ? 120000 : 15000;
        watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, timeout), this);
    }
    // QtDBus holds its own duplicate of the write end for the message.
    ::close(fds[1]);
    const int readFd = fds[0];
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, readFd, allowWorkspaceFallback](QDBusPendingCallWatcher *w) {
        kwinReply(w, readFd, allowWorkspaceFallback);
    });
}

void ScreenGrabber::kwinReply(QDBusPendingCallWatcher *watcher, int readFd, bool allowWorkspaceFallback)
{
    watcher->deleteLater();
    const QDBusPendingReply<QVariantMap> reply = *watcher;
    if (reply.isError()) {
        ::close(readFd);
        const QDBusError error = reply.error();
        if (error.name().endsWith(".Cancelled"_L1)) {
            finish({}, tr("The capture was cancelled."), true, u"kwin"_s);
            return;
        }
        // KWin older than ScreenShot2 version 4 has no CaptureWorkspace: ask
        // for the area that covers every screen instead.
        if (allowWorkspaceFallback && error.type() == QDBusError::UnknownMethod) {
            const QRect all = virtualGeometry();
            callKWin(u"CaptureArea"_s, {all.x(), all.y(), uint(all.width()), uint(all.height())}, false);
            return;
        }
        QString why;
        if (isUnavailableError(error))
            why = tr("The compositor does not offer the org.kde.KWin.ScreenShot2 interface (%1). KWin provides it only while it composites with OpenGL.").arg(error.message());
        else if (error.name().endsWith(".NoAuthorized"_L1))
            why = tr("The compositor refused the request: this application is not authorised to take screenshots (%1).").arg(error.message());
        else
            why = tr("The compositor could not take the screenshot: %1").arg(error.message());
        tryPortal(why);
        return;
    }

    const QVariantMap meta = reply.value();
    const int width = meta.value(u"width"_s).toInt();
    const int height = meta.value(u"height"_s).toInt();
    const int stride = meta.value(u"stride"_s).toInt();
    const int format = meta.value(u"format"_s).toInt();
    const qreal scale = meta.value(u"scale"_s, 1.0).toDouble();
    if (meta.value(u"type"_s).toString() != "raw"_L1 || width <= 0 || height <= 0 || width > 65535 || height > 65535
        || stride < width || format <= QImage::Format_Invalid || format >= QImage::NImageFormats) {
        ::close(readFd);
        finish({}, tr("The compositor answered with image data this application does not understand."), false, u"kwin"_s);
        return;
    }

    const qsizetype size = qsizetype(stride) * height;
    auto *reader = new QFutureWatcher<QByteArray>(this);
    connect(reader, &QFutureWatcher<QByteArray>::finished, this, [this, reader, width, height, stride, format, scale, size] {
        reader->deleteLater();
        const QByteArray data = reader->result();
        if (data.size() != size) {
            finish({}, tr("The compositor stopped sending image data (%1 of %2 bytes received).").arg(data.size()).arg(size), false, u"kwin"_s);
            return;
        }
        QImage image(width, height, QImage::Format(format));
        if (image.isNull()) {
            finish({}, tr("Not enough memory for a %1 × %2 image.").arg(width).arg(height), false, u"kwin"_s);
            return;
        }
        const qsizetype rowBytes = qMin<qsizetype>(stride, image.bytesPerLine());
        for (int y = 0; y < height; ++y)
            memcpy(image.scanLine(y), data.constData() + qsizetype(y) * stride, size_t(rowBytes));
        image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
        image.setDevicePixelRatio(scale > 0 ? scale : 1.0);
        finish(image, {}, false, u"kwin"_s);
    });
    reader->setFuture(QtConcurrent::run(readPipe, readFd, size));
}

// ---- xdg-desktop-portal fall-back ------------------------------------------------

void ScreenGrabber::tryPortal(const QString &kwinError)
{
    m_kwinError = kwinError;
    if (m_request.target == ActiveWindow || m_request.target == PickWindow) {
        finish({}, kwinError + u' ' + tr("Single windows can only be captured through the compositor."), false, {});
        return;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString token = u"shadow_%1"_s.arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1);
    sender.replace(u'.', u'_');
    m_portalPath = u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(sender, token);
    bus.connect(kPortalService, m_portalPath, u"org.freedesktop.portal.Request"_s, u"Response"_s,
                this, SLOT(portalResponse(uint,QVariantMap)));

    QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kPortalInterface, u"Screenshot"_s);
    call << QString() << QVariantMap{{u"handle_token"_s, token}, {u"interactive"_s, false}, {u"modal"_s, false}};
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 15000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> reply = *w;
        if (!reply.isError())
            return;     // the Response signal delivers the result
        QDBusConnection::sessionBus().disconnect(kPortalService, m_portalPath, u"org.freedesktop.portal.Request"_s, u"Response"_s,
                                                 this, SLOT(portalResponse(uint,QVariantMap)));
        const QString portalWhy = isUnavailableError(reply.error())
            ? tr("The desktop portal's Screenshot interface is not available either.")
            : tr("The desktop portal failed as well: %1").arg(reply.error().message());
        finish({}, m_kwinError + u' ' + portalWhy, false, {});
    });
}

void ScreenGrabber::portalResponse(uint response, const QVariantMap &results)
{
    QDBusConnection::sessionBus().disconnect(kPortalService, m_portalPath, u"org.freedesktop.portal.Request"_s, u"Response"_s,
                                             this, SLOT(portalResponse(uint,QVariantMap)));
    if (!m_busy)
        return;
    if (response == 1) {
        finish({}, tr("The capture was cancelled."), true, u"portal"_s);
        return;
    }
    const QUrl uri(results.value(u"uri"_s).toString());
    if (response != 0 || !uri.isLocalFile()) {
        finish({}, m_kwinError + u' ' + tr("The desktop portal did not deliver a screenshot."), false, u"portal"_s);
        return;
    }
    QImage image(uri.toLocalFile());
    // The portal stores the picture as a file of its own; this application
    // manages saving itself, so that copy is not kept.
    QFile::remove(uri.toLocalFile());
    if (image.isNull()) {
        finish({}, tr("The screenshot delivered by the desktop portal could not be read."), false, u"portal"_s);
        return;
    }
    image = image.convertToFormat(QImage::Format_RGB32);

    // The portal always captures the whole workspace; cut out what was asked for.
    const QRect all = virtualGeometry();
    QRect wanted;
    if (m_request.target == Area) {
        wanted = m_request.area;
    } else if (m_request.target == Screen) {
        const auto screens = QGuiApplication::screens();
        for (const QScreen *s : screens) {
            if (s->name() == m_request.screenName)
                wanted = s->geometry();
        }
    }
    if (wanted.isValid() && all.isValid()) {
        const qreal f = qreal(image.width()) / all.width();
        const QRect px(qRound((wanted.x() - all.x()) * f), qRound((wanted.y() - all.y()) * f),
                       qRound(wanted.width() * f), qRound(wanted.height() * f));
        image = image.copy(px.intersected(image.rect()));
    }
    finish(image, {}, false, u"portal"_s);
}
