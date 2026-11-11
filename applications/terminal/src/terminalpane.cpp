// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminalpane.h"

#include "schemes.h"
#include "settings.h"
#include "shells.h"
#include "termwidget.h"
#include "theme.h"
#include "tokens.h"
#include "ui.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <termios.h>

namespace {

constexpr int kMinPointSize = 4;
constexpr int kMaxPointSize = 96;
constexpr int kHangUpGraceMs = 1500;

QString processName(int pid)
{
    if (pid <= 0)
        return QString();
    QFile file(QStringLiteral("/proc/%1/comm").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLocal8Bit(file.readAll()).trimmed();
}

// Text as it may be typed into a terminal on the user's behalf: line ends
// normalised, and every control character that could act as a key press or
// end a bracketed paste (ESC, ^C, ^D, backspace, C1 controls, ...) removed.
QString sanitizePaste(QString text, bool *removed)
{
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QString clean;
    clean.reserve(text.size());
    for (const QChar ch : std::as_const(text)) {
        const char16_t u = ch.unicode();
        const bool control = (u < 0x20 && u != u'\n' && u != u'\t') || u == 0x7f || (u >= 0x80 && u <= 0x9f);
        if (control) {
            *removed = true;
            continue;
        }
        clean += ch;
    }
    return clean;
}

// A file name as one shell word (single quotes; nothing inside is expanded).
QString shellQuote(const QString &word)
{
    QString quoted = word;
    quoted.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

} // namespace

TerminalPane::TerminalPane(const PaneSpec &spec, QWidget *parent)
    : QWidget(parent)
    , m_spec(spec)
    , m_layout(new QVBoxLayout(this))
{
    setObjectName(QStringLiteral("TerminalPane"));
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    // Bar shown below the terminal when the program could not be started or
    // has ended in a pane that is kept open.
    m_notice = new QWidget(this);
    m_notice->setObjectName(QStringLiteral("PaneNotice"));
    m_notice->setAttribute(Qt::WA_StyledBackground, true);
    auto *noticeLayout = new QHBoxLayout(m_notice);
    noticeLayout->setContentsMargins(Tokens::SpaceMd, Tokens::SpaceSm, Tokens::SpaceMd, Tokens::SpaceSm);
    noticeLayout->setSpacing(Tokens::SpaceSm);
    m_noticeLabel = Ui::label(QString(), "secondary", m_notice);
    m_noticeLabel->setWordWrap(true);
    m_restartButton = Ui::button(QString(), "secondary", m_notice);
    m_closeButton = Ui::button(tr("Close"), "ghost", m_notice);
    noticeLayout->addWidget(m_noticeLabel, 1);
    noticeLayout->addWidget(m_restartButton);
    noticeLayout->addWidget(m_closeButton);
    m_notice->setMinimumHeight(Tokens::ControlHeight + 2 * Tokens::SpaceSm);
    m_notice->hide();
    m_layout->addWidget(m_notice);
    connect(m_restartButton, &QPushButton::clicked, this, &TerminalPane::restart);
    connect(m_closeButton, &QPushButton::clicked, this, [this] { Q_EMIT closeRequested(this); });

    createTerminal();
    updateFrame();

    connect(Theme::instance(), &Theme::changed, this, [this] {
        // A style sheet switch re-polishes every widget, which puts the
        // terminal display's palette back to what it was when first shown;
        // setting the colour scheme again restores the terminal's colours.
        applyProfile();
        update();
    });
    connect(ProfileManager::instance(), &ProfileManager::changed, this, &TerminalPane::applyProfile);
    connect(Settings::instance(), &Settings::changed, this, &TerminalPane::applySettings);
}

TerminalPane::~TerminalPane() = default;

Profile TerminalPane::profile() const
{
    return ProfileManager::instance()->profile(m_spec.profileId);
}

void TerminalPane::createTerminal()
{
    m_term = new QTermWidget(0, this);
    // The size is shown in the status bar instead of a pop-up on every resize.
    m_term->setTerminalSizeHint(false);
    m_term->setMargin(Tokens::SpaceXs + Tokens::SpaceXxs);
    m_term->setContextMenuPolicy(Qt::CustomContextMenu);
    m_term->setMotionAfterPasting(2);                   // scroll to the end
    m_term->setTrimPastedTrailingNewlines(false);
    m_term->setAutoClose(true);
    m_layout->insertWidget(0, m_term, 1);

    m_display = m_term->focusProxy();
    if (m_display)
        m_display->installEventFilter(this);

    // With nothing to scroll, the scroll bar would be one long handle along
    // the whole terminal; the style sheet hides the handle in that state.
    if (auto *bar = m_display ? m_display->findChild<QScrollBar *>() : nullptr) {
        const auto updateIdle = [bar] { Ui::setProperty(bar, "nxIdle", bar->maximum() <= bar->minimum()); };
        connect(bar, &QScrollBar::rangeChanged, bar, updateIdle);
        updateIdle();
    }

    // Pasting goes through pasteText() (sanitised, confirmed). For the middle
    // mouse button that needs to know whether the program has taken over the
    // mouse — the display tells through its usesMouse() slot. Without it the
    // library pastes on middle click itself, with its own confirmation.
    bool marks = false;
    m_ownMiddlePaste = m_display
        && QMetaObject::invokeMethod(m_display.data(), "usesMouse", Qt::DirectConnection,
                                     Q_RETURN_ARG(bool, marks));
    m_term->setConfirmMultilinePaste(!m_ownMiddlePaste && Settings::instance()->confirmMultilinePaste());

    // QTermWidget searches with the state of its own (hidden) search bar and
    // the find slots declared in qtermwidget.h. The application's search bar
    // drives those; if this build of the library is laid out differently the
    // built-in bar is shown instead.
    m_searchEdit = m_term->findChild<QLineEdit *>(QStringLiteral("searchTextEdit"));
    m_searchMatchCase.clear();
    m_searchRegex.clear();
    if (const auto *options = m_term->findChild<QToolButton *>(QStringLiteral("optionsButton"))) {
        if (const QMenu *menu = options->menu()) {
            const QList<QAction *> entries = menu->actions();
            if (entries.size() >= 2 && entries.at(0)->isCheckable() && entries.at(1)->isCheckable()) {
                m_searchMatchCase = entries.at(0);
                m_searchRegex = entries.at(1);
            }
        }
    }
    const QMetaObject *meta = m_term->metaObject();
    m_searchSupported = m_searchEdit && m_searchMatchCase && m_searchRegex
        && meta->indexOfSlot("noMatchFound()") >= 0 && meta->indexOfSlot("findNext()") >= 0
        && meta->indexOfSlot("findPrevious()") >= 0;

    connect(m_term, &QTermWidget::finished, this, &TerminalPane::onFinished);
    connect(m_term, &QTermWidget::titleChanged, this, [this] { Q_EMIT titleChanged(this); });
    connect(m_term, &QTermWidget::termGetFocus, this, [this] { Q_EMIT activated(this); });
    connect(m_term, &QTermWidget::bell, this, &TerminalPane::onBell);
    connect(m_term, &QTermWidget::termKeyPressed, this, &TerminalPane::onKeyPressed);
    connect(m_term, &QTermWidget::receivedData, this, [this] {
        // Output is flowing again (Ctrl+Q, or the program reset the terminal).
        if (m_suspendedHint)
            hideNotice();
    });
    connect(m_term, &QTermWidget::urlActivated, this, &TerminalPane::onUrlActivated);
    connect(m_term, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        Q_EMIT contextMenuRequested(this, m_term->mapToGlobal(pos), pos);
    });

    m_appliedScrollback = INT_MIN;
    applyProfile();
    applySettings();
}

bool TerminalPane::start()
{
    m_startError.clear();
    m_started = false;
    m_finished = false;

    QString error, note;
    QStringList argv;
    const Profile p = profile();
    if (m_spec.kind == PaneSpec::Kind::Shell) {
        argv = p.resolveCommand(&error, &note);
    } else if (m_spec.command.isEmpty()) {
        error = tr("No command was given.");
    } else {
        const QString program = Shells::resolveProgram(m_spec.command.first());
        if (program.isEmpty()) {
            error = tr("“%1” was not found or is not executable.").arg(m_spec.command.first());
        } else {
            argv = m_spec.command;
            argv[0] = program;
        }
    }
    if (argv.isEmpty()) {
        // Stays visible in the pane; "Restart" tries again (e.g. after the
        // profile was corrected).
        m_startError = error;
        showNotice(error, true);
        return false;
    }

    QString directory = m_spec.workingDirectory;
    if (directory.isEmpty() || !QFileInfo(directory).isDir())
        directory = Shells::homeDirectory();
    m_initialDirectory = directory;
    m_lastDirectory = directory;

    // The environment of the program is the one this application was started
    // with (locale, PATH, session variables) plus what identifies the terminal.
    QStringList environment = {
        QStringLiteral("TERM=xterm-256color"),
        QStringLiteral("COLORTERM=truecolor"),
        QStringLiteral("TERM_PROGRAM=nexus-terminal"),
        QStringLiteral("TERM_PROGRAM_VERSION=") + QCoreApplication::applicationVersion(),
        QStringLiteral("SHELL=")
            + (m_spec.kind == PaneSpec::Kind::Shell && p.commandMode != QLatin1String("custom")
                   ? argv.first() : Shells::loginShell()),
    };
    // The terminal always speaks UTF-8. With no locale configured at all the
    // shell would treat multi-byte input as single bytes; C.UTF-8 exists on
    // every Debian system.
    if (qEnvironmentVariableIsEmpty("LC_ALL") && qEnvironmentVariableIsEmpty("LC_CTYPE")
        && qEnvironmentVariableIsEmpty("LANG"))
        environment << QStringLiteral("LANG=C.UTF-8");

    m_term->setShellProgram(argv.first());
    m_term->setArgs(argv.mid(1));
    m_term->setWorkingDirectory(directory);
    m_term->setEnvironment(environment);
    m_term->startShellProgram();

    m_started = m_term->getShellPID() > 0;
    if (!m_started) {
        m_startError = tr("%1 could not be started.").arg(argv.first());
        showNotice(m_startError, true);
        return false;
    }
    hideNotice();
    if (!note.isEmpty())
        Q_EMIT notice(note, false);
    Q_EMIT titleChanged(this);
    return true;
}

void TerminalPane::restart()
{
    if (isRunning())
        return;
    const bool hadFocus = m_display && m_display->hasFocus();
    if (m_term) {
        m_term->hide();
        m_term->deleteLater();
        m_term = nullptr;
        m_display.clear();
    }
    hideNotice();
    createTerminal();
    start();
    if (hadFocus || m_active)
        focusTerminal();
}

void TerminalPane::onFinished()
{
    m_finished = true;
    const bool hold = m_spec.hold || (m_spec.kind == PaneSpec::Kind::Shell && profile().holdOnExit);
    if (m_closing || !hold) {
        Q_EMIT finished(this);
        return;
    }
    // QTermWidget closes (hides) its display when the program ends. A pane
    // that is kept open exists to show what the program printed last — an
    // ssh error, the output of a finished command — so bring it back.
    if (m_display) {
        m_display->show();
        if (m_active)
            focusTerminal();
    }
    showNotice(m_spec.kind == PaneSpec::Kind::Ssh ? tr("The connection has ended.") : tr("The program has ended."),
               true);
    Q_EMIT titleChanged(this);
}

void TerminalPane::showNotice(const QString &text, bool offerRestart)
{
    m_suspendedHint = false;
    m_noticeLabel->setText(text);
    m_restartButton->setText(m_spec.kind == PaneSpec::Kind::Ssh ? tr("Reconnect") : tr("Restart"));
    m_restartButton->setVisible(offerRestart);
    m_closeButton->show();
    m_notice->show();
}

void TerminalPane::hideNotice()
{
    m_suspendedHint = false;
    m_notice->hide();
}

void TerminalPane::onKeyPressed(const QKeyEvent *event)
{
    // Ctrl+S freezes the output of a terminal whose line discipline has
    // XON/XOFF flow control switched on (the default for shells) — a classic
    // "my terminal hangs". Say what happened and how to get out of it. The
    // tty itself is asked, so programs that turned flow control off (editors
    // that use Ctrl+S) get no hint.
    if (!isRunning() || !(event->modifiers() & Qt::ControlModifier))
        return;
    if (event->key() == Qt::Key_S && !m_notice->isVisible()) {
        struct termios mode;
        const int fd = m_term->getPtySlaveFd();
        if (fd >= 0 && ::tcgetattr(fd, &mode) == 0 && (mode.c_iflag & IXON)) {
            m_noticeLabel->setText(tr("Output is paused (Ctrl+S). Press Ctrl+Q to continue."));
            m_restartButton->hide();
            m_closeButton->hide();
            m_notice->show();
            m_suspendedHint = true;
        }
    } else if (event->key() == Qt::Key_Q && m_suspendedHint) {
        hideNotice();
    }
}

// ---- process ----------------------------------------------------------------------

int TerminalPane::shellPid() const
{
    return m_term && isRunning() ? m_term->getShellPID() : 0;
}

int TerminalPane::foregroundPid() const
{
    return m_term && isRunning() ? m_term->getForegroundProcessId() : 0;
}

QString TerminalPane::foregroundName() const
{
    return processName(foregroundPid());
}

bool TerminalPane::isBusy() const
{
    if (!isRunning())
        return false;
    // An ssh session or an explicit command is itself what would be lost.
    if (m_spec.kind != PaneSpec::Kind::Shell)
        return true;
    const int foreground = foregroundPid();
    return foreground > 0 && foreground != shellPid();
}

QString TerminalPane::busyDescription() const
{
    const int pid = foregroundPid();
    const QString name = processName(pid);
    if (pid <= 0)
        return QString();
    return name.isEmpty() ? tr("PID %1").arg(pid) : tr("%1 (PID %2)").arg(name).arg(pid);
}

bool TerminalPane::sendSignal(int signal, QString *error)
{
    const int group = foregroundPid();
    // 0 and 1 would address this process group or every process.
    if (group <= 1) {
        if (error)
            *error = tr("There is no foreground process in this terminal.");
        return false;
    }
    if (::kill(-group, signal) != 0) {
        if (error)
            *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    return true;
}

void TerminalPane::hangUp()
{
    m_closing = true;
    if (!isRunning())
        return;
    const int shell = shellPid();
    const int foreground = foregroundPid();
    if (foreground > 1 && foreground != shell)
        ::kill(-foreground, SIGHUP);
    if (shell > 1)
        ::kill(shell, SIGHUP);
}

void TerminalPane::dispose()
{
    // Out of the window now; destroyed once the program has reacted to the
    // hang-up, so a shell gets the chance to save its history.
    hide();
    setParent(nullptr);
    disconnect(this, nullptr, nullptr, nullptr);
    if (!isRunning() || !m_term) {
        deleteLater();
        return;
    }
    hangUp();
    connect(m_term, &QTermWidget::finished, this, &QObject::deleteLater);
    QTimer::singleShot(kHangUpGraceMs, this, &QObject::deleteLater);
}

// ---- state ------------------------------------------------------------------------

QString TerminalPane::workingDirectory() const
{
    if (m_term && isRunning()) {
        const QString dir = m_term->workingDirectory();
        if (!dir.isEmpty())
            m_lastDirectory = dir;
    }
    return m_lastDirectory.isEmpty() ? m_initialDirectory : m_lastDirectory;
}

QString TerminalPane::displayDirectory() const
{
    QString dir = workingDirectory();
    const QString home = Shells::homeDirectory();
    if (dir == home)
        return QStringLiteral("~");
    if (home.size() > 1 && dir.startsWith(home + QLatin1Char('/')))
        dir = QLatin1Char('~') + dir.mid(home.size());
    return dir;
}

QString TerminalPane::title() const
{
    if (m_term && m_term->isTitleChanged()) {
        const QString fromProgram = m_term->title().simplified();
        if (!fromProgram.isEmpty())
            return fromProgram;
    }
    if (m_spec.kind != PaneSpec::Kind::Shell && !m_spec.label.isEmpty())
        return m_spec.label;

    // The shell did not announce a title: show where it is, and what runs.
    const QString dir = displayDirectory();
    QString place = dir == QLatin1String("~") || dir == QLatin1String("/") ? dir : QFileInfo(dir).fileName();
    if (place.isEmpty())
        place = dir;
    if (isBusy()) {
        const QString name = foregroundName();
        if (!name.isEmpty())
            return tr("%1 — %2").arg(name, place);
    }
    return place;
}

QSize TerminalPane::gridSize() const
{
    return m_term ? QSize(m_term->screenColumnsCount(), m_term->screenLinesCount()) : QSize();
}

int TerminalPane::zoomPercent() const
{
    const int base = profile().fontSize;
    return qRound(100.0 * qBound(kMinPointSize, base + m_zoomSteps, kMaxPointSize) / base);
}

// ---- appearance -------------------------------------------------------------------

void TerminalPane::applyFont()
{
    if (!m_term)
        return;
    const Profile p = profile();
    QStringList families{p.fontFamily};
    const QString tokenMono = QString::fromLatin1(Tokens::FontMono);
    if (!families.contains(tokenMono))
        families << tokenMono;
    const QStringList fallbacks = QString::fromLatin1(Tokens::FontMonoFallback).split(QLatin1Char(','));
    for (const QString &family : fallbacks)
        families << family.trimmed();

    QFont font;
    font.setFamilies(families);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    // Every attribute is set explicitly so that nothing is inherited from the
    // window chrome around the terminal.
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setUnderline(false);
    font.setStrikeOut(false);
    const qreal size = qBound(qreal(kMinPointSize), (p.fontSize + m_zoomSteps) * Theme::instance()->fontScale(),
                              qreal(kMaxPointSize));
    font.setPointSizeF(size);
    m_term->setTerminalFont(font);
}

void TerminalPane::applyProfile()
{
    if (!m_term)
        return;
    const Profile p = profile();
    const Theme *theme = Theme::instance();

    applyFont();
    m_term->setColorScheme(Schemes::resolve(p.colorScheme, theme->dark()));

    using Shape = QTermWidget::KeyboardCursorShape;
    const Shape shape = p.cursorShape == QLatin1String("underline") ? Shape::UnderlineCursor
        : p.cursorShape == QLatin1String("ibeam") ? Shape::IBeamCursor : Shape::BlockCursor;
    m_term->setKeyboardCursorShape(shape);
    m_term->setBlinkingCursor(p.cursorBlink);

    if (m_appliedScrollback != p.scrollbackLines) {
        m_term->setHistorySize(p.scrollbackLines);
        m_appliedScrollback = p.scrollbackLines;
    }

    // "Transparency" is a system-wide appearance switch; when it is off every
    // terminal is opaque whatever its profile says.
    m_term->setTerminalOpacity(theme->transparency() ? p.opacity / 100.0 : 1.0);
    if (m_display)
        m_display->update();
}

void TerminalPane::applySettings()
{
    if (!m_term)
        return;
    const Settings *settings = Settings::instance();
    m_term->setScrollBarPosition(settings->showScrollBar() ? QTermWidget::ScrollBarRight : QTermWidget::NoScrollBar);
    m_term->setConfirmMultilinePaste(!m_ownMiddlePaste && settings->confirmMultilinePaste());
}

void TerminalPane::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    update();
}

void TerminalPane::setSingle(bool single)
{
    if (m_single == single)
        return;
    m_single = single;
    updateFrame();
}

void TerminalPane::updateFrame()
{
    // A lone pane fills the tab; panes of a split tab are outlined.
    const int border = m_single ? 0 : Tokens::BorderWidth;
    m_layout->setContentsMargins(border, border, border, border);
    update();
}

void TerminalPane::paintEvent(QPaintEvent *)
{
    if (m_single && !m_bellFlash)
        return;
    const Theme *theme = Theme::instance();
    const QColor color = m_bellFlash ? theme->color("warning")
        : m_active ? theme->color("accent") : theme->color("borderSubtle");
    QPainter p(this);
    const int w = Tokens::BorderWidth;
    p.fillRect(QRect(0, 0, width(), w), color);
    p.fillRect(QRect(0, height() - w, width(), w), color);
    p.fillRect(QRect(0, 0, w, height()), color);
    p.fillRect(QRect(width() - w, 0, w, height()), color);
}

void TerminalPane::focusTerminal()
{
    if (m_term)
        m_term->setFocus(Qt::OtherFocusReason);
}

// ---- commands ---------------------------------------------------------------------

bool TerminalPane::hasSelection() const
{
    return m_term && !m_term->selectedText().isEmpty();
}

void TerminalPane::copy()
{
    if (m_term)
        m_term->copyClipboard();
}

void TerminalPane::paste()
{
    pasteText(QGuiApplication::clipboard()->text(QClipboard::Clipboard));
}

void TerminalPane::pasteSelection()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    // Without a primary selection (Wayland compositors may not offer one)
    // there is nothing to paste; the clipboard is deliberately not used instead.
    if (clipboard->supportsSelection())
        pasteText(clipboard->text(QClipboard::Selection));
}

void TerminalPane::pasteText(const QString &raw)
{
    if (!m_term || !isRunning() || raw.isEmpty())
        return;
    const Settings *settings = Settings::instance();

    bool controlsRemoved = false;
    QString text = sanitizePaste(raw, &controlsRemoved);
    if (settings->trimPastedNewlines()) {
        while (text.endsWith(QLatin1Char('\n')))
            text.chop(1);
    }
    if (text.isEmpty())
        return;

    // Several lines run as several commands, and text copied from a web page
    // can carry more than was visible there: show exactly what would be
    // entered and let the user decide.
    if (settings->confirmMultilinePaste() && (text.contains(QLatin1Char('\n')) || controlsRemoved)) {
        if (!Ui::confirmPaste(window(), text, controlsRemoved)) {
            focusTerminal();
            return;
        }
    }

    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    m_term->bracketText(text);          // adds the markers if the program enabled bracketed paste
    m_term->sendText(text);
    focusTerminal();
}

void TerminalPane::handleDrop(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    QString text;
    if (mime->hasUrls()) {
        // Dropped files are inserted as quoted words; nothing is executed.
        QStringList words;
        const QList<QUrl> urls = mime->urls();
        for (const QUrl &url : urls)
            words << shellQuote(url.isLocalFile() ? url.toLocalFile() : url.toString());
        text = words.join(QLatin1Char(' ')) + QLatin1Char(' ');
    } else if (mime->hasText()) {
        text = mime->text();
    }
    event->acceptProposedAction();
    // Not from inside the drop: a confirmation dialog must not keep the drag
    // source waiting.
    if (!text.isEmpty())
        QTimer::singleShot(0, this, [this, text] { pasteText(text); });
}

void TerminalPane::clear()
{
    if (m_term)
        m_term->clear();
}

void TerminalPane::zoomIn()
{
    if (profile().fontSize + m_zoomSteps >= kMaxPointSize)
        return;
    ++m_zoomSteps;
    applyFont();
    Q_EMIT zoomChanged(this);
}

void TerminalPane::zoomOut()
{
    if (profile().fontSize + m_zoomSteps <= kMinPointSize)
        return;
    --m_zoomSteps;
    applyFont();
    Q_EMIT zoomChanged(this);
}

void TerminalPane::zoomReset()
{
    if (m_zoomSteps == 0)
        return;
    m_zoomSteps = 0;
    applyFont();
    Q_EMIT zoomChanged(this);
}

void TerminalPane::onBell()
{
    const QString mode = profile().bell;
    if (mode == QLatin1String("none"))
        return;
    if (mode == QLatin1String("sound")) {
        QApplication::beep();
    } else if (!m_bellFlash) {
        m_bellFlash = true;
        update();
        QTimer::singleShot(Tokens::MotionNormal, this, [this] {
            m_bellFlash = false;
            update();
        });
    }
    Q_EMIT bellRang(this);
}

void TerminalPane::onUrlActivated(const QUrl &url, bool fromContextMenu)
{
    // A plain click only selects; a link is followed with Ctrl+click or
    // from the context menu.
    if (!fromContextMenu && !(QGuiApplication::keyboardModifiers() & Qt::ControlModifier))
        return;
    static const QStringList allowed = {
        QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("ftp"), QStringLiteral("mailto"),
    };
    if (!url.isValid() || !allowed.contains(url.scheme().toLower())) {
        Q_EMIT notice(tr("Links of type “%1” are not opened from the terminal.").arg(url.scheme()), true);
        return;
    }
    if (!QDesktopServices::openUrl(url))
        Q_EMIT notice(tr("No application is available to open %1.").arg(url.toDisplayString()), true);
}

QList<QAction *> TerminalPane::linkActions(const QPoint &pos) const
{
    return m_term ? m_term->filterActions(pos) : QList<QAction *>();
}

// ---- search -----------------------------------------------------------------------

TerminalPane::SearchResult TerminalPane::searchResult() const
{
    // A match is shown as the selection; no match clears it.
    return hasSelection() ? SearchResult::Found : SearchResult::NotFound;
}

TerminalPane::SearchResult TerminalPane::search(const QString &pattern, bool caseSensitive, bool regularExpression)
{
    if (!m_term || !m_searchSupported)
        return SearchResult::NotFound;
    // QTermWidget matches against many lines joined into one string; "(?m)"
    // makes ^ and $ in a regular expression mean the ends of each line.
    const QString effective = regularExpression && !pattern.isEmpty() ? QStringLiteral("(?m)") + pattern : pattern;
    {
        // Set the criteria quietly, then search once.
        const QSignalBlocker b1(m_searchEdit), b2(m_searchMatchCase), b3(m_searchRegex);
        m_searchEdit->setText(effective);
        m_searchMatchCase->setChecked(caseSensitive);
        m_searchRegex->setChecked(regularExpression);
    }
    if (pattern.isEmpty())
        return SearchResult::NoPattern;
    if (regularExpression && !QRegularExpression(effective).isValid())
        return SearchResult::InvalidPattern;
    // New criteria: drop the current match and look for the newest one, from
    // the cursor upwards. (noMatchFound() is the library's slot that clears
    // the selection the search works from.)
    QMetaObject::invokeMethod(m_term, "noMatchFound", Qt::DirectConnection);
    QMetaObject::invokeMethod(m_term, "findPrevious", Qt::DirectConnection);
    return searchResult();
}

TerminalPane::SearchResult TerminalPane::searchNext()
{
    if (!m_term || !m_searchSupported)
        return SearchResult::NotFound;
    if (m_searchEdit->text().isEmpty())
        return SearchResult::NoPattern;
    if (m_searchRegex->isChecked() && !QRegularExpression(m_searchEdit->text()).isValid())
        return SearchResult::InvalidPattern;
    QMetaObject::invokeMethod(m_term, "findNext", Qt::DirectConnection);
    return searchResult();
}

TerminalPane::SearchResult TerminalPane::searchPrevious()
{
    if (!m_term || !m_searchSupported)
        return SearchResult::NotFound;
    if (m_searchEdit->text().isEmpty())
        return SearchResult::NoPattern;
    if (m_searchRegex->isChecked() && !QRegularExpression(m_searchEdit->text()).isValid())
        return SearchResult::InvalidPattern;
    QMetaObject::invokeMethod(m_term, "findPrevious", Qt::DirectConnection);
    return searchResult();
}

void TerminalPane::showBuiltinSearchBar()
{
    if (m_term)
        m_term->toggleShowSearchBar();
}

// ---- events from the terminal display -----------------------------------------------

bool TerminalPane::terminalOwnsMouse() const
{
    // usesMouse() is true while the terminal itself handles the mouse
    // (selection, paste) and false once the program asked for mouse events.
    bool marks = true;
    if (m_display)
        QMetaObject::invokeMethod(m_display.data(), "usesMouse", Qt::DirectConnection, Q_RETURN_ARG(bool, marks));
    return marks;
}

bool TerminalPane::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_display)
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::MiddleButton && m_ownMiddlePaste
            && (terminalOwnsMouse() || (mouse->modifiers() & Qt::ShiftModifier))) {
            QTimer::singleShot(0, this, &TerminalPane::pasteSelection);
            return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && Settings::instance()->copyOnSelect()) {
            // After the display has finished the selection for this release.
            QTimer::singleShot(0, this, [this] {
                if (hasSelection())
                    copy();
            });
        }
        break;
    }
    case QEvent::Wheel: {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            if (wheel->angleDelta().y() > 0)
                zoomIn();
            else if (wheel->angleDelta().y() < 0)
                zoomOut();
            return true;
        }
        break;
    }
    case QEvent::Drop:
        handleDrop(static_cast<QDropEvent *>(event));
        return true;
    default:
        break;
    }
    return false;
}
