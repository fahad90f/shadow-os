// SPDX-License-Identifier: GPL-3.0-or-later
#include "searchbar.h"

#include "theme.h"
#include "tokens.h"
#include "ui.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

SearchBar::SearchBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SearchBar"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(Tokens::SpaceMd, Tokens::SpaceSm, Tokens::SpaceSm, Tokens::SpaceSm);
    layout->setSpacing(Tokens::SpaceXs);

    m_edit = Ui::lineEdit(this);
    m_edit->setPlaceholderText(tr("Find in scrollback"));
    m_edit->setAccessibleName(tr("Find in scrollback"));
    m_edit->setClearButtonEnabled(false);
    m_edit->setMinimumWidth(Tokens::ControlHeight * 4);
    m_edit->addAction(Theme::instance()->icon(QStringLiteral("search"), QStringLiteral("textMuted")),
                      QLineEdit::LeadingPosition);
    m_edit->installEventFilter(this);

    auto textToggle = [this](const QString &text, const QString &toolTip) {
        auto *button = new QToolButton(this);
        button->setText(text);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFixedSize(Tokens::ControlHeight - Tokens::SpaceXs, Tokens::ControlHeight - Tokens::SpaceXs);
        button->setToolTip(toolTip);
        button->setAccessibleName(toolTip);
        button->setFocusPolicy(Qt::TabFocus);
        return button;
    };
    m_caseButton = textToggle(QStringLiteral("Aa"), tr("Match case"));
    m_regexButton = textToggle(QStringLiteral(".*"), tr("Regular expression (^ and $ match line ends)"));
    // A terminal is searched from the newest output upwards.
    m_previousButton = Ui::iconButton(QStringLiteral("chevron-up"), tr("Previous match, further up (Return)"), this);
    m_nextButton = Ui::iconButton(QStringLiteral("chevron-down"), tr("Next match, further down (Shift+Return)"),
                                  this);
    QToolButton *closeButton = Ui::iconButton(QStringLiteral("close"), tr("Close search (Esc)"), this);

    m_status = Ui::label(QString(), "caption", this);

    layout->addWidget(m_edit, 1);
    layout->addWidget(m_caseButton);
    layout->addWidget(m_regexButton);
    layout->addSpacing(Tokens::SpaceXs);
    layout->addWidget(m_previousButton);
    layout->addWidget(m_nextButton);
    layout->addWidget(m_status);
    layout->addWidget(closeButton);

    connect(m_edit, &QLineEdit::textChanged, this, &SearchBar::criteriaChanged);
    connect(m_caseButton, &QToolButton::toggled, this, &SearchBar::criteriaChanged);
    connect(m_regexButton, &QToolButton::toggled, this, &SearchBar::criteriaChanged);
    connect(m_previousButton, &QToolButton::clicked, this, &SearchBar::previousRequested);
    connect(m_nextButton, &QToolButton::clicked, this, &SearchBar::nextRequested);
    connect(closeButton, &QToolButton::clicked, this, [this] {
        hide();
        Q_EMIT closed();
    });
    connect(Theme::instance(), &Theme::changed, this, [this] {
        const QList<QAction *> glyphs = m_edit->actions();
        for (QAction *glyph : glyphs)
            glyph->setIcon(Theme::instance()->icon(QStringLiteral("search"), QStringLiteral("textMuted")));
    });
    setStatus(Status::Idle);
}

QString SearchBar::pattern() const
{
    return m_edit->text();
}

bool SearchBar::caseSensitive() const
{
    return m_caseButton->isChecked();
}

bool SearchBar::regularExpression() const
{
    return m_regexButton->isChecked();
}

void SearchBar::activate()
{
    show();
    m_edit->setFocus(Qt::ShortcutFocusReason);
    m_edit->selectAll();
}

void SearchBar::setStatus(Status status)
{
    const bool failed = status == Status::NotFound || status == Status::InvalidPattern;
    m_status->setText(status == Status::NotFound ? tr("No matches")
                      : status == Status::InvalidPattern ? tr("Invalid expression") : QString());
    m_status->setVisible(failed);
    Ui::setRole(m_status, failed ? "error" : "caption");
    Ui::setProperty(m_edit, "nxInvalid", failed);
    const bool usable = !m_edit->text().isEmpty() && status != Status::InvalidPattern;
    m_previousButton->setEnabled(usable);
    m_nextButton->setEnabled(usable);
}

bool SearchBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (key->modifiers() & Qt::ShiftModifier)
                Q_EMIT nextRequested();
            else
                Q_EMIT previousRequested();
            return true;
        case Qt::Key_Escape:
            hide();
            Q_EMIT closed();
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}
