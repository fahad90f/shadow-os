// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"

#include "schemes.h"
#include "searchbar.h"
#include "shells.h"
#include "shortcuts.h"
#include "sshdialog.h"
#include "tabbar.h"
#include "termwidget.h"
#include "theme.h"
#include "tokens.h"
#include "ui.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QProcess>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <csignal>

namespace {

constexpr int kRefreshMs = 1000;            // how often titles and the status bar follow the terminals
constexpr int kNoticeMs = 6000;             // how long a status message stays
constexpr int kHangUpWaitMs = 300;          // grace period for shells when the window closes
constexpr int kMaxRestoredTabs = 24;
constexpr int kSessionVersion = 1;
const char kFileManager[] = "nexus-files";

QString plainText(QString text)
{
    text.remove(QStringLiteral("…"));
    return text;
}

// Tab bars treat '&' as a mnemonic marker.
QString tabText(QString title)
{
    title.replace(QLatin1Char('&'), QLatin1String("&&"));
    return title;
}

// Menu icon of a profile; the default profile carries a star.
QIcon profileIcon(bool isDefault)
{
    return isDefault ? Theme::instance()->icon(QStringLiteral("star"), QStringLiteral("accent"), QStringLiteral("accent"))
                     : Theme::instance()->icon(QStringLiteral("terminal"), QStringLiteral("textSecondary"),
                                               QStringLiteral("accent"));
}

int paneCountOf(const QJsonObject &node)
{
    if (node.value(QStringLiteral("type")).toString() == QLatin1String("pane"))
        return 1;
    int count = 0;
    const QJsonArray children = node.value(QStringLiteral("children")).toArray();
    for (const QJsonValue &child : children)
        count += paneCountOf(child.toObject());
    return count;
}

bool sessionUsable(const QJsonObject &session)
{
    return session.value(QStringLiteral("version")).toInt() == kSessionVersion
        && !session.value(QStringLiteral("tabs")).toArray().isEmpty();
}

} // namespace

MainWindow::MainWindow(const StartupOptions &options, QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("TerminalWindow"));
    setWindowTitle(tr("Terminal"));
    setMinimumSize(Tokens::WindowMinWidth, Tokens::WindowMinHeight);
    resize(Tokens::WindowDefaultWidth, Tokens::WindowDefaultHeight);

    // A profile opacity below 100 % lets the desktop show through the
    // terminal background, which takes a window with an alpha channel. The
    // chrome stays opaque: #Root paints the whole window, the terminal
    // display then replaces its own area (alpha included).
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("xcb") || platform.startsWith(QLatin1String("wayland")))
        setAttribute(Qt::WA_TranslucentBackground);

    createActions();

    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("Root"));
    root->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(createHeader());
    layout->addWidget(createBanner());
    m_stack = new QStackedWidget(root);
    layout->addWidget(m_stack, 1);
    layout->addWidget(createStatusBar());
    setCentralWidget(root);

    // The search bar floats over the top right corner of the terminals. It
    // must not take space from them: resizing a terminal clears its selection
    // (the match that was just found) and makes the program redraw.
    m_searchBar = new SearchBar(root);
    m_searchBar->hide();
    m_stack->installEventFilter(this);

    createMenus();
    applyShortcuts();
    applySettings();

    connect(m_tabBar, &QTabBar::currentChanged, this, &MainWindow::onCurrentTabChanged);
    connect(m_tabBar, &QTabBar::tabMoved, this, &MainWindow::onTabMoved);
    connect(m_tabBar, &TabBar::closeRequested, this, [this](int index) { closeTab(index); });
    connect(m_tabBar, &TabBar::newTabRequested, this, [this] { newTab(); });
    connect(m_tabBar, &TabBar::tabMenuRequested, this, &MainWindow::showTabMenu);
    connect(m_tabBar, &TabBar::renameRequested, this, [this](int index, const QString &title) {
        if (TerminalTab *tab = tabAt(index)) {
            tab->setCustomTitle(title);
            updateTab(tab);
        }
    });
    connect(m_tabBar, &TabBar::editingFinished, this, [this] {
        if (TerminalPane *pane = currentPane())
            pane->focusTerminal();
    });

    connect(m_searchBar, &SearchBar::criteriaChanged, this, &MainWindow::runSearch);
    connect(m_searchBar, &SearchBar::nextRequested, this, [this] { searchStep(true); });
    connect(m_searchBar, &SearchBar::previousRequested, this, [this] { searchStep(false); });
    connect(m_searchBar, &SearchBar::closed, this, [this] {
        if (TerminalPane *pane = currentPane())
            pane->focusTerminal();
        updateActionStates();
    });

    connect(Settings::instance(), &Settings::changed, this, &MainWindow::applySettings);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(kRefreshMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::refreshStatus);
    m_refreshTimer->start();
    m_noticeTimer = new QTimer(this);
    m_noticeTimer->setSingleShot(true);
    m_noticeTimer->setInterval(kNoticeMs);
    connect(m_noticeTimer, &QTimer::timeout, this, [this] {
        m_statusNotice->hide();
        m_statusProcess->show();
        m_statusDirectory->show();
    });

    const QByteArray geometry = Settings::instance()->windowGeometry();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);

    openInitialContent(options);
}

MainWindow::~MainWindow() = default;

// ---- construction -------------------------------------------------------------------

QAction *MainWindow::action(const QString &id) const
{
    QAction *a = m_actions.value(id);
    Q_ASSERT_X(a, "MainWindow::action", qPrintable(id));
    return a;
}

void MainWindow::createActions()
{
    for (const ActionDef &def : Shortcuts::definitions()) {
        auto *a = new QAction(def.text, this);
        a->setObjectName(def.id);
        a->setCheckable(def.checkable);
        if (!def.icon.isEmpty())
            Theme::instance()->bindIcon(a, def.icon);
        // Added to the window itself, so shortcuts keep working while the
        // menu bar is hidden (compact mode).
        addAction(a);
        m_actions.insert(def.id, a);
    }

    auto on = [this](const char *id, const std::function<void()> &handler) {
        connect(action(QString::fromLatin1(id)), &QAction::triggered, this, handler);
    };
    auto withPane = [this](void (TerminalPane::*method)()) {
        return [this, method] {
            if (TerminalPane *pane = currentPane())
                (pane->*method)();
        };
    };

    on("new-tab", [this] { newTab(); });
    on("new-window", [this] { newWindow(); });
    on("new-ssh", [this] { newSshConnection(); });
    on("open-file-manager", [this] { openFileManager(); });
    on("close-pane", [this] { closeCurrentPane(); });
    on("close-tab", [this] { closeTab(m_tabBar->currentIndex()); });
    on("quit", [this] { close(); });

    on("copy", withPane(&TerminalPane::copy));
    on("paste", withPane(&TerminalPane::paste));
    on("paste-selection", withPane(&TerminalPane::pasteSelection));
    on("find", [this] { showSearch(); });
    on("find-next", [this] { searchStep(true); });
    on("find-previous", [this] { searchStep(false); });
    on("clear", withPane(&TerminalPane::clear));
    on("preferences", [this] { showPreferences(PreferencesDialog::Page::General); });
    on("shortcuts", [this] { showPreferences(PreferencesDialog::Page::Shortcuts); });

    on("zoom-in", withPane(&TerminalPane::zoomIn));
    on("zoom-out", withPane(&TerminalPane::zoomOut));
    on("zoom-reset", withPane(&TerminalPane::zoomReset));
    connect(action(QStringLiteral("fullscreen")), &QAction::triggered, this, &MainWindow::setFullScreen);
    connect(action(QStringLiteral("compact-mode")), &QAction::triggered, this, &MainWindow::setCompactMode);
    connect(action(QStringLiteral("status-bar")), &QAction::triggered, this,
            [](bool visible) { Settings::instance()->setShowStatusBar(visible); });

    on("next-tab", [this] { stepTab(+1); });
    on("previous-tab", [this] { stepTab(-1); });
    on("move-tab-left", [this] { moveTab(-1); });
    on("move-tab-right", [this] { moveTab(+1); });
    on("rename-tab", [this] { renameCurrentTab(); });
    for (int n = 1; n <= 9; ++n) {
        connect(action(QStringLiteral("tab-%1").arg(n)), &QAction::triggered, this, [this, n] {
            if (n <= tabCount())
                m_tabBar->setCurrentIndex(n - 1);
        });
    }

    on("split-right", [this] { split(Qt::Horizontal); });
    on("split-down", [this] { split(Qt::Vertical); });
    on("focus-left", [this] { focusPane(TerminalTab::Direction::Left); });
    on("focus-right", [this] { focusPane(TerminalTab::Direction::Right); });
    on("focus-up", [this] { focusPane(TerminalTab::Direction::Up); });
    on("focus-down", [this] { focusPane(TerminalTab::Direction::Down); });

    on("signal-int", [this] { sendSignal(SIGINT, QStringLiteral("SIGINT"), false); });
    on("signal-term", [this] { sendSignal(SIGTERM, QStringLiteral("SIGTERM"), false); });
    on("signal-kill", [this] { sendSignal(SIGKILL, QStringLiteral("SIGKILL"), true); });
    on("signal-stop", [this] { sendSignal(SIGSTOP, QStringLiteral("SIGSTOP"), false); });
    on("signal-cont", [this] { sendSignal(SIGCONT, QStringLiteral("SIGCONT"), false); });

    on("about", [this] { showAbout(); });
}

void MainWindow::applyShortcuts()
{
    for (const ActionDef &def : Shortcuts::definitions()) {
        QAction *a = action(def.id);
        const QList<QKeySequence> bindings = Shortcuts::current(def.id);
        a->setShortcuts(bindings);
        const QString name = plainText(def.text);
        a->setToolTip(bindings.isEmpty() ? name
                      : tr("%1 (%2)").arg(name, bindings.first().toString(QKeySequence::NativeText)));
    }
}

QToolButton *MainWindow::actionButton(const QString &id)
{
    QAction *a = action(id);
    auto *button = new QToolButton(this);
    button->setDefaultAction(a);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(Tokens::IconSmall + Tokens::SpaceXxs, Tokens::IconSmall + Tokens::SpaceXxs));
    button->setFixedSize(Tokens::ControlHeight - Tokens::SpaceXs, Tokens::ControlHeight - Tokens::SpaceXs);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    button->setAccessibleName(plainText(a->text()));
    return button;
}

QWidget *MainWindow::createHeader()
{
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("Header"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setFixedHeight(Tokens::ControlHeight + 2 * Tokens::SpaceXs + Tokens::BorderWidth);
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(Tokens::SpaceXs, 0, Tokens::SpaceSm, Tokens::BorderWidth);
    layout->setSpacing(Tokens::SpaceXxs);

    m_tabBar = new TabBar(header);
    layout->addWidget(m_tabBar);

    layout->addWidget(actionButton(QStringLiteral("new-tab")));
    QToolButton *more = Ui::iconButton(QStringLiteral("chevron-down"), tr("New tab with a profile, SSH connections"),
                                       header);
    more->setFixedWidth(Tokens::Icon + Tokens::SpaceXs);
    more->setPopupMode(QToolButton::InstantPopup);
    QMenu *moreMenu = Ui::menu(more);
    connect(moreMenu, &QMenu::aboutToShow, this, [this, moreMenu] { populateNewTabMenu(moreMenu); });
    more->setMenu(moreMenu);
    layout->addWidget(more);

    layout->addStretch(1);

    layout->addWidget(actionButton(QStringLiteral("split-right")));
    layout->addWidget(actionButton(QStringLiteral("split-down")));
    layout->addWidget(actionButton(QStringLiteral("find")));
    layout->addWidget(actionButton(QStringLiteral("new-ssh")));

    // The whole menu in one button, for compact mode (no menu bar).
    m_menuButton = Ui::iconButton(QStringLiteral("menu"), tr("Menu"), header);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    layout->addWidget(m_menuButton);
    return header;
}

QWidget *MainWindow::createBanner()
{
    m_bannerHolder = new QWidget(this);
    auto *holderLayout = new QVBoxLayout(m_bannerHolder);
    holderLayout->setContentsMargins(Tokens::SpaceSm, Tokens::SpaceSm, Tokens::SpaceSm, Tokens::SpaceSm);

    m_banner = new QFrame(m_bannerHolder);
    m_banner->setObjectName(QStringLiteral("Banner"));
    auto *layout = new QHBoxLayout(m_banner);
    layout->setContentsMargins(Tokens::SpaceLg, Tokens::SpaceSm, Tokens::SpaceSm, Tokens::SpaceSm);
    layout->setSpacing(Tokens::SpaceSm);
    m_bannerText = Ui::label(QString(), nullptr, m_banner);
    m_bannerText->setWordWrap(true);
    m_bannerAction = Ui::button(QString(), "primary", m_banner);
    QToolButton *dismiss = Ui::iconButton(QStringLiteral("close"), tr("Dismiss"), m_banner);
    layout->addWidget(m_bannerText, 1);
    layout->addWidget(m_bannerAction);
    layout->addWidget(dismiss);
    holderLayout->addWidget(m_banner);
    m_bannerHolder->hide();

    connect(dismiss, &QToolButton::clicked, this, &MainWindow::hideBanner);
    connect(m_bannerAction, &QPushButton::clicked, this, [this] {
        const std::function<void()> callback = m_bannerCallback;
        hideBanner();
        if (callback)
            callback();
    });
    return m_bannerHolder;
}

QWidget *MainWindow::createStatusBar()
{
    m_statusBar = new QWidget(this);
    m_statusBar->setObjectName(QStringLiteral("StatusBar"));
    m_statusBar->setAttribute(Qt::WA_StyledBackground, true);
    m_statusBar->setFixedHeight(Tokens::ControlHeight - Tokens::SpaceSm + Tokens::BorderWidth);
    auto *layout = new QHBoxLayout(m_statusBar);
    layout->setContentsMargins(Tokens::SpaceMd, Tokens::BorderWidth, Tokens::SpaceMd, 0);
    layout->setSpacing(Tokens::SpaceLg);

    m_statusProcess = Ui::label(QString(), "status", m_statusBar);
    m_statusProcess->setAccessibleName(tr("Foreground process"));
    m_statusDirectory = Ui::label(QString(), "status", m_statusBar);
    m_statusDirectory->setAccessibleName(tr("Working directory"));
    m_statusNotice = Ui::label(QString(), "statusStrong", m_statusBar);
    m_statusNotice->hide();
    m_statusProfile = Ui::label(QString(), "status", m_statusBar);
    m_statusProfile->setAccessibleName(tr("Profile"));
    m_statusSize = Ui::label(QString(), "status", m_statusBar);
    m_statusSize->setAccessibleName(tr("Terminal size"));
    m_statusZoom = Ui::label(QString(), "status", m_statusBar);
    m_statusZoom->setAccessibleName(tr("Zoom"));

    layout->addWidget(m_statusProcess);
    layout->addWidget(m_statusDirectory);
    layout->addWidget(m_statusNotice, 1);
    layout->addStretch(1);
    layout->addWidget(m_statusZoom);
    layout->addWidget(m_statusSize);
    layout->addWidget(m_statusProfile);
    return m_statusBar;
}

void MainWindow::createMenus()
{
    auto topMenu = [this](const QString &title) {
        QMenu *menu = Ui::menu(this);
        menu->setTitle(title);
        return menu;
    };
    auto add = [this](QMenu *menu, std::initializer_list<const char *> ids) {
        for (const char *id : ids) {
            if (id)
                menu->addAction(action(QString::fromLatin1(id)));
            else
                menu->addSeparator();
        }
    };

    // ---- File ---------------------------------------------------------------
    QMenu *file = topMenu(tr("File"));
    add(file, {"new-tab"});
    QMenu *profileMenu = Ui::subMenu(file, tr("New Tab with Profile"));
    connect(profileMenu, &QMenu::aboutToShow, this, [this, profileMenu] {
        profileMenu->clear();
        const QList<Profile> profiles = ProfileManager::instance()->profiles();
        const QString defaultId = ProfileManager::instance()->defaultId();
        for (const Profile &profile : profiles) {
            const QString id = profile.id;
            QAction *entry = profileMenu->addAction(tabText(profile.name), this, [this, id] { newTab(id); });
            entry->setIcon(profileIcon(id == defaultId));
        }
    });
    add(file, {"new-window", nullptr, "new-ssh"});
    QMenu *sshMenu = Ui::subMenu(file, tr("Saved SSH Connections"));
    connect(sshMenu, &QMenu::aboutToShow, this, [this, sshMenu] {
        sshMenu->clear();
        const QList<SshTarget> targets = Settings::instance()->sshTargets();
        for (const SshTarget &target : targets)
            sshMenu->addAction(tabText(target.label()), this, [this, target] { openSsh(target); });
        if (targets.isEmpty())
            sshMenu->addAction(tr("No saved connections"))->setEnabled(false);
    });
    add(file, {nullptr, "open-file-manager", nullptr, "close-pane", "close-tab", "quit"});

    // ---- Edit ---------------------------------------------------------------
    QMenu *edit = topMenu(tr("Edit"));
    add(edit, {"copy", "paste", "paste-selection", nullptr, "find", "find-next", "find-previous", nullptr,
               "clear", nullptr, "shortcuts", "preferences"});

    // ---- View ---------------------------------------------------------------
    QMenu *view = topMenu(tr("View"));
    add(view, {"zoom-in", "zoom-out", "zoom-reset", nullptr, "fullscreen", "compact-mode", "status-bar"});

    // ---- Terminal -----------------------------------------------------------
    QMenu *terminal = topMenu(tr("Terminal"));
    add(terminal, {"split-right", "split-down", "close-pane", nullptr});
    m_focusMenu = Ui::subMenu(terminal, tr("Focus Pane"));
    add(m_focusMenu, {"focus-left", "focus-right", "focus-up", "focus-down"});
    terminal->addSeparator();
    m_signalMenu = Ui::subMenu(terminal, tr("Send Signal"));
    m_signalTarget = m_signalMenu->addAction(QString());
    m_signalTarget->setEnabled(false);
    m_signalMenu->addSeparator();
    add(m_signalMenu, {"signal-int", "signal-term", "signal-kill", nullptr, "signal-stop", "signal-cont"});
    connect(m_signalMenu, &QMenu::aboutToShow, this, &MainWindow::updateSignalMenu);
    add(terminal, {"clear", nullptr, "open-file-manager"});

    // ---- Tabs ---------------------------------------------------------------
    QMenu *tabs = topMenu(tr("Tabs"));
    add(tabs, {"next-tab", "previous-tab", nullptr, "move-tab-left", "move-tab-right", nullptr, "rename-tab",
               "close-tab"});

    // ---- Help ---------------------------------------------------------------
    QMenu *help = topMenu(tr("Help"));
    add(help, {"shortcuts", "about"});

    // No "&" mnemonics: Alt+letter belongs to the shell (readline uses it).
    m_compactMenu = Ui::menu(this);
    for (QMenu *menu : {file, edit, view, terminal, tabs, help}) {
        menuBar()->addMenu(menu);
        m_compactMenu->addMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, &MainWindow::updateActionStates);
    }
    connect(m_compactMenu, &QMenu::aboutToShow, this, &MainWindow::updateActionStates);
    m_menuButton->setMenu(m_compactMenu);
}

void MainWindow::applySettings()
{
    const Settings *settings = Settings::instance();
    const bool compact = settings->compactMode();
    menuBar()->setVisible(!compact);
    m_menuButton->setVisible(compact);
    m_statusBar->setVisible(settings->showStatusBar());
    action(QStringLiteral("compact-mode"))->setChecked(compact);
    action(QStringLiteral("status-bar"))->setChecked(settings->showStatusBar());
}

void MainWindow::openInitialContent(const StartupOptions &options)
{
    Settings *settings = Settings::instance();
    const ProfileManager *profiles = ProfileManager::instance();

    // Where the first terminal starts: --workdir, else where the application
    // was started from. A launcher that starts programs in "/" means "no
    // particular place": use the home directory.
    m_startDirectory = QDir::currentPath();
    if (m_startDirectory == QLatin1String("/"))
        m_startDirectory = Shells::homeDirectory();
    bool workdirGiven = false;
    if (!options.workingDirectory.isEmpty()) {
        const QFileInfo info(options.workingDirectory);
        if (info.isDir()) {
            m_startDirectory = info.absoluteFilePath();
            workdirGiven = true;
        } else {
            showNotice(tr("%1 is not a directory; starting in %2.").arg(options.workingDirectory, m_startDirectory),
                       true);
        }
    }

    QString profileId = profiles->defaultId();
    if (!options.profile.isEmpty()) {
        const Profile wanted = profiles->byName(options.profile);
        if (wanted.id.isEmpty())
            showNotice(tr("There is no profile “%1”; using the default profile.").arg(options.profile), true);
        else
            profileId = wanted.id;
    }

    // A window opened for one command is not the user's session: it neither
    // restores the last one nor replaces it when it closes.
    m_saveSession = options.command.isEmpty();
    if (!options.command.isEmpty()) {
        PaneSpec spec;
        spec.kind = PaneSpec::Kind::Command;
        spec.profileId = profileId;
        spec.workingDirectory = m_startDirectory;
        spec.command = options.command;
        spec.label = QFileInfo(options.command.first()).fileName();
        spec.hold = options.hold;
        TerminalTab *tab = addTab(spec);
        if (!options.title.isEmpty()) {
            tab->setCustomTitle(options.title);
            updateTab(tab);
            refreshStatus();
        }
        return;
    }

    // The saved session belongs to a plain start. It is taken out of the
    // settings file as soon as it is used or offered, so a second window
    // started while this one is open does not restore the same tabs again.
    const bool plainStart = !options.noRestore && !workdirGiven && options.profile.isEmpty()
        && options.title.isEmpty();
    const QString mode = settings->restoreSession();
    QJsonObject saved;
    if (plainStart && mode != QLatin1String("never")) {
        saved = settings->savedSession();
        settings->clearSavedSession();
    }

    if (mode == QLatin1String("always") && sessionUsable(saved) && restoreSession(saved))
        return;

    PaneSpec spec = shellSpec(profileId, nullptr);
    if (workdirGiven)
        spec.workingDirectory = m_startDirectory;      // an explicit directory beats the profile's rule
    TerminalTab *first = addTab(spec);
    if (!options.title.isEmpty()) {
        first->setCustomTitle(options.title);
        updateTab(first);
        refreshStatus();
    }
    if (mode == QLatin1String("ask") && sessionUsable(saved))
        offerRestore(saved);
}

// ---- tabs ---------------------------------------------------------------------------

int MainWindow::tabCount() const
{
    return m_stack->count();
}

TerminalTab *MainWindow::tabAt(int index) const
{
    return qobject_cast<TerminalTab *>(m_stack->widget(index));
}

TerminalTab *MainWindow::currentTab() const
{
    return qobject_cast<TerminalTab *>(m_stack->currentWidget());
}

TerminalPane *MainWindow::currentPane() const
{
    const TerminalTab *tab = currentTab();
    return tab ? tab->activePane() : nullptr;
}

QList<TerminalPane *> MainWindow::allPanes() const
{
    QList<TerminalPane *> panes;
    for (int i = 0; i < tabCount(); ++i)
        panes += tabAt(i)->panes();
    return panes;
}

TerminalTab *MainWindow::createTab(int index)
{
    auto *tab = new TerminalTab(m_stack);
    const int at = index < 0 || index > tabCount() ? tabCount() : index;
    m_stack->insertWidget(at, tab);
    m_tabBar->insertTerminalTab(at, tr("Terminal"));

    connect(tab, &TerminalTab::titleChanged, this, [this, tab] {
        updateTab(tab);
        if (tab == currentTab())
            refreshStatus();
    });
    connect(tab, &TerminalTab::activePaneChanged, this, [this, tab] {
        if (tab == currentTab())
            onActivePaneChanged();
    });
    connect(tab, &TerminalTab::paneCountChanged, this, &MainWindow::updateActionStates);
    connect(tab, &TerminalTab::emptied, this, [this, tab] { removeTab(tab); });
    connect(tab, &TerminalTab::bellChanged, this, [this, tab] { updateTab(tab); });
    connect(tab, &TerminalTab::contextMenuRequested, this, &MainWindow::showTerminalMenu);
    connect(tab, &TerminalTab::notice, this, &MainWindow::showNotice);
    connect(tab, &TerminalTab::zoomChanged, this, &MainWindow::refreshStatus);
    return tab;
}

TerminalTab *MainWindow::addTab(const PaneSpec &spec)
{
    TerminalTab *tab = createTab();
    TerminalPane *pane = tab->setFirstPane(spec);
    m_tabBar->setCurrentIndex(m_stack->indexOf(tab));
    m_stack->setCurrentWidget(tab);
    pane->focusTerminal();
    updateTab(tab);
    refreshStatus();
    return tab;
}

void MainWindow::removeTab(TerminalTab *tab)
{
    const int index = m_stack->indexOf(tab);
    if (index < 0)
        return;
    tab->disconnect(this);
    const QList<TerminalPane *> panes = tab->panes();
    for (TerminalPane *pane : panes)
        pane->dispose();

    // Stack first: the tab bar then announces a current index that is
    // already valid for the stack.
    m_stack->removeWidget(tab);
    m_tabBar->removeTab(index);
    tab->deleteLater();

    if (tabCount() == 0) {
        Q_EMIT allTabsClosed();
        if (!m_closing)
            close();
        return;
    }
    m_stack->setCurrentIndex(m_tabBar->currentIndex());
    if (TerminalPane *pane = currentPane())
        pane->focusTerminal();
    refreshStatus();
}

bool MainWindow::confirmClose(const QList<TerminalPane *> &panes, const QString &title)
{
    if (!Settings::instance()->confirmCloseRunning())
        return true;
    QStringList running;
    for (const TerminalPane *pane : panes) {
        if (pane->isBusy())
            running << pane->busyDescription();
    }
    if (running.isEmpty())
        return true;
    const QString message = running.size() == 1
        ? tr("%1 is still running and will be ended.").arg(running.first())
        : tr("These programs are still running and will be ended: %1.").arg(running.join(QStringLiteral(", ")));
    const bool confirmed = Ui::confirm(this, title, message, tr("Close"), true);
    if (!confirmed) {
        if (TerminalPane *pane = currentPane())
            pane->focusTerminal();
    }
    return confirmed;
}

void MainWindow::closeTab(int index, bool ask)
{
    TerminalTab *tab = tabAt(index);
    if (!tab)
        return;
    if (ask && !confirmClose(tab->panes(), tr("Close this tab?")))
        return;
    removeTab(tab);
}

void MainWindow::closeOtherTabs(int index)
{
    TerminalTab *keep = tabAt(index);
    if (!keep)
        return;
    QList<TerminalTab *> others;
    QList<TerminalPane *> panes;
    for (int i = 0; i < tabCount(); ++i) {
        if (tabAt(i) != keep) {
            others << tabAt(i);
            panes += tabAt(i)->panes();
        }
    }
    if (others.isEmpty() || !confirmClose(panes, tr("Close the other tabs?")))
        return;
    for (TerminalTab *tab : std::as_const(others))
        removeTab(tab);
}

void MainWindow::closeCurrentPane()
{
    TerminalTab *tab = currentTab();
    TerminalPane *pane = currentPane();
    if (!tab || !pane)
        return;
    if (!confirmClose({pane}, tr("Close this pane?")))
        return;
    tab->closePane(pane);
}

void MainWindow::onTabMoved(int from, int to)
{
    QWidget *widget = m_stack->widget(from);
    if (!widget)
        return;
    m_stack->removeWidget(widget);
    m_stack->insertWidget(to, widget);
    m_stack->setCurrentIndex(m_tabBar->currentIndex());
}

void MainWindow::onCurrentTabChanged(int index)
{
    if (index < 0 || index >= tabCount())
        return;
    m_stack->setCurrentIndex(index);
    if (TerminalTab *tab = currentTab()) {
        tab->clearBell();
        if (!m_tabBar->isEditing()) {
            if (TerminalPane *pane = tab->activePane())
                pane->focusTerminal();
        }
    }
    onActivePaneChanged();
}

void MainWindow::onActivePaneChanged()
{
    refreshStatus();
    if (m_searchBar->isVisible())
        runSearch();
}

void MainWindow::updateTab(TerminalTab *tab)
{
    const int index = m_stack->indexOf(tab);
    if (index < 0 || index >= m_tabBar->count())
        return;
    const QString text = tabText(tab->title());
    if (m_tabBar->tabText(index) != text)
        m_tabBar->setTabText(index, text);
    const QString toolTip = tab->toolTipText();
    if (m_tabBar->tabToolTip(index) != toolTip)
        m_tabBar->setTabToolTip(index, toolTip);
    m_tabBar->setTabGlyph(index, tab->hasBell() ? QStringLiteral("bell")
                                 : tab->isSsh() ? QStringLiteral("server") : QStringLiteral("terminal"));
}

PaneSpec MainWindow::shellSpec(const QString &profileId, const TerminalPane *reference) const
{
    const ProfileManager *profiles = ProfileManager::instance();
    const Profile profile = profiles->profile(profileId.isEmpty() ? profiles->defaultId() : profileId);

    PaneSpec spec;
    spec.kind = PaneSpec::Kind::Shell;
    spec.profileId = profile.id;
    if (profile.workingDirMode == QLatin1String("home")) {
        spec.workingDirectory = Shells::homeDirectory();
    } else if (profile.workingDirMode == QLatin1String("custom")) {
        QString dir = profile.workingDir;
        if (dir == QLatin1String("~"))
            dir = Shells::homeDirectory();
        else if (dir.startsWith(QLatin1String("~/")))
            dir = Shells::homeDirectory() + dir.mid(1);
        spec.workingDirectory = dir;        // a missing directory falls back to home in the pane
    } else {
        spec.workingDirectory = reference ? reference->workingDirectory() : m_startDirectory;
    }
    return spec;
}

// ---- commands -----------------------------------------------------------------------

void MainWindow::newTab(const QString &profileId)
{
    addTab(shellSpec(profileId, currentPane()));
}

void MainWindow::newWindow()
{
    const TerminalPane *pane = currentPane();
    const QString directory = pane ? pane->workingDirectory() : m_startDirectory;
    // Another process, so one misbehaving window cannot take the others down.
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 {QStringLiteral("--no-restore"), QStringLiteral("--workdir"), directory}))
        showNotice(tr("A new window could not be started."), true);
}

void MainWindow::newSshConnection()
{
    SshDialog dialog(this);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (accepted)
        openSsh(dialog.target());
    else if (TerminalPane *pane = currentPane())
        pane->focusTerminal();
}

void MainWindow::openSsh(const SshTarget &target)
{
    // Saved entries are checked again: the settings file can be edited by hand.
    QString error;
    if (!Ssh::validate(target, &error)) {
        showNotice(tr("Cannot connect to “%1”: %2").arg(target.label(), error), true);
        return;
    }
    if (Ssh::program().isEmpty()) {
        showNotice(tr("The ssh program is not installed (Debian package openssh-client)."), true);
        return;
    }
    const TerminalPane *reference = currentPane();
    PaneSpec spec;
    spec.kind = PaneSpec::Kind::Ssh;
    spec.profileId = reference ? reference->spec().profileId : ProfileManager::instance()->defaultId();
    spec.workingDirectory = Shells::homeDirectory();
    spec.command = Ssh::command(target);
    spec.label = target.label();
    spec.hold = true;           // a failed or closed connection stays readable, with "Reconnect"
    addTab(spec);
}

void MainWindow::openFileManager()
{
    const TerminalPane *pane = currentPane();
    if (!pane)
        return;
    const QString program = QStandardPaths::findExecutable(QString::fromLatin1(kFileManager));
    if (program.isEmpty()) {
        showNotice(tr("NEXUS Files (%1) is not installed.").arg(QString::fromLatin1(kFileManager)), true);
        return;
    }
    const QString directory = pane->workingDirectory();
    if (!QProcess::startDetached(program, {directory}))
        showNotice(tr("%1 could not be started.").arg(program), true);
}

void MainWindow::split(Qt::Orientation orientation)
{
    TerminalTab *tab = currentTab();
    TerminalPane *pane = currentPane();
    if (!tab || !pane)
        return;
    // A split is always a local shell with the look of the pane it came from,
    // in that pane's current directory.
    TerminalPane *fresh = tab->split(pane, orientation, shellSpec(pane->spec().profileId, pane));
    if (!fresh)
        return;
    tab->setActivePane(fresh);
    fresh->focusTerminal();
    updateActionStates();
}

void MainWindow::focusPane(TerminalTab::Direction direction)
{
    if (TerminalTab *tab = currentTab())
        tab->focusNeighbour(direction);
}

void MainWindow::placeSearchBar()
{
    const QRect area = m_stack->geometry();
    const int margin = Tokens::SpaceLg;
    const int width = qMin(Tokens::SidebarWidth * 2 + Tokens::SpaceXxxl, area.width() - 2 * margin);
    const int height = m_searchBar->sizeHint().height();
    m_searchBar->setGeometry(area.right() - margin - width + 1, area.top() + Tokens::SpaceSm, width, height);
    m_searchBar->raise();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_stack && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        placeSearchBar();
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::showSearch()
{
    TerminalPane *pane = currentPane();
    if (!pane)
        return;
    if (!pane->searchSupported()) {
        pane->showBuiltinSearchBar();
        return;
    }
    placeSearchBar();
    m_searchBar->activate();
    runSearch();
    updateActionStates();
}

void MainWindow::runSearch()
{
    TerminalPane *pane = currentPane();
    if (!pane || !pane->searchSupported())
        return;
    using Result = TerminalPane::SearchResult;
    const Result result = pane->search(m_searchBar->pattern(), m_searchBar->caseSensitive(),
                                       m_searchBar->regularExpression());
    m_searchBar->setStatus(result == Result::Found ? SearchBar::Status::Found
                           : result == Result::NotFound ? SearchBar::Status::NotFound
                           : result == Result::InvalidPattern ? SearchBar::Status::InvalidPattern
                           : SearchBar::Status::Idle);
    updateActionStates();
}

void MainWindow::searchStep(bool forward)
{
    TerminalPane *pane = currentPane();
    if (!pane)
        return;
    if (!pane->searchSupported()) {
        pane->showBuiltinSearchBar();
        return;
    }
    if (m_searchBar->pattern().isEmpty()) {
        showSearch();
        return;
    }
    using Result = TerminalPane::SearchResult;
    const Result result = forward ? pane->searchNext() : pane->searchPrevious();
    m_searchBar->setStatus(result == Result::Found ? SearchBar::Status::Found
                           : result == Result::NotFound ? SearchBar::Status::NotFound
                           : result == Result::InvalidPattern ? SearchBar::Status::InvalidPattern
                           : SearchBar::Status::Idle);
}

void MainWindow::stepTab(int delta)
{
    const int count = tabCount();
    if (count < 2)
        return;
    m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + delta + count) % count);
}

void MainWindow::moveTab(int delta)
{
    const int from = m_tabBar->currentIndex();
    const int to = from + delta;
    if (from < 0 || to < 0 || to >= tabCount())
        return;
    m_tabBar->moveTab(from, to);        // emits tabMoved, which reorders the stack
}

void MainWindow::renameCurrentTab()
{
    m_tabBar->editTab(m_tabBar->currentIndex());
}

void MainWindow::sendSignal(int signal, const QString &name, bool ask)
{
    TerminalPane *pane = currentPane();
    if (!pane)
        return;
    const QString target = pane->busyDescription();
    if (target.isEmpty()) {
        showNotice(tr("There is no foreground process in this terminal."), true);
        return;
    }
    if (ask) {
        const bool confirmed = Ui::confirm(
            this, tr("Kill %1?").arg(target),
            tr("%1 is sent to the foreground process group of this terminal. The programs end at once "
               "and cannot save their work.").arg(name),
            tr("Kill"), true);
        pane->focusTerminal();
        if (!confirmed)
            return;
    }
    QString error;
    if (pane->sendSignal(signal, &error))
        showNotice(tr("Sent %1 to %2.").arg(name, target), false);
    else
        showNotice(tr("%1 could not be sent to %2: %3").arg(name, target, error), true);
}

void MainWindow::showPreferences(PreferencesDialog::Page page)
{
    PreferencesDialog dialog(page, this);
    if (dialog.exec() == QDialog::Accepted) {
        if (dialog.shortcutsChanged())
            applyShortcuts();
        applySettings();
        refreshStatus();
    }
    if (TerminalPane *pane = currentPane())
        pane->focusTerminal();
}

void MainWindow::showAbout()
{
    Ui::Dialog dialog(tr("About Terminal"), this);
    dialog.setSubtitle(tr("Terminal emulator of NEXUS OS: tabs, split panes, profiles and SSH connections."));
    dialog.setFixedWidth(Tokens::SidebarWidth * 2);
    QFrame *card = Ui::card(&dialog);
    dialog.content()->addWidget(card);
    auto value = [](const QString &text) {
        QLabel *label = Ui::label(text, "mono");
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    Ui::addRow(card, tr("Version"), QString(), value(QCoreApplication::applicationVersion()));
    Ui::addRow(card, tr("Terminal emulation"), QString(),
               value(QStringLiteral("QTermWidget %1.%2.%3")
                         .arg(QTERMWIDGET_VERSION_MAJOR).arg(QTERMWIDGET_VERSION_MINOR)
                         .arg(QTERMWIDGET_VERSION_PATCH)));
    Ui::addRow(card, tr("Qt"), QString(), value(QString::fromLatin1(qVersion())));
    Ui::addRow(card, tr("Settings file"), QString(), value(Settings::instance()->filePath()));
    Ui::addRow(card, tr("Licence"), QString(), value(QStringLiteral("GPL-3.0-or-later")));
    QPushButton *close = dialog.addFooterButton(tr("Close"), "primary");
    close->setDefault(true);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
    if (TerminalPane *pane = currentPane())
        pane->focusTerminal();
}

void MainWindow::setFullScreen(bool on)
{
    setWindowState(on ? windowState() | Qt::WindowFullScreen : windowState() & ~Qt::WindowFullScreen);
}

void MainWindow::setCompactMode(bool on)
{
    Settings::instance()->setCompactMode(on);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && !m_actions.isEmpty())
        action(QStringLiteral("fullscreen"))->setChecked(isFullScreen());
}

void MainWindow::populateNewTabMenu(QMenu *menu)
{
    menu->clear();
    const ProfileManager *profiles = ProfileManager::instance();
    const QList<Profile> list = profiles->profiles();
    for (const Profile &profile : list) {
        const QString id = profile.id;
        QAction *entry = menu->addAction(tabText(profile.name), this, [this, id] { newTab(id); });
        entry->setIcon(profileIcon(id == profiles->defaultId()));
    }
    menu->addSeparator();
    const QList<SshTarget> targets = Settings::instance()->sshTargets();
    for (const SshTarget &target : targets) {
        QAction *entry = menu->addAction(tabText(target.label()), this, [this, target] { openSsh(target); });
        entry->setIcon(Theme::instance()->icon(QStringLiteral("server"), QStringLiteral("textSecondary"),
                                               QStringLiteral("accent")));
    }
    menu->addAction(action(QStringLiteral("new-ssh")));
    menu->addSeparator();
    QAction *manage = menu->addAction(tr("Manage Profiles…"), this,
                                      [this] { showPreferences(PreferencesDialog::Page::Profiles); });
    manage->setIcon(Theme::instance()->icon(QStringLiteral("sliders"), QStringLiteral("textSecondary"),
                                            QStringLiteral("accent")));
}

void MainWindow::updateSignalMenu()
{
    const TerminalPane *pane = currentPane();
    const QString target = pane ? pane->busyDescription() : QString();
    m_signalTarget->setText(target.isEmpty() ? tr("No foreground process") : tr("To %1").arg(tabText(target)));
    for (const char *id : {"signal-int", "signal-term", "signal-kill", "signal-stop", "signal-cont"})
        action(QString::fromLatin1(id))->setEnabled(!target.isEmpty());
}

void MainWindow::updateActionStates()
{
    const TerminalTab *tab = currentTab();
    const int panes = tab ? tab->paneCount() : 0;
    const int tabs = tabCount();

    // Commands that have nothing to act on are disabled, so their keys reach
    // the program in the terminal instead (Alt+Arrow, Ctrl+PgUp, ...).
    for (const char *id : {"focus-left", "focus-right", "focus-up", "focus-down"})
        action(QString::fromLatin1(id))->setEnabled(panes > 1);
    for (const char *id : {"next-tab", "previous-tab"})
        action(QString::fromLatin1(id))->setEnabled(tabs > 1);
    const int current = m_tabBar->currentIndex();
    action(QStringLiteral("move-tab-left"))->setEnabled(current > 0);
    action(QStringLiteral("move-tab-right"))->setEnabled(current >= 0 && current < tabs - 1);
    for (int n = 1; n <= 9; ++n)
        action(QStringLiteral("tab-%1").arg(n))->setEnabled(n <= tabs && tabs > 1);

    const bool searching = m_searchBar->isVisible() && !m_searchBar->pattern().isEmpty();
    action(QStringLiteral("find-next"))->setEnabled(searching);
    action(QStringLiteral("find-previous"))->setEnabled(searching);

    // The signal commands stay enabled outside their menu so that a shortcut
    // bound to them always answers (with a message if there is no process).
    for (const char *id : {"signal-int", "signal-term", "signal-kill", "signal-stop", "signal-cont"})
        action(QString::fromLatin1(id))->setEnabled(true);

    QAction *files = action(QStringLiteral("open-file-manager"));
    const bool haveFiles = !QStandardPaths::findExecutable(QString::fromLatin1(kFileManager)).isEmpty();
    files->setEnabled(haveFiles);
    files->setStatusTip(haveFiles ? QString() : tr("NEXUS Files is not installed"));
    files->setText(haveFiles ? tr("Open File Manager Here") : tr("Open File Manager Here (not installed)"));
}

void MainWindow::showTerminalMenu(TerminalPane *pane, const QPoint &globalPos, const QPoint &terminalPos)
{
    if (TerminalTab *tab = currentTab())
        tab->setActivePane(pane);
    updateActionStates();

    QMenu *menu = Ui::menu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QList<QAction *> links = pane->linkActions(terminalPos);
    if (!links.isEmpty()) {
        menu->addActions(links);
        menu->addSeparator();
    }
    for (const char *id : {"copy", "paste", "paste-selection"})
        menu->addAction(action(QString::fromLatin1(id)));
    menu->addSeparator();
    menu->addAction(action(QStringLiteral("find")));
    menu->addSeparator();
    for (const char *id : {"split-right", "split-down", "close-pane"})
        menu->addAction(action(QString::fromLatin1(id)));
    menu->addSeparator();
    menu->addAction(action(QStringLiteral("open-file-manager")));
    menu->addMenu(m_signalMenu);
    menu->addAction(action(QStringLiteral("clear")));
    menu->addSeparator();
    menu->addAction(action(QStringLiteral("preferences")));
    menu->popup(globalPos);
}

void MainWindow::showTabMenu(int index, const QPoint &globalPos)
{
    QMenu *menu = Ui::menu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(Theme::instance()->icon(QStringLiteral("edit"), QStringLiteral("textSecondary"),
                                            QStringLiteral("accent")),
                    tr("Rename Tab…"), this, [this, index] { m_tabBar->editTab(index); });
    menu->addSeparator();
    menu->addAction(tr("Close Tab"), this, [this, index] { closeTab(index); });
    QAction *others = menu->addAction(tr("Close Other Tabs"), this, [this, index] { closeOtherTabs(index); });
    others->setEnabled(tabCount() > 1);
    menu->popup(globalPos);
}

// ---- status -------------------------------------------------------------------------

void MainWindow::refreshStatus()
{
    for (int i = 0; i < tabCount(); ++i)
        updateTab(tabAt(i));

    const TerminalTab *tab = currentTab();
    const TerminalPane *pane = currentPane();
    setWindowTitle(tab ? tr("%1 — Terminal").arg(tab->title()) : tr("Terminal"));
    if (!pane)
        return;

    QString process;
    if (pane->isRunning()) {
        process = pane->busyDescription();
        if (process.isEmpty())
            process = tr("Running");
    } else {
        process = pane->hasStarted() ? tr("Ended") : tr("Not running");
    }
    m_statusProcess->setText(process);
    Ui::setRole(m_statusProcess, pane->isBusy() ? "statusStrong" : "status");
    m_statusDirectory->setText(pane->displayDirectory());

    m_statusProfile->setText(pane->profile().name);
    const QSize grid = pane->gridSize();
    m_statusSize->setText(grid.isValid() ? tr("%1 × %2").arg(grid.width()).arg(grid.height()) : QString());
    const int zoom = pane->zoomPercent();
    m_statusZoom->setText(zoom == 100 ? QString() : tr("%1 %").arg(zoom));
    m_statusZoom->setVisible(zoom != 100);
    updateActionStates();
}

void MainWindow::showNotice(const QString &text, bool isError)
{
    m_statusNotice->setText(text);
    Ui::setRole(m_statusNotice, isError ? "statusDanger" : "statusStrong");
    m_statusProcess->hide();
    m_statusDirectory->hide();
    m_statusNotice->show();
    m_noticeTimer->start();
    // A status bar that is switched off must not swallow the message.
    if (!Settings::instance()->showStatusBar())
        showBanner(text, isError ? QStringLiteral("danger") : QStringLiteral("info"), QString(), {});
}

void MainWindow::showBanner(const QString &text, const QString &kind, const QString &actionText,
                            const std::function<void()> &onAction)
{
    m_bannerText->setText(text);
    Ui::setProperty(m_banner, "nxKind", kind);
    m_bannerAction->setText(actionText);
    m_bannerAction->setVisible(!actionText.isEmpty());
    m_bannerCallback = onAction;
    m_bannerHolder->show();
}

void MainWindow::hideBanner()
{
    m_bannerHolder->hide();
    m_bannerCallback = nullptr;
    if (TerminalPane *pane = currentPane())
        pane->focusTerminal();
}

// ---- session ------------------------------------------------------------------------

QJsonObject MainWindow::sessionState() const
{
    QJsonArray tabs;
    for (int i = 0; i < tabCount(); ++i)
        tabs.append(tabAt(i)->saveLayout());
    return QJsonObject{
        {QStringLiteral("version"), kSessionVersion},
        {QStringLiteral("current"), m_tabBar->currentIndex()},
        {QStringLiteral("tabs"), tabs},
    };
}

void MainWindow::saveSession()
{
    Settings *settings = Settings::instance();
    settings->setWindowGeometry(saveGeometry());
    if (!m_saveSession)
        return;
    if (settings->restoreSession() == QLatin1String("never") || tabCount() == 0)
        settings->clearSavedSession();
    else
        settings->setSavedSession(sessionState());
}

bool MainWindow::restoreSession(const QJsonObject &session)
{
    const ProfileManager *profiles = ProfileManager::instance();
    // A restored terminal is always a new shell of its profile in the saved
    // directory. What ran there before (commands, ssh sessions) is not started
    // again, and a directory that has disappeared becomes the home directory.
    const auto makeSpec = [this, profiles](const QJsonObject &pane) {
        const QString id = pane.value(QStringLiteral("profile")).toString();
        PaneSpec spec = shellSpec(profiles->contains(id) ? id : profiles->defaultId(), nullptr);
        const QString directory = pane.value(QStringLiteral("directory")).toString();
        spec.workingDirectory = QFileInfo(directory).isDir() ? directory : Shells::homeDirectory();
        return spec;
    };

    int restored = 0;
    const QJsonArray tabs = session.value(QStringLiteral("tabs")).toArray();
    for (const QJsonValue &value : tabs) {
        if (restored >= kMaxRestoredTabs)
            break;
        TerminalTab *tab = createTab();
        if (tab->restoreLayout(value.toObject(), makeSpec)) {
            updateTab(tab);
            ++restored;
        } else {
            tab->disconnect(this);
            const int index = m_stack->indexOf(tab);
            m_stack->removeWidget(tab);
            m_tabBar->removeTab(index);
            tab->deleteLater();
        }
    }
    if (restored == 0)
        return false;

    const int first = tabCount() - restored;
    const int current = first + qBound(0, session.value(QStringLiteral("current")).toInt(), restored - 1);
    m_tabBar->setCurrentIndex(current);
    m_stack->setCurrentIndex(current);
    if (TerminalPane *pane = currentPane())
        pane->focusTerminal();
    refreshStatus();
    return true;
}

void MainWindow::offerRestore(const QJsonObject &session)
{
    const QJsonArray tabs = session.value(QStringLiteral("tabs")).toArray();
    int panes = 0;
    for (const QJsonValue &tab : tabs)
        panes += paneCountOf(tab.toObject().value(QStringLiteral("layout")).toObject());
    const QPointer<TerminalTab> placeholder = currentTab();

    const QString tabText = tabs.size() == 1 ? tr("1 tab") : tr("%1 tabs").arg(tabs.size());
    const QString paneText = panes == 1 ? tr("1 terminal") : tr("%1 terminals").arg(panes);
    showBanner(tr("Restore your previous session (%1, %2)? New shells are started in the same directories; "
                  "nothing is run again.").arg(tabText, paneText),
               QStringLiteral("info"), tr("Restore"), [this, session, placeholder] {
        if (!restoreSession(session)) {
            showNotice(tr("The previous session could not be restored."), true);
            return;
        }
        // The tab opened at start-up has served its purpose, unless the user
        // already started something in it.
        if (placeholder && placeholder->paneCount() == 1 && !placeholder->activePane()->isBusy())
            removeTab(placeholder);
    });
}

void MainWindow::hangUpAll(int waitMs)
{
    QList<QPointer<TerminalPane>> panes;
    const QList<TerminalPane *> all = allPanes();
    for (TerminalPane *pane : all) {
        pane->hangUp();
        panes << pane;
    }
    // Give the shells a moment to write their history before the widgets,
    // and with them the processes, are destroyed.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < waitMs) {
        const bool anyRunning = std::any_of(panes.cbegin(), panes.cend(), [](const QPointer<TerminalPane> &pane) {
            return pane && pane->isRunning();
        });
        if (!anyRunning)
            break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

bool MainWindow::shellRunning(QString *reason) const
{
    const TerminalTab *tab = tabAt(0);
    const QList<TerminalPane *> panes = tab ? tab->panes() : QList<TerminalPane *>();
    if (panes.isEmpty()) {
        if (reason)
            *reason = QStringLiteral("there is no terminal (the program of the first tab has already ended)");
        return false;
    }
    const TerminalPane *pane = panes.first();
    if (!pane->hasStarted()) {
        if (reason)
            *reason = pane->startError().isEmpty() ? QStringLiteral("the shell was not started") : pane->startError();
        return false;
    }
    if (!pane->isRunning() || pane->shellPid() <= 0 || ::kill(pane->shellPid(), 0) != 0) {
        if (reason)
            *reason = QStringLiteral("the shell is no longer running");
        return false;
    }
    return true;
}

void MainWindow::shutdownTerminals()
{
    m_closing = true;
    m_refreshTimer->stop();
    for (int i = 0; i < tabCount(); ++i)
        tabAt(i)->disconnect(this);
    hangUpAll(kHangUpWaitMs);
}

void MainWindow::closeNow()
{
    m_forceClose = true;
    close();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_closing) {
        event->accept();
        return;
    }
    if (!m_forceClose && tabCount() > 0
        && !confirmClose(allPanes(), tabCount() > 1 ? tr("Close all tabs?") : tr("Close the terminal?"))) {
        event->ignore();
        return;
    }
    m_closing = true;
    m_refreshTimer->stop();
    saveSession();
    for (int i = 0; i < tabCount(); ++i)
        tabAt(i)->disconnect(this);
    hangUpAll(kHangUpWaitMs);
    event->accept();
}
