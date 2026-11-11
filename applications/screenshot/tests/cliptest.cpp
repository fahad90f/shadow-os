// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test client for SHADOW Screenshot: an independent application that reads
// the clipboard through the ordinary Qt/Wayland path (wl_data_device, which
// requires a focused window) and stores the picture it receives.
//
//   shadow-screenshot-cliptest read  <out.png>   exit 0 if an image was pasted
//   shadow-screenshot-cliptest text  <string>    take the clipboard over with text
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
            // Qt needs a recent input serial to set the selection; give the test
            // time to click into the window first.
            if (attempts < 25)
                return;
            clipboard->setText(argument);
            std::printf("cliptest: clipboard set to text (window active: %d, after %d ms)\n", int(window.isActive()), attempts * 100);
            std::fflush(stdout);
            timer.stop();
            // Stay for a moment so the selection is announced, then leave.
            QTimer::singleShot(1500, &app, [clipboard, argument] {
                const bool took = clipboard->text() == argument;
                std::printf("cliptest: clipboard now holds the text: %s\n", took ? "yes" : "NO");
                QCoreApplication::exit(took ? 0 : 1);
            });
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
