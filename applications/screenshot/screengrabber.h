// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QString>
#include <QVariantMap>

class QDBusPendingCallWatcher;

// Captures screen content.
//
// Primary back-end: KWin's org.kde.KWin.ScreenShot2 D-Bus interface. The
// caller hands KWin the write end of a pipe; KWin answers with the image
// geometry (width, height, stride, QImage format) and then writes the raw
// pixels into the pipe. KWin only serves applications whose installed
// .desktop file lists the interface in X-KDE-DBUS-Restricted-Interfaces.
//
// Fall-back, used only when KWin's interface cannot be used: the
// xdg-desktop-portal Screenshot interface, which delivers the whole
// workspace as a file. Single windows cannot be captured that way.
class ScreenGrabber : public QObject
{
    Q_OBJECT
public:
    enum Target {
        Workspace,      // every screen
        Screen,         // one monitor (screenName)
        ActiveWindow,   // the window that has focus
        PickWindow,     // KWin lets the user click a window
        Area,           // rectangle in compositor (logical) coordinates
    };
    struct Request {
        Target target = Workspace;
        QString screenName;
        QRect area;
        bool includePointer = false;
        bool includeDecoration = true;
    };

    explicit ScreenGrabber(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    void grab(const Request &request);

    // What is available right now (for --dump backends and the UI).
    static QJsonObject probe();

Q_SIGNALS:
    // image.isNull() → error says why; cancelled is true when the user
    // aborted an interactive selection. backend is "kwin" or "portal".
    void finished(const QImage &image, const QString &error, bool cancelled, const QString &backend);

private Q_SLOTS:
    void portalResponse(uint response, const QVariantMap &results);

private:
    void callKWin(const QString &method, const QVariantList &leadingArguments, bool allowWorkspaceFallback);
    void kwinReply(QDBusPendingCallWatcher *watcher, int readFd, bool allowWorkspaceFallback);
    void tryPortal(const QString &kwinError);
    void finish(const QImage &image, const QString &error, bool cancelled, const QString &backend);

    Request m_request;
    bool m_busy = false;
    QString m_portalPath;
    QString m_kwinError;
};
