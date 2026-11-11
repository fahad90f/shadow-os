// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document.h"
#include "screengrabber.h"

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickPaintedItem>
#include <QVariantList>

class QMimeData;

// Process exit codes of the non-interactive mode (documented in --help and README.md).
namespace ExitCode {
enum {
    Ok = 0,
    CaptureFailed = 1,      // no usable capture back-end, or the compositor refused
    Usage = 2,              // invalid command line
    Cancelled = 3,          // the user cancelled the region / window selection
    WriteFailed = 4,        // the output file could not be written
    ClipboardFailed = 5,    // the clipboard could not be set
};
}

struct CliOptions
{
    bool nonInteractive = false;    // --output and/or --clipboard: no result window
    QString output;
    bool clipboard = false;
    int statusFd = -1;              // --clipboard: where the waiting parent process gets the exit code
};

// Drives a capture from request to result: hiding the window, the delay, the
// frozen-screen region selection, and what happens with the picture
// afterwards (result window, or file / clipboard in non-interactive mode).
class Controller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Document *document READ document CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool windowVisible READ windowVisible NOTIFY stateChanged)
    Q_PROPERTY(bool selectingRegion READ selectingRegion NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString backend READ backend NOTIFY stateChanged)
    // "full" | "monitor" | "window" | "pick" | "region"
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY settingsChanged)
    Q_PROPERTY(int delay READ delay WRITE setDelay NOTIFY settingsChanged)
    Q_PROPERTY(bool includePointer READ includePointer WRITE setIncludePointer NOTIFY settingsChanged)
    Q_PROPERTY(QString monitor READ monitor WRITE setMonitor NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged)
    Q_PROPERTY(QStringList formats READ formats CONSTANT)
    Q_PROPERTY(QString format READ format WRITE setFormat NOTIFY settingsChanged)
    Q_PROPERTY(QString savedPath READ savedPath NOTIFY savedChanged)
    Q_PROPERTY(QString saveDirectory READ saveDirectory CONSTANT)
    Q_PROPERTY(bool holdsClipboard READ holdsClipboard NOTIFY clipboardChanged)
public:
    explicit Controller(QObject *parent = nullptr);
    static Controller *instance() { return s_instance; }

    Document *document() { return &m_document; }
    bool busy() const { return m_busy; }
    bool windowVisible() const { return m_windowVisible; }
    bool selectingRegion() const { return m_selecting; }
    QString error() const { return m_error; }
    QString backend() const { return m_backend; }
    QString mode() const { return m_mode; }
    void setMode(const QString &mode);
    int delay() const { return m_delay; }
    void setDelay(int seconds);
    bool includePointer() const { return m_pointer; }
    void setIncludePointer(bool on);
    QString monitor() const { return m_monitor; }
    void setMonitor(const QString &name);
    QVariantList screens() const;
    QStringList formats() const { return m_formats; }
    QString format() const { return m_format; }
    void setFormat(const QString &format);
    QString savedPath() const { return m_savedPath; }
    QString saveDirectory() const;
    bool holdsClipboard() const { return !m_clipboardData.isNull(); }
    const QImage &frozen() const { return m_frozen; }

    void setCli(const CliOptions &cli) { m_cli = cli; }
    // First capture of this process. In interactive mode the window appears
    // when the capture is done.
    void start();

    // Write 'image' to 'path' in the format named by the file extension.
    // Returns an empty string on success, otherwise the reason.
    static QString writeImage(const QImage &image, const QString &path);
    static QStringList writableFormats();

    // A fresh "<Pictures>/Screenshots/Screenshot_<date>_<time>.<format>" that does not exist yet.
    Q_INVOKABLE QString suggestedPath() const;
    Q_INVOKABLE void capture();
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool saveAs(const QString &path);
    Q_INVOKABLE bool copyToClipboard();
    Q_INVOKABLE void openFolder();
    // Region selection, in compositor (logical) coordinates.
    Q_INVOKABLE void finishRegion(qreal x, qreal y, qreal width, qreal height);
    Q_INVOKABLE void cancelRegion();
    // The result window was closed: quit, unless this process still has to
    // serve the clipboard content to other applications.
    Q_INVOKABLE void windowClosed();

Q_SIGNALS:
    void stateChanged();
    void settingsChanged();
    void screensChanged();
    void savedChanged();
    void clipboardChanged();
    // For the toaster: kind is "success" | "danger" | "info".
    void message(const QString &text, const QString &kind);

private:
    void grabNow();
    void grabFinished(const QImage &image, const QString &error, bool cancelled, const QString &backend);
    void deliver(const QImage &image);
    void fail(const QString &error, int exitCode);
    void exitCli(int code);
    bool setClipboard(const QImage &image);

    static Controller *s_instance;
    Document m_document;
    ScreenGrabber m_grabber;
    CliOptions m_cli;
    QImage m_frozen;
    bool m_busy = false;
    bool m_windowVisible = false;
    bool m_selecting = false;
    bool m_windowClosed = false;
    QString m_error;
    QString m_backend;
    QString m_mode = QStringLiteral("full");
    int m_delay = 0;
    bool m_pointer = false;
    QString m_monitor;
    QStringList m_formats;
    QString m_format = QStringLiteral("png");
    QString m_savedPath;
    QPointer<QMimeData> m_clipboardData;
};

// One screen's part of the frozen capture, shown full-screen while the user
// selects a region.
class FrozenView : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    // Geometry of the screen this item covers, compositor coordinates.
    Q_PROPERTY(QRectF screenGeometry MEMBER m_geometry NOTIFY screenGeometryChanged)
public:
    explicit FrozenView(QQuickItem *parent = nullptr);
    void paint(QPainter *painter) override;
Q_SIGNALS:
    void screenGeometryChanged();
private:
    QRectF m_geometry;
};
