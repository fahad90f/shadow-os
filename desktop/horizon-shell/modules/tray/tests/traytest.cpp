// SPDX-License-Identifier: GPL-3.0-or-later
//
// traytest — status notifier items for testing the Shadow Horizon system
// tray. Everything the tray does to an item is printed on stdout as an
// "event ..." line, so a test can check that a click in the shell arrived.
//
//   main item     theme icon, tool tip with markup, a menu with a checkable
//                 entry, a radio group, a separator, a disabled entry, an
//                 entry that appears while the menu is open, and submenus
//   pixmap item   its icon exists only as pixels (IconPixmap), with an
//                 overlay icon
//   status item   goes Passive → Active → NeedsAttention in turn
//                 (--status-interval MS, default 4000)
//   --hostile     adds an item that misbehaves: oversized pixmaps, a title
//                 of 100 000 characters, markup and control characters in
//                 its texts, a menu of 3000 entries nested 12 levels deep
#include <KStatusNotifierItem>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCommandLineParser>
#include <QMenu>
#include <QPainter>
#include <QTextStream>
#include <QTimer>

static void event(const QString &text)
{
    QTextStream out(stdout);
    out << "event " << text << Qt::endl;
}

static QIcon paintedIcon(const QColor &colour, const QString &letter, const QList<int> &sizes)
{
    QIcon icon;
    for (const int size : sizes) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(colour);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QRectF(1, 1, size - 2, size - 2));
        QFont font = painter.font();
        font.setPixelSize(qMax(6, size * 6 / 10));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(pixmap.rect(), Qt::AlignCenter, letter);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

static QAction *logged(QMenu *menu, const QString &text)
{
    QAction *action = menu->addAction(text);
    QObject::connect(action, &QAction::triggered, action, [text](bool checked) {
        event(QStringLiteral("triggered \"%1\" checked=%2").arg(text).arg(checked));
    });
    return action;
}

static void watch(KStatusNotifierItem *item, const QString &name)
{
    QObject::connect(item, &KStatusNotifierItem::activateRequested, item, [name](bool active, const QPoint &pos) {
        event(QStringLiteral("%1 activate active=%2 pos=%3,%4").arg(name).arg(active).arg(pos.x()).arg(pos.y()));
    });
    QObject::connect(item, &KStatusNotifierItem::secondaryActivateRequested, item, [name](const QPoint &pos) {
        event(QStringLiteral("%1 secondaryActivate pos=%2,%3").arg(name).arg(pos.x()).arg(pos.y()));
    });
    QObject::connect(item, &KStatusNotifierItem::scrollRequested, item, [name](int delta, Qt::Orientation orientation) {
        event(QStringLiteral("%1 scroll delta=%2 %3").arg(name).arg(delta)
                  .arg(orientation == Qt::Horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical")));
    });
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("hostile"), QStringLiteral("Add an item that misbehaves.")});
    parser.addOption({QStringLiteral("status-interval"), QStringLiteral("Status change interval in ms."),
                      QStringLiteral("ms"), QStringLiteral("4000")});
    parser.process(app);

    // ---- main item ----------------------------------------------------------------
    auto *mainItem = new KStatusNotifierItem(QStringLiteral("shadow-tray-test-main"), &app);
    mainItem->setCategory(KStatusNotifierItem::ApplicationStatus);
    mainItem->setStatus(KStatusNotifierItem::Active);
    mainItem->setTitle(QStringLiteral("Tray test"));
    mainItem->setIconByName(QStringLiteral("dialog-information"));
    mainItem->setToolTip(QStringLiteral("dialog-information"), QStringLiteral("Tray test"),
                         QStringLiteral("Main item with a <b>menu</b><br/>second line &amp; more"));
    mainItem->setStandardActionsEnabled(false);
    watch(mainItem, QStringLiteral("main"));

    auto *menu = new QMenu;
    logged(menu, QStringLiteral("&Open window"))->setIcon(QIcon::fromTheme(QStringLiteral("document-open")));
    QAction *feature = logged(menu, QStringLiteral("Enable &feature"));
    feature->setCheckable(true);
    feature->setChecked(true);
    auto *group = new QActionGroup(menu);
    for (const QString &mode : {QStringLiteral("Mode: quiet"), QStringLiteral("Mode: loud")}) {
        QAction *action = logged(menu, mode);
        action->setCheckable(true);
        group->addAction(action);
    }
    group->actions().constFirst()->setChecked(true);
    menu->addSeparator();
    logged(menu, QStringLiteral("Unavailable entry"))->setEnabled(false);
    logged(menu, QStringLiteral("Under_score and __double"));
    // An entry that only exists while the menu is open: tests live updates.
    QAction *live = menu->addAction(QStringLiteral("Seconds open: 0"));
    live->setVisible(false);
    auto *liveTimer = new QTimer(menu);
    int seconds = 0;
    QObject::connect(menu, &QMenu::aboutToShow, menu, [&] {
        event(QStringLiteral("menu aboutToShow"));
        seconds = 0;
        live->setText(QStringLiteral("Seconds open: 0"));
        live->setVisible(true);
        liveTimer->start(1000);
    });
    QObject::connect(menu, &QMenu::aboutToHide, menu, [&] {
        event(QStringLiteral("menu aboutToHide"));
        liveTimer->stop();
        live->setVisible(false);
    });
    QObject::connect(liveTimer, &QTimer::timeout, menu, [&] {
        live->setText(QStringLiteral("Seconds open: %1").arg(++seconds));
    });
    QMenu *more = menu->addMenu(QStringLiteral("&More"));
    logged(more, QStringLiteral("Sub one"));
    logged(more, QStringLiteral("Sub two"))->setCheckable(true);
    more->addSeparator();
    QMenu *deeper = more->addMenu(QStringLiteral("Deeper"));
    logged(deeper, QStringLiteral("Leaf A"));
    logged(deeper, QStringLiteral("Leaf B"));
    menu->addSeparator();
    QAction *quit = logged(menu, QStringLiteral("&Quit test client"));
    quit->setIcon(QIcon::fromTheme(QStringLiteral("application-exit")));
    QObject::connect(quit, &QAction::triggered, &app, &QCoreApplication::quit, Qt::QueuedConnection);
    mainItem->setContextMenu(menu);

    // ---- pixmap item --------------------------------------------------------------
    auto *pixmapItem = new KStatusNotifierItem(QStringLiteral("shadow-tray-test-pixmap"), &app);
    pixmapItem->setStatus(KStatusNotifierItem::Active);
    pixmapItem->setTitle(QStringLiteral("Pixmap item"));
    pixmapItem->setIconByPixmap(paintedIcon(QColor(0x9b, 0x6b, 0xff), QStringLiteral("P"), {16, 22, 32, 48}));
    pixmapItem->setOverlayIconByPixmap(paintedIcon(QColor(0xff, 0x4d, 0x6a), QStringLiteral("!"), {16, 24}));
    pixmapItem->setToolTip(QIcon(), QStringLiteral("Pixmap item"), QStringLiteral("Its icon is sent as pixels"));
    pixmapItem->setStandardActionsEnabled(false);
    watch(pixmapItem, QStringLiteral("pixmap"));
    auto *pixmapMenu = new QMenu;
    logged(pixmapMenu, QStringLiteral("Pixmap entry"))->setIcon(paintedIcon(QColor(0x2e, 0xe6, 0xa6), QStringLiteral("E"), {22}));
    pixmapItem->setContextMenu(pixmapMenu);

    // ---- status item --------------------------------------------------------------
    auto *statusItem = new KStatusNotifierItem(QStringLiteral("shadow-tray-test-status"), &app);
    statusItem->setTitle(QStringLiteral("Status item"));
    statusItem->setIconByPixmap(paintedIcon(QColor(0x3d, 0x8b, 0xff), QStringLiteral("S"), {22, 44}));
    statusItem->setAttentionIconByPixmap(paintedIcon(QColor(0xff, 0xb0, 0x20), QStringLiteral("A"), {22, 44}));
    statusItem->setStandardActionsEnabled(false);
    statusItem->setStatus(KStatusNotifierItem::Passive);
    watch(statusItem, QStringLiteral("status"));
    auto *statusTimer = new QTimer(&app);
    QObject::connect(statusTimer, &QTimer::timeout, statusItem, [statusItem] {
        const auto next = statusItem->status() == KStatusNotifierItem::Passive ? KStatusNotifierItem::Active
            : statusItem->status() == KStatusNotifierItem::Active ? KStatusNotifierItem::NeedsAttention
                                                                 : KStatusNotifierItem::Passive;
        statusItem->setStatus(next);
        statusItem->setToolTip(QIcon(), QStringLiteral("Status item"),
                               next == KStatusNotifierItem::Passive ? QStringLiteral("passive")
                               : next == KStatusNotifierItem::Active ? QStringLiteral("active")
                                                                     : QStringLiteral("needs attention"));
        event(QStringLiteral("status now %1").arg(next == KStatusNotifierItem::Passive ? QStringLiteral("Passive")
                                                  : next == KStatusNotifierItem::Active ? QStringLiteral("Active")
                                                                                        : QStringLiteral("NeedsAttention")));
    });
    statusTimer->start(qMax(250, parser.value(QStringLiteral("status-interval")).toInt()));

    // ---- an item that misbehaves ----------------------------------------------------
    if (parser.isSet(QStringLiteral("hostile"))) {
        auto *hostile = new KStatusNotifierItem(QStringLiteral("shadow-tray-test-hostile"), &app);
        hostile->setStatus(KStatusNotifierItem::Active);
        hostile->setTitle(QString(100000, QLatin1Char('T')) + QChar(0x202E) + QStringLiteral("evil\n\a"));
        hostile->setIconByPixmap(paintedIcon(QColor(0xff, 0x4d, 0x6a), QStringLiteral("H"), {16, 512, 1500, 2048}));
        hostile->setToolTip(QIcon(), QStringLiteral("<script>alert(1)</script><img src=\"http://127.0.0.1:9/x.png\">"),
                            QStringLiteral("line<br>").repeated(500) + QString(200000, QLatin1Char('x')));
        hostile->setStandardActionsEnabled(false);
        watch(hostile, QStringLiteral("hostile"));
        auto *big = new QMenu;
        QMenu *nested = big;
        for (int depth = 0; depth < 12; ++depth) {
            nested = nested->addMenu(QStringLiteral("Level %1").arg(depth + 1));
            nested->addAction(QStringLiteral("Inside level %1").arg(depth + 1));
        }
        for (int i = 0; i < 3000; ++i)
            big->addAction(QStringLiteral("Entry %1 ").arg(i) + (i == 1 ? QString(5000, QLatin1Char('L')) : QString()));
        hostile->setContextMenu(big);
    }

    event(QStringLiteral("started"));
    return app.exec();
}
