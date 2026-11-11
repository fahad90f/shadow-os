// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller.h"

#include <command.h>

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageWriter>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#ifdef NEXUS_HAVE_KGUIADDONS
#include <KSystemClipboard>
#endif

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

Controller *Controller::s_instance = nullptr;

namespace {
QRect virtualGeometry()
{
    QRect all;
    const auto screens = QGuiApplication::screens();
    for (const QScreen *s : screens)
        all |= s->geometry();
    return all;
}

// Image format for a file name; empty if the extension is not one we write.
QByteArray formatForPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "png"_L1)
        return "png";
    if (suffix == "jpg"_L1 || suffix == "jpeg"_L1)
        return "jpeg";
    if (suffix == "webp"_L1)
        return "webp";
    return {};
}
} // namespace

Controller::Controller(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
    m_formats = writableFormats();
    connect(&m_grabber, &ScreenGrabber::finished, this, &Controller::grabFinished);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &Controller::screensChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &Controller::screensChanged);
    if (const QScreen *primary = QGuiApplication::primaryScreen())
        m_monitor = primary->name();

    // Notice when another application takes the clipboard over: from then on
    // this process no longer has to stay around to serve the picture.
#ifdef NEXUS_HAVE_KGUIADDONS
    connect(KSystemClipboard::instance(), &KSystemClipboard::changed, this, [this](QClipboard::Mode mode) {
        { FILE *f = fopen("/tmp/clipdbg", "a"); if (f) { fprintf(f, "%d changed mode=%d holds=%d cur=%p mine=%p\n", int(getpid()), int(mode), int(m_holdsClipboard), static_cast<const void *>(KSystemClipboard::instance()->mimeData(QClipboard::Clipboard)), static_cast<const void *>(m_clipboardData)); fclose(f); } }
        if (mode == QClipboard::Clipboard && m_holdsClipboard
            && KSystemClipboard::instance()->mimeData(QClipboard::Clipboard) != m_clipboardData)
            clipboardLost();
    });
#else
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        if (m_holdsClipboard && !QGuiApplication::clipboard()->ownsClipboard())
            clipboardLost();
    });
#endif
}

void Controller::clipboardLost()
{
    m_holdsClipboard = false;
    m_clipboardData = nullptr;
    Q_EMIT clipboardChanged();
    if (m_cli.nonInteractive || m_windowClosed)
        QCoreApplication::quit();
}

void Controller::assignScreen(QObject *window, const QString &screenName)
{
    auto *w = qobject_cast<QWindow *>(window);
    if (!w)
        return;
    const auto screens = QGuiApplication::screens();
    for (QScreen *s : screens) {
        if (s->name() == screenName) {
            w->setScreen(s);
            w->setGeometry(s->geometry());
            return;
        }
    }
}

QStringList Controller::writableFormats()
{
    QStringList formats;
    const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
    for (const char *f : {"png", "jpeg", "webp"}) {
        if (supported.contains(f))
            formats << QString::fromLatin1(f);
    }
    return formats;
}

void Controller::setMode(const QString &mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    Q_EMIT settingsChanged();
}

void Controller::setDelay(int seconds)
{
    seconds = qBound(0, seconds, 60);
    if (m_delay == seconds)
        return;
    m_delay = seconds;
    Q_EMIT settingsChanged();
}

void Controller::setIncludePointer(bool on)
{
    if (m_pointer == on)
        return;
    m_pointer = on;
    Q_EMIT settingsChanged();
}

void Controller::setMonitor(const QString &name)
{
    if (m_monitor == name)
        return;
    m_monitor = name;
    Q_EMIT settingsChanged();
}

void Controller::setFormat(const QString &format)
{
    if (m_format == format || !m_formats.contains(format))
        return;
    m_format = format;
    Q_EMIT settingsChanged();
}

QVariantList Controller::screens() const
{
    QVariantList list;
    const auto all = QGuiApplication::screens();
    for (const QScreen *s : all) {
        const QRect g = s->geometry();
        QString label = s->name();
        const QString model = (s->manufacturer() + u' ' + s->model()).trimmed();
        if (!model.isEmpty() && model != s->name())
            label += u" — "_s + model;
        list << QVariantMap{
            {u"name"_s, s->name()},
            {u"label"_s, label},
            {u"x"_s, g.x()}, {u"y"_s, g.y()}, {u"width"_s, g.width()}, {u"height"_s, g.height()},
            {u"scale"_s, s->devicePixelRatio()},
        };
    }
    return list;
}

QString Controller::saveDirectory() const
{
    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (pictures.isEmpty())
        pictures = QDir::homePath() + u"/Pictures"_s;
    return pictures + u"/Screenshots"_s;
}

QString Controller::suggestedPath() const
{
    const QString ext = m_format == "jpeg"_L1 ? u"jpg"_s : m_format;
    const QString base = saveDirectory() + u"/Screenshot_"_s
        + QDateTime::currentDateTime().toString(u"yyyy-MM-dd_HH-mm-ss"_s);
    QString path = base + u'.' + ext;
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = base + u'_' + QString::number(n) + u'.' + ext;
    return path;
}

// ---- capture -------------------------------------------------------------------

void Controller::start()
{
    m_busy = true;
    Q_EMIT stateChanged();
    // A short pause lets the launcher or menu that started us disappear.
    QTimer::singleShot(qMax(m_delay * 1000, 200), this, &Controller::grabNow);
}

void Controller::capture()
{
    if (m_busy)
        return;
    m_busy = true;
    m_error.clear();
    // Our own window must not be in the picture: hide it and give the
    // compositor time to finish its closing animation.
    m_windowVisible = false;
    Q_EMIT stateChanged();
    QTimer::singleShot(qMax(m_delay * 1000, 600), this, &Controller::grabNow);
}

void Controller::grabNow()
{
    ScreenGrabber::Request request;
    request.includePointer = m_pointer;
    if (m_mode == "monitor"_L1) {
        request.target = ScreenGrabber::Screen;
        request.screenName = m_monitor;
    } else if (m_mode == "window"_L1) {
        request.target = ScreenGrabber::ActiveWindow;
    } else if (m_mode == "pick"_L1) {
        request.target = ScreenGrabber::PickWindow;
    } else {
        request.target = ScreenGrabber::Workspace;     // "full", and the frozen picture for "region"
    }
    m_grabber.grab(request);
}

void Controller::grabFinished(const QImage &image, const QString &error, bool cancelled, const QString &backend)
{
    if (!backend.isEmpty())
        m_backend = backend;
    if (image.isNull()) {
        fail(error, cancelled ? ExitCode::Cancelled : ExitCode::CaptureFailed);
        return;
    }
    if (m_mode == "region"_L1) {
        m_frozen = image;
        m_selecting = true;
        Q_EMIT stateChanged();      // the overlays appear; finishRegion()/cancelRegion() continue
        return;
    }
    deliver(image);
}

void Controller::finishRegion(qreal x, qreal y, qreal width, qreal height)
{
    if (!m_selecting)
        return;
    m_selecting = false;
    const QRect all = virtualGeometry();
    const qreal f = all.width() > 0 ? qreal(m_frozen.width()) / all.width() : 1.0;
    const QRect px = QRectF((x - all.x()) * f, (y - all.y()) * f, width * f, height * f).toAlignedRect().intersected(m_frozen.rect());
    QImage image = px.width() >= 1 && px.height() >= 1 ? m_frozen.copy(px) : QImage();
    image.setDevicePixelRatio(m_frozen.devicePixelRatio());
    m_frozen = {};
    if (image.isNull()) {
        fail(tr("The selected area is empty."), ExitCode::Cancelled);
        return;
    }
    Q_EMIT stateChanged();
    deliver(image);
}

void Controller::cancelRegion()
{
    if (!m_selecting)
        return;
    m_selecting = false;
    m_frozen = {};
    fail(tr("The region selection was cancelled."), ExitCode::Cancelled);
}

void Controller::fail(const QString &error, int exitCode)
{
    m_busy = false;
    if (m_cli.nonInteractive) {
        std::fprintf(stderr, "nexus-screenshot: %s\n", qPrintable(error));
        exitCli(exitCode);
        return;
    }
    if (exitCode == ExitCode::Cancelled) {
        if (m_document.isEmpty() && m_error.isEmpty()) {
            // Started for a selection (e.g. Meta+Shift+S) that was abandoned:
            // there is nothing to show.
            QCoreApplication::exit(ExitCode::Cancelled);
            return;
        }
        // Otherwise keep showing what was there; cancelling is not an error.
        Q_EMIT message(error, u"info"_s);
    } else {
        m_error = error;
    }
    m_windowVisible = true;
    Q_EMIT stateChanged();
}

void Controller::deliver(const QImage &image)
{
    m_busy = false;
    if (m_cli.nonInteractive) {
        int code = ExitCode::Ok;
        if (!m_cli.output.isEmpty()) {
            const QString why = writeImage(image, m_cli.output);
            if (!why.isEmpty()) {
                std::fprintf(stderr, "nexus-screenshot: %s\n", qPrintable(why));
                exitCli(ExitCode::WriteFailed);
                return;
            }
            std::printf("%s\n", qPrintable(QFileInfo(m_cli.output).absoluteFilePath()));
            std::fflush(stdout);
        }
        if (m_cli.clipboard && !setClipboard(image)) {
            std::fprintf(stderr, "nexus-screenshot: the picture could not be placed on the clipboard\n");
            code = ExitCode::ClipboardFailed;
        }
        exitCli(code);
        return;
    }
    m_error.clear();
    m_document.setImage(image);
    m_savedPath.clear();
    Q_EMIT savedChanged();
    m_windowVisible = true;
    m_windowClosed = false;
    Q_EMIT stateChanged();
}

void Controller::exitCli(int code)
{
    if (m_cli.statusFd >= 0) {
        // The process that the user started is waiting for this exit code.
        // If the clipboard was set, this (background) process stays to serve
        // the data until another application replaces the clipboard content.
        const ssize_t written = ::write(m_cli.statusFd, &code, sizeof code);
        Q_UNUSED(written)
        ::close(m_cli.statusFd);
        m_cli.statusFd = -1;
        const int devnull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        if (code == ExitCode::Ok && holdsClipboard())
            return;
    }
    QCoreApplication::exit(code);
}

// ---- result actions ------------------------------------------------------------

QString Controller::writeImage(const QImage &image, const QString &path)
{
    const QByteArray format = formatForPath(path);
    if (format.isEmpty())
        return tr("“%1”: unsupported file type. Use .png, .jpg or .webp.").arg(path);
    if (!QImageWriter::supportedImageFormats().contains(format))
        return tr("This system cannot write %1 files (the Qt image format plug-in is not installed).").arg(QString::fromLatin1(format).toUpper());
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
        return tr("The folder “%1” could not be created.").arg(info.absolutePath());
    QImage out = image;
    out.setDevicePixelRatio(1.0);
    if (format == "jpeg")
        out = out.convertToFormat(QImage::Format_RGB32);
    QImageWriter writer(path, format);
    if (format == "jpeg")
        writer.setQuality(92);
    else if (format == "webp")
        writer.setQuality(90);
    if (!writer.write(out))
        return tr("“%1” could not be written: %2").arg(path, writer.errorString());
    return {};
}

bool Controller::saveAs(const QString &path)
{
    if (m_document.isEmpty())
        return false;
    QString target = path.trimmed();
    if (target.startsWith("~/"_L1))
        target = QDir::homePath() + target.mid(1);
    if (QFileInfo(target).isRelative())
        target = saveDirectory() + u'/' + target;
    const QString why = writeImage(m_document.image(), target);
    if (!why.isEmpty()) {
        Q_EMIT message(why, u"danger"_s);
        return false;
    }
    m_savedPath = target;
    Q_EMIT savedChanged();
    Q_EMIT message(tr("Saved to %1").arg(target), u"success"_s);
    return true;
}

bool Controller::save()
{
    return saveAs(suggestedPath());
}

bool Controller::setClipboard(const QImage &image)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    QImage out = image;
    out.setDevicePixelRatio(1.0);
    if (!out.save(&buffer, "PNG"))
        return false;
    auto *mime = new QMimeData;
    mime->setData(u"image/png"_s, png);
#ifdef NEXUS_HAVE_KGUIADDONS
    // Uses the compositor's data-control protocol, so it also works while no
    // window of ours has keyboard focus (non-interactive mode).
    KSystemClipboard::instance()->setMimeData(mime, QClipboard::Clipboard);
#else
    QGuiApplication::clipboard()->setMimeData(mime);
#endif
    m_clipboardData = mime;
    m_holdsClipboard = true;
    Q_EMIT clipboardChanged();
    return true;
}

bool Controller::copyToClipboard()
{
    if (m_document.isEmpty())
        return false;
    if (!setClipboard(m_document.image())) {
        Q_EMIT message(tr("The picture could not be placed on the clipboard."), u"danger"_s);
        return false;
    }
    Q_EMIT message(tr("Copied to the clipboard"), u"success"_s);
    return true;
}

void Controller::openFolder()
{
    const QString dir = m_savedPath.isEmpty() ? saveDirectory() : QFileInfo(m_savedPath).absolutePath();
    if (!QDir(dir).exists()) {
        Q_EMIT message(tr("%1 does not exist yet — nothing has been saved there.").arg(dir), u"info"_s);
        return;
    }
    if (Nexus::Command::available(u"nexus-files"_s)) {
        if (QProcess::startDetached(u"nexus-files"_s, {dir}))
            return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir)))
        Q_EMIT message(tr("No file manager could be started for %1.").arg(dir), u"danger"_s);
}

void Controller::windowClosed()
{
    m_windowClosed = true;
    m_windowVisible = false;
    Q_EMIT stateChanged();
    if (m_busy || m_selecting)
        return;
    // On Wayland the clipboard content lives in the application that copied
    // it. Stay (without a window) until someone else takes the clipboard.
    if (!holdsClipboard())
        QCoreApplication::quit();
}

// ---- FrozenView ----------------------------------------------------------------

FrozenView::FrozenView(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
}

void FrozenView::paint(QPainter *painter)
{
    const Controller *c = Controller::instance();
    if (!c || c->frozen().isNull())
        return;
    const QImage &image = c->frozen();
    const QRect all = virtualGeometry();
    const qreal f = all.width() > 0 ? qreal(image.width()) / all.width() : 1.0;
    const QRectF source((m_geometry.x() - all.x()) * f, (m_geometry.y() - all.y()) * f, m_geometry.width() * f, m_geometry.height() * f);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(QRectF(0, 0, width(), height()), image, source);
}
