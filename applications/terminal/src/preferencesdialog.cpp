// SPDX-License-Identifier: GPL-3.0-or-later
#include "preferencesdialog.h"

#include "schemes.h"
#include "settings.h"
#include "shells.h"
#include "shortcuts.h"
#include "theme.h"
#include "tokens.h"

#include <QAction>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kFieldWidth = Tokens::SidebarWidth + Tokens::SpaceXl;        // wide controls in rows
constexpr int kShortcutWidth = Tokens::ControlHeight * 5;
const char kShellPrefix[] = "shell:";

// Adds "<title>" and a card to a page layout; rows go into the card.
QFrame *addSection(QVBoxLayout *page, const QString &title)
{
    QLabel *label = Ui::label(title, "section");
    label->setContentsMargins(Tokens::SpaceXs, Tokens::SpaceSm, 0, 0);
    page->addWidget(label);
    QFrame *card = Ui::card();
    page->addWidget(card);
    return card;
}

QVBoxLayout *newPage(QWidget *page, const QString &heading, const QString &description)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(Tokens::SpaceXl, Tokens::SpaceXl, Tokens::SpaceXl, Tokens::SpaceXl);
    layout->setSpacing(Tokens::SpaceSm);
    layout->addWidget(Ui::label(heading, "heading", page));
    QLabel *text = Ui::label(description, "secondary", page);
    text->setWordWrap(true);
    layout->addWidget(text);
    layout->addSpacing(Tokens::SpaceSm);
    return layout;
}

// A key may be bound without Ctrl, Alt or Meta only if it never types text
// into the terminal.
bool isUsableShortcut(const QKeySequence &sequence)
{
    if (sequence.isEmpty())
        return true;
    const QKeyCombination combination = sequence[0];
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return true;
    const Qt::Key key = combination.key();
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return true;
    static const QList<Qt::Key> navigation = {
        Qt::Key_Insert, Qt::Key_Delete, Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End,
    };
    return (modifiers & Qt::ShiftModifier) && navigation.contains(key);
}

} // namespace

PreferencesDialog::PreferencesDialog(Page page, QWidget *parent)
    : Ui::Dialog(tr("Preferences"), parent)
{
    hideHeader();
    resize(Tokens::WindowDefaultWidth - Tokens::SpaceXxl * 2 - Tokens::SpaceLg,
           Tokens::WindowDefaultHeight - Tokens::SpaceLg - Tokens::SpaceXs);
    // Narrower than this the profile editor's fixed-width controls no longer fit.
    setMinimumSize(Tokens::WindowMinWidth + Tokens::SpaceXxxl * 4, Tokens::WindowMinHeight + Tokens::SpaceXxxl);

    auto *columns = new QHBoxLayout;
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);
    content()->addLayout(columns, 1);

    // ---- sidebar ------------------------------------------------------------------
    auto *side = new QWidget(this);
    side->setObjectName(QStringLiteral("Sidebar"));
    side->setAttribute(Qt::WA_StyledBackground, true);
    side->setFixedWidth(Tokens::SidebarWidth - Tokens::SpaceXxl);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(Tokens::SpaceMd, Tokens::SpaceXl, Tokens::SpaceMd, Tokens::SpaceMd);
    sideLayout->setSpacing(Tokens::SpaceMd);
    QLabel *sideTitle = Ui::label(tr("Preferences"), "title", side);
    sideTitle->setContentsMargins(Tokens::SpaceSm, 0, 0, 0);
    sideLayout->addWidget(sideTitle);

    m_sidebar = new QListWidget(side);
    m_sidebar->setAccessibleName(tr("Sections"));
    m_sidebar->setIconSize(QSize(Tokens::IconSmall + Tokens::SpaceXxs, Tokens::IconSmall + Tokens::SpaceXxs));
    m_sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const struct { const char *icon; QString title; } sections[] = {
        {"sliders", tr("General")},
        {"terminal", tr("Profiles")},
        {"keyboard", tr("Shortcuts")},
    };
    for (const auto &section : sections) {
        QListWidgetItem *item = Ui::listItem(m_sidebar, section.title);
        item->setData(Qt::UserRole, QString::fromLatin1(section.icon));
    }
    auto tintSidebar = [this] {
        for (int i = 0; i < m_sidebar->count(); ++i) {
            QListWidgetItem *item = m_sidebar->item(i);
            item->setIcon(Theme::instance()->icon(item->data(Qt::UserRole).toString(),
                                                  QStringLiteral("textSecondary"), QStringLiteral("accent")));
        }
    };
    tintSidebar();
    connect(Theme::instance(), &Theme::changed, this, tintSidebar);
    sideLayout->addWidget(m_sidebar, 1);
    columns->addWidget(side);

    // ---- pages ---------------------------------------------------------------------
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildGeneralPage());
    m_pages->addWidget(buildProfilesPage());
    m_pages->addWidget(buildShortcutsPage());
    columns->addWidget(m_pages, 1);

    QPushButton *cancel = addFooterButton(tr("Cancel"));
    m_saveButton = addFooterButton(tr("Save"), "primary");
    m_saveButton->setDefault(true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_saveButton, &QPushButton::clicked, this, &PreferencesDialog::save);
    connect(m_sidebar, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_sidebar->setCurrentRow(int(page));
    checkShortcuts();
}

QWidget *PreferencesDialog::scrollPage(QWidget *inner)
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(inner);
    // The dialog's own background shows through.
    inner->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    return scroll;
}

// ---- General --------------------------------------------------------------------------

QWidget *PreferencesDialog::buildGeneralPage()
{
    const Settings *settings = Settings::instance();
    auto *page = new QWidget;
    QVBoxLayout *layout = newPage(page, tr("General"), tr("Behaviour of the window, the clipboard and tabs."));

    QFrame *window = addSection(layout, tr("Window"));
    m_compact = new Ui::Switch;
    m_compact->setChecked(settings->compactMode());
    Ui::addRow(window, tr("Compact mode"),
               tr("Hide the menu bar. All commands stay available from the menu button in the tab strip."),
               m_compact);
    m_statusBar = new Ui::Switch;
    m_statusBar->setChecked(settings->showStatusBar());
    Ui::addRow(window, tr("Status bar"),
               tr("Show the foreground process, working directory and terminal size at the bottom."), m_statusBar);
    m_scrollBar = new Ui::Switch;
    m_scrollBar->setChecked(settings->showScrollBar());
    Ui::addRow(window, tr("Scroll bar"), tr("Show a scroll bar at the right edge of each terminal."), m_scrollBar);

    QFrame *clipboard = addSection(layout, tr("Clipboard"));
    m_copyOnSelect = new Ui::Switch;
    m_copyOnSelect->setChecked(settings->copyOnSelect());
    Ui::addRow(clipboard, tr("Copy on select"),
               tr("Put text on the clipboard as soon as it is selected with the mouse."), m_copyOnSelect);
    m_confirmPaste = new Ui::Switch;
    m_confirmPaste->setChecked(settings->confirmMultilinePaste());
    Ui::addRow(clipboard, tr("Confirm multi-line paste"),
               tr("Show what would be entered before pasting several lines. Protects against text that "
                  "hides commands."),
               m_confirmPaste);
    m_trimPaste = new Ui::Switch;
    m_trimPaste->setChecked(settings->trimPastedNewlines());
    Ui::addRow(clipboard, tr("Remove trailing line breaks"),
               tr("A pasted command is not run until you press Return yourself."), m_trimPaste);

    QFrame *tabs = addSection(layout, tr("Tabs and session"));
    m_confirmClose = new Ui::Switch;
    m_confirmClose->setChecked(settings->confirmCloseRunning());
    Ui::addRow(tabs, tr("Confirm closing running programs"),
               tr("Ask before closing a tab, pane or window in which a program other than the shell is running."),
               m_confirmClose);
    m_restore = Ui::comboBox();
    m_restore->addItem(tr("Ask"), QStringLiteral("ask"));
    m_restore->addItem(tr("Always"), QStringLiteral("always"));
    m_restore->addItem(tr("Never"), QStringLiteral("never"));
    m_restore->setCurrentIndex(qMax(0, m_restore->findData(settings->restoreSession())));
    Ui::addRow(tabs, tr("Restore previous session"),
               tr("Reopen the tabs, splits and working directories of the last window with new shells. "
                  "Commands are never run again and terminal contents are not kept."),
               m_restore);

    layout->addStretch(1);
    return scrollPage(page);
}

// ---- Profiles ---------------------------------------------------------------------------

QWidget *PreferencesDialog::buildProfilesPage()
{
    m_profiles = ProfileManager::instance()->profiles();
    m_defaultId = ProfileManager::instance()->defaultId();

    auto *page = new QWidget;
    QVBoxLayout *layout = newPage(page, tr("Profiles"),
                                  tr("A profile decides what a terminal runs and how it looks. "
                                     "New tabs use the default profile."));
    auto *columns = new QHBoxLayout;
    columns->setSpacing(Tokens::SpaceLg);
    layout->addLayout(columns, 1);

    // ---- list ---------------------------------------------------------------------
    auto *listColumn = new QVBoxLayout;
    listColumn->setSpacing(Tokens::SpaceSm);
    m_profileList = new QListWidget(page);
    m_profileList->setAccessibleName(tr("Profiles"));
    m_profileList->setFixedWidth(Tokens::SidebarWidth - Tokens::SpaceXxxl - Tokens::SpaceXl);
    m_profileList->setIconSize(QSize(Tokens::IconSmall, Tokens::IconSmall));
    m_profileList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_profileList->setTextElideMode(Qt::ElideRight);
    listColumn->addWidget(m_profileList, 1);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(Tokens::SpaceXs);
    QToolButton *add = Ui::iconButton(QStringLiteral("plus"), tr("New profile"), page);
    QToolButton *duplicate = Ui::iconButton(QStringLiteral("copy"), tr("Duplicate profile"), page);
    m_deleteProfile = Ui::iconButton(QStringLiteral("trash"), tr("Delete profile"), page);
    m_defaultProfile = Ui::iconButton(QStringLiteral("star"), tr("Use as default profile"), page);
    buttons->addWidget(add);
    buttons->addWidget(duplicate);
    buttons->addWidget(m_deleteProfile);
    buttons->addStretch(1);
    buttons->addWidget(m_defaultProfile);
    listColumn->addLayout(buttons);
    columns->addLayout(listColumn);

    // ---- editor -------------------------------------------------------------------
    m_profileEditor = new QWidget;
    auto *editor = new QVBoxLayout(m_profileEditor);
    editor->setContentsMargins(0, 0, Tokens::SpaceSm, 0);
    editor->setSpacing(Tokens::SpaceSm);

    QFrame *identity = Ui::card();
    editor->addWidget(identity);
    m_name = Ui::lineEdit();
    m_name->setMaxLength(48);
    m_name->setFixedWidth(kFieldWidth);
    Ui::addRow(identity, tr("Name"), QString(), m_name);

    QFrame *command = addSection(editor, tr("Command"));
    m_commandMode = Ui::comboBox();
    m_commandMode->setFixedWidth(kFieldWidth);
    Ui::addRow(command, tr("Run"), tr("The shell or program started in a new terminal."), m_commandMode);
    m_command = Ui::lineEdit();
    m_command->setFixedWidth(kFieldWidth);
    m_command->setPlaceholderText(tr("Program and arguments"));
    Ui::addRow(command, tr("Custom command"),
               tr("Started directly, without a shell. Quotes and backslashes group words as in a shell; "
                  "variables and wildcards are not expanded."),
               m_command);
    m_commandHint = Ui::label(QString(), "error");
    m_commandHint->setWordWrap(true);
    m_commandHint->setContentsMargins(0, 0, 0, Tokens::SpaceSm);
    Ui::cardLayout(command)->addWidget(m_commandHint);
    m_loginShell = new Ui::Switch;
    Ui::addRow(command, tr("Login shell"),
               tr("Start the shell with -l, so that it reads the login start-up files."), m_loginShell);

    QFrame *directory = addSection(editor, tr("Working directory"));
    m_dirMode = Ui::comboBox();
    m_dirMode->setFixedWidth(kFieldWidth);
    m_dirMode->addItem(tr("Directory of the current terminal"), QStringLiteral("inherit"));
    m_dirMode->addItem(tr("Home directory"), QStringLiteral("home"));
    m_dirMode->addItem(tr("A fixed directory"), QStringLiteral("custom"));
    Ui::addRow(directory, tr("New terminals start in"), QString(), m_dirMode);
    auto *dirHolder = new QWidget;
    auto *dirLayout = new QHBoxLayout(dirHolder);
    dirLayout->setContentsMargins(0, 0, 0, 0);
    dirLayout->setSpacing(Tokens::SpaceXs);
    dirHolder->setFixedWidth(kFieldWidth);
    m_dir = Ui::lineEdit();
    m_dir->setAccessibleName(tr("Fixed directory"));
    m_dirBrowse = Ui::iconButton(QStringLiteral("folder-open"), tr("Choose directory…"));
    dirLayout->addWidget(m_dir, 1);
    dirLayout->addWidget(m_dirBrowse);
    Ui::addRow(directory, tr("Fixed directory"), QString(), dirHolder);

    QFrame *text = addSection(editor, tr("Text"));
    m_font = new QFontComboBox;
    m_font->setFontFilters(QFontComboBox::MonospacedFonts);
    m_font->setEditable(false);
    m_font->setFixedSize(kFieldWidth, Tokens::ControlHeight);
    Ui::addRow(text, tr("Font"), tr("Only fixed-width fonts are offered."), m_font);
    m_fontSize = Ui::spinBox(Profile::MinFontSize, Profile::MaxFontSize);
    m_fontSize->setSuffix(tr(" pt"));
    m_fontSize->setFixedWidth(Tokens::ControlHeight * 3);
    Ui::addRow(text, tr("Size"), QString(), m_fontSize);

    QFrame *colours = addSection(editor, tr("Colours"));
    m_scheme = Ui::comboBox();
    m_scheme->setFixedWidth(kFieldWidth);
    m_scheme->addItem(tr("Automatic"), QStringLiteral("auto"));
    const QStringList schemes = Schemes::available();
    for (const QString &scheme : schemes)
        m_scheme->addItem(Schemes::label(scheme), scheme);
    Ui::addRow(colours, tr("Colour scheme"),
               tr("Automatic follows the system appearance: %1 when dark, %2 when light.")
                   .arg(Schemes::label(Schemes::automatic(true)), Schemes::label(Schemes::automatic(false))),
               m_scheme);
    auto *opacityHolder = new QWidget;
    auto *opacityLayout = new QHBoxLayout(opacityHolder);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    opacityLayout->setSpacing(Tokens::SpaceMd);
    m_opacity = new QSlider(Qt::Horizontal);
    m_opacity->setRange(Profile::MinOpacity, 100);
    m_opacity->setPageStep(10);
    m_opacity->setAccessibleName(tr("Opacity"));
    opacityHolder->setFixedWidth(kFieldWidth);
    m_opacityValue = Ui::label(QString(), "secondary");
    m_opacityValue->setFixedWidth(Tokens::ControlHeight + Tokens::SpaceMd);
    m_opacityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opacityLayout->addWidget(m_opacity, 1);
    opacityLayout->addWidget(m_opacityValue);
    Ui::addRow(colours, tr("Opacity"),
               tr("Below 100 % the desktop shows through the terminal background. Needs a compositing "
                  "window manager and is ignored while transparency is turned off in the appearance settings."),
               opacityHolder);

    QFrame *cursor = addSection(editor, tr("Cursor"));
    m_cursorShape = Ui::comboBox();
    m_cursorShape->setFixedWidth(kFieldWidth);
    m_cursorShape->addItem(tr("Block"), QStringLiteral("block"));
    m_cursorShape->addItem(tr("Underline"), QStringLiteral("underline"));
    m_cursorShape->addItem(tr("I-beam"), QStringLiteral("ibeam"));
    Ui::addRow(cursor, tr("Shape"), QString(), m_cursorShape);
    m_cursorBlink = new Ui::Switch;
    Ui::addRow(cursor, tr("Blink"), QString(), m_cursorBlink);

    QFrame *scrollback = addSection(editor, tr("Scrollback"));
    m_scrollbackUnlimited = new Ui::Switch;
    Ui::addRow(scrollback, tr("Unlimited"),
               tr("Keep every line. Long-running output is stored in a temporary file."), m_scrollbackUnlimited);
    m_scrollback = Ui::spinBox(0, Profile::MaxScrollback);
    m_scrollback->setSingleStep(1000);
    m_scrollback->setSuffix(tr(" lines"));
    m_scrollback->setFixedWidth(Tokens::ControlHeight * 5);
    Ui::addRow(scrollback, tr("Lines to keep"), tr("0 turns the scrollback off."), m_scrollback);

    QFrame *behaviour = addSection(editor, tr("Behaviour"));
    m_bell = Ui::comboBox();
    m_bell->setFixedWidth(kFieldWidth);
    m_bell->addItem(tr("None"), QStringLiteral("none"));
    m_bell->addItem(tr("Visual (flash the pane, mark the tab)"), QStringLiteral("visual"));
    m_bell->addItem(tr("Sound"), QStringLiteral("sound"));
    Ui::addRow(behaviour, tr("Bell"), QString(), m_bell);
    m_hold = new Ui::Switch;
    Ui::addRow(behaviour, tr("Keep the pane open"),
               tr("When the shell exits, leave its output on screen until the pane is closed or restarted."),
               m_hold);
    editor->addStretch(1);

    columns->addWidget(scrollPage(m_profileEditor), 1);

    // ---- behaviour -----------------------------------------------------------------
    connect(m_profileList, &QListWidget::currentRowChanged, this, &PreferencesDialog::selectProfile);
    connect(add, &QToolButton::clicked, this, &PreferencesDialog::addProfile);
    connect(duplicate, &QToolButton::clicked, this, &PreferencesDialog::duplicateProfile);
    connect(m_deleteProfile, &QToolButton::clicked, this, &PreferencesDialog::deleteProfile);
    connect(m_defaultProfile, &QToolButton::clicked, this, &PreferencesDialog::makeDefaultProfile);
    connect(m_dirBrowse, &QToolButton::clicked, this, &PreferencesDialog::chooseWorkingDirectory);
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &name) {
        if (QListWidgetItem *item = m_profileList->currentItem(); item && m_currentProfile >= 0) {
            m_profiles[m_currentProfile].name = name;
            item->setText(name);
        }
    });
    connect(m_commandMode, &QComboBox::currentIndexChanged, this, &PreferencesDialog::updateProfileEditorState);
    connect(m_command, &QLineEdit::textChanged, this, &PreferencesDialog::updateProfileEditorState);
    connect(m_dirMode, &QComboBox::currentIndexChanged, this, &PreferencesDialog::updateProfileEditorState);
    connect(m_scrollbackUnlimited, &Ui::Switch::toggled, this, &PreferencesDialog::updateProfileEditorState);
    connect(m_opacity, &QSlider::valueChanged, this, [this](int value) {
        m_opacityValue->setText(tr("%1 %").arg(value));
    });

    refreshProfileList();
    int initial = 0;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles.at(i).id == m_defaultId)
            initial = i;
    }
    m_profileList->setCurrentRow(initial);
    return page;
}

void PreferencesDialog::refreshProfileList()
{
    const QSignalBlocker blocker(m_profileList);
    const int current = m_currentProfile;
    m_profileList->clear();
    for (const Profile &profile : std::as_const(m_profiles)) {
        // A star marks the profile new tabs use.
        const bool isDefault = profile.id == m_defaultId;
        QListWidgetItem *item = Ui::listItem(m_profileList, profile.name);
        item->setIcon(isDefault ? Theme::instance()->icon(QStringLiteral("star"), QStringLiteral("accent"),
                                                          QStringLiteral("accent"))
                                : Theme::instance()->icon(QStringLiteral("terminal")));
        item->setToolTip(isDefault ? tr("%1 — default profile").arg(profile.name) : profile.name);
    }
    if (current >= 0 && current < m_profiles.size())
        m_profileList->setCurrentRow(current);
}

void PreferencesDialog::selectProfile(int index)
{
    storeProfileEditor();
    m_currentProfile = index >= 0 && index < m_profiles.size() ? index : -1;
    m_profileEditor->setEnabled(m_currentProfile >= 0);
    if (m_currentProfile >= 0)
        loadProfileEditor(m_profiles.at(m_currentProfile));
    updateProfileEditorState();
}

void PreferencesDialog::loadProfileEditor(const Profile &profile)
{
    m_loadingProfile = true;
    m_name->setText(profile.name);

    // What can be run: the login shell, every shell of /etc/shells, a command.
    m_commandMode->clear();
    m_commandMode->addItem(tr("Login shell — %1").arg(Shells::displayName(Shells::loginShell())),
                           QStringLiteral("login-shell"));
    bool shellListed = false;
    const QList<Shells::Shell> shells = Shells::available();
    for (const Shells::Shell &shell : shells) {
        m_commandMode->addItem(tr("%1 — %2").arg(Shells::displayName(shell.path), shell.path),
                               QLatin1String(kShellPrefix) + shell.path);
        shellListed = shellListed || shell.path == profile.shell;
    }
    if (profile.commandMode == QLatin1String("shell") && !shellListed) {
        // Keep a shell that has since been removed visible instead of
        // silently switching the profile to something else.
        m_commandMode->addItem(tr("%1 (not available)").arg(profile.shell),
                               QLatin1String(kShellPrefix) + profile.shell);
    }
    m_commandMode->addItem(tr("Custom command"), QStringLiteral("custom"));
    const QString modeData = profile.commandMode == QLatin1String("shell")
        ? QLatin1String(kShellPrefix) + profile.shell : profile.commandMode;
    m_commandMode->setCurrentIndex(qMax(0, m_commandMode->findData(modeData)));
    m_command->setText(profile.command);
    m_loginShell->setChecked(profile.loginShell);

    m_dirMode->setCurrentIndex(qMax(0, m_dirMode->findData(profile.workingDirMode)));
    m_dir->setText(profile.workingDir);

    m_font->setCurrentFont(QFont(profile.fontFamily));
    m_fontSize->setValue(profile.fontSize);
    m_scheme->setCurrentIndex(qMax(0, m_scheme->findData(profile.colorScheme)));
    m_opacity->setValue(profile.opacity);
    m_opacityValue->setText(tr("%1 %").arg(profile.opacity));
    m_cursorShape->setCurrentIndex(qMax(0, m_cursorShape->findData(profile.cursorShape)));
    m_cursorBlink->setChecked(profile.cursorBlink);
    m_scrollbackUnlimited->setChecked(profile.scrollbackLines < 0);
    m_scrollback->setValue(profile.scrollbackLines < 0 ? Profile::builtinDefault().scrollbackLines
                                                       : profile.scrollbackLines);
    m_bell->setCurrentIndex(qMax(0, m_bell->findData(profile.bell)));
    m_hold->setChecked(profile.holdOnExit);
    m_loadingProfile = false;
}

void PreferencesDialog::storeProfileEditor()
{
    if (m_currentProfile < 0 || m_currentProfile >= m_profiles.size() || m_loadingProfile)
        return;
    Profile &profile = m_profiles[m_currentProfile];
    profile.name = m_name->text();

    const QString mode = m_commandMode->currentData().toString();
    if (mode.startsWith(QLatin1String(kShellPrefix))) {
        profile.commandMode = QStringLiteral("shell");
        profile.shell = mode.mid(int(qstrlen(kShellPrefix)));
    } else {
        profile.commandMode = mode;
    }
    profile.command = m_command->text();
    profile.loginShell = m_loginShell->isChecked();
    profile.workingDirMode = m_dirMode->currentData().toString();
    profile.workingDir = m_dir->text();
    // The font box shows nothing for a family that is not installed; keep
    // the stored one in that case.
    if (m_font->currentIndex() >= 0)
        profile.fontFamily = m_font->currentFont().family();
    profile.fontSize = m_fontSize->value();
    profile.colorScheme = m_scheme->currentData().toString();
    profile.opacity = m_opacity->value();
    profile.cursorShape = m_cursorShape->currentData().toString();
    profile.cursorBlink = m_cursorBlink->isChecked();
    profile.scrollbackLines = m_scrollbackUnlimited->isChecked() ? -1 : m_scrollback->value();
    profile.bell = m_bell->currentData().toString();
    profile.holdOnExit = m_hold->isChecked();
    profile.sanitize();
}

void PreferencesDialog::updateProfileEditorState()
{
    if (m_loadingProfile)
        return;
    const bool custom = m_commandMode->currentData().toString() == QLatin1String("custom");
    m_command->setEnabled(custom);
    m_loginShell->setEnabled(!custom);
    const bool fixedDir = m_dirMode->currentData().toString() == QLatin1String("custom");
    m_dir->setEnabled(fixedDir);
    m_dirBrowse->setEnabled(fixedDir);
    m_scrollback->setEnabled(!m_scrollbackUnlimited->isChecked());

    // Tell right away when a custom command cannot be started.
    QString hint;
    if (custom) {
        bool balanced = true;
        const QStringList argv = Shells::splitCommand(m_command->text(), &balanced);
        if (!balanced)
            hint = tr("A quote is not closed.");
        else if (argv.isEmpty())
            hint = tr("Enter the program to run.");
        else if (Shells::resolveProgram(argv.first()).isEmpty())
            hint = tr("“%1” was not found or is not executable.").arg(argv.first());
    }
    m_commandHint->setText(hint);
    m_commandHint->setVisible(!hint.isEmpty());

    const int count = int(m_profiles.size());
    m_deleteProfile->setEnabled(count > 1 && m_currentProfile >= 0);
    m_deleteProfile->setToolTip(count > 1 ? tr("Delete profile") : tr("The last profile cannot be deleted"));
    const bool isDefault = m_currentProfile >= 0 && m_profiles.at(m_currentProfile).id == m_defaultId;
    m_defaultProfile->setEnabled(m_currentProfile >= 0 && !isDefault);
    m_defaultProfile->setToolTip(isDefault ? tr("This is the default profile") : tr("Use as default profile"));
}

void PreferencesDialog::addProfile()
{
    storeProfileEditor();
    Profile profile = Profile::builtinDefault();
    profile.id = ProfileManager::newId();
    profile.name = ProfileManager::uniqueName(m_profiles, tr("New profile"));
    m_profiles.append(profile);
    m_currentProfile = -1;          // nothing to store when the selection moves
    refreshProfileList();
    m_profileList->setCurrentRow(int(m_profiles.size()) - 1);
    m_name->setFocus();
    m_name->selectAll();
}

void PreferencesDialog::duplicateProfile()
{
    if (m_currentProfile < 0)
        return;
    storeProfileEditor();
    Profile profile = m_profiles.at(m_currentProfile);
    profile.id = ProfileManager::newId();
    profile.name = ProfileManager::uniqueName(m_profiles, tr("%1 copy").arg(profile.name));
    m_profiles.append(profile);
    m_currentProfile = -1;
    refreshProfileList();
    m_profileList->setCurrentRow(int(m_profiles.size()) - 1);
}

void PreferencesDialog::deleteProfile()
{
    if (m_currentProfile < 0 || m_profiles.size() <= 1)
        return;
    const Profile profile = m_profiles.at(m_currentProfile);
    if (!Ui::confirm(this, tr("Delete profile “%1”?").arg(profile.name),
                     tr("Terminals that use it switch to the default profile. This takes effect when you "
                        "save the preferences."),
                     tr("Delete"), true))
        return;
    const int removed = m_currentProfile;
    m_profiles.removeAt(removed);
    if (profile.id == m_defaultId)
        m_defaultId = m_profiles.first().id;
    m_currentProfile = -1;
    refreshProfileList();
    m_profileList->setCurrentRow(qMin(removed, int(m_profiles.size()) - 1));
}

void PreferencesDialog::makeDefaultProfile()
{
    if (m_currentProfile < 0)
        return;
    storeProfileEditor();
    m_defaultId = m_profiles.at(m_currentProfile).id;
    refreshProfileList();
    updateProfileEditorState();
}

void PreferencesDialog::chooseWorkingDirectory()
{
    QString start = m_dir->text().trimmed();
    if (start.isEmpty() || !QFileInfo(start).isDir())
        start = Shells::homeDirectory();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose Directory"), start);
    if (!dir.isEmpty())
        m_dir->setText(dir);
}

// ---- Shortcuts ------------------------------------------------------------------------------

QWidget *PreferencesDialog::buildShortcutsPage()
{
    auto *page = new QWidget;
    QVBoxLayout *layout = newPage(page, tr("Keyboard shortcuts"),
                                  tr("Click a field and press the new key combination. Every command can "
                                     "have a second, alternative shortcut."));

    auto *top = new QHBoxLayout;
    m_shortcutError = Ui::label(QString(), "error", page);
    m_shortcutError->setWordWrap(true);
    QPushButton *restoreAll = Ui::button(tr("Restore Defaults"), "secondary", page);
    top->addWidget(m_shortcutError, 1);
    top->addWidget(restoreAll, 0, Qt::AlignTop);
    layout->addLayout(top);

    auto makeEdit = [this](const QString &accessibleName) {
        auto *edit = new QKeySequenceEdit;
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        edit->setFixedSize(kShortcutWidth, Tokens::ControlHeight);
        edit->setAccessibleName(accessibleName);
        if (auto *inner = edit->findChild<QLineEdit *>()) {
            inner->setFixedHeight(Tokens::ControlHeight);
            inner->setPlaceholderText(tr("None"));
            // Qt's stock clear glyph is dark grey; use the themed one.
            if (auto *clear = inner->findChild<QAction *>(QStringLiteral("_q_qlineeditclearaction")))
                clear->setIcon(Theme::instance()->icon(QStringLiteral("close"), QStringLiteral("textMuted")));
        }
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, &PreferencesDialog::checkShortcuts);
        return edit;
    };

    QFrame *card = nullptr;
    QString category;
    for (const ActionDef &def : Shortcuts::definitions()) {
        if (def.category != category) {
            category = def.category;
            card = addSection(layout, category);
        }
        ShortcutRow row;
        row.id = def.id;
        QString plain = def.text;
        plain.remove(QStringLiteral("…"));
        row.primary = makeEdit(tr("Shortcut for %1").arg(plain));
        row.alternative = makeEdit(tr("Alternative shortcut for %1").arg(plain));
        row.reset = Ui::iconButton(QStringLiteral("undo"), tr("Restore the default"));
        auto *holder = new QWidget;
        auto *holderLayout = new QHBoxLayout(holder);
        holderLayout->setContentsMargins(0, 0, 0, 0);
        holderLayout->setSpacing(Tokens::SpaceSm);
        holderLayout->addWidget(row.primary);
        holderLayout->addWidget(row.alternative);
        holderLayout->addWidget(row.reset);
        Ui::addRow(card, plain, QString(), holder);

        setRowBindings(row, Shortcuts::current(def.id));
        const QString id = def.id;
        connect(row.reset, &QToolButton::clicked, this, [this, id] {
            for (const ShortcutRow &r : std::as_const(m_shortcutRows)) {
                if (r.id == id)
                    setRowBindings(r, Shortcuts::defaults(id));
            }
            checkShortcuts();
        });
        m_shortcutRows.append(row);
    }
    connect(restoreAll, &QPushButton::clicked, this, [this] {
        for (const ShortcutRow &row : std::as_const(m_shortcutRows))
            setRowBindings(row, Shortcuts::defaults(row.id));
        checkShortcuts();
    });

    layout->addStretch(1);
    return scrollPage(page);
}

QList<QKeySequence> PreferencesDialog::rowBindings(const ShortcutRow &row) const
{
    QList<QKeySequence> bindings;
    for (const QKeySequenceEdit *edit : {row.primary, row.alternative}) {
        const QKeySequence sequence = edit->keySequence();
        if (!sequence.isEmpty() && !bindings.contains(sequence))
            bindings.append(sequence);
    }
    return bindings;
}

void PreferencesDialog::setRowBindings(const ShortcutRow &row, const QList<QKeySequence> &bindings)
{
    const QSignalBlocker b1(row.primary), b2(row.alternative);
    row.primary->setKeySequence(bindings.value(0));
    row.alternative->setKeySequence(bindings.value(1));
}

void PreferencesDialog::checkShortcuts()
{
    // Which commands use which key combination.
    QHash<QString, QStringList> users;
    for (const ShortcutRow &row : std::as_const(m_shortcutRows)) {
        const QList<QKeySequence> bindings = rowBindings(row);
        for (const QKeySequence &sequence : bindings)
            users[sequence.toString(QKeySequence::PortableText)] << row.id;
    }

    QString error;
    for (const ShortcutRow &row : std::as_const(m_shortcutRows)) {
        const ActionDef *def = Shortcuts::definition(row.id);
        for (QKeySequenceEdit *edit : {row.primary, row.alternative}) {
            const QKeySequence sequence = edit->keySequence();
            const QStringList sharedWith = users.value(sequence.toString(QKeySequence::PortableText));
            const bool conflict = !sequence.isEmpty() && sharedWith.size() > 1;
            const bool unusable = !isUsableShortcut(sequence);
            if (auto *inner = edit->findChild<QLineEdit *>())
                Ui::setProperty(inner, "nxInvalid", conflict || unusable);
            if (!error.isEmpty())
                continue;
            if (unusable) {
                error = tr("%1 cannot be used alone: it would no longer reach the terminal. "
                           "Combine it with Ctrl, Alt or Meta.")
                            .arg(sequence.toString(QKeySequence::NativeText));
            } else if (conflict) {
                QStringList names;
                for (const QString &id : sharedWith) {
                    if (const ActionDef *other = Shortcuts::definition(id))
                        names << other->text;
                }
                error = tr("%1 is assigned to more than one command: %2.")
                            .arg(sequence.toString(QKeySequence::NativeText), names.join(QStringLiteral(", ")));
            }
        }
        row.reset->setEnabled(def && rowBindings(row) != def->defaults);
    }

    m_shortcutConflict = !error.isEmpty();
    m_shortcutError->setText(error);
    m_saveButton->setEnabled(!m_shortcutConflict);
    m_saveButton->setToolTip(m_shortcutConflict ? tr("Resolve the shortcut problem first") : QString());
}

// ---- save ------------------------------------------------------------------------------------

void PreferencesDialog::save()
{
    if (m_shortcutConflict)
        return;
    storeProfileEditor();

    Settings *settings = Settings::instance();
    settings->setCompactMode(m_compact->isChecked());
    settings->setShowStatusBar(m_statusBar->isChecked());
    settings->setShowScrollBar(m_scrollBar->isChecked());
    settings->setCopyOnSelect(m_copyOnSelect->isChecked());
    settings->setConfirmMultilinePaste(m_confirmPaste->isChecked());
    settings->setTrimPastedNewlines(m_trimPaste->isChecked());
    settings->setConfirmCloseRunning(m_confirmClose->isChecked());
    settings->setRestoreSession(m_restore->currentData().toString());

    // Profile names identify profiles to the user (and to --profile): keep
    // them distinct.
    QList<Profile> profiles;
    for (Profile profile : std::as_const(m_profiles)) {
        profile.sanitize();
        profile.name = ProfileManager::uniqueName(profiles, profile.name);
        profiles.append(profile);
    }
    if (profiles != ProfileManager::instance()->profiles() || m_defaultId != ProfileManager::instance()->defaultId())
        ProfileManager::instance()->setProfiles(profiles, m_defaultId);

    QHash<QString, QList<QKeySequence>> bindings;
    for (const ShortcutRow &row : std::as_const(m_shortcutRows)) {
        const QList<QKeySequence> wanted = rowBindings(row);
        bindings.insert(row.id, wanted);
        if (wanted != Shortcuts::current(row.id))
            m_shortcutsChanged = true;
    }
    if (m_shortcutsChanged)
        Shortcuts::store(bindings);

    accept();
}
