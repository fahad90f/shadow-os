// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "preferencesdialog.h"
#include "settings.h"
#include "terminaltab.h"

#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>

#include <functional>

class QAction;
class QFrame;
class QLabel;
class QMenu;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;
class SearchBar;
class TabBar;

// What the command line asked for.
struct StartupOptions
{
    QString workingDirectory;       // --working-directory: directory of the first terminal
    QString title;                  // --title: fixed title of the first tab
    QStringList command;            // -e: run this instead of the profile's shell
    QString profile;                // --profile: name (or id) of the profile to use
    bool hold = false;              // --hold: keep the tab when the -e command ends
    bool noRestore = false;         // --no-restore: never restore or offer the last session
};

// The terminal window: menu bar (optional), tab strip with tool buttons, the
// terminals, search bar and status bar.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const StartupOptions &options, QWidget *parent = nullptr);
    ~MainWindow() override;

    // For the smoke test: is the program of the first terminal running?
    // On "no", *reason says why.
    bool shellRunning(QString *reason) const;

    // Close without asking questions (session shutdown). The session is
    // still saved and the programs get their hang-up.
    void closeNow();

    // End all programs without touching the saved session (test hooks).
    void shutdownTerminals();

Q_SIGNALS:
    // The last tab is gone; the window is about to close.
    void allTabsClosed();

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // ---- construction ----------------------------------------------------
    void createActions();
    void createMenus();
    QWidget *createHeader();
    QWidget *createBanner();
    QWidget *createStatusBar();
    QToolButton *actionButton(const QString &id);
    QAction *action(const QString &id) const;
    void applyShortcuts();
    void applySettings();
    void openInitialContent(const StartupOptions &options);

    // ---- tabs ------------------------------------------------------------------
    int tabCount() const;
    TerminalTab *tabAt(int index) const;
    TerminalTab *currentTab() const;
    TerminalPane *currentPane() const;
    QList<TerminalPane *> allPanes() const;
    TerminalTab *createTab(int index = -1);
    TerminalTab *addTab(const PaneSpec &spec);
    void removeTab(TerminalTab *tab);
    void closeTab(int index, bool ask = true);
    void closeOtherTabs(int index);
    void onTabMoved(int from, int to);
    void onCurrentTabChanged(int index);
    void onActivePaneChanged();
    void updateTab(TerminalTab *tab);
    bool confirmClose(const QList<TerminalPane *> &panes, const QString &title);
    // What a new shell terminal starts: profile, and the directory its
    // profile asks for ('reference' is the terminal it is opened from).
    PaneSpec shellSpec(const QString &profileId, const TerminalPane *reference) const;

    // ---- commands ---------------------------------------------------------------
    void newTab(const QString &profileId = QString());
    void newWindow();
    void newSshConnection();
    void openSsh(const SshTarget &target);
    void openFileManager();
    void closeCurrentPane();
    void split(Qt::Orientation orientation);
    void focusPane(TerminalTab::Direction direction);
    void showSearch();
    void placeSearchBar();
    void runSearch();
    void searchStep(bool forward);
    void stepTab(int delta);
    void moveTab(int delta);
    void renameCurrentTab();
    void sendSignal(int signal, const QString &name, bool ask);
    void showPreferences(PreferencesDialog::Page page);
    void showAbout();
    void setFullScreen(bool on);
    void setCompactMode(bool on);
    void showTerminalMenu(TerminalPane *pane, const QPoint &globalPos, const QPoint &terminalPos);
    void showTabMenu(int index, const QPoint &globalPos);
    void populateNewTabMenu(QMenu *menu);
    void updateSignalMenu();
    void updateActionStates();

    // ---- status -----------------------------------------------------------------
    void refreshStatus();
    void showNotice(const QString &text, bool isError);
    void showBanner(const QString &text, const QString &kind, const QString &actionText,
                    const std::function<void()> &onAction);
    void hideBanner();

    // ---- session ----------------------------------------------------------------
    QJsonObject sessionState() const;
    void saveSession();
    bool restoreSession(const QJsonObject &session);
    void offerRestore(const QJsonObject &session);
    void hangUpAll(int waitMs);

    QHash<QString, QAction *> m_actions;
    QString m_startDirectory;
    bool m_saveSession = true;
    bool m_closing = false;
    bool m_forceClose = false;

    TabBar *m_tabBar = nullptr;
    QStackedWidget *m_stack = nullptr;
    SearchBar *m_searchBar = nullptr;
    QToolButton *m_menuButton = nullptr;
    QMenu *m_compactMenu = nullptr;
    QMenu *m_signalMenu = nullptr;
    QAction *m_signalTarget = nullptr;
    QMenu *m_focusMenu = nullptr;

    QWidget *m_bannerHolder = nullptr;
    QFrame *m_banner = nullptr;
    QLabel *m_bannerText = nullptr;
    QPushButton *m_bannerAction = nullptr;
    std::function<void()> m_bannerCallback;

    QWidget *m_statusBar = nullptr;
    QLabel *m_statusProcess = nullptr;
    QLabel *m_statusDirectory = nullptr;
    QLabel *m_statusNotice = nullptr;
    QLabel *m_statusProfile = nullptr;
    QLabel *m_statusSize = nullptr;
    QLabel *m_statusZoom = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QTimer *m_noticeTimer = nullptr;
};
