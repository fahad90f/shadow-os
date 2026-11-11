// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractButton>
#include <QDialog>
#include <QString>

class QBoxLayout;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;
class QSpinBox;
class QToolButton;
class QVBoxLayout;
class QVariantAnimation;

// Widget building blocks that carry the NEXUS look: every size comes from the
// design tokens, every colour from the generated style sheet. The roles and
// kinds named here are the selectors used in style/terminal.qss.in.
namespace Ui {

// Text label. role: heading, title, section, rowTitle, secondary, caption,
// overline, error, status, statusStrong, statusDanger, mono, keycap — or
// nullptr for plain body text.
QLabel *label(const QString &text, const char *role = nullptr, QWidget *parent = nullptr);
void setRole(QWidget *widget, const char *role);

// Push button. kind: secondary (default), primary, danger, ghost.
QPushButton *button(const QString &text, const char *kind = "secondary", QWidget *parent = nullptr);

// Icon-only tool button with a tool tip (which is also its accessible name).
QToolButton *iconButton(const QString &icon, const QString &toolTip, QWidget *parent = nullptr);

QLineEdit *lineEdit(QWidget *parent = nullptr);
QComboBox *comboBox(QWidget *parent = nullptr);
QSpinBox *spinBox(int minimum, int maximum, QWidget *parent = nullptr);

// Row of a QListWidget at the height the style sheet's ::item box is made for.
QListWidgetItem *listItem(QListWidget *list, const QString &text);

// Menu prepared by Theme (rounded where the window system allows it).
QMenu *menu(QWidget *parent = nullptr);
QMenu *subMenu(QMenu *parent, const QString &title);

// Raised surface with a vertical layout: the container of settings rows.
QFrame *card(QWidget *parent = nullptr);
QVBoxLayout *cardLayout(QFrame *card);
// Settings row: title and optional description on the left, a control on the
// right. Appends a divider above itself unless it is the card's first row.
QWidget *addRow(QFrame *card, const QString &title, const QString &description, QWidget *control);
QFrame *divider(QWidget *parent = nullptr);

// Re-evaluate the style sheet after a dynamic property changed.
void repolish(QWidget *widget);
void setProperty(QWidget *widget, const char *name, const QVariant &value);

// Modal question in the NEXUS style. Returns true if the user confirmed.
// 'destructive' makes the confirm button red and Cancel the default.
bool confirm(QWidget *parent, const QString &title, const QString &message, const QString &confirmText,
             bool destructive = false);

// Confirmation for a paste that would enter several lines (or from which
// control characters were removed). Shows exactly what would be sent.
bool confirmPaste(QWidget *parent, const QString &text, bool controlsRemoved);

// On/off switch, the NEXUS equivalent of a check box in settings rows.
class Switch : public QAbstractButton
{
    Q_OBJECT
public:
    explicit Switch(QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void checkStateSet() override;
    void nextCheckState() override;

private:
    void animateTo(bool on);
    QVariantAnimation *m_animation;
    qreal m_position = 0.0;     // 0 = off, 1 = on
};

// Base of the application's dialogs: title block, content area and a footer
// with right-aligned buttons.
class Dialog : public QDialog
{
    Q_OBJECT
public:
    explicit Dialog(const QString &title, QWidget *parent = nullptr);

    void setSubtitle(const QString &text);
    // Layout of the area between header and footer.
    QVBoxLayout *content() const { return m_content; }
    // Footer buttons are added left to right; addFooterStretch() separates a
    // left-aligned group (e.g. "Delete") from the right-aligned ones.
    QPushButton *addFooterButton(const QString &text, const char *kind = "secondary");
    void addFooterWidget(QWidget *widget);
    void addFooterStretch();
    // Remove the header block (for dialogs that lay out their own).
    void hideHeader();

private:
    QWidget *m_header;
    QLabel *m_title;
    QLabel *m_subtitle;
    QVBoxLayout *m_content;
    QBoxLayout *m_footer;
};

} // namespace Ui
