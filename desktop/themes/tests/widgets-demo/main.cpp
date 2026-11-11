// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test application for the NEXUS look of third-party Qt applications: a plain
// Qt Widgets window that knows nothing about NEXUS. Everything it shows comes
// from the platform theme (kdeglobals: palette, fonts, icon theme, widget
// style), so a screenshot of it is a screenshot of the system defaults.
//
//   nexus-themes-widgets-demo [--report FILE] [--title TEXT] [--page N]
//
// --report writes what the application was given (style, fonts, icon theme,
// palette) as text, for comparison with the design tokens.
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCommandLineParser>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

using namespace Qt::StringLiterals;

namespace {

QString hex(const QColor &c) { return c.name(QColor::HexRgb).toUpper(); }

// Runs a function whenever the application palette changes, which is what
// happens in running applications when the user switches the appearance.
class PaletteWatcher : public QObject
{
public:
    explicit PaletteWatcher(std::function<void()> fn, QObject *parent = nullptr)
        : QObject(parent), m_fn(std::move(fn))
    {
        qApp->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
            // After the event has been delivered, so the new palette is in place.
            QTimer::singleShot(0, this, m_fn);
        }
        return false;
    }

private:
    std::function<void()> m_fn;
};

QString report()
{
    QString out;
    QTextStream s(&out);
    const QPalette p = QApplication::palette();
    s << "platform=" << QGuiApplication::platformName() << '\n';
    s << "style=" << QApplication::style()->name() << '\n';
    s << "iconTheme=" << QIcon::themeName() << '\n';
    s << "font=" << QApplication::font().family() << ',' << QApplication::font().pointSizeF() << '\n';
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    s << "fixedFont=" << fixed.family() << ',' << fixed.pointSizeF() << '\n';
    const QFont title = QFontDatabase::systemFont(QFontDatabase::TitleFont);
    s << "titleFont=" << title.family() << ',' << title.pointSizeF() << ',' << title.weight() << '\n';
    const struct { const char *name; QPalette::ColorRole role; } roles[] = {
        {"Window", QPalette::Window}, {"WindowText", QPalette::WindowText}, {"Base", QPalette::Base},
        {"AlternateBase", QPalette::AlternateBase}, {"Text", QPalette::Text}, {"Button", QPalette::Button},
        {"ButtonText", QPalette::ButtonText}, {"Highlight", QPalette::Highlight},
        {"HighlightedText", QPalette::HighlightedText}, {"ToolTipBase", QPalette::ToolTipBase},
        {"ToolTipText", QPalette::ToolTipText}, {"Link", QPalette::Link}, {"LinkVisited", QPalette::LinkVisited},
        {"PlaceholderText", QPalette::PlaceholderText}, {"Accent", QPalette::Accent},
    };
    for (const auto &r : roles) {
        s << "palette." << r.name << '=' << hex(p.color(QPalette::Active, r.role)) << '\n';
    }
    s << "palette.disabled.WindowText=" << hex(p.color(QPalette::Disabled, QPalette::WindowText)) << '\n';
    s << "palette.inactive.Highlight=" << hex(p.color(QPalette::Inactive, QPalette::Highlight)) << '\n';
    for (const char *icon : {"folder", "folder-documents", "user-home", "document-open", "org.nexus.files",
                             "nexus-os", "nexus-forensics", "edit-copy", "text-x-generic"}) {
        s << "icon." << icon << '=' << (QIcon::hasThemeIcon(QLatin1String(icon)) ? "found" : "MISSING") << '\n';
    }
    s << "singleClick=" << QApplication::style()->styleHint(QStyle::SH_ItemView_ActivateItemOnSingleClick) << '\n';
    return out;
}

QWidget *controlsPage()
{
    auto *page = new QWidget;
    auto *root = new QHBoxLayout(page);

    auto *buttons = new QGroupBox(u"Buttons"_s);
    auto *bl = new QVBoxLayout(buttons);
    auto *normal = new QPushButton(QIcon::fromTheme(u"document-save"_s), u"Save"_s);
    auto *primary = new QPushButton(u"Default button"_s);
    primary->setDefault(true);
    auto *disabled = new QPushButton(u"Disabled"_s);
    disabled->setEnabled(false);
    auto *checked = new QPushButton(u"Checked"_s);
    checked->setCheckable(true);
    checked->setChecked(true);
    auto *flat = new QPushButton(u"Flat"_s);
    flat->setFlat(true);
    auto *tool = new QToolButton;
    tool->setText(u"Tool button"_s);
    tool->setIcon(QIcon::fromTheme(u"configure"_s));
    tool->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tool->setPopupMode(QToolButton::MenuButtonPopup);
    auto *toolMenu = new QMenu(tool);
    toolMenu->addAction(u"First"_s);
    toolMenu->addAction(u"Second"_s);
    tool->setMenu(toolMenu);
    for (QWidget *w : {static_cast<QWidget *>(normal), static_cast<QWidget *>(primary), static_cast<QWidget *>(disabled),
                       static_cast<QWidget *>(checked), static_cast<QWidget *>(flat), static_cast<QWidget *>(tool)}) {
        bl->addWidget(w);
    }
    bl->addStretch();

    auto *choices = new QGroupBox(u"Choices"_s);
    auto *cl = new QVBoxLayout(choices);
    auto *c1 = new QCheckBox(u"Checked"_s);
    c1->setChecked(true);
    auto *c2 = new QCheckBox(u"Unchecked"_s);
    auto *c3 = new QCheckBox(u"Partially checked"_s);
    c3->setTristate(true);
    c3->setCheckState(Qt::PartiallyChecked);
    auto *c4 = new QCheckBox(u"Disabled"_s);
    c4->setChecked(true);
    c4->setEnabled(false);
    auto *r1 = new QRadioButton(u"Selected"_s);
    r1->setChecked(true);
    auto *r2 = new QRadioButton(u"Not selected"_s);
    for (QWidget *w : {static_cast<QWidget *>(c1), static_cast<QWidget *>(c2), static_cast<QWidget *>(c3),
                       static_cast<QWidget *>(c4), static_cast<QWidget *>(r1), static_cast<QWidget *>(r2)}) {
        cl->addWidget(w);
    }
    cl->addStretch();

    auto *inputs = new QGroupBox(u"Input"_s);
    auto *fl = new QFormLayout(inputs);
    auto *edit = new QLineEdit(u"Selected text in a field"_s);
    edit->setSelection(0, 13);
    auto *placeholder = new QLineEdit;
    placeholder->setPlaceholderText(u"Placeholder text"_s);
    auto *off = new QLineEdit(u"Disabled field"_s);
    off->setEnabled(false);
    auto *combo = new QComboBox;
    combo->addItems({u"Combo box"_s, u"Second entry"_s, u"Third entry"_s});
    auto *spin = new QSpinBox;
    spin->setValue(42);
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setValue(60);
    auto *progress = new QProgressBar;
    progress->setValue(60);
    auto *link = new QLabel(u"Secondary text with a <a href=\"https://example.org\">link</a>"_s);
    fl->addRow(u"Text"_s, edit);
    fl->addRow(u"Empty"_s, placeholder);
    fl->addRow(u"Disabled"_s, off);
    fl->addRow(u"Choice"_s, combo);
    fl->addRow(u"Number"_s, spin);
    fl->addRow(u"Slider"_s, slider);
    fl->addRow(u"Progress"_s, progress);
    fl->addRow(u"Label"_s, link);

    root->addWidget(buttons);
    root->addWidget(choices);
    root->addWidget(inputs, 1);
    return page;
}

QWidget *viewsPage()
{
    auto *split = new QSplitter;
    auto *list = new QListWidget;
    const QPair<const char *, const char *> places[] = {
        {"user-home", "Home"}, {"folder-documents", "Documents"}, {"folder-download", "Downloads"},
        {"folder-pictures", "Pictures"}, {"folder-music", "Music"}, {"folder-videos", "Videos"},
        {"folder", "Projects"}, {"user-trash", "Trash"}, {"org.nexus.files", "NEXUS Files (application icon)"},
        {"nexus-forensics", "Forensics (category icon)"},
    };
    for (const auto &[icon, label] : places) {
        list->addItem(new QListWidgetItem(QIcon::fromTheme(QLatin1String(icon)), QLatin1String(label)));
    }
    list->setIconSize(QSize(32, 32));
    list->setCurrentRow(1);

    auto *tree = new QTreeWidget;
    tree->setAlternatingRowColors(true);
    tree->setHeaderLabels({u"Name"_s, u"Size"_s, u"Modified"_s});
    tree->setSortingEnabled(true);
    const char *rows[][3] = {{"report.odt", "48 KiB", "Today"}, {"capture.pcap", "1.2 MiB", "Yesterday"},
                             {"notes.txt", "2 KiB", "Monday"}, {"firmware.bin", "16 MiB", "3 Nov"},
                             {"exploit.py", "7 KiB", "1 Nov"}, {"wordlist.txt", "134 MiB", "28 Oct"}};
    for (const auto &row : rows) {
        auto *item = new QTreeWidgetItem(tree, {QLatin1String(row[0]), QLatin1String(row[1]), QLatin1String(row[2])});
        item->setIcon(0, QIcon::fromTheme(u"text-x-generic"_s));
    }
    tree->setCurrentItem(tree->topLevelItem(2));
    tree->header()->setStretchLastSection(true);
    tree->setColumnWidth(0, 220);

    split->addWidget(list);
    split->addWidget(tree);
    split->setStretchFactor(1, 1);
    return split;
}

QWidget *textPage()
{
    auto *edit = new QPlainTextEdit;
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    edit->setPlainText(u"#!/bin/sh\n# The fixed-width system font (kdeglobals: fixed=)\n"
                       "nmap -sV --top-ports 100 192.0.2.10\n"
                       "0O 1lI |{}[] => != <= ->\n"_s);
    return edit;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(u"nexus-themes-widgets-demo"_s);
    QGuiApplication::setDesktopFileName(u"nexus-themes-widgets-demo"_s);

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption reportOption(u"report"_s, u"Write style, fonts, icon theme and palette to <file>."_s, u"file"_s);
    QCommandLineOption titleOption(u"title"_s, u"Window title."_s, u"text"_s, u"Third-party Qt application"_s);
    QCommandLineOption pageOption(u"page"_s, u"Tab shown at start (0-2)."_s, u"n"_s, u"0"_s);
    parser.addOptions({reportOption, titleOption, pageOption});
    parser.process(app);

    QMainWindow window;
    window.setWindowTitle(parser.value(titleOption));

    auto *file = window.menuBar()->addMenu(u"&File"_s);
    auto *newAction = file->addAction(QIcon::fromTheme(u"document-new"_s), u"&New"_s);
    newAction->setShortcut(QKeySequence::New);
    auto *openAction = file->addAction(QIcon::fromTheme(u"document-open"_s), u"&Open…"_s);
    openAction->setShortcut(QKeySequence::Open);
    auto *recent = file->addMenu(QIcon::fromTheme(u"document-open-recent"_s), u"Open &Recent"_s);
    recent->addAction(u"report.odt"_s);
    recent->addAction(u"capture.pcap"_s);
    auto *saveAction = file->addAction(QIcon::fromTheme(u"document-save"_s), u"&Save"_s);
    saveAction->setShortcut(QKeySequence::Save);
    auto *disabledAction = file->addAction(u"Disabled entry"_s);
    disabledAction->setEnabled(false);
    file->addSeparator();
    auto *check = file->addAction(u"Checkable entry"_s);
    check->setCheckable(true);
    check->setChecked(true);
    file->addSeparator();
    auto *quit = file->addAction(QIcon::fromTheme(u"application-exit"_s), u"&Quit"_s);
    quit->setShortcut(QKeySequence::Quit);
    QObject::connect(quit, &QAction::triggered, &app, &QApplication::quit);
    auto *edit = window.menuBar()->addMenu(u"&Edit"_s);
    edit->addAction(QIcon::fromTheme(u"edit-undo"_s), u"Undo"_s);
    edit->addAction(QIcon::fromTheme(u"edit-copy"_s), u"Copy"_s);
    window.menuBar()->addMenu(u"&View"_s)->addAction(u"Full screen"_s);
    window.menuBar()->addMenu(u"&Help"_s)->addAction(u"About"_s);

    auto *bar = window.addToolBar(u"Main"_s);
    bar->setMovable(false);
    bar->addAction(newAction);
    bar->addAction(openAction);
    bar->addAction(saveAction);
    bar->addSeparator();
    bar->addAction(QIcon::fromTheme(u"edit-undo"_s), u"Undo"_s);
    bar->addAction(QIcon::fromTheme(u"edit-redo"_s), u"Redo"_s);
    bar->addSeparator();
    auto *toggle = bar->addAction(QIcon::fromTheme(u"view-list-details"_s), u"Details"_s);
    toggle->setCheckable(true);
    toggle->setChecked(true);

    auto *tabs = new QTabWidget;
    tabs->addTab(controlsPage(), u"Controls"_s);
    tabs->addTab(viewsPage(), u"Views"_s);
    tabs->addTab(textPage(), u"Fixed-width text"_s);
    tabs->setCurrentIndex(qBound(0, parser.value(pageOption).toInt(), 2));
    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    layout->addWidget(tabs);
    window.setCentralWidget(central);

    auto updateStatus = [&window] {
        window.statusBar()->showMessage(u"style: %1 · font: %2 %3pt · icons: %4 · window %5 · base %6 · highlight %7"_s
            .arg(QApplication::style()->name(), QApplication::font().family())
            .arg(QApplication::font().pointSizeF())
            .arg(QIcon::themeName(), hex(QApplication::palette().color(QPalette::Window)),
                 hex(QApplication::palette().color(QPalette::Base)),
                 hex(QApplication::palette().color(QPalette::Highlight))));
    };
    updateStatus();
    PaletteWatcher statusWatcher(updateStatus);

    window.resize(980, 620);
    window.show();

    if (parser.isSet(reportOption)) {
        const QString path = parser.value(reportOption);
        auto write = [path] {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(report().toUtf8());
            }
        };
        QTimer::singleShot(500, &app, write);
        new PaletteWatcher(write, &app);
    }
    return app.exec();
}
