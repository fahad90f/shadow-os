// SPDX-License-Identifier: GPL-3.0-or-later
#include "tabbar.h"

#include "theme.h"
#include "tokens.h"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QToolButton>

namespace {
// Widths are multiples of the control height so they follow the tokens.
constexpr int kMinTabWidth = Tokens::ControlHeight * 4;
constexpr int kMaxTabWidth = Tokens::ControlHeight * 7;
constexpr int kTabHeight = Tokens::ControlHeight + 2 * Tokens::SpaceXs;
constexpr int kCloseButtonSize = Tokens::Icon;
} // namespace

TabBar::TabBar(QWidget *parent)
    : QTabBar(parent)
    , m_editor(new QLineEdit(this))
{
    setObjectName(QStringLiteral("TabBar"));
    setMovable(true);
    setExpanding(false);
    setDocumentMode(true);
    setDrawBase(false);
    setUsesScrollButtons(true);
    setElideMode(Qt::ElideRight);
    setSelectionBehaviorOnRemove(QTabBar::SelectPreviousTab);
    setIconSize(QSize(Tokens::IconSmall, Tokens::IconSmall));
    setFocusPolicy(Qt::NoFocus);        // the keyboard stays in the terminal; tabs have shortcuts
    setAccessibleName(tr("Tabs"));

    m_editor->setObjectName(QStringLiteral("TabRenameEdit"));
    m_editor->setAccessibleName(tr("Tab name"));
    m_editor->hide();
    m_editor->installEventFilter(this);
    connect(m_editor, &QLineEdit::editingFinished, this, [this] { finishEditing(true); });

    connect(this, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index >= 0)
            editTab(index);
        else
            Q_EMIT newTabRequested();
    });
    connect(this, &QTabBar::tabMoved, this, [this] { finishEditing(false); });
    connect(Theme::instance(), &Theme::changed, this, [this] {
        for (int i = 0; i < count(); ++i)
            setTabIcon(i, Theme::instance()->icon(tabData(i).toString()));
    });
}

int TabBar::insertTerminalTab(int index, const QString &title)
{
    const int at = insertTab(index, title);

    auto *close = new QToolButton(this);
    close->setObjectName(QStringLiteral("TabClose"));
    Theme::instance()->bindIcon(close, QStringLiteral("close"), QStringLiteral("textMuted"));
    close->setIconSize(QSize(Tokens::IconSmall - Tokens::SpaceXxs, Tokens::IconSmall - Tokens::SpaceXxs));
    close->setFixedSize(kCloseButtonSize, kCloseButtonSize);
    close->setAutoRaise(true);
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(tr("Close tab"));
    close->setAccessibleName(tr("Close tab"));
    connect(close, &QToolButton::clicked, this, [this, close] {
        for (int i = 0; i < count(); ++i) {
            if (tabButton(i, QTabBar::RightSide) == close) {
                Q_EMIT closeRequested(i);
                return;
            }
        }
    });
    setTabButton(at, QTabBar::RightSide, close);
    setTabGlyph(at, QStringLiteral("terminal"));
    return at;
}

void TabBar::setTabGlyph(int index, const QString &iconName)
{
    if (index < 0 || index >= count() || tabData(index).toString() == iconName)
        return;
    setTabData(index, iconName);
    setTabIcon(index, Theme::instance()->icon(iconName));
}

QSize TabBar::tabSizeHint(int index) const
{
    const QSize base = QTabBar::tabSizeHint(index);
    return QSize(qBound(kMinTabWidth, base.width(), kMaxTabWidth), kTabHeight);
}

QSize TabBar::minimumTabSizeHint(int index) const
{
    const QSize base = QTabBar::minimumTabSizeHint(index);
    return QSize(qMin(base.width(), kMinTabWidth), kTabHeight);
}

void TabBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        const int index = tabAt(event->position().toPoint());
        if (index >= 0) {
            Q_EMIT closeRequested(index);
            return;
        }
    }
    QTabBar::mouseReleaseEvent(event);
}

void TabBar::contextMenuEvent(QContextMenuEvent *event)
{
    const int index = tabAt(event->pos());
    if (index >= 0)
        Q_EMIT tabMenuRequested(index, event->globalPos());
}

void TabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    // Keep an open editor on its tab when tabs are added, removed or resized.
    if (m_editingIndex >= 0 && m_editingIndex < count()) {
        const int inset = Tokens::SpaceXs;
        m_editor->setGeometry(tabRect(m_editingIndex).adjusted(inset, inset, -inset, -inset));
    }
}

bool TabBar::isEditing() const
{
    return m_editingIndex >= 0;
}

void TabBar::editTab(int index)
{
    if (index < 0 || index >= count())
        return;
    finishEditing(true);
    m_editingIndex = index;
    m_editingOriginal = tabText(index);
    m_editor->setText(m_editingOriginal);
    const int inset = Tokens::SpaceXs;
    m_editor->setGeometry(tabRect(index).adjusted(inset, inset, -inset, -inset));
    m_editor->show();
    m_editor->raise();
    m_editor->selectAll();
    m_editor->setFocus(Qt::OtherFocusReason);
}

void TabBar::finishEditing(bool accept)
{
    if (m_editingIndex < 0)
        return;
    const int index = m_editingIndex;
    m_editingIndex = -1;            // first: hiding the editor ends editing again
    const QString text = m_editor->text().simplified();
    m_editor->hide();
    // Unchanged text keeps the tab on its automatic title.
    if (accept && index < count() && text != m_editingOriginal.simplified())
        Q_EMIT renameRequested(index, text);
    Q_EMIT editingFinished();
}

bool TabBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_editor && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            finishEditing(false);
            return true;
        }
    }
    return QTabBar::eventFilter(watched, event);
}
