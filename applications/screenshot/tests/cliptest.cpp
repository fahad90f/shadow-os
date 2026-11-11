// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test client for NEXUS Screenshot: an independent application that reads
// the clipboard through the ordinary Qt/Wayland path (wl_data_device, which
// requires a focused window) and stores the picture it receives.
//
//   nexus-screenshot-cliptest read  <out.png>   exit 0 if an image was pasted
//   nexus-screenshot-cliptest text  <string>    take the clipboard over with text
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QRasterWindow>
#include <QTimer>

#include <cstdio>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s read <out.png> | text <string>\n", argv[0]);
        return 2;
    }
    const QString command = QString::fromLocal8Bit(argv[1]);
    const QString argument = QString::fromLocal8Bit(argv[2]);

    QRasterWindow window;
    window.resize(240, 120);
    window.setTitle(QStringLiteral("cliptest"));
    window.show();
    window.requestActivate();

    int attempts = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        ++attempts;
        if (!window.isActive() && attempts < 60)
            return;
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (command == QLatin1String("text")) {
            clipboard->setText(argument);
            std::printf("cliptest: clipboard set to text\n");
            std::fflush(stdout);
            timer.stop();
            // Stay for a moment so the selection is announced, then leave.
            QTimer::singleShot(1500, &app, [] { QCoreApplication::exit(0); });
            return;
        }
        const QMimeData *mime = clipboard->mimeData();
        if (mime && mime->hasImage()) {
            const QImage image = qvariant_cast<QImage>(mime->imageData());
            if (!image.isNull() && image.save(argument)) {
                std::printf("cliptest: pasted image %dx%d, offered formats: %s\n", image.width(), image.height(),
                            qPrintable(mime->formats().join(QLatin1Char(' '))));
                QCoreApplication::exit(0);
                return;
            }
        }
        if (attempts >= 60) {
            std::fprintf(stderr, "cliptest: no image on the clipboard (active=%d, formats: %s)\n", int(window.isActive()),
                         mime ? qPrintable(mime->formats().join(QLatin1Char(' '))) : "-");
            QCoreApplication::exit(1);
        }
    });
    timer.start(100);
    return app.exec();
}
