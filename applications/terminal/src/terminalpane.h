// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "profile.h"

#include <QPointer>
#include <QSize>
#include <climits>
#include <QStringList>
#include <QUrl>
#include <QWidget>

class QAction;
class QDropEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QTermWidget;
class QVBoxLayout;

// What a pane runs and how it was created.
struct PaneSpec
{
    enum class Kind {
        Shell,      // the profile's shell or command
        Command,    // an explicit argument vector (shadow-terminal -e ...)
        Ssh,        // an ssh client started from the SSH dialog
    };
    Kind kind = Kind::Shell;
    QString profileId;              // look of the terminal; for Kind::Shell also what runs
    QString workingDirectory;       // must be decided by the caller; home if empty or missing
    QStringList command;            // argv for Command and Ssh
    QString label;                  // title of a command pane until the program sets one
    bool hold = false;              // keep the pane after the program ended
};

// One terminal: a QTermWidget with its process, wrapped with everything the
// window needs to know about it (title, directory, foreground process) and
// the behaviour QTermWidget leaves to the application (paste safety, zoom,
// bell, search state, what happens when the program ends).
class TerminalPane : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalPane(const PaneSpec &spec, QWidget *parent = nullptr);
    ~TerminalPane() override;

    const PaneSpec &spec() const { return m_spec; }
    Profile profile() const;

    // Start the program. On failure the reason is shown inside the pane and
    // returned by startError().
    bool start();
    bool hasStarted() const { return m_started; }
    bool isRunning() const { return m_started && !m_finished; }
    QString startError() const { return m_startError; }

    // ---- process ----------------------------------------------------------
    int shellPid() const;                   // the program started in this pane
    int foregroundPid() const;              // process group in the foreground of the tty
    QString foregroundName() const;         // its name from /proc/<pid>/comm
    // True if closing the pane would end something other than an idle shell.
    bool isBusy() const;
    QString busyDescription() const;        // "vim (PID 4711)"
    // Send a signal to the foreground process group.
    bool sendSignal(int signal, QString *error);
    // Ask the program to end (SIGHUP) before the pane goes away.
    void hangUp();
    // Remove the pane from the window and delete it after its program ended.
    void dispose();

    // ---- state shown by the window -------------------------------------------
    QString workingDirectory() const;       // of the pane's program (QTermWidget::workingDirectory())
    QString displayDirectory() const;       // with the home directory as "~"
    QString title() const;                  // title set by the program, else derived
    QSize gridSize() const;                 // columns × lines
    int zoomPercent() const;

    // ---- commands -----------------------------------------------------------------
    bool hasSelection() const;
    void copy();
    void paste();                           // clipboard
    void pasteSelection();                  // primary selection
    void clear();
    void zoomIn();
    void zoomOut();
    void zoomReset();
    void focusTerminal();

    // Appearance of the frame: the active pane of a split tab is outlined.
    void setActive(bool active);
    void setSingle(bool single);
    bool isActive() const { return m_active; }

    // Re-read the profile and the general settings and apply what can change
    // while the program keeps running.
    void applyProfile();
    void applySettings();

    // ---- search in the scrollback ---------------------------------------------------
    enum class SearchResult { Found, NotFound, InvalidPattern, NoPattern };
    // False if this QTermWidget does not have the search interface the
    // application's own search bar drives; showBuiltinSearchBar() still works.
    bool searchSupported() const { return m_searchSupported; }
    SearchResult search(const QString &pattern, bool caseSensitive, bool regularExpression);
    SearchResult searchNext();
    SearchResult searchPrevious();
    void showBuiltinSearchBar();

    // Actions for a link under 'pos' (terminal coordinates), for context menus.
    QList<QAction *> linkActions(const QPoint &pos) const;

Q_SIGNALS:
    void activated(TerminalPane *pane);
    void titleChanged(TerminalPane *pane);
    // The program ended and the pane is to be removed.
    void finished(TerminalPane *pane);
    void closeRequested(TerminalPane *pane);
    void contextMenuRequested(TerminalPane *pane, const QPoint &globalPos, const QPoint &terminalPos);
    void bellRang(TerminalPane *pane);
    void zoomChanged(TerminalPane *pane);
    // Something the user should know, for the status bar.
    void notice(const QString &text, bool isError);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void createTerminal();
    void restart();
    void onFinished();
    void onBell();
    void onKeyPressed(const QKeyEvent *event);
    void onUrlActivated(const QUrl &url, bool fromContextMenu);
    void showNotice(const QString &text, bool offerRestart);
    void hideNotice();
    void updateFrame();
    void updateDim();
    void applyFont();
    void pasteText(const QString &text);
    void handleDrop(QDropEvent *event);
    bool terminalOwnsMouse() const;
    SearchResult searchResult() const;

    PaneSpec m_spec;
    QVBoxLayout *m_layout;
    QTermWidget *m_term = nullptr;
    QPointer<QWidget> m_display;            // QTermWidget's terminal display (its focus proxy)

    QWidget *m_dim = nullptr;               // faint scrim over an inactive split pane
    QWidget *m_notice = nullptr;
    QLabel *m_noticeLabel = nullptr;
    QPushButton *m_restartButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    bool m_suspendedHint = false;           // the notice bar shows the Ctrl+S hint

    // Handles into QTermWidget's hidden search bar (see createTerminal()).
    QPointer<QLineEdit> m_searchEdit;
    QPointer<QAction> m_searchMatchCase;
    QPointer<QAction> m_searchRegex;
    bool m_searchSupported = false;
    bool m_ownMiddlePaste = false;

    bool m_started = false;
    bool m_finished = false;
    bool m_closing = false;
    bool m_active = false;
    bool m_single = true;
    bool m_bellFlash = false;
    int m_zoomSteps = 0;
    int m_appliedScrollback = INT_MIN;
    QString m_startError;
    QString m_initialDirectory;
    mutable QString m_lastDirectory;
};
