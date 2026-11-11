// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plain window that asks the compositor not to consider the session idle
// while it is visible (zwp_idle_inhibit_manager_v1), like a video player.
// Prints "inhibiting" once the inhibitor exists.
#include "qwayland-idle-inhibit-unstable-v1.h"

#include <QGuiApplication>
#include <QRasterWindow>
#include <QTextStream>
#include <QTimer>
#include <QtWaylandClient/QWaylandClientExtension>
#include <qpa/qplatformnativeinterface.h>

class InhibitManager : public QWaylandClientExtensionTemplate<InhibitManager>,
                       public QtWayland::zwp_idle_inhibit_manager_v1
{
    Q_OBJECT
public:
    InhibitManager() : QWaylandClientExtensionTemplate<InhibitManager>(1) { initialize(); }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QRasterWindow window;
    window.setTitle(QStringLiteral("idle inhibit test"));
    window.resize(320, 200);
    window.show();
    InhibitManager manager;
    QTimer::singleShot(1000, &app, [&] {
        auto *surface = static_cast<wl_surface *>(
            QGuiApplication::platformNativeInterface()->nativeResourceForWindow(QByteArrayLiteral("surface"), &window));
        if (!manager.isActive() || !surface) {
            QTextStream(stdout) << "unavailable" << Qt::endl;
            QCoreApplication::exit(1);
            return;
        }
        manager.create_inhibitor(surface);
        QTextStream(stdout) << "inhibiting" << Qt::endl;
    });
    return app.exec();
}

#include "idle-inhibit-window.moc"
