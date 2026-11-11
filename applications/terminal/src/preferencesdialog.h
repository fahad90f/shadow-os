// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "profile.h"
#include "ui.h"

#include <QHash>
#include <QKeySequence>
#include <QList>

class QComboBox;
class QFontComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QToolButton;

// Preferences: general behaviour, profiles and keyboard shortcuts.
//
// The dialog edits copies; nothing changes until "Save", which writes
// terminal.conf and lets the running terminals pick the changes up.
class PreferencesDialog : public Ui::Dialog
{
    Q_OBJECT
public:
    enum class Page { General, Profiles, Shortcuts };

    explicit PreferencesDialog(Page page, QWidget *parent = nullptr);

    // True if key bindings were changed by this dialog.
    bool shortcutsChanged() const { return m_shortcutsChanged; }

private:
    QWidget *buildGeneralPage();
    QWidget *buildProfilesPage();
    QWidget *buildShortcutsPage();
    QWidget *scrollPage(QWidget *inner);

    // ---- profiles ------------------------------------------------------
    void refreshProfileList();
    void selectProfile(int index);
    void loadProfileEditor(const Profile &profile);
    void storeProfileEditor();
    void updateProfileEditorState();
    void addProfile();
    void duplicateProfile();
    void deleteProfile();
    void makeDefaultProfile();
    void chooseWorkingDirectory();

    // ---- shortcuts -----------------------------------------------------
    struct ShortcutRow
    {
        QString id;
        QKeySequenceEdit *primary = nullptr;
        QKeySequenceEdit *alternative = nullptr;
        QToolButton *reset = nullptr;
    };
    QList<QKeySequence> rowBindings(const ShortcutRow &row) const;
    void setRowBindings(const ShortcutRow &row, const QList<QKeySequence> &bindings);
    void checkShortcuts();

    void save();

    QListWidget *m_sidebar;
    QStackedWidget *m_pages;
    QPushButton *m_saveButton;

    // general
    Ui::Switch *m_compact;
    Ui::Switch *m_statusBar;
    Ui::Switch *m_scrollBar;
    Ui::Switch *m_copyOnSelect;
    Ui::Switch *m_confirmPaste;
    Ui::Switch *m_trimPaste;
    Ui::Switch *m_confirmClose;
    QComboBox *m_restore;

    // profiles
    QList<Profile> m_profiles;
    QString m_defaultId;
    int m_currentProfile = -1;
    bool m_loadingProfile = false;
    QListWidget *m_profileList;
    QToolButton *m_deleteProfile;
    QToolButton *m_defaultProfile;
    QWidget *m_profileEditor;
    QLineEdit *m_name;
    QComboBox *m_commandMode;
    QLineEdit *m_command;
    QLabel *m_commandHint;
    Ui::Switch *m_loginShell;
    QComboBox *m_dirMode;
    QLineEdit *m_dir;
    QToolButton *m_dirBrowse;
    QFontComboBox *m_font;
    QSpinBox *m_fontSize;
    QComboBox *m_scheme;
    QComboBox *m_cursorShape;
    Ui::Switch *m_cursorBlink;
    QSlider *m_opacity;
    QLabel *m_opacityValue;
    Ui::Switch *m_scrollbackUnlimited;
    QSpinBox *m_scrollback;
    QComboBox *m_bell;
    Ui::Switch *m_hold;

    // shortcuts
    QList<ShortcutRow> m_shortcutRows;
    QLabel *m_shortcutError;
    bool m_shortcutConflict = false;
    bool m_shortcutsChanged = false;
};
